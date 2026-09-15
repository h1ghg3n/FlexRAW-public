#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <exception>
#include <limits>
#include <utility>

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QHostAddress>
#include <QTextStream>
#include <QTimer>

#include "log.h"
#include "result.h"
#include "worker_application_context.h"
#include "worker_path_resolver.h"

namespace
{

constexpr quint16 DefaultWorkerPort = 47331;
volatile std::sig_atomic_t StopRequested = 0;

struct WorkerCommandLineConfiguration
{
    flexraw::worker::runtime::WorkerRootConfiguration roots;
    flexraw::worker::app::WorkerApplicationConfiguration application;
    QHostAddress listenAddress{QHostAddress::LocalHost};
    quint16 port{DefaultWorkerPort};
};

struct WorkerCommandLineEarlyExit
{
    bool requested{false};
    QString output;
};

using ParseConfigurationResult = flexraw::core::types::Result<WorkerCommandLineConfiguration, QString>;

// 목적: Worker CLI와 오류 text에 사용할 번역 가능한 문자열 생성
// 입력: sourceText: source language 문자열
// 출력: WorkerMain context로 번역된 QString
[[nodiscard]] QString trWorker(const char* const sourceText)
{
    return QCoreApplication::translate("WorkerMain", sourceText);
}

// 목적: process stop signal을 async-signal-safe flag로 전달
// 입력: signalNumber: SIGINT 또는 SIGTERM 값
// 출력: 다음 Qt timer tick에서 종료할 global flag 설정
void handleStopSignal(const int signalNumber)
{
    static_cast<void>(signalNumber);
    StopRequested = 1;
}

// 목적: CLI unsigned option을 지정 범위 안의 정수로 검증
// 입력: value/name/minimum/maximum: 원문, 진단 이름과 허용 범위
// 출력: 성공 시 parsed 값, 실패 시 localized 진단 text
[[nodiscard]] flexraw::core::types::Result<qulonglong, QString> parseUnsignedOption(const QString& value,
                                                                                    const QString& name,
                                                                                    const qulonglong minimum,
                                                                                    const qulonglong maximum)
{
    bool parsed = false;
    const qulonglong result = value.toULongLong(&parsed);
    if (!parsed || result < minimum || result > maximum)
    {
        return flexraw::core::types::Result<qulonglong, QString>::failure(
            trWorker("Option %1 must be an integer from %2 through %3.").arg(name).arg(minimum).arg(maximum));
    }
    return flexraw::core::types::Result<qulonglong, QString>::success(result);
}

// 목적: Worker resource claim의 양수 floating-point CLI option 검증
// 입력: value/name: 원문과 진단에 사용할 option 이름
// 출력: finite positive 값 또는 localized 진단 text
[[nodiscard]] flexraw::core::types::Result<double, QString> parsePositiveDoubleOption(const QString& value,
                                                                                      const QString& name)
{
    bool parsed = false;
    const double result = value.toDouble(&parsed);
    if (!parsed || !std::isfinite(result) || result <= 0.0)
    {
        return flexraw::core::types::Result<double, QString>::failure(
            trWorker("Option %1 must be a finite number greater than zero.").arg(name));
    }
    return flexraw::core::types::Result<double, QString>::success(result);
}

// 목적: QCommandLineParser에서 Worker root, endpoint와 resource limit 읽기
// 입력: application: process argument를 소유한 Qt application, earlyExit: help/version 출력 반환 위치
// 출력: 검증된 실행 configuration 또는 사용자 진단
[[nodiscard]] ParseConfigurationResult parseCommandLine(QCoreApplication& application,
                                                        WorkerCommandLineEarlyExit& earlyExit)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(trWorker("Flexraw bounded RAW render worker"));
    const QCommandLineOption helpOption = parser.addHelpOption();
    const QCommandLineOption versionOption = parser.addVersionOption();

