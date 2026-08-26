#include "display_frame_qt_adapter.h"

#include <limits>
#include <utility>
#include <vector>

namespace flexraw::ui::facade
{

// 목적: Qt preview image를 Qt-free BGRA8/top-down/sRGB display frame으로 변환
// 입력: image: display-ready sRGB QImage, previewRevision: frame 순서
// 출력: 유효한 immutable frame 또는 변환 실패 시 빈 값
std::optional<core::client::DisplayFrame> toDisplayFrame(const QImage& image, std::uint64_t previewRevision)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
    {
        return std::nullopt;
    }

    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull())
    {
        return std::nullopt;
    }

    const std::size_t width = static_cast<std::size_t>(rgba.width());
    const std::size_t height = static_cast<std::size_t>(rgba.height());
    if (width > std::numeric_limits<std::size_t>::max() / 4)
    {
        return std::nullopt;
    }
    const std::size_t rowStride = width * 4;
    if (rowStride > std::numeric_limits<std::size_t>::max() / height)
    {
        return std::nullopt;
    }

    std::vector<std::uint8_t> bytes(rowStride * height);
    for (int row = 0; row < rgba.height(); ++row)
    {
        const auto* source = rgba.constScanLine(row);
        std::uint8_t* destination = bytes.data() + static_cast<std::size_t>(row) * rowStride;
        for (int column = 0; column < rgba.width(); ++column)
        {
            const std::size_t offset = static_cast<std::size_t>(column) * 4;
            destination[offset] = source[offset + 2];
            destination[offset + 1] = source[offset + 1];
            destination[offset + 2] = source[offset];
            destination[offset + 3] = 255;
        }
    }

    return core::client::DisplayFrame::create(static_cast<std::uint32_t>(rgba.width()),
                                              static_cast<std::uint32_t>(rgba.height()),
                                              rowStride,
                                              previewRevision,
                                              std::move(bytes));
}

// 목적: Qt-free display frame을 Qt Widgets presentation용 image로 변환
// 입력: frame: BGRA8/top-down/sRGB immutable frame
// 출력: opaque RGBA8888 QImage 또는 변환 실패 시 null image
QImage toQImage(const core::client::DisplayFrame& frame)
{
    if (frame.width() > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        frame.height() > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
    {
        return {};
    }

    QImage image(static_cast<int>(frame.width()), static_cast<int>(frame.height()), QImage::Format_RGBA8888);
    if (image.isNull())
    {
        return {};
    }

    for (std::uint32_t row = 0; row < frame.height(); ++row)
    {
        const std::span<const std::uint8_t> source = frame.rowBytes(row);
        auto* destination = image.scanLine(static_cast<int>(row));
        for (std::uint32_t column = 0; column < frame.width(); ++column)
        {
            const std::size_t offset = static_cast<std::size_t>(column) * 4;
            destination[offset] = source[offset + 2];
            destination[offset + 1] = source[offset + 1];
            destination[offset + 2] = source[offset];
            destination[offset + 3] = 255;
        }
    }
    return image;
}

}  // namespace flexraw::ui::facade
