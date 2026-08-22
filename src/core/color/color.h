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

// 목적: LittleCMS 가 제공하는 표준 sRGB ICC 프로필을 직렬화해 반환
// 입력: 없음
// 출력: ICC 프로필 byte data 또는 생성 실패 정보
[[nodiscard]] IccProfileResult createSrgbIccProfile();

// 목적: 지정한 표준 RGB 색 공간의 ICC 프로필을 직렬화해 반환
// 입력: colorSpace: sRGB, Adobe RGB 또는 Display P3 표준 색 공간
// 출력: ICC 프로필 byte data 또는 지원하지 않는 색 공간/생성 실패 정보
[[nodiscard]] IccProfileResult createRgbIccProfile(RgbColorSpace colorSpace);

// 목적: 입력 ICC 프로필에서 출력 ICC 프로필로 RGBA 이미지를 변환
// 입력: sourceImage — 변환할 이미지, sourceProfileData/destinationProfileData — ICC 프로필 byte data
// 출력: 변환된 RGBA 이미지 또는 입력/프로필/변환 실패 정보
[[nodiscard]] ColorImageResult transformIccImage(const QImage& sourceImage,
                                                 const QByteArray& sourceProfileData,
                                                 const QByteArray& destinationProfileData);

}  // namespace flexraw::core::color
