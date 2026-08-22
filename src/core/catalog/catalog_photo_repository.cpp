#include "catalog_photo_repository.h"

#include <algorithm>
#include <utility>

#include <QDateTime>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "source_path.h"

namespace flexraw::core::catalog
{
namespace
{

// 목적: 사진 persistence 실패 정보를 DatabaseError CoreError로 생성
// 입력: message: 호출자에게 전달할 database 오류 설명
// 출력: DatabaseError로 분류된 CoreError 객체
[[nodiscard]] types::CoreError makeDatabaseError(QString message)
{
    return {types::ErrorCode::DatabaseError, std::move(message)};
}

// 목적: source binding의 optimistic state 변경 충돌을 CoreError로 생성
// 입력: message: 호출자에게 전달할 conflict 설명
// 출력: Conflict로 분류된 CoreError 객체
[[nodiscard]] types::CoreError makeConflictError(QString message)
{
    return {types::ErrorCode::Conflict, std::move(message)};
}

// 목적: CatalogEntry가 photos table에 저장 가능한 최소 정보를 보유하는지 확인
// 입력: entry: 저장할 사진 항목
// 출력: 유효 여부
[[nodiscard]] bool isPersistableEntry(const CatalogEntry& entry)
{
    return !entry.file.path.trimmed().isEmpty() && !entry.file.extension.trimmed().isEmpty() &&
           !entry.file.displayName.trimmed().isEmpty() && entry.file.kind != types::SupportedFileKind::Unknown &&
           !sourceParentPath(entry.file.path).isEmpty();
}

// 목적: source 상태만 기록해도 되는 미해결 observation state인지 확인
// 입력: state: repository에 기록할 source binding state
// 출력: identity나 fingerprint를 변경하지 않는 관찰 state면 true
[[nodiscard]] bool isRecordableSourceState(SourceBindingState state)
{
    return state == SourceBindingState::Missing || state == SourceBindingState::VerificationRequired ||
           state == SourceBindingState::IdentityUnverified || state == SourceBindingState::ReplacementDetected ||
           state == SourceBindingState::Unreadable;
}

// 목적: database에서 읽은 파일 종류 enum 값이 지원 범위인지 확인
// 입력: value: SQLite photos.kind 열 값
// 출력: 지원 파일 종류 여부
[[nodiscard]] bool isSupportedFileKind(int value)
{
    return value == static_cast<int>(types::SupportedFileKind::Raw) ||
           value == static_cast<int>(types::SupportedFileKind::RasterImage);
}

// 목적: database에서 읽은 scan 상태 enum 값이 지원 범위인지 확인
// 입력: value: SQLite photos.scan_status 열 값
// 출력: 지원 scan 상태 여부
[[nodiscard]] bool isFileScanStatus(int value)
{
    return value >= static_cast<int>(types::FileScanStatus::Pending) &&
           value <= static_cast<int>(types::FileScanStatus::Failed);
}

// 목적: 사진 파일의 현재 수정 시각을 millisecond epoch 값으로 조회
// 입력: path: 수정 시각을 읽을 사진 파일 경로
// 출력: 파일이 없거나 시각이 없으면 0, 그 외 마지막 수정 시각
[[nodiscard]] qint64 fileModifiedAtMs(const QString& path)
{
    const QDateTime modifiedAt = QFileInfo(path).lastModified();
    return modifiedAt.isValid() ? modifiedAt.toMSecsSinceEpoch() : 0;
}

// 목적: 사진 source의 현재 file size를 byte 단위로 조회
// 입력: path: size를 읽을 source file 경로
// 출력: file이 없거나 file이 아니면 0, 그 외 byte size
[[nodiscard]] qint64 fileSizeBytes(const QString& path)
{
    const QFileInfo fileInfo(path);
    return fileInfo.isFile() ? fileInfo.size() : 0;
}

// 목적: SQLite query의 현재 row를 source binding을 포함한 catalog photo record로 변환
// 입력: query: photo record column을 정해진 순서로 보유한 query row
// 출력: 검증된 CatalogPhotoRecord 또는 손상된 catalog data 오류
[[nodiscard]] types::Result<CatalogPhotoRecord, types::CoreError> readPhotoRecord(const QSqlQuery& query)
{
    const int kindValue = query.value(5).toInt();
    const int scanStatusValue = query.value(6).toInt();
    const int sourceStateValue = query.value(10).toInt();
    const types::PhotoId photoId{query.value(0).toLongLong()};
    const types::SourceFingerprint fingerprint{
        query.value(7).toLongLong(), query.value(8).toLongLong(), query.value(9).toByteArray()};

    if (!types::isValidPhotoId(photoId) || query.value(2).toString().trimmed().isEmpty() ||
        !isSupportedFileKind(kindValue) || !isFileScanStatus(scanStatusValue) ||
        !isSourceBindingState(sourceStateValue) || !types::isValidSourceFingerprint(fingerprint))
    {
        return types::Result<CatalogPhotoRecord, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("The catalog contains an invalid photo record.")));
    }