    const QCommandLineOption sourceRootOption(QStringList{QStringLiteral("source-root")},
                                              trWorker("Existing root containing source RAW files."),
                                              trWorker("path"));
    const QCommandLineOption outputRootOption(QStringList{QStringLiteral("output-root")},
                                              trWorker("Existing root for rendered output files."),
                                              trWorker("path"));
    const QCommandLineOption addressOption(QStringList{QStringLiteral("listen-address")},
                                           trWorker("IPv4 or IPv6 address to bind."),
                                           trWorker("address"),
                                           QStringLiteral("127.0.0.1"));
    const QCommandLineOption portOption(QStringList{QStringLiteral("port")},
                                        trWorker("TCP port to listen on."),
                                        trWorker("port"),
                                        QString::number(DefaultWorkerPort));
    const QCommandLineOption concurrencyOption(QStringList{QStringLiteral("concurrency")},
                                               trWorker("Maximum simultaneously running render jobs."),
                                               trWorker("count"),
                                               QStringLiteral("1"));
    const QCommandLineOption queueCapacityOption(QStringList{QStringLiteral("queue-capacity")},
                                                 trWorker("Maximum waiting render jobs."),
                                                 trWorker("count"),
                                                 QStringLiteral("2"));
    const QCommandLineOption connectionLimitOption(QStringList{QStringLiteral("max-connections")},
                                                   trWorker("Maximum simultaneous TCP sessions."),
                                                   trWorker("count"),
                                                   QStringLiteral("16"));
    const QCommandLineOption timeoutOption(QStringList{QStringLiteral("idle-timeout-ms")},
                                           trWorker("Idle or partial-frame connection timeout in milliseconds."),
                                           trWorker("milliseconds"),
                                           QStringLiteral("30000"));
    const QCommandLineOption resourceRouterUrlOption(QStringList{QStringLiteral("resource-router-url")},
                                                     trWorker("Optional Resource Router HTTP(S) base URL."),
                                                     trWorker("url"));
    const QCommandLineOption renderMemoryClaimOption(
        QStringList{QStringLiteral("render-memory-claim-mib")},
        trWorker("Declared incremental peak memory per render in MiB; 0 keeps only the local minimum reserve."),
        trWorker("MiB"),
        QString::number(flexraw::worker::admission::DefaultRenderMemoryClaimMiB));
    const QCommandLineOption renderCpuClaimOption(QStringList{QStringLiteral("render-cpu-cores")},
                                                  trWorker("Declared CPU cores per render."),
                                                  trWorker("cores"),
                                                  QStringLiteral("1"));
    const QCommandLineOption resourceRouterLeaseTtlOption(
        QStringList{QStringLiteral("resource-router-lease-ttl-seconds")},
        trWorker("Requested Resource Router lease TTL in seconds."),
        trWorker("seconds"),
        QStringLiteral("60"));
    const QCommandLineOption resourceRouterTimeoutOption(
        QStringList{QStringLiteral("resource-router-timeout-ms")},
        trWorker("Resource Router HTTP operation timeout in milliseconds."),
        trWorker("milliseconds"),
        QStringLiteral("5000"));

    parser.addOptions({sourceRootOption,
                       outputRootOption,
                       addressOption,
                       portOption,
                       concurrencyOption,
                       queueCapacityOption,
                       connectionLimitOption,
                       timeoutOption,
                       resourceRouterUrlOption,
                       renderMemoryClaimOption,
                       renderCpuClaimOption,
                       resourceRouterLeaseTtlOption,
                       resourceRouterTimeoutOption});
    if (!parser.parse(application.arguments()))
    {
        return ParseConfigurationResult::failure(parser.errorText());
    }
    if (parser.isSet(helpOption) || parser.isSet(QStringLiteral("help-all")))
    {
        earlyExit.requested = true;
        earlyExit.output = parser.helpText();
        return ParseConfigurationResult::success({});
    }
    if (parser.isSet(versionOption))
    {
        earlyExit.requested = true;
        earlyExit.output =
            QStringLiteral("%1 %2\n").arg(application.applicationName(), application.applicationVersion());
        return ParseConfigurationResult::success({});
    }

    if (!parser.isSet(sourceRootOption) || !parser.isSet(outputRootOption))
    {
        return ParseConfigurationResult::failure(trWorker("Both --source-root and --output-root are required."));
    }

    WorkerCommandLineConfiguration configuration;
    configuration.roots.sourceRoot = parser.value(sourceRootOption);
    configuration.roots.outputRoot = parser.value(outputRootOption);
    if (!configuration.listenAddress.setAddress(parser.value(addressOption)))
    {
        return ParseConfigurationResult::failure(trWorker("--listen-address must be a numeric IPv4 or IPv6 address."));
    }

