#include <chrono>
#include <latch>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "high_water_counter.h"
#include "measurement_csv.h"
#include "stage_timer.h"

namespace flexraw::core::measurement
{
namespace
{

TEST(StageTimerTest, ReportsPositiveElapsedNanoseconds)
{
    const StageTimer timer;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    EXPECT_GT(timer.elapsedNanoseconds(), 0U);
}

TEST(HighWaterCounterTest, PreservesPeakAfterCurrentReturnsToZero)
{
    HighWaterCounter counter;

    EXPECT_EQ(counter.increment(), 1U);
    EXPECT_EQ(counter.increment(), 2U);
    counter.decrement();
    counter.decrement();

    const HighWaterSnapshot snapshot = counter.snapshot();
    EXPECT_EQ(snapshot.current, 0U);
    EXPECT_EQ(snapshot.highWater, 2U);
}

TEST(HighWaterCounterTest, RecordsConcurrentPeak)
{
    constexpr int ThreadCount = 4;
    HighWaterCounter counter;
    std::latch ready(ThreadCount);
    std::latch release(1);
    std::vector<std::thread> threads;
    threads.reserve(ThreadCount);

    for (int index = 0; index < ThreadCount; ++index)
    {
        threads.emplace_back([&]() {
            static_cast<void>(counter.increment());
            ready.count_down();
            release.wait();
            counter.decrement();
        });
    }

    ready.wait();
    EXPECT_EQ(counter.snapshot().current, static_cast<std::uint64_t>(ThreadCount));
    release.count_down();
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    const HighWaterSnapshot snapshot = counter.snapshot();
    EXPECT_EQ(snapshot.current, 0U);
    EXPECT_EQ(snapshot.highWater, static_cast<std::uint64_t>(ThreadCount));
}

TEST(MeasurementCsvTest, WritesStableSchemaAndEscapesTextFields)
{
    RenderMeasurementRecord record;
    record.runId = "run-1";
    record.mode = "cpu-develop";
    record.target = "windows-x64";
    record.sourceLabel = "sample,\"edited\".jpg";
    record.width = 6000;
    record.height = 4000;
    record.sampleIndex = 3;
    record.succeeded = true;
    record.scheduler = {2, 1};
    record.render = {11, 12, 13, 14, 50};
    record.endToEndNanoseconds = 55;

    std::ostringstream output;
    writeRenderMeasurementCsvHeader(output);
    writeRenderMeasurementCsvRow(output, record);

    EXPECT_EQ(output.str(),
              "schema_version,run_id,mode,target,source,width,height,concurrency,sample_index,succeeded,"
              "peak_rss_bytes,queued_high_water,running_high_water,decode_ns,source_conversion_ns,develop_ns,"
              "output_ns,pipeline_total_ns,end_to_end_ns\n"
              "2,run-1,cpu-develop,windows-x64,\"sample,\"\"edited\"\".jpg\",6000,4000,1,3,1,,2,1,11,12,13,"
              "14,50,55\n");
}

}  // namespace
}  // namespace flexraw::core::measurement