    std::optional<types::SourceLocator> source;
    if (!query.value(1).isNull())
    {
        const QString sourcePath = query.value(1).toString();
        if (sourcePath.trimmed().isEmpty())
        {
            return types::Result<CatalogPhotoRecord, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("The catalog contains an empty source locator.")));
        }
        source = types::SourceLocator{sourcePath};
    }

    return types::Result<CatalogPhotoRecord, types::CoreError>::success({
        photoId,
        std::move(source),
        query.value(2).toString(),
        query.value(3).toString(),
        query.value(4).toString(),
        static_cast<types::SupportedFileKind>(kindValue),
        static_cast<types::FileScanStatus>(scanStatusValue),
        fingerprint,
        static_cast<SourceBindingState>(sourceStateValue),
    });
}

// 목적: source baseline을 변경하는 command에 필요한 입력 검증
// 입력: photoId: 대상 identity, fingerprint: 새로 저장할 baseline
// 출력: 유효한 PhotoId와 완전한 SHA-256 fingerprint면 true
[[nodiscard]] bool isValidBaselineUpdate(types::PhotoId photoId, const types::SourceFingerprint& fingerprint)
{
    return types::isValidPhotoId(photoId) && types::isValidSourceFingerprint(fingerprint) &&
           types::hasSourceContentHash(fingerprint);
}

// 목적: bounded keyset page 요청의 크기, folder scope와 cursor invariant 검증
// 입력: request: caller가 지정한 page 조건, folderScope: 정규화된 optional exact-folder path
// 출력: 유효하면 빈 값, 아니면 InvalidArgument 오류
[[nodiscard]] std::optional<types::CoreError> validatePageRequest(const CatalogPhotoPageRequest& request,
                                                                  const std::optional<QString>& folderScope)
{
    if (request.pageSize <= 0 || request.pageSize > MaximumCatalogPhotoPageSize)
    {
        return types::CoreError{
            types::ErrorCode::InvalidArgument,
            QStringLiteral("Catalog photo page size must be between 1 and %1.").arg(MaximumCatalogPhotoPageSize),
        };
    }
    if (request.direction == CatalogPhotoPageDirection::Backward && !request.cursor.has_value())
    {
        return types::CoreError{
            types::ErrorCode::InvalidArgument,
            QStringLiteral("Backward catalog paging requires a cursor."),
        };
    }
    if (request.exactFolderPath.has_value() && folderScope->isEmpty())
    {
        return types::CoreError{
            types::ErrorCode::InvalidArgument,
            QStringLiteral("Catalog photo folder scope is invalid."),
        };
    }
    if (request.cursor.has_value() && !types::isValidPhotoId(request.cursor->photoId))
    {
        return types::CoreError{
            types::ErrorCode::InvalidArgument,
            QStringLiteral("Catalog photo page cursor is invalid."),
        };
    }
    if (request.cursor.has_value())
    {
        const std::optional<QString> cursorScope =
            request.cursor->exactFolderPath.has_value()
                ? std::optional<QString>{normalizeSourceFolderPath(*request.cursor->exactFolderPath)}
                : std::nullopt;
        if ((request.cursor->exactFolderPath.has_value() && cursorScope->isEmpty()) || cursorScope != folderScope)
        {
            return types::CoreError{
                types::ErrorCode::InvalidArgument,
                QStringLiteral("Catalog photo page cursor does not match the requested folder scope."),
            };
        }
    }
    return std::nullopt;
}