    const auto port = parseUnsignedOption(parser.value(portOption), QStringLiteral("--port"), 1, 65535);
    const auto concurrency = parseUnsignedOption(parser.value(concurrencyOption),
                                                 QStringLiteral("--concurrency"),
                                                 1,
                                                 static_cast<qulonglong>(std::numeric_limits<int>::max()));
    const auto queueCapacity = parseUnsignedOption(parser.value(queueCapacityOption),
                                                   QStringLiteral("--queue-capacity"),
                                                   0,
                                                   std::numeric_limits<std::uint64_t>::max());
    const auto connectionLimit = parseUnsignedOption(parser.value(connectionLimitOption),
                                                     QStringLiteral("--max-connections"),
                                                     1,
                                                     static_cast<qulonglong>(std::numeric_limits<int>::max()));
    const auto timeout = parseUnsignedOption(parser.value(timeoutOption),
                                             QStringLiteral("--idle-timeout-ms"),
                                             1,
                                             static_cast<qulonglong>(std::numeric_limits<int>::max()));
    const auto renderMemoryClaim = parseUnsignedOption(parser.value(renderMemoryClaimOption),
                                                       QStringLiteral("--render-memory-claim-mib"),
                                                       0,
                                                       std::numeric_limits<std::uint64_t>::max());
    const auto renderCpuClaim =
        parsePositiveDoubleOption(parser.value(renderCpuClaimOption), QStringLiteral("--render-cpu-cores"));
    const auto resourceRouterLeaseTtl = parseUnsignedOption(
        parser.value(resourceRouterLeaseTtlOption), QStringLiteral("--resource-router-lease-ttl-seconds"), 1, 60);
    const auto resourceRouterTimeout = parseUnsignedOption(parser.value(resourceRouterTimeoutOption),
                                                           QStringLiteral("--resource-router-timeout-ms"),
                                                           1,
                                                           static_cast<qulonglong>(std::numeric_limits<int>::max()));

    const auto firstError = [&]() -> QString {
        if (port.hasError())
        {
            return port.error();
        }
        if (concurrency.hasError())
        {
            return concurrency.error();
        }
        if (queueCapacity.hasError())
        {
            return queueCapacity.error();
        }
        if (connectionLimit.hasError())
        {
            return connectionLimit.error();
        }
        if (timeout.hasError())
        {
            return timeout.error();
        }
        if (renderMemoryClaim.hasError())
        {
            return renderMemoryClaim.error();
        }
        if (renderCpuClaim.hasError())
        {
            return renderCpuClaim.error();
        }
        if (resourceRouterLeaseTtl.hasError())
        {
            return resourceRouterLeaseTtl.error();
        }
        return resourceRouterTimeout.hasError() ? resourceRouterTimeout.error() : QString{};
    }();
    if (!firstError.isEmpty())
    {
        return ParseConfigurationResult::failure(firstError);
    }

    const QString resourceRouterUrl = parser.value(resourceRouterUrlOption).trimmed();
    if (!resourceRouterUrl.isEmpty() && renderMemoryClaim.value() == 0)
    {
        return ParseConfigurationResult::failure(
            trWorker("--render-memory-claim-mib must be greater than zero when --resource-router-url is set."));
    }

    configuration.port = static_cast<quint16>(port.value());
    configuration.application.maximumConcurrency = static_cast<std::uint32_t>(concurrency.value());
    configuration.application.queueCapacity = static_cast<std::uint64_t>(queueCapacity.value());
    configuration.application.server.maximumConnections = static_cast<int>(connectionLimit.value());
    configuration.application.server.session.inactivityTimeoutMilliseconds = static_cast<int>(timeout.value());
    configuration.application.admission.resourceRouterUrl = resourceRouterUrl.toStdString();
    configuration.application.admission.renderMemoryClaimMiB = static_cast<std::uint64_t>(renderMemoryClaim.value());
    configuration.application.admission.renderCpuCores = renderCpuClaim.value();
    configuration.application.admission.resourceRouterLeaseTtl =
        std::chrono::seconds(static_cast<std::int64_t>(resourceRouterLeaseTtl.value()));
    configuration.application.admission.resourceRouterRequestTimeout =
        std::chrono::milliseconds(static_cast<std::int64_t>(resourceRouterTimeout.value()));
    return ParseConfigurationResult::success(std::move(configuration));
}

