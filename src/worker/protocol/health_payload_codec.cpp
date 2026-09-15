#include "health_payload_codec.h"

#include <utility>

namespace flexraw::worker::protocol
{
namespace
{

constexpr qsizetype MajorVersionOffset = 0;
constexpr qsizetype MinorVersionOffset = 2;
constexpr qsizetype ServiceStateOffset = 4;
constexpr qsizetype ReservedOffset = 6;
constexpr qsizetype QueuedJobsOffset = 8;
constexpr qsizetype RunningJobsOffset = 16;
constexpr qsizetype MaximumConcurrentJobsOffset = 24;
constexpr qsizetype QueueCapacityOffset = 32;

// 목적: 16-bit unsigned 값을 network byte order로 기록
// 입력: destination: 2-byte 출력 위치, value: 기록할 값
// 출력: 없음
void writeUnsigned16(char* const destination, const std::uint16_t value)
{
    destination[0] = static_cast<char>((value >> 8U) & 0xFFU);
    destination[1] = static_cast<char>(value & 0xFFU);
}

// 목적: 64-bit unsigned 값을 network byte order로 기록
// 입력: destination: 8-byte 출력 위치, value: 기록할 값
// 출력: 없음
void writeUnsigned64(char* const destination, const std::uint64_t value)
{
    for (std::uint32_t index = 0; index < 8U; ++index)
    {
        const std::uint32_t shift = (7U - index) * 8U;
        destination[index] = static_cast<char>((value >> shift) & 0xFFU);
    }
}

// 목적: network byte order의 16-bit unsigned 값 해석
// 입력: source: 2-byte 입력 위치
// 출력: host byte order 값
[[nodiscard]] std::uint16_t readUnsigned16(const char* const source)
{
    const auto byte0 = static_cast<std::uint8_t>(source[0]);
    const auto byte1 = static_cast<std::uint8_t>(source[1]);
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(byte0) << 8U) | byte1);
}

// 목적: network byte order의 64-bit unsigned 값 해석
// 입력: source: 8-byte 입력 위치
// 출력: host byte order 값
[[nodiscard]] std::uint64_t readUnsigned64(const char* const source)
{
    std::uint64_t value = 0;
    for (std::uint32_t index = 0; index < 8U; ++index)
    {
        value = (value << 8U) | static_cast<std::uint8_t>(source[index]);
    }
    return value;
}

// 목적: health payload 오류를 일관된 value로 생성
// 입력: code: 분류, message: bounded 진단 text
// 출력: HealthPayloadError
[[nodiscard]] HealthPayloadError makeError(const HealthPayloadErrorCode code, QString message)
{
    return {code, std::move(message)};
}

// 목적: wire service state 정수 값의 지원 여부 확인
// 입력: value: payload에서 읽은 16-bit 값
// 출력: Ready/Draining/ShuttingDown 중 하나이면 true
[[nodiscard]] bool isKnownServiceState(const std::uint16_t value) noexcept
{
    switch (static_cast<HealthServiceState>(value))
    {
    case HealthServiceState::Ready:
    case HealthServiceState::Draining:
    case HealthServiceState::ShuttingDown:
        return true;
    }
    return false;
}

// 목적: current load가 configured scheduler limit 안에 있는지 검증
// 입력: payload: current와 configured count
// 출력: 최대 concurrency가 양수이고 current가 각 상한 이내이면 true
[[nodiscard]] bool hasValidLoad(const HealthResponsePayload& payload) noexcept
{
    return payload.maximumConcurrentJobs > 0 && payload.runningJobs <= payload.maximumConcurrentJobs &&
           payload.queuedJobs <= payload.queueCapacity;
}

}  // namespace

// 목적: Worker Runtime 관측값을 versioned fixed-width health response payload로 직렬화
// 입력: payload: service state, current load와 configured limit
// 출력: 40-byte big-endian payload 또는 invariant 오류
EncodeHealthPayloadResult encodeHealthResponsePayload(const HealthResponsePayload& payload)
{
    if (!isKnownServiceState(static_cast<std::uint16_t>(payload.serviceState)))
    {
        return EncodeHealthPayloadResult::failure(
            makeError(HealthPayloadErrorCode::UnknownServiceState, QStringLiteral("Unknown Worker service state.")));
    }
    if (!hasValidLoad(payload))
    {
        return EncodeHealthPayloadResult::failure(
            makeError(HealthPayloadErrorCode::InvalidLoad, QStringLiteral("Worker health load is invalid.")));
    }

    QByteArray encoded(HealthResponsePayloadBytes, Qt::Uninitialized);
    char* const output = encoded.data();
    writeUnsigned16(output + MajorVersionOffset, HealthPayloadMajorVersion);
    writeUnsigned16(output + MinorVersionOffset, HealthPayloadMinorVersion);
    writeUnsigned16(output + ServiceStateOffset, static_cast<std::uint16_t>(payload.serviceState));
    writeUnsigned16(output + ReservedOffset, 0);
    writeUnsigned64(output + QueuedJobsOffset, payload.queuedJobs);
    writeUnsigned64(output + RunningJobsOffset, payload.runningJobs);
    writeUnsigned64(output + MaximumConcurrentJobsOffset, payload.maximumConcurrentJobs);
    writeUnsigned64(output + QueueCapacityOffset, payload.queueCapacity);
    return EncodeHealthPayloadResult::success(std::move(encoded));
}

// 목적: health response payload의 version, enum, 크기와 load invariant 검증
// 입력: bytes: HealthResponse frame payload 전체
// 출력: 검증된 관측값 또는 payload 오류
DecodeHealthPayloadResult decodeHealthResponsePayload(const QByteArrayView bytes)
{
    if (bytes.size() != HealthResponsePayloadBytes)
    {
        return DecodeHealthPayloadResult::failure(
            makeError(HealthPayloadErrorCode::InvalidSize, QStringLiteral("Worker health payload size is invalid.")));
    }
    const char* const input = bytes.data();
    if (readUnsigned16(input + MajorVersionOffset) != HealthPayloadMajorVersion ||
        readUnsigned16(input + MinorVersionOffset) != HealthPayloadMinorVersion)
    {
        return DecodeHealthPayloadResult::failure(
            makeError(HealthPayloadErrorCode::UnsupportedVersion,
                      QStringLiteral("Worker health payload version is unsupported.")));
    }
    const std::uint16_t serviceState = readUnsigned16(input + ServiceStateOffset);
    if (!isKnownServiceState(serviceState))
    {
        return DecodeHealthPayloadResult::failure(
            makeError(HealthPayloadErrorCode::UnknownServiceState, QStringLiteral("Unknown Worker service state.")));
    }
    if (readUnsigned16(input + ReservedOffset) != 0)
    {
        return DecodeHealthPayloadResult::failure(makeError(
            HealthPayloadErrorCode::ReservedFieldSet, QStringLiteral("Worker health reserved field must be zero.")));
    }

    const HealthResponsePayload payload{
        static_cast<HealthServiceState>(serviceState),
        readUnsigned64(input + QueuedJobsOffset),
        readUnsigned64(input + RunningJobsOffset),
        readUnsigned64(input + MaximumConcurrentJobsOffset),
        readUnsigned64(input + QueueCapacityOffset),
    };
    if (!hasValidLoad(payload))
    {
        return DecodeHealthPayloadResult::failure(
            makeError(HealthPayloadErrorCode::InvalidLoad, QStringLiteral("Worker health load is invalid.")));
    }
    return DecodeHealthPayloadResult::success(payload);
}

}  // namespace flexraw::worker::protocol