// 목적: page 방향, optional folder scope와 cursor에 맞는 SQLite keyset query 생성
// 입력: request: 검증된 page 요청, folderScope: 정규화된 optional exact-folder path
// 출력: display name·PhotoId 순서의 LIMIT query
[[nodiscard]] QString makePageStatement(const CatalogPhotoPageRequest& request,
                                        const std::optional<QString>& folderScope)
{
    const bool backwards = request.direction == CatalogPhotoPageDirection::Backward;
    QString statement =
        QStringLiteral("SELECT id, source_path, last_known_path, extension, display_name, kind, scan_status, "
                       "source_size_bytes, source_mtime_ms, source_sha256, source_binding_state FROM photos ");
    if (folderScope.has_value())
    {
        statement += QStringLiteral("WHERE source_parent_path = :sourceParentPath ");
    }
    if (request.cursor.has_value())
    {
        const QString comparison = backwards ? QStringLiteral("<") : QStringLiteral(">");
        statement += folderScope.has_value() ? QStringLiteral("AND ") : QStringLiteral("WHERE ");
        statement += QStringLiteral("display_name COLLATE NOCASE %1= :cursorDisplayName COLLATE NOCASE "
                                    "AND ((display_name COLLATE NOCASE %1 :cursorDisplayName COLLATE NOCASE) "
                                    "OR (display_name COLLATE NOCASE = :cursorDisplayName COLLATE NOCASE "
                                    "AND id %1 :cursorPhotoId)) ")
                         .arg(comparison);
    }
    statement += backwards ? QStringLiteral("ORDER BY display_name COLLATE NOCASE DESC, id DESC ")
                           : QStringLiteral("ORDER BY display_name COLLATE NOCASE ASC, id ASC ");
    statement += QStringLiteral("LIMIT :fetchLimit");
    return statement;
}

// 목적: 반환 record의 stable sort key와 query scope를 후속 page cursor로 변환
// 입력: photo: page 경계의 catalog photo, folderScope: page에 적용된 optional exact-folder path
// 출력: display name, immutable PhotoId와 normalized folder scope cursor
[[nodiscard]] CatalogPhotoPageCursor makePageCursor(const CatalogPhotoRecord& photo,
                                                    const std::optional<QString>& folderScope)
{
    return {photo.displayName, photo.id, folderScope};
}

}  // namespace

CatalogPhotoRepository::CatalogPhotoRepository(CatalogDatabase& database) : m_database(database) {}

CatalogPhotoStoreResult CatalogPhotoRepository::upsert(const QVector<CatalogEntry>& entries)
{
    for (const CatalogEntry& entry : entries)
    {
        if (!isPersistableEntry(entry))
        {
            return CatalogPhotoStoreResult::failure(
                {types::ErrorCode::InvalidArgument, QStringLiteral("Catalog entry is incomplete.")});
        }
    }

    QSqlDatabase& database = m_database.m_database;

    if (!database.transaction())
    {
        return CatalogPhotoStoreResult::failure(makeDatabaseError(
            QStringLiteral("Unable to begin the photo persistence transaction: %1").arg(database.lastError().text())));
    }

    QSqlQuery query(database);
    constexpr auto UpsertStatement = R"(
        INSERT INTO photos (
            source_path, source_parent_path, last_known_path, extension, display_name, kind, scan_status,
            source_size_bytes, source_mtime_ms, source_sha256, source_binding_state, imported_at_ms)
        VALUES (
            :path, :sourceParentPath, :path, :extension, :displayName, :kind, :scanStatus,
            :sourceSizeBytes, :sourceMtimeMs, NULL, :sourceState, :importedAtMs)
        ON CONFLICT(source_path) DO UPDATE SET
            source_parent_path = excluded.source_parent_path,
            extension = excluded.extension,
            display_name = excluded.display_name,
            kind = excluded.kind,
            scan_status = excluded.scan_status
    )";

    if (!query.prepare(QString::fromUtf8(UpsertStatement)))
    {
        database.rollback();
        return CatalogPhotoStoreResult::failure(makeDatabaseError(
            QStringLiteral("Unable to prepare the photo upsert statement: %1").arg(query.lastError().text())));
    }

    const qint64 importedAtMs = QDateTime::currentMSecsSinceEpoch();

    for (const CatalogEntry& entry : entries)
    {
        query.bindValue(QStringLiteral(":path"), entry.file.path);
        query.bindValue(QStringLiteral(":sourceParentPath"), sourceParentPath(entry.file.path));
        query.bindValue(QStringLiteral(":extension"), entry.file.extension);
        query.bindValue(QStringLiteral(":displayName"), entry.file.displayName);
        query.bindValue(QStringLiteral(":kind"), static_cast<int>(entry.file.kind));
        query.bindValue(QStringLiteral(":scanStatus"), static_cast<int>(entry.status));
        query.bindValue(QStringLiteral(":sourceSizeBytes"), fileSizeBytes(entry.file.path));
        query.bindValue(QStringLiteral(":sourceMtimeMs"), fileModifiedAtMs(entry.file.path));
        query.bindValue(QStringLiteral(":sourceState"), static_cast<int>(SourceBindingState::FingerprintPending));
        query.bindValue(QStringLiteral(":importedAtMs"), importedAtMs);

        if (!query.exec())
        {
            database.rollback();
            return CatalogPhotoStoreResult::failure(
                makeDatabaseError(QStringLiteral("Unable to upsert catalog photo: %1").arg(query.lastError().text())));
        }
    }

    if (!database.commit())
    {
        database.rollback();
        return CatalogPhotoStoreResult::failure(makeDatabaseError(
            QStringLiteral("Unable to commit the photo persistence transaction: %1").arg(database.lastError().text())));
    }

    return CatalogPhotoStoreResult::success(entries.size());
}