// 목적: Worker CLI 오류를 standard error stream에 출력
// 입력: message: 사용자에게 전달할 진단 text
// 출력: 없음
void writeError(const QString& message)
{
    QTextStream stream(stderr);
    stream << message << Qt::endl;
}

// 목적: Worker CLI informational text를 standard output stream에 출력
// 입력: message: help 또는 version text
// 출력: 없음
void writeOutput(const QString& message)
{
    QTextStream stream(stdout);
    stream << message;
    stream.flush();
}

class ScopedLoggingShutdown final
{
public:
    // 목적: 자신보다 나중에 생성된 Worker context가 먼저 파괴된 뒤 logging 종료
    // 입력: 없음
    // 출력: process logging queue가 flush되고 종료된 상태
    ~ScopedLoggingShutdown()
    {
        flexraw::core::util::shutdownLogging();
    }
};

// 목적: Flexraw Worker configuration, Composition Root와 Qt event loop 실행
// 입력: argc: process argument 수, argv: process argument 값
// 출력: 정상 종료 0, configuration 2, listen 실패 3
int runWorkerProcess(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Flexraw"));
    QCoreApplication::setApplicationName(QStringLiteral("Flexraw Worker"));
    QCoreApplication::setApplicationVersion(QStringLiteral(FLEXRAW_VERSION_STRING));

    WorkerCommandLineEarlyExit earlyExit;
    ParseConfigurationResult parsed = parseCommandLine(application, earlyExit);
    if (parsed.hasError())
    {
        writeError(parsed.error());
        return 2;
    }
    if (earlyExit.requested)
    {
        writeOutput(earlyExit.output);
        return 0;
    }

    flexraw::worker::runtime::WorkerPathResolver::CreateResult resolver =
        flexraw::worker::runtime::WorkerPathResolver::create(parsed.value().roots);
    if (resolver.hasError())
    {
        writeError(resolver.error().message);
        return 2;
    }

    flexraw::core::util::initializeLogging();
    const ScopedLoggingShutdown loggingShutdown;
    std::signal(SIGINT, handleStopSignal);
    std::signal(SIGTERM, handleStopSignal);

    int exitCode = 0;
    {
        flexraw::worker::app::WorkerApplicationContext context(std::move(resolver.value()), parsed.value().application);
        if (!context.listen(parsed.value().listenAddress, parsed.value().port))
        {
            const QString diagnostic = trWorker("Unable to listen: %1").arg(context.errorString());
            writeError(diagnostic);
            LOG_ERROR("worker", "Unable to listen: {}", context.errorString().toStdString());
            return 3;
        }

        LOG_INFO("worker",
                 "Flexraw Worker listening on {}:{} with concurrency {} and queue capacity {}",
                 parsed.value().listenAddress.toString().toStdString(),
                 context.serverPort(),
                 parsed.value().application.maximumConcurrency,
                 parsed.value().application.queueCapacity);

        QTimer signalTimer;
        QObject::connect(&signalTimer, &QTimer::timeout, &application, [&application]() {
            if (StopRequested != 0)
            {
                application.quit();
            }
        });
        signalTimer.start(100);
        exitCode = application.exec();
    }

    LOG_INFO("worker", "Flexraw Worker stopped with exit code {}", exitCode);
    return exitCode;
}

}  // namespace

// 목적: Worker Composition 생성 예외를 process diagnostic과 실패 code로 정규화
// 입력: argc: process argument 수, argv: process argument 값
// 출력: 정상 Worker code 또는 예상하지 못한 startup 실패 1
int main(int argc, char* argv[])
{
    try
    {
        return runWorkerProcess(argc, argv);
    }
    catch (const std::exception& exception)
    {
        flexraw::core::util::initializeLogging();
        writeError(trWorker("Unable to start Flexraw Worker: %1").arg(QString::fromUtf8(exception.what())));
        LOG_ERROR("worker", "Unable to start Flexraw Worker: {}", exception.what());
        flexraw::core::util::shutdownLogging();
        return 1;
    }
    catch (...)
    {
        flexraw::core::util::initializeLogging();
        writeError(trWorker("Unable to start Flexraw Worker: unknown exception"));
        LOG_ERROR("worker", "Unable to start Flexraw Worker: unknown exception");
        flexraw::core::util::shutdownLogging();
        return 1;
    }
}
