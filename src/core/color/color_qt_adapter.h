#pragma once

#include <QByteArray>
#include <QImage>

#include "error.h"
#include "result.h"

namespace flexraw::core::color
{

enum class RgbColorSpace
{
    Srgb,
    AdobeRgb,
    DisplayP3,
};

using IccProfileResult = types::Result<QByteArray, types::CoreError>;
using ColorImageResult = types::Result<QImage, types::CoreError>;

namespace qt_adapter
{

// 목적: LittleCMS 가 제공하는 표준 sRGB ICC profile을 QByteArray로 투영
// 입력: 없음
// 출력: ICC profile byte data 또는 생성 실패 정보
[[nodiscard]] IccProfileResult createSrgbIccProfile();

// 목적: 지정한 표준 RGB 색 공간의 ICC profile을 QByteArray로 투영
// 입력: colorSpace: sRGB, Adobe RGB 또는 Display P3 표준 색 공간
// 출력: ICC profile byte data 또는 지원하지 않는 색 공간/생성 실패 정보
[[nodiscard]] IccProfileResult createRgbIccProfile(RgbColorSpace colorSpace);

// 목적: QImage를 neutral ICC RGBA8 processing에 연결
// 입력: sourceImage: 변환할 이미지, sourceProfileData/destinationProfileData: ICC profile byte data
// 출력: 변환된 RGBA image 또는 입력/profile/변환 실패 정보
[[nodiscard]] ColorImageResult transformIccImage(const QImage& sourceImage,
                                                 const QByteArray& sourceProfileData,
                                                 const QByteArray& destinationProfileData);

}  // namespace qt_adapter
}  // namespace flexraw::core::color
