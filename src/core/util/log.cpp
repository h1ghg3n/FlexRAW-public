#include "log.h"

#include <array>
#include <memory>
#include <vector>

#include <QDir>
#include <QStandardPaths>

#include <spdlog/async.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace flexraw::core::util
{
namespace
{

constexpr std::array<const char*, 15> LoggerCategories{"app",
                                                       "worker",
                                                       "ui",
                                                       "catalog",
                                                       "raw",
                                                       "develop",
                                                       "color",
                                                       "preview",
                                                       "ml",
                                                       "import",
                                                       "export",
                                                       "lens",
                                                       "hdr",
                                                       "pano",
                                                       "history"};

// 목적: AppData를 사용할 수 없을 때 log를 기록할 임시 directory 반환
// 입력: 없음
// 출력: 임시 Flexraw log directory 경로
[[nodiscard]] QString fallbackLogDirectoryPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("Flexraw/logs"));
}

// 목적: file 및 console sink를 공유하는 category logger 생성
// 입력: category: logger category 이름, sinks: 기록할 file 및 console sink
// 출력: 초기화된 비동기 logger
[[nodiscard]] std::shared_ptr<spdlog::logger> createLogger(const char* category,
                                                           const std::vector<spdlog::sink_ptr>& sinks)
{
    auto logger = std::make_shared<spdlog::async_logger>(
        category, sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    logger->set_pattern("[%Y-%m-%d %T.%e] [%n] [%^%l%$] %v");
    logger->set_level(spdlog::level::debug);
    logger->flush_on(spdlog::level::info);
    return logger;
}

}  // namespace

// 목적: 현재 실행 환경에서 Flexraw log가 기록되는 directory 경로 반환
// 입력: 없음
// 출력: log directory의 절대 경로
QString logDirectoryPath()
{
    const QString appDataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    if (appDataPath.isEmpty())
    {
        return fallbackLogDirectoryPath();
    }

    return QDir(appDataPath).filePath(QStringLiteral("logs"));
}

// 목적: Flexraw file 및 개발 console logging system을 초기화
// 입력: 없음
// 출력: 없음
void initializeLogging()
{
    if (spdlog::get("app") != nullptr)
    {
        return;
    }

    QString directoryPath = logDirectoryPath();

    if (!QDir().mkpath(directoryPath))
    {
        directoryPath = fallbackLogDirectoryPath();
        QDir().mkpath(directoryPath);
    }

    try
    {
        spdlog::init_thread_pool(8192, 1);
        const auto fileSink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
            QDir(directoryPath).filePath(QStringLiteral("flexraw.log")).toStdString(), 0, 0, false, 30);
        const auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
#ifdef NDEBUG
        fileSink->set_level(spdlog::level::info);
#else
        fileSink->set_level(spdlog::level::debug);
#endif
        consoleSink->set_level(spdlog::level::debug);
        const std::vector<spdlog::sink_ptr> sinks{fileSink, consoleSink};

        for (const char* category : LoggerCategories)
        {
            spdlog::register_logger(createLogger(category, sinks));
        }
    }
    catch (const spdlog::spdlog_ex&)
    {
        spdlog::shutdown();
    }
}

// 목적: 파일 logging system을 종료하고 대기 중인 기록을 flush
// 입력: 없음
// 출력: 없음
void shutdownLogging()
{
    spdlog::shutdown();
}

}  // namespace flexraw::core::util
