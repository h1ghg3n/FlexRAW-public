#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QTemporaryDir>
#include <QUuid>

#include "catalog_database.h"
#include "catalog_photo_page.h"
#include "catalog_photo_repository.h"
#include "current_process_memory_probe.h"

namespace
{

using flexraw::core::catalog::CatalogFolderQueryResult;
using flexraw::core::catalog::CatalogPhotoPageCursor;
using flexraw::core::catalog::CatalogPhotoPageRequest;
using flexraw::core::catalog::CatalogPhotoQueryResult;
using flexraw::core::catalog::CatalogPhotoRepository;

constexpr int DefaultPhotoCount = 100000;
constexpr int DefaultFolderCount = 1000;
constexpr int DefaultSampleCount = 30;
constexpr int DenseFolderMaximumPhotoCount = 10000;

struct MeasurementSummary
{
    QString scope;
    QString position;
    int pageSize{0};
    int sampleCount{0};
    int returnedCount{0};
    qint64 medianMicroseconds{0};
    qint64 percentile95Microseconds{0};
    qint64 maximumMicroseconds{0};
};

// 목적: QString을 benchmark 표준 출력에 사용할 UTF-8 string으로 변환
// 입력: value: 변환할 Qt text
// 출력: UTF-8 byte sequence
[[nodiscard]] std::string toUtf8(const QString& value)
{
    const QByteArray bytes = value.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

// 목적: CSV field의 quote와 내부 quote를 escaping
// 입력: value: CSV에 기록할 text
// 출력: RFC 4180 방식으로 quote된 UTF-8 field
[[nodiscard]] std::string quoteCsv(const QString& value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return toUtf8(QStringLiteral("\"") + escaped + QStringLiteral("\""));
}

// 목적: synthetic photo id와 stable sort가 일치하는 display name 생성
// 입력: photoId: 1부터 시작하는 synthetic PhotoId
// 출력: zero-padded RAW display name
[[nodiscard]] QString photoDisplayName(qint64 photoId)
{
    return QStringLiteral("photo-%1.raw").arg(photoId, 8, 10, QLatin1Char('0'));
}

// 목적: synthetic Catalog의 canonical Folder path 생성
// 입력: folderIndex: 0부터 시작하는 Folder index
// 출력: slash separator를 사용하는 absolute-like benchmark path
[[nodiscard]] QString folderPath(int folderIndex)
{
    return QStringLiteral("C:/flexraw-catalog-benchmark/folder-%1").arg(folderIndex, 4, 10, QLatin1Char('0'));
}

// 목적: positive integer command-line option을 검증하며 읽기
// 입력: parser: 처리된 parser, option: 읽을 option, value: 결과 저장 위치
// 출력: option이 positive integer면 true
[[nodiscard]] bool readPositiveInteger(const QCommandLineParser& parser, const QCommandLineOption& option, int& value)
{
    bool parsed = false;
    const int candidate = parser.value(option).toInt(&parsed);
    if (!parsed || candidate <= 0)
    {
        return false;
    }
    value = candidate;
    return true;
}

// 목적: product query 측정용 대규모 Catalog row를 별도 SQLite connection으로 빠르게 seed
// 입력: catalogPath: migrated Catalog path, photoCount/folderCount: 생성 규모, denseFolderPhotoCount: 첫 Folder 규모
// 출력: seed 성공 여부
[[nodiscard]] bool seedCatalog(const QString& catalogPath, int photoCount, int folderCount, int denseFolderPhotoCount)
{
    const QString connectionName =
        QStringLiteral("catalog_pagination_seed_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(catalogPath);
    if (!database.open())
    {
        std::cerr << "Unable to open benchmark seed connection: " << toUtf8(database.lastError().text()) << '\n';
        database = QSqlDatabase{};
        QSqlDatabase::removeDatabase(connectionName);
        return false;
    }

    bool succeeded = database.transaction();
    QString queryError;
    {
        QSqlQuery query(database);
        succeeded = succeeded && query.prepare(QStringLiteral(
                                     "INSERT INTO photos (id, source_path, last_known_path, extension, display_name, "
                                     "kind, scan_status, source_size_bytes, source_mtime_ms, source_sha256, "
                                     "source_binding_state, imported_at_ms, source_parent_path) VALUES (:id, "
                                     ":sourcePath, :lastKnownPath, 'raw', :displayName, 1, 1, 0, 0, NULL, 0, 0, "
                                     ":sourceParentPath)"));

        for (int photoIndex = 0; succeeded && photoIndex < photoCount; ++photoIndex)
        {
            const qint64 photoId = static_cast<qint64>(photoIndex) + 1;
            int folderIndex = 0;
            if (photoIndex >= denseFolderPhotoCount && folderCount > 1)
            {
                folderIndex = 1 + ((photoIndex - denseFolderPhotoCount) % (folderCount - 1));
            }
            const QString parentPath = folderPath(folderIndex);
            const QString displayName = photoDisplayName(photoId);
            const QString sourcePath = parentPath + QLatin1Char('/') + displayName;
            query.bindValue(QStringLiteral(":id"), photoId);
            query.bindValue(QStringLiteral(":sourcePath"), sourcePath);
            query.bindValue(QStringLiteral(":lastKnownPath"), sourcePath);
            query.bindValue(QStringLiteral(":displayName"), displayName);
            query.bindValue(QStringLiteral(":sourceParentPath"), parentPath);
            succeeded = query.exec();
        }
        queryError = query.lastError().text();
    }

    if (succeeded)
    {
        succeeded = database.commit();
    }
    else
    {
        database.rollback();
    }
    if (!succeeded)
    {
        std::cerr << "Unable to seed benchmark Catalog: " << toUtf8(queryError) << '\n';
    }
    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
    return succeeded;
}

// 목적: sorted duration sample에서 median, p95와 maximum을 계산
// 입력: durations: microsecond 단위 non-empty sample
// 출력: latency 통계가 채워진 summary
[[nodiscard]] MeasurementSummary summarize(MeasurementSummary summary, std::vector<qint64> durations)
{
    std::sort(durations.begin(), durations.end());
    const std::size_t medianIndex = durations.size() / 2;
    const std::size_t percentile95Index = ((durations.size() * 95U) + 99U) / 100U - 1U;
    summary.medianMicroseconds = durations[medianIndex];
    summary.percentile95Microseconds = durations[percentile95Index];
    summary.maximumMicroseconds = durations.back();
    return summary;
}

// 목적: 동일한 bounded photo page query의 warm-cache latency 분포 측정
// 입력: repository/request: 측정 대상, scope/position: CSV label, sampleCount: 반복 횟수, error: 실패 설명
// 출력: 성공 시 page latency summary
[[nodiscard]] std::optional<MeasurementSummary> measurePhotoPage(const CatalogPhotoRepository& repository,
                                                                 const CatalogPhotoPageRequest& request,
                                                                 const QString& scope,
                                                                 const QString& position,
                                                                 int sampleCount,
                                                                 QString& error)
{
    const CatalogPhotoQueryResult warmup = repository.queryPage(request);
    if (warmup.hasError())
    {
        error = warmup.error().message;
        return std::nullopt;
    }

    std::vector<qint64> durations;
    durations.reserve(static_cast<std::size_t>(sampleCount));
    int returnedCount = 0;
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        QElapsedTimer timer;
        timer.start();
        const CatalogPhotoQueryResult result = repository.queryPage(request);
        const qint64 elapsedNanoseconds = timer.nsecsElapsed();
        if (result.hasError())
        {
            error = result.error().message;
            return std::nullopt;
        }
        returnedCount = result.value().photos.size();
        durations.push_back((elapsedNanoseconds + 999) / 1000);
    }

    MeasurementSummary summary{scope, position, request.pageSize, sampleCount, returnedCount};
    return summarize(std::move(summary), std::move(durations));
}

// 목적: distinct Folder summary query의 warm-cache latency 분포 측정
// 입력: repository: 측정 대상, sampleCount: 반복 횟수, error: 실패 설명
// 출력: 성공 시 Folder query latency summary
[[nodiscard]] std::optional<MeasurementSummary> measureFolderSummary(const CatalogPhotoRepository& repository,
                                                                     int sampleCount,
                                                                     QString& error)
{
    const CatalogFolderQueryResult warmup = repository.queryFolders();
    if (warmup.hasError())
    {
        error = warmup.error().message;
        return std::nullopt;
    }

    std::vector<qint64> durations;
    durations.reserve(static_cast<std::size_t>(sampleCount));
    int returnedCount = 0;
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        QElapsedTimer timer;
        timer.start();
        const CatalogFolderQueryResult result = repository.queryFolders();
        const qint64 elapsedNanoseconds = timer.nsecsElapsed();
        if (result.hasError())
        {
            error = result.error().message;
            return std::nullopt;
        }
        returnedCount = result.value().size();
        durations.push_back((elapsedNanoseconds + 999) / 1000);
    }

    MeasurementSummary summary{QStringLiteral("folder-summary"), QStringLiteral("all"), 0, sampleCount, returnedCount};
    return summarize(std::move(summary), std::move(durations));
}

// 목적: benchmark measurement 한 건을 versioned CSV row로 출력
// 입력: runId/target/규모: 실행 metadata, summary: latency 측정값
// 출력: 없음
void writeMeasurement(const QString& runId,
                      const QString& target,
                      int photoCount,
                      int folderCount,
                      const MeasurementSummary& summary,
                      const std::optional<flexraw::platform::ProcessMemorySnapshot>& baselineMemory,
                      const std::optional<flexraw::platform::ProcessMemorySnapshot>& finalMemory)
{
    std::cout << "1," << quoteCsv(runId) << ',' << quoteCsv(target) << ',' << photoCount << ',' << folderCount << ','
              << quoteCsv(summary.scope) << ',' << quoteCsv(summary.position) << ',' << summary.pageSize << ','
              << summary.sampleCount << ',' << summary.returnedCount << ',' << summary.medianMicroseconds << ','
              << summary.percentile95Microseconds << ',' << summary.maximumMicroseconds << ',';
    if (baselineMemory.has_value())
    {
        std::cout << baselineMemory->residentBytes;
    }
    std::cout << ',';
    if (finalMemory.has_value())
    {
        std::cout << finalMemory->residentBytes << ',' << finalMemory->peakResidentBytes;
    }
    else
    {
        std::cout << ',';
    }
    std::cout << '\n';
}

// 목적: Catalog benchmark option 해석, synthetic seed와 product query latency 측정 실행
// 입력: application: 초기화된 Qt Core application
// 출력: 성공 0, 인자·database·query 실패 시 non-zero process code
int runBenchmark(QCoreApplication& application)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QCoreApplication::translate(
        "CatalogPaginationBenchmark", "Measures bounded Catalog and exact-Folder keyset queries."));
    parser.addHelpOption();
    const QCommandLineOption photosOption(
        QStringLiteral("photos"),
        QCoreApplication::translate("CatalogPaginationBenchmark", "Synthetic photo count."),
        QCoreApplication::translate("CatalogPaginationBenchmark", "count"),
        QString::number(DefaultPhotoCount));
    const QCommandLineOption foldersOption(
        QStringLiteral("folders"),
        QCoreApplication::translate("CatalogPaginationBenchmark", "Synthetic Folder count."),
        QCoreApplication::translate("CatalogPaginationBenchmark", "count"),
        QString::number(DefaultFolderCount));
    const QCommandLineOption samplesOption(
        QStringLiteral("samples"),
        QCoreApplication::translate("CatalogPaginationBenchmark", "Sample count per query."),
        QCoreApplication::translate("CatalogPaginationBenchmark", "count"),
        QString::number(DefaultSampleCount));
    const QCommandLineOption runIdOption(
        QStringLiteral("run-id"),
        QCoreApplication::translate("CatalogPaginationBenchmark", "Measurement run label."),
        QCoreApplication::translate("CatalogPaginationBenchmark", "id"),
        QStringLiteral("manual-1"));
    const QCommandLineOption targetOption(QStringLiteral("target"),
                                          QCoreApplication::translate("CatalogPaginationBenchmark", "Machine label."),
                                          QCoreApplication::translate("CatalogPaginationBenchmark", "name"),
                                          QStringLiteral("unspecified"));
    parser.addOptions({photosOption, foldersOption, samplesOption, runIdOption, targetOption});
    parser.process(application);

    int photoCount = 0;
    int folderCount = 0;
    int sampleCount = 0;
    if (!readPositiveInteger(parser, photosOption, photoCount) ||
        !readPositiveInteger(parser, foldersOption, folderCount) ||
        !readPositiveInteger(parser, samplesOption, sampleCount) || folderCount > photoCount)
    {
        std::cerr << "--photos, --folders and --samples must be positive, and folders cannot exceed photos.\n";
        return 2;
    }

    QTemporaryDir directory;
    if (!directory.isValid())
    {
        std::cerr << "Unable to create temporary benchmark directory.\n";
        return 3;
    }
    const QString catalogPath = directory.filePath(QStringLiteral("representative.flexraw-catalog"));
    flexraw::core::catalog::CatalogDatabaseOpenResult databaseResult =
        flexraw::core::catalog::CatalogDatabase::open(catalogPath);
    if (databaseResult.hasError())
    {
        std::cerr << "Unable to create benchmark Catalog: " << toUtf8(databaseResult.error().message) << '\n';
        return 4;
    }

    const int denseFolderPhotoCount =
        std::min(photoCount, std::max(1, std::min(DenseFolderMaximumPhotoCount, photoCount / 10)));
    if (!seedCatalog(catalogPath, photoCount, folderCount, denseFolderPhotoCount))
    {
        return 5;
    }

    const std::unique_ptr<flexraw::platform::IProcessMemoryProbe> processMemoryProbe =
        flexraw::platform::createCurrentProcessMemoryProbe();
    const std::optional<flexraw::platform::ProcessMemorySnapshot> baselineMemory = processMemoryProbe->snapshot();
    CatalogPhotoRepository repository(*databaseResult.value());
    const QString denseFolderPath = folderPath(0);
    const std::vector<int> pageSizes{50, 100, 200};
    std::vector<MeasurementSummary> measurements;
    QString error;
    for (const int pageSize : pageSizes)
    {
        CatalogPhotoPageRequest catalogFirst;
        catalogFirst.pageSize = pageSize;
        std::optional<MeasurementSummary> result = measurePhotoPage(
            repository, catalogFirst, QStringLiteral("catalog"), QStringLiteral("first"), sampleCount, error);
        if (!result.has_value())
        {
            break;
        }
        measurements.push_back(*result);

        CatalogPhotoPageRequest catalogMiddle = catalogFirst;
        const qint64 catalogMiddlePhotoId = std::max<qint64>(1, photoCount / 2);
        catalogMiddle.cursor = CatalogPhotoPageCursor{
            photoDisplayName(catalogMiddlePhotoId), flexraw::core::types::PhotoId{catalogMiddlePhotoId}, std::nullopt};
        result = measurePhotoPage(
            repository, catalogMiddle, QStringLiteral("catalog"), QStringLiteral("middle"), sampleCount, error);
        if (!result.has_value())
        {
            break;
        }
        measurements.push_back(*result);

        CatalogPhotoPageRequest folderFirst;
        folderFirst.pageSize = pageSize;
        folderFirst.exactFolderPath = denseFolderPath;
        result = measurePhotoPage(
            repository, folderFirst, QStringLiteral("exact-folder"), QStringLiteral("first"), sampleCount, error);
        if (!result.has_value())
        {
            break;
        }
        measurements.push_back(*result);

        CatalogPhotoPageRequest folderMiddle = folderFirst;
        const qint64 folderMiddlePhotoId = std::max<qint64>(1, denseFolderPhotoCount / 2);
        folderMiddle.cursor = CatalogPhotoPageCursor{
            photoDisplayName(folderMiddlePhotoId), flexraw::core::types::PhotoId{folderMiddlePhotoId}, denseFolderPath};
        result = measurePhotoPage(
            repository, folderMiddle, QStringLiteral("exact-folder"), QStringLiteral("middle"), sampleCount, error);
        if (!result.has_value())
        {
            break;
        }
        measurements.push_back(*result);
    }

    const std::optional<MeasurementSummary> folderSummary =
        error.isEmpty() ? measureFolderSummary(repository, sampleCount, error) : std::nullopt;
    if (!folderSummary.has_value())
    {
        std::cerr << "Catalog benchmark query failed: " << toUtf8(error) << '\n';
        return 6;
    }
    measurements.push_back(*folderSummary);

    const std::optional<flexraw::platform::ProcessMemorySnapshot> finalMemory = processMemoryProbe->snapshot();
    std::cout << "schema_version,run_id,target,photo_count,folder_count,scope,position,page_size,samples,"
                 "returned_count,median_us,p95_us,max_us,baseline_rss_bytes,final_rss_bytes,peak_rss_bytes\n";
    for (const MeasurementSummary& measurement : measurements)
    {
        writeMeasurement(parser.value(runIdOption),
                         parser.value(targetOption),
                         photoCount,
                         folderCount,
                         measurement,
                         baselineMemory,
                         finalMemory);
    }
    return 0;
}

}  // namespace

// 목적: Catalog pagination benchmark용 Qt Core runtime 생성과 측정 실행
// 입력: argc/argv: benchmark command-line arguments
// 출력: benchmark 성공 또는 실패 process exit code
int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Flexraw Catalog Pagination Benchmark"));
    return runBenchmark(application);
}
