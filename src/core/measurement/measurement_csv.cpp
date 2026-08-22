#include "measurement_csv.h"

#include <string_view>

namespace flexraw::core::measurement
{
namespace
{

constexpr std::uint32_t CsvSchemaVersion = 2;

// 목적: comma, quote 또는 줄바꿈을 포함한 text를 RFC 4180 방식으로 escaping
// 입력: output: CSV stream, value: 기록할 UTF-8 text
// 출력: output stream에 안전한 field 추가
void writeEscapedField(std::ostream& output, const std::string_view value)
{
    const bool needsQuotes = value.find_first_of(",\"\r\n") != std::string_view::npos;
    if (!needsQuotes)
    {
        output << value;
        return;
    }

    output << '"';
    for (const char character : value)
    {
        if (character == '"')
        {
            output << "\"\"";
        }
        else
        {
            output << character;
        }
    }
    output << '"';
}

}  // namespace

// 목적: version 2 render measurement CSV column header 출력
// 입력: output: CSV를 기록할 standard output stream
// 출력: output stream에 header 한 줄 추가
void writeRenderMeasurementCsvHeader(std::ostream& output)
{
    output << "schema_version,run_id,mode,target,source,width,height,concurrency,sample_index,succeeded,"
              "peak_rss_bytes,queued_high_water,running_high_water,decode_ns,source_conversion_ns,develop_ns,"
              "output_ns,pipeline_total_ns,end_to_end_ns\n";
}

// 목적: render measurement record를 version 2 CSV row로 출력
// 입력: output: CSV stream, record: context와 측정값
// 출력: output stream에 escaping된 row 한 줄 추가
void writeRenderMeasurementCsvRow(std::ostream& output, const RenderMeasurementRecord& record)
{
    output << CsvSchemaVersion << ',';
    writeEscapedField(output, record.runId);
    output << ',';
    writeEscapedField(output, record.mode);
    output << ',';
    writeEscapedField(output, record.target);
    output << ',';
    writeEscapedField(output, record.sourceLabel);
    output << ',' << record.width << ',' << record.height << ',' << record.concurrency << ',' << record.sampleIndex
           << ',' << (record.succeeded ? 1 : 0) << ',';
    if (record.peakRssBytes.has_value())
    {
        output << *record.peakRssBytes;
    }
    output << ',' << record.scheduler.queuedHighWater << ',' << record.scheduler.runningHighWater << ','
           << record.render.decodeNanoseconds << ',' << record.render.sourceConversionNanoseconds << ','
           << record.render.developNanoseconds << ',' << record.render.outputNanoseconds << ','
           << record.render.totalNanoseconds << ',' << record.endToEndNanoseconds << '\n';
}

}  // namespace flexraw::core::measurement
