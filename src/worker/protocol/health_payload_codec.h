#pragma once

#include <cstdint>

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include "result.h"

namespace flexraw::worker::protocol
{

inline constexpr std::uint16_t HealthPayloadMajorVersion = 1;
inline constexpr std::uint16_t HealthPayloadMinorVersion = 0;
inline constexpr qsizetype HealthResponsePayloadBytes = 40;

enum class HealthServiceState : std::uint16_t
{
    Ready = 1,
    Draining = 2,
    ShuttingDown = 3,
};

struct HealthResponsePayload
{
    HealthServiceState serviceState{HealthServiceState::Ready};
    std::uint64_t queuedJobs{0};
    std::uint64_t runningJobs{0};
    std::uint64_t maximumConcurrentJobs{0};
    std::uint64_t queueCapacity{0};

    bool operator==(const HealthResponsePayload&) const = default;
};

enum class HealthPayloadErrorCode
{
    InvalidSize,
    UnsupportedVersion,
    UnknownServiceState,
    ReservedFieldSet,
    InvalidLoad,
};

struct HealthPayloadError
{
    HealthPayloadErrorCode code{HealthPayloadErrorCode::InvalidSize};
    QString message;
};

using EncodeHealthPayloadResult = core::types::Result<QByteArray, HealthPayloadError>;
using DecodeHealthPayloadResult = core::types::Result<HealthResponsePayload, HealthPayloadError>;

// 목적: Worker Runtime 관측값을 versioned fixed-width health response payload로 직렬화
// 입력: payload: service state, current load와 configured limit
// 출력: 40-byte big-endian payload 또는 invariant 오류
[[nodiscard]] EncodeHealthPayloadResult encodeHealthResponsePayload(const HealthResponsePayload& payload);

// 목적: health response payload의 version, enum, 크기와 load invariant 검증
// 입력: bytes: HealthResponse frame payload 전체
// 출력: 검증된 관측값 또는 payload 오류
[[nodiscard]] DecodeHealthPayloadResult decodeHealthResponsePayload(QByteArrayView bytes);

}  // namespace flexraw::worker::protocol
