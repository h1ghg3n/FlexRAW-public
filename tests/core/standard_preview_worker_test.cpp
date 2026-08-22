#include "standard_preview_worker.h"

#include <gtest/gtest.h>

namespace flexraw::core::preview {
namespace {

TEST(StandardPreviewWorker, ReportsCancellationBeforeDecode)
{
    StandardPreviewWorker worker;
    bool cancelled = false;

    QObject::connect(
        &worker,
        &StandardPreviewWorker::standardPreviewCancelled,
        [&cancelled](quint64 requestId, const QString& filePath) {
            cancelled = requestId == 1 && filePath == QStringLiteral("cancelled.dng");
        });
    worker.cancelRequestsThrough(1);
    worker.generateStandardPreview(1, QStringLiteral("cancelled.dng"), QSize{128, 128});

    EXPECT_TRUE(cancelled);
}

TEST(StandardPreviewWorker, PreservesRequestsAfterCancellationRange)
{
    StandardPreviewWorker worker;
    types::ErrorCode errorCode = types::ErrorCode::Unknown;

    QObject::connect(
        &worker,
        &StandardPreviewWorker::standardPreviewFailed,
        [&errorCode](quint64 requestId, const QString&, types::ErrorCode code, const QString&) {
            if (requestId == 2) {
                errorCode = code;
            }
        });
    worker.cancelRequestsThrough(1);
    worker.generateStandardPreview(2, QStringLiteral("later.dng"), QSize{});

    EXPECT_EQ(types::ErrorCode::InvalidArgument, errorCode);
}

} // namespace
} // namespace flexraw::core::preview
