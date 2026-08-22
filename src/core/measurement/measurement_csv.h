#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>

#include "render_stats.h"

namespace flexraw::core::measurement
{

struct RenderMeasurementRecord
{
    std::string runId;
    std::string mode;
    std::string target;
    std::string sourceLabel;
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t concurrency{1};
    std::uint32_t sampleIndex{1};
    bool succeeded{false};
    std::optional<std::uint64_t> peakRssBytes;
    SchedulerStats scheduler;
    RenderStats render;
    std::uint64_t endToEndNanoseconds{0};
};

// 목적: version 2 render measurement CSV column header 출력
// 입력: output: CSV를 기록할 standard output stream
// 출력: output stream에 header 한 줄 추가
void writeRenderMeasurementCsvHeader(std::ostream& output);

// 목적: render measurement record를 version 2 CSV row로 출력
// 입력: output: CSV stream, record: context와 측정값
// 출력: output stream에 escaping된 row 한 줄 추가
void writeRenderMeasurementCsvRow(std::ostream& output, const RenderMeasurementRecord& record);

}  // namespace flexraw::core::measurement
