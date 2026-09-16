#pragma once

#include <cstdint>
#include <optional>

#include <QImage>
#include <QMetaType>

#include "display_frame.h"

namespace flexraw::ui::facade
{

// 목적: Qt preview image를 Qt-free BGRA8/top-down/sRGB display frame으로 변환
// 입력: image: display-ready sRGB QImage, previewRevision: frame 순서
// 출력: 유효한 immutable frame 또는 변환 실패 시 빈 값
[[nodiscard]] std::optional<core::client::DisplayFrame> toDisplayFrame(const QImage& image,
                                                                       std::uint64_t previewRevision);

// 목적: Qt-free display frame을 Qt Widgets presentation용 image로 변환
// 입력: frame: BGRA8/top-down/sRGB immutable frame
// 출력: opaque RGBA8888 QImage 또는 변환 실패 시 null image
[[nodiscard]] QImage toQImage(const core::client::DisplayFrame& frame);

}  // namespace flexraw::ui::facade

Q_DECLARE_METATYPE(flexraw::core::client::DisplayFrame)