// 목적: optional exact-folder scope와 stable cursor로 bounded catalog photo page 조회
// 입력: request: page 크기, 이동 방향, optional exclusive cursor와 folder scope
// 출력: 오름차순 record와 이전·다음 cursor 또는 validation·database 오류
CatalogPhotoQueryResult CatalogPhotoRepository::queryPage(const CatalogPhotoPageRequest& request) const
{
    const std::optional<QString> folderScope =
        request.exactFolderPath.has_value()
            ? std::optional<QString>{normalizeSourceFolderPath(*request.exactFolderPath)}
            : std::nullopt;
    if (const std::optional<types::CoreError> error = validatePageRequest(request, folderScope); error.has_value())
    {
        return CatalogPhotoQueryResult::failure(*error);
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(makePageStatement(request, folderScope));
    if (folderScope.has_value())
    {
        query.bindValue(QStringLiteral(":sourceParentPath"), *folderScope);
    }
    if (request.cursor.has_value())
    {
        query.bindValue(QStringLiteral(":cursorDisplayName"), request.cursor->displayName);
        query.bindValue(QStringLiteral(":cursorPhotoId"), request.cursor->photoId.value);
    }
    query.bindValue(QStringLiteral(":fetchLimit"), request.pageSize + 1);
    if (!query.exec())
    {
        return CatalogPhotoQueryResult::failure(
            makeDatabaseError(QStringLiteral("Unable to query catalog photo page: %1").arg(query.lastError().text())));
    }

    QVector<CatalogPhotoRecord> records;
    records.reserve(request.pageSize + 1);
    while (query.next())
    {
        types::Result<CatalogPhotoRecord, types::CoreError> record = readPhotoRecord(query);
        if (record.hasError())
        {
            return CatalogPhotoQueryResult::failure(record.error());
        }
        records.push_back(std::move(record.value()));
    }

    const bool hasMore = records.size() > request.pageSize;
    if (hasMore)
    {
        records.removeLast();
    }
    const bool backwards = request.direction == CatalogPhotoPageDirection::Backward;
    if (backwards)
    {
        std::reverse(records.begin(), records.end());
    }

    CatalogPhotoPage page;
    page.photos = std::move(records);
    if (page.photos.isEmpty())
    {
        return CatalogPhotoQueryResult::success(std::move(page));
    }

    if (backwards)
    {
        if (hasMore)
        {
            page.previousCursor = makePageCursor(page.photos.front(), folderScope);
        }
        page.nextCursor = makePageCursor(page.photos.back(), folderScope);
    }
    else
    {
        if (request.cursor.has_value())
        {
            page.previousCursor = makePageCursor(page.photos.front(), folderScope);
        }
        if (hasMore)
        {
            page.nextCursor = makePageCursor(page.photos.back(), folderScope);
        }
    }
    return CatalogPhotoQueryResult::success(std::move(page));
}

// 목적: catalog-local PhotoId로 photo record 조회
// 입력: photoId: 조회할 안정적 photo identity
// 출력: 일치하는 record 또는 없으면 빈 값
CatalogPhotoRecordResult CatalogPhotoRepository::findById(types::PhotoId photoId) const
{
    if (!types::isValidPhotoId(photoId))
    {
        return CatalogPhotoRecordResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Photo ID must be positive.")});
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral(
        "SELECT id, source_path, last_known_path, extension, display_name, kind, scan_status, "
        "source_size_bytes, source_mtime_ms, source_sha256, source_binding_state FROM photos WHERE id = :photoId"));
    query.bindValue(QStringLiteral(":photoId"), photoId.value);
    if (!query.exec())
    {
        return CatalogPhotoRecordResult::failure(
            makeDatabaseError(QStringLiteral("Unable to find catalog photo by ID: %1").arg(query.lastError().text())));
    }
    if (!query.next())
    {
        return CatalogPhotoRecordResult::success(std::nullopt);
    }

    types::Result<CatalogPhotoRecord, types::CoreError> record = readPhotoRecord(query);
    return record.hasError() ? CatalogPhotoRecordResult::failure(record.error())
                             : CatalogPhotoRecordResult::success(std::move(record.value()));
}

// 목적: 현재 source locator path에 binding된 photo record 조회
// 입력: sourcePath: catalog에 저장된 현재 source path
// 출력: path에 binding된 record 또는 없으면 빈 값
CatalogPhotoRecordResult CatalogPhotoRepository::findBySourcePath(const QString& sourcePath) const
{
    if (sourcePath.trimmed().isEmpty())
    {
        return CatalogPhotoRecordResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Source path is empty.")});
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("SELECT id, source_path, last_known_path, extension, display_name, kind, scan_status, "
                                 "source_size_bytes, source_mtime_ms, source_sha256, source_binding_state "
                                 "FROM photos WHERE source_path = :sourcePath"));
    query.bindValue(QStringLiteral(":sourcePath"), sourcePath);
    if (!query.exec())
    {
        return CatalogPhotoRecordResult::failure(makeDatabaseError(
            QStringLiteral("Unable to find catalog photo by source path: %1").arg(query.lastError().text())));
    }
    if (!query.next())
    {
        return CatalogPhotoRecordResult::success(std::nullopt);
    }

    types::Result<CatalogPhotoRecord, types::CoreError> record = readPhotoRecord(query);
    return record.hasError() ? CatalogPhotoRecordResult::failure(record.error())
                             : CatalogPhotoRecordResult::success(std::move(record.value()));
}

// 목적: identity 해결 전에 관찰된 source 상태만 기록
// 입력: photoId: 대상 identity, state: 해결되지 않은 관찰 상태
// 출력: fingerprint와 develop data를 변경하지 않은 성공 표식 또는 오류
CatalogPhotoMutationResult CatalogPhotoRepository::recordSourceState(types::PhotoId photoId, SourceBindingState state)
{
    if (!types::isValidPhotoId(photoId) || !isRecordableSourceState(state))
    {
        return CatalogPhotoMutationResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Source observation state is invalid.")});
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("UPDATE photos SET source_binding_state = :state "
                                 "WHERE id = :photoId AND source_path IS NOT NULL"));
    query.bindValue(QStringLiteral(":state"), static_cast<int>(state));
    query.bindValue(QStringLiteral(":photoId"), photoId.value);
    if (!query.exec())
    {
        return CatalogPhotoMutationResult::failure(makeDatabaseError(
            QStringLiteral("Unable to record catalog source state: %1").arg(query.lastError().text())));
    }
    if (query.numRowsAffected() != 1)
    {
        return CatalogPhotoMutationResult::failure(
            makeConflictError(QStringLiteral("Catalog source binding changed before its state was recorded.")));
    }

    return CatalogPhotoMutationResult::success({});
}

// 목적: import 직후 pending source의 baseline SHA-256를 최초 확정
// 입력: photoId: 대상 identity, fingerprint: metadata와 완전한 SHA-256 baseline
// 출력: 성공 표식 또는 stale state·입력·database 오류
CatalogPhotoMutationResult CatalogPhotoRepository::establishSourceFingerprint(
    types::PhotoId photoId, const types::SourceFingerprint& fingerprint)
{
    if (!isValidBaselineUpdate(photoId, fingerprint))
    {
        return CatalogPhotoMutationResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Complete source fingerprint is required.")});
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("UPDATE photos SET source_size_bytes = :sizeBytes, "
                                 "source_mtime_ms = :modifiedAtMs, source_sha256 = :sha256, "
                                 "source_binding_state = :available WHERE id = :photoId AND source_path IS NOT NULL "
                                 "AND source_binding_state = :pending AND "
                                 "((source_size_bytes = :sizeBytes AND source_mtime_ms = :modifiedAtMs) "
                                 "OR (source_size_bytes = 0 AND source_sha256 IS NULL))"));
    query.bindValue(QStringLiteral(":sha256"), fingerprint.sha256);
    query.bindValue(QStringLiteral(":available"), static_cast<int>(SourceBindingState::Available));
    query.bindValue(QStringLiteral(":photoId"), photoId.value);
    query.bindValue(QStringLiteral(":pending"), static_cast<int>(SourceBindingState::FingerprintPending));
    query.bindValue(QStringLiteral(":sizeBytes"), fingerprint.sizeBytes);
    query.bindValue(QStringLiteral(":modifiedAtMs"), fingerprint.modifiedAtMs);
    if (!query.exec())
    {
        return CatalogPhotoMutationResult::failure(makeDatabaseError(
            QStringLiteral("Unable to establish catalog source fingerprint: %1").arg(query.lastError().text())));
    }
    if (query.numRowsAffected() != 1)
    {
        return CatalogPhotoMutationResult::failure(
            makeConflictError(QStringLiteral("Catalog source changed while its fingerprint was generated.")));
    }

    return CatalogPhotoMutationResult::success({});
}

// 목적: metadata는 변경됐지만 SHA-256가 일치하는 source를 동일 photo로 재확인
// 입력: photoId: 대상 identity, observed: 현재 metadata와 baseline과 일치하는 SHA-256
// 출력: baseline hash를 변경하지 않은 성공 표식 또는 hash·state conflict 오류
CatalogPhotoMutationResult CatalogPhotoRepository::confirmSourceMatch(types::PhotoId photoId,
                                                                      const types::SourceFingerprint& observed)
{
    if (!isValidBaselineUpdate(photoId, observed))
    {
        return CatalogPhotoMutationResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Complete observed fingerprint is required.")});
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("UPDATE photos SET source_size_bytes = :sizeBytes, source_mtime_ms = :modifiedAtMs, "
                                 "source_binding_state = :available WHERE id = :photoId AND source_path IS NOT NULL "
                                 "AND source_binding_state = :verificationRequired AND source_sha256 = :sha256"));
    query.bindValue(QStringLiteral(":sizeBytes"), observed.sizeBytes);
    query.bindValue(QStringLiteral(":modifiedAtMs"), observed.modifiedAtMs);
    query.bindValue(QStringLiteral(":available"), static_cast<int>(SourceBindingState::Available));
    query.bindValue(QStringLiteral(":photoId"), photoId.value);
    query.bindValue(QStringLiteral(":verificationRequired"),
                    static_cast<int>(SourceBindingState::VerificationRequired));
    query.bindValue(QStringLiteral(":sha256"), observed.sha256);
    if (!query.exec())
    {
        return CatalogPhotoMutationResult::failure(makeDatabaseError(
            QStringLiteral("Unable to confirm matching catalog source: %1").arg(query.lastError().text())));
    }
    if (query.numRowsAffected() != 1)
    {
        return CatalogPhotoMutationResult::failure(
            makeConflictError(QStringLiteral("Catalog source hash or verification state no longer matches.")));
    }

    return CatalogPhotoMutationResult::success({});
}

// 목적: 사용자가 교체된 source에 기존 보정값 적용을 선택한 결과 반영
// 입력: photoId: 유지할 identity, replacement: 새로 수용할 source fingerprint
// 출력: PhotoId와 develop data를 유지한 성공 표식 또는 오류
CatalogPhotoMutationResult CatalogPhotoRepository::acceptReplacement(types::PhotoId photoId,
                                                                     const types::SourceFingerprint& replacement)
{
    if (!isValidBaselineUpdate(photoId, replacement))
    {
        return CatalogPhotoMutationResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Complete replacement fingerprint is required.")});
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("UPDATE photos SET source_size_bytes = :sizeBytes, source_mtime_ms = :modifiedAtMs, "
                                 "source_sha256 = :sha256, source_binding_state = :available "
                                 "WHERE id = :photoId AND source_path IS NOT NULL "
                                 "AND source_binding_state IN (:unverified, :replacement)"));
    query.bindValue(QStringLiteral(":sizeBytes"), replacement.sizeBytes);
    query.bindValue(QStringLiteral(":modifiedAtMs"), replacement.modifiedAtMs);
    query.bindValue(QStringLiteral(":sha256"), replacement.sha256);
    query.bindValue(QStringLiteral(":available"), static_cast<int>(SourceBindingState::Available));
    query.bindValue(QStringLiteral(":photoId"), photoId.value);
    query.bindValue(QStringLiteral(":unverified"), static_cast<int>(SourceBindingState::IdentityUnverified));
    query.bindValue(QStringLiteral(":replacement"), static_cast<int>(SourceBindingState::ReplacementDetected));
    if (!query.exec())
    {
        return CatalogPhotoMutationResult::failure(
            makeDatabaseError(QStringLiteral("Unable to accept replacement source: %1").arg(query.lastError().text())));
    }
    if (query.numRowsAffected() != 1)
    {
        return CatalogPhotoMutationResult::failure(
            makeConflictError(QStringLiteral("Catalog replacement state changed before it was accepted.")));
    }

    return CatalogPhotoMutationResult::success({});
}

// 목적: 교체된 현재 file을 새 PhotoId로 등록하고 기존 photo는 unlinked로 보존
// 입력: replacedPhotoId: 기존 identity, replacement: 현재 file 정보, fingerprint: 새 source baseline
// 출력: develop data가 없는 새 PhotoId 또는 transaction 오류
CatalogPhotoCreateResult CatalogPhotoRepository::registerReplacementAsNew(types::PhotoId replacedPhotoId,
                                                                          const CatalogEntry& replacement,
                                                                          const types::SourceFingerprint& fingerprint)
{
    if (!isPersistableEntry(replacement) || !isValidBaselineUpdate(replacedPhotoId, fingerprint))
    {
        return CatalogPhotoCreateResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Replacement photo data is invalid.")});
    }

    QSqlDatabase& database = m_database.m_database;
    if (!database.transaction())
    {
        return CatalogPhotoCreateResult::failure(
            makeDatabaseError(QStringLiteral("Unable to begin replacement registration transaction: %1")
                                  .arg(database.lastError().text())));
    }

    QSqlQuery detachQuery(database);
    detachQuery.prepare(QStringLiteral("UPDATE photos SET source_path = NULL, source_parent_path = NULL, "
                                       "source_binding_state = :unlinked "
                                       "WHERE id = :photoId AND source_path = :sourcePath "
                                       "AND source_binding_state IN (:unverified, :replacement)"));
    detachQuery.bindValue(QStringLiteral(":unlinked"), static_cast<int>(SourceBindingState::Unlinked));
    detachQuery.bindValue(QStringLiteral(":photoId"), replacedPhotoId.value);
    detachQuery.bindValue(QStringLiteral(":sourcePath"), replacement.file.path);
    detachQuery.bindValue(QStringLiteral(":unverified"), static_cast<int>(SourceBindingState::IdentityUnverified));
    detachQuery.bindValue(QStringLiteral(":replacement"), static_cast<int>(SourceBindingState::ReplacementDetected));
    if (!detachQuery.exec())
    {
        database.rollback();
        return CatalogPhotoCreateResult::failure(makeDatabaseError(
            QStringLiteral("Unable to detach replaced catalog source: %1").arg(detachQuery.lastError().text())));
    }
    if (detachQuery.numRowsAffected() != 1)
    {
        database.rollback();
        return CatalogPhotoCreateResult::failure(
            makeConflictError(QStringLiteral("Catalog replacement state changed before new registration.")));
    }

    QSqlQuery insertQuery(database);
    insertQuery.prepare(
        QStringLiteral("INSERT INTO photos (source_path, source_parent_path, last_known_path, extension, display_name, "
                       "kind, scan_status, "
                       "source_size_bytes, source_mtime_ms, source_sha256, source_binding_state, imported_at_ms) "
                       "VALUES (:sourcePath, :sourceParentPath, :sourcePath, :extension, :displayName, :kind, "
                       ":scanStatus, "
                       ":sizeBytes, :modifiedAtMs, :sha256, :available, :importedAtMs)"));
    insertQuery.bindValue(QStringLiteral(":sourcePath"), replacement.file.path);
    insertQuery.bindValue(QStringLiteral(":sourceParentPath"), sourceParentPath(replacement.file.path));
    insertQuery.bindValue(QStringLiteral(":extension"), replacement.file.extension);
    insertQuery.bindValue(QStringLiteral(":displayName"), replacement.file.displayName);
    insertQuery.bindValue(QStringLiteral(":kind"), static_cast<int>(replacement.file.kind));
    insertQuery.bindValue(QStringLiteral(":scanStatus"), static_cast<int>(replacement.status));
    insertQuery.bindValue(QStringLiteral(":sizeBytes"), fingerprint.sizeBytes);
    insertQuery.bindValue(QStringLiteral(":modifiedAtMs"), fingerprint.modifiedAtMs);
    insertQuery.bindValue(QStringLiteral(":sha256"), fingerprint.sha256);
    insertQuery.bindValue(QStringLiteral(":available"), static_cast<int>(SourceBindingState::Available));
    insertQuery.bindValue(QStringLiteral(":importedAtMs"), QDateTime::currentMSecsSinceEpoch());
    if (!insertQuery.exec())
    {
        database.rollback();
        return CatalogPhotoCreateResult::failure(makeDatabaseError(
            QStringLiteral("Unable to register replacement as a new photo: %1").arg(insertQuery.lastError().text())));
    }

    const types::PhotoId newPhotoId{insertQuery.lastInsertId().toLongLong()};
    if (!types::isValidPhotoId(newPhotoId) || !database.commit())
    {
        database.rollback();
        return CatalogPhotoCreateResult::failure(makeDatabaseError(
            QStringLiteral("Unable to commit replacement registration: %1").arg(database.lastError().text())));
    }

    return CatalogPhotoCreateResult::success(newPhotoId);
}

// 목적: 기존 PhotoId를 검증된 다른 source locator에 재연결
// 입력: photoId: 유지할 identity, locator: 새 path, fingerprint: 동일 source로 검증된 fingerprint
// 출력: develop data를 유지한 relink 성공 표식 또는 오류
CatalogPhotoMutationResult CatalogPhotoRepository::relinkSource(types::PhotoId photoId,
                                                                const types::SourceLocator& locator,
                                                                const types::SourceFingerprint& fingerprint)
{
    const QString parentPath = sourceParentPath(locator.path);
    if (parentPath.isEmpty() || !isValidBaselineUpdate(photoId, fingerprint))
    {
        return CatalogPhotoMutationResult::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Relink source data is invalid.")});
    }

    QSqlQuery query(m_database.m_database);
    query.prepare(QStringLiteral("UPDATE photos SET source_path = :sourcePath, source_parent_path = :sourceParentPath, "
                                 "last_known_path = :sourcePath, "
                                 "source_size_bytes = :sizeBytes, source_mtime_ms = :modifiedAtMs, "
                                 "source_sha256 = COALESCE(source_sha256, :sha256), source_binding_state = :available "
                                 "WHERE id = :photoId AND source_binding_state IN "
                                 "(:missing, :unverified, :replacement, :unreadable, :unlinked) "
                                 "AND (source_sha256 IS NULL OR source_sha256 = :sha256)"));
    query.bindValue(QStringLiteral(":sourcePath"), locator.path);
    query.bindValue(QStringLiteral(":sourceParentPath"), parentPath);
    query.bindValue(QStringLiteral(":sizeBytes"), fingerprint.sizeBytes);
    query.bindValue(QStringLiteral(":modifiedAtMs"), fingerprint.modifiedAtMs);
    query.bindValue(QStringLiteral(":sha256"), fingerprint.sha256);
    query.bindValue(QStringLiteral(":available"), static_cast<int>(SourceBindingState::Available));
    query.bindValue(QStringLiteral(":photoId"), photoId.value);
    query.bindValue(QStringLiteral(":missing"), static_cast<int>(SourceBindingState::Missing));
    query.bindValue(QStringLiteral(":unverified"), static_cast<int>(SourceBindingState::IdentityUnverified));
    query.bindValue(QStringLiteral(":replacement"), static_cast<int>(SourceBindingState::ReplacementDetected));
    query.bindValue(QStringLiteral(":unreadable"), static_cast<int>(SourceBindingState::Unreadable));
    query.bindValue(QStringLiteral(":unlinked"), static_cast<int>(SourceBindingState::Unlinked));
    if (!query.exec())
    {
        return CatalogPhotoMutationResult::failure(
            makeDatabaseError(QStringLiteral("Unable to relink catalog source: %1").arg(query.lastError().text())));
    }
    if (query.numRowsAffected() != 1)
    {
        return CatalogPhotoMutationResult::failure(
            makeConflictError(QStringLiteral("Catalog source state changed before relink.")));
    }

    return CatalogPhotoMutationResult::success({});
}

}  // namespace flexraw::core::catalog
