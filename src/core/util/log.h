#pragma once

#include <QString>

#include <spdlog/spdlog.h>

namespace flexraw::core::util
{

// 목적: Flexraw file 및 개발 console logging system을 초기화
// 입력: 없음
// 출력: 없음
void initializeLogging();

// 목적: logging system을 종료하고 대기 중인 기록을 flush
// 입력: 없음
// 출력: 없음
void shutdownLogging();

// 목적: 현재 실행 환경에서 Flexraw log가 기록되는 directory 경로 반환
// 입력: 없음
// 출력: log directory의 절대 경로
[[nodiscard]] QString logDirectoryPath();

}  // namespace flexraw::core::util

#define LOG_TRACE(category, ...)                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (const auto logger = spdlog::get(category); logger != nullptr)                                              \
        {                                                                                                              \
            logger->trace(__VA_ARGS__);                                                                                \
        }                                                                                                              \
    } while (false)

#define LOG_DEBUG(category, ...)                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (const auto logger = spdlog::get(category); logger != nullptr)                                              \
        {                                                                                                              \
            logger->debug(__VA_ARGS__);                                                                                \
        }                                                                                                              \
    } while (false)

#define LOG_INFO(category, ...)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        if (const auto logger = spdlog::get(category); logger != nullptr)                                              \
        {                                                                                                              \
            logger->info(__VA_ARGS__);                                                                                 \
        }                                                                                                              \
    } while (false)

#define LOG_WARN(category, ...)                                                                                        \
    do                                                                                                                 \
    {                                                                                                                  \
        if (const auto logger = spdlog::get(category); logger != nullptr)                                              \
        {                                                                                                              \
            logger->warn(__VA_ARGS__);                                                                                 \
        }                                                                                                              \
    } while (false)

#define LOG_ERROR(category, ...)                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (const auto logger = spdlog::get(category); logger != nullptr)                                              \
        {                                                                                                              \
            logger->error(__VA_ARGS__);                                                                                \
        }                                                                                                              \
    } while (false)
