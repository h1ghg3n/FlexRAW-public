#include "render_payload_codec.h"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include <QByteArray>
#include <QString>

#include "develop.h"
#include "export.h"
#include "protocol_contracts.h"

namespace flexraw::worker::runtime
{
namespace
{

static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559,
              "Worker protocol v1 requires 32-bit IEEE-754 float values.");
static_assert(std::bit_cast<std::uint32_t>(std::int32_t{-1}) == std::numeric_limits<std::uint32_t>::max(),
              "Worker protocol v1 requires two's-complement signed integers.");

using DevelopFloatMember = float core::types::DevelopParams::*;
using StatsMember = std::uint64_t core::measurement::RenderStats::*;

constexpr std::array<DevelopFloatMember, 27> DevelopFloatMembers{
    &core::types::DevelopParams::exposureEv,
    &core::types::DevelopParams::contrast,
    &core::types::DevelopParams::highlights,
    &core::types::DevelopParams::shadows,
    &core::types::DevelopParams::whites,
    &core::types::DevelopParams::blacks,
    &core::types::DevelopParams::saturation,
    &core::types::DevelopParams::vibrance,
    &core::types::DevelopParams::whiteBalanceTemperatureKelvin,
    &core::types::DevelopParams::whiteBalanceTint,
    &core::types::DevelopParams::clarity,
    &core::types::DevelopParams::dehaze,
    &core::types::DevelopParams::sharpeningAmount,
    &core::types::DevelopParams::sharpeningRadius,
    &core::types::DevelopParams::sharpeningDetail,
    &core::types::DevelopParams::sharpeningMasking,
    &core::types::DevelopParams::luminanceNoiseReduction,
    &core::types::DevelopParams::colorNoiseReduction,
    &core::types::DevelopParams::toneCurveShadows,
    &core::types::DevelopParams::toneCurveDarks,
    &core::types::DevelopParams::toneCurveLights,
    &core::types::DevelopParams::toneCurveHighlights,
    &core::types::DevelopParams::pointCurveBlack,
    &core::types::DevelopParams::pointCurveShadows,
    &core::types::DevelopParams::pointCurveMidtones,
    &core::types::DevelopParams::pointCurveHighlights,
    &core::types::DevelopParams::pointCurveWhite,
};

constexpr std::array<StatsMember, 5> RenderStatsMembers{
    &core::measurement::RenderStats::decodeNanoseconds,
    &core::measurement::RenderStats::sourceConversionNanoseconds,
    &core::measurement::RenderStats::developNanoseconds,
    &core::measurement::RenderStats::outputNanoseconds,
    &core::measurement::RenderStats::totalNanoseconds,
};

class PayloadWriter final
{
public:
    // 목적: binary payload 끝에 unsigned 8-bit 값 기록
    // 입력: value: 기록할 값
    // 출력: 없음
    void writeUnsigned8(const std::uint8_t value)
    {
        m_bytes.append(static_cast<char>(value));
    }

    // 목적: binary payload 끝에 big-endian unsigned 16-bit 값 기록
    // 입력: value: 기록할 값
    // 출력: 없음
    void writeUnsigned16(const std::uint16_t value)
    {
        for (int shift = 8; shift >= 0; shift -= 8)
        {
            writeUnsigned8(static_cast<std::uint8_t>((value >> static_cast<unsigned int>(shift)) & 0xFFU));
        }
    }

    // 목적: binary payload 끝에 big-endian unsigned 32-bit 값 기록
    // 입력: value: 기록할 값
    // 출력: 없음
    void writeUnsigned32(const std::uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            writeUnsigned8(static_cast<std::uint8_t>((value >> static_cast<unsigned int>(shift)) & 0xFFU));
        }
    }

    // 목적: signed 32-bit 값을 two's-complement bit pattern으로 기록
    // 입력: value: 기록할 signed 값
    // 출력: 없음
    void writeSigned32(const std::int32_t value)
    {
        writeUnsigned32(std::bit_cast<std::uint32_t>(value));
    }

    // 목적: binary payload 끝에 big-endian unsigned 64-bit 값 기록
    // 입력: value: 기록할 값
    // 출력: 없음
    void writeUnsigned64(const std::uint64_t value)
    {
        for (int shift = 56; shift >= 0; shift -= 8)
        {
            writeUnsigned8(static_cast<std::uint8_t>((value >> static_cast<unsigned int>(shift)) & 0xFFU));
        }
    }

    // 목적: IEEE-754 float bit pattern을 big-endian 32-bit 값으로 기록
    // 입력: value: 기록할 single-precision 값
    // 출력: 없음
    void writeFloat(const float value)
    {
        writeUnsigned32(std::bit_cast<std::uint32_t>(value));
    }

    // 목적: 길이 prefix가 있는 UTF-8 string을 bounded payload에 기록
    // 입력: value: 기록할 text, maximumBytes: 허용 UTF-8 byte 수, error: 실패 정보 출력
    // 출력: 기록 성공 여부
    [[nodiscard]] bool writeString(const QString& value, const std::uint32_t maximumBytes, PayloadError& error)
    {
        const QByteArray encoded = value.toUtf8();
        if (encoded.size() > static_cast<qsizetype>(maximumBytes))
        {
            error = {PayloadErrorCode::StringTooLong, QStringLiteral("Payload string exceeds its byte limit.")};
            return false;
        }
        writeUnsigned32(static_cast<std::uint32_t>(encoded.size()));
        m_bytes.append(encoded);
        return true;
    }

    // 목적: 완성된 payload byte 소유권 반환
    // 입력: 없음
    // 출력: 지금까지 기록한 binary payload
    [[nodiscard]] QByteArray takeBytes()
    {
        return std::move(m_bytes);
    }

private:
    QByteArray m_bytes;
};

class PayloadReader final
{
public:
    // 목적: payload 전체를 읽는 bounded cursor 초기화
    // 입력: bytes: 역직렬화할 binary payload
    // 출력: payload 시작 위치의 reader
    explicit PayloadReader(const QByteArrayView bytes) : m_bytes(bytes) {}

    // 목적: cursor에서 unsigned 8-bit 값 읽기
    // 입력: value: 성공 시 읽은 값 출력
    // 출력: 남은 byte가 충분한지 여부
    [[nodiscard]] bool readUnsigned8(std::uint8_t& value)
    {
        if (!hasBytes(1))
        {
            return false;
        }
        value = static_cast<std::uint8_t>(m_bytes[m_offset]);
        ++m_offset;
        return true;
    }

    // 목적: cursor에서 big-endian unsigned 16-bit 값 읽기
    // 입력: value: 성공 시 읽은 값 출력
    // 출력: 남은 byte가 충분한지 여부
    [[nodiscard]] bool readUnsigned16(std::uint16_t& value)
    {
        std::uint64_t decoded = 0;
        if (!readUnsigned(2, decoded))
        {
            return false;
        }
        value = static_cast<std::uint16_t>(decoded);
        return true;
    }

    // 목적: cursor에서 big-endian unsigned 32-bit 값 읽기
    // 입력: value: 성공 시 읽은 값 출력
    // 출력: 남은 byte가 충분한지 여부
    [[nodiscard]] bool readUnsigned32(std::uint32_t& value)
    {
        std::uint64_t decoded = 0;
        if (!readUnsigned(4, decoded))
        {
            return false;
        }
        value = static_cast<std::uint32_t>(decoded);
        return true;
    }

    // 목적: cursor의 32-bit two's-complement bit pattern을 signed 값으로 복원
    // 입력: value: 성공 시 읽은 signed 값 출력
    // 출력: 남은 byte가 충분한지 여부
    [[nodiscard]] bool readSigned32(std::int32_t& value)
    {
        std::uint32_t bits = 0;
        if (!readUnsigned32(bits))
        {
            return false;
        }
        value = std::bit_cast<std::int32_t>(bits);
        return true;
    }

    // 목적: cursor에서 big-endian unsigned 64-bit 값 읽기
    // 입력: value: 성공 시 읽은 값 출력
    // 출력: 남은 byte가 충분한지 여부
    [[nodiscard]] bool readUnsigned64(std::uint64_t& value)
    {
        return readUnsigned(8, value);
    }

    // 목적: cursor의 32-bit IEEE-754 bit pattern을 float로 복원
    // 입력: value: 성공 시 읽은 single-precision 값 출력
    // 출력: 남은 byte가 충분한지 여부
    [[nodiscard]] bool readFloat(float& value)
    {
        std::uint32_t bits = 0;
        if (!readUnsigned32(bits))
        {
            return false;
        }
        value = std::bit_cast<float>(bits);
        return true;
    }

    // 목적: 길이 prefix가 있는 strict UTF-8 string 읽기
    // 입력: maximumBytes: 허용 byte 수, value/error: 성공 또는 실패 정보 출력
    // 출력: 길이, 남은 byte와 UTF-8 검증 성공 여부
    [[nodiscard]] bool readString(const std::uint32_t maximumBytes, QString& value, PayloadError& error)
    {
        std::uint32_t byteCount = 0;
        if (!readUnsigned32(byteCount))
        {
            error = {PayloadErrorCode::MalformedPayload, QStringLiteral("Payload string is truncated.")};
            return false;
        }
        if (byteCount > maximumBytes)
        {
            error = {PayloadErrorCode::StringTooLong, QStringLiteral("Payload string exceeds its byte limit.")};
            return false;
        }
        if (!hasBytes(static_cast<qsizetype>(byteCount)))
        {
            error = {PayloadErrorCode::MalformedPayload, QStringLiteral("Payload string is truncated.")};
            return false;
        }

        const QByteArray encoded(m_bytes.data() + m_offset, static_cast<qsizetype>(byteCount));
        m_offset += static_cast<qsizetype>(byteCount);
        value = QString::fromUtf8(encoded);
        if (value.toUtf8() != encoded)
        {
            error = {PayloadErrorCode::InvalidUtf8, QStringLiteral("Payload string is not valid UTF-8.")};
            return false;
        }
        return true;
    }

    // 목적: payload cursor가 마지막 byte까지 소비됐는지 확인
    // 입력: 없음
    // 출력: 읽지 않은 trailing byte가 없으면 true
    [[nodiscard]] bool isFinished() const noexcept
    {
        return m_offset == m_bytes.size();
    }

private:
    // 목적: 지정 byte 수를 cursor에서 big-endian unsigned 값으로 누적
    // 입력: byteCount: 1~8 byte 수, value: 성공 시 해석 값 출력
    // 출력: 남은 byte가 충분한지 여부
    [[nodiscard]] bool readUnsigned(const qsizetype byteCount, std::uint64_t& value)
    {
        if (!hasBytes(byteCount))
        {
            return false;
        }
        value = 0;
        for (qsizetype index = 0; index < byteCount; ++index)
        {
            value = (value << 8U) | static_cast<std::uint8_t>(m_bytes[m_offset + index]);
        }
        m_offset += byteCount;
        return true;
    }

    // 목적: cursor 뒤에 요청한 byte 수가 남았는지 overflow 없이 확인
    // 입력: byteCount: 읽으려는 non-negative byte 수
    // 출력: 안전하게 읽을 수 있으면 true
    [[nodiscard]] bool hasBytes(const qsizetype byteCount) const noexcept
    {
        return byteCount >= 0 && byteCount <= m_bytes.size() - m_offset;
    }

    QByteArrayView m_bytes;
    qsizetype m_offset{0};
};

// 목적: payload codec 오류를 지정 분류와 text로 생성
// 입력: code: 오류 분류, message: 진단 text
// 출력: 구성된 PayloadError
[[nodiscard]] PayloadError makePayloadError(const PayloadErrorCode code, QString message)
{
    return {code, std::move(message)};
}

// 목적: 모든 payload 앞에 v1 schema version 기록
// 입력: writer: payload output cursor
// 출력: 없음
void writePayloadVersion(PayloadWriter& writer)
{
    writer.writeUnsigned16(RenderPayloadMajorVersion);
    writer.writeUnsigned16(RenderPayloadMinorVersion);
}

// 목적: payload schema version을 읽고 현재 exact v1 지원 여부 확인
// 입력: reader: payload input cursor, error: 실패 정보 출력
// 출력: version header가 완전하고 지원되면 true
[[nodiscard]] bool readPayloadVersion(PayloadReader& reader, PayloadError& error)
{
    std::uint16_t majorVersion = 0;
    std::uint16_t minorVersion = 0;
    if (!reader.readUnsigned16(majorVersion) || !reader.readUnsigned16(minorVersion))
    {
        error = makePayloadError(PayloadErrorCode::MalformedPayload,
                                 QStringLiteral("Render payload version header is truncated."));
        return false;
    }
    if (majorVersion != RenderPayloadMajorVersion || minorVersion != RenderPayloadMinorVersion)
    {
        error = makePayloadError(PayloadErrorCode::UnsupportedVersion,
                                 QStringLiteral("Render payload version is not supported."));
        return false;
    }
    return true;
}

// 목적: WhiteBalanceMode를 안정적인 wire enum 값으로 변환
// 입력: value: domain enum 값
// 출력: 알려진 값의 wire representation 또는 unknown
[[nodiscard]] std::optional<std::uint8_t> encodeWhiteBalanceMode(const core::types::WhiteBalanceMode value)
{
    switch (value)
    {
    case core::types::WhiteBalanceMode::AsShot:
        return 0;
    case core::types::WhiteBalanceMode::Custom:
        return 1;
    }
    return std::nullopt;
}

// 목적: wire enum 값을 WhiteBalanceMode로 변환
// 입력: wireValue: payload에서 읽은 값, value: 성공 시 domain enum 출력
// 출력: 알려진 enum 값인지 여부
[[nodiscard]] bool decodeWhiteBalanceMode(const std::uint8_t wireValue, core::types::WhiteBalanceMode& value)
{
    switch (wireValue)
    {
    case 0:
        value = core::types::WhiteBalanceMode::AsShot;
        return true;
    case 1:
        value = core::types::WhiteBalanceMode::Custom;
        return true;
    default:
        return false;
    }
}

// 목적: DevelopParams를 schema에 고정된 enum/float 순서로 기록
// 입력: writer: output cursor, params: validation된 develop 값, error: enum 실패 정보 출력
// 출력: 모든 field 기록 성공 여부
[[nodiscard]] bool writeDevelopParams(PayloadWriter& writer,
                                      const core::types::DevelopParams& params,
                                      PayloadError& error)
{
    const std::optional<std::uint8_t> whiteBalanceMode = encodeWhiteBalanceMode(params.whiteBalanceMode);
    if (!whiteBalanceMode.has_value())
    {
        error = makePayloadError(PayloadErrorCode::InvalidEnumValue,
                                 QStringLiteral("White balance mode is not supported by the payload schema."));
        return false;
    }

    writer.writeUnsigned8(*whiteBalanceMode);
    for (const DevelopFloatMember member : DevelopFloatMembers)
    {
        writer.writeFloat(params.*member);
    }
    return true;
}

// 목적: schema에 고정된 enum/float 순서에서 DevelopParams 복원
// 입력: reader: input cursor, params/error: 성공 또는 실패 정보 출력
// 출력: 모든 field와 enum을 읽었으면 true
[[nodiscard]] bool readDevelopParams(PayloadReader& reader, core::types::DevelopParams& params, PayloadError& error)
{
    std::uint8_t whiteBalanceMode = 0;
    if (!reader.readUnsigned8(whiteBalanceMode))
    {
        error = makePayloadError(PayloadErrorCode::MalformedPayload,
                                 QStringLiteral("Develop parameter payload is truncated."));
        return false;
    }
    if (!decodeWhiteBalanceMode(whiteBalanceMode, params.whiteBalanceMode))
    {
        error = makePayloadError(PayloadErrorCode::InvalidEnumValue, QStringLiteral("White balance mode is invalid."));
        return false;
    }

    for (const DevelopFloatMember member : DevelopFloatMembers)
    {
        if (!reader.readFloat(params.*member))
        {
            error = makePayloadError(PayloadErrorCode::MalformedPayload,
                                     QStringLiteral("Develop parameter payload is truncated."));
            return false;
        }
    }
    return true;
}

// 목적: RasterExportFormat을 stable wire enum으로 변환
// 입력: value: domain format
// 출력: 알려진 format의 wire 값 또는 unknown
[[nodiscard]] std::optional<std::uint8_t> encodeRasterFormat(const core::export_::RasterExportFormat value)
{
    switch (value)
    {
    case core::export_::RasterExportFormat::Jpeg:
        return 0;
    case core::export_::RasterExportFormat::Png:
        return 1;
    case core::export_::RasterExportFormat::Tiff:
        return 2;
    }
    return std::nullopt;
}

// 목적: wire enum을 RasterExportFormat으로 변환
// 입력: wireValue: payload 값, value: 성공 시 domain format 출력
// 출력: 알려진 format인지 여부
[[nodiscard]] bool decodeRasterFormat(const std::uint8_t wireValue, core::export_::RasterExportFormat& value)
{
    switch (wireValue)
    {
    case 0:
        value = core::export_::RasterExportFormat::Jpeg;
        return true;
    case 1:
        value = core::export_::RasterExportFormat::Png;
        return true;
    case 2:
        value = core::export_::RasterExportFormat::Tiff;
        return true;
    default:
        return false;
    }
}

// 목적: TiffCompression을 stable wire enum으로 변환
// 입력: value: domain compression
// 출력: 알려진 compression의 wire 값 또는 unknown
[[nodiscard]] std::optional<std::uint8_t> encodeTiffCompression(const core::export_::TiffCompression value)
{
    switch (value)
    {
    case core::export_::TiffCompression::None:
        return 0;
    case core::export_::TiffCompression::Lzw:
        return 1;
    }
    return std::nullopt;
}

// 목적: wire enum을 TiffCompression으로 변환
// 입력: wireValue: payload 값, value: 성공 시 domain compression 출력
// 출력: 알려진 compression인지 여부
[[nodiscard]] bool decodeTiffCompression(const std::uint8_t wireValue, core::export_::TiffCompression& value)
{
    switch (wireValue)
    {
    case 0:
        value = core::export_::TiffCompression::None;
        return true;
    case 1:
        value = core::export_::TiffCompression::Lzw;
        return true;
    default:
        return false;
    }
}

// 목적: RasterOutputColorSpace를 stable wire enum으로 변환
// 입력: value: domain color space
// 출력: 알려진 color space의 wire 값 또는 unknown
[[nodiscard]] std::optional<std::uint8_t> encodeOutputColorSpace(const core::export_::RasterOutputColorSpace value)
{
    switch (value)
    {
    case core::export_::RasterOutputColorSpace::Srgb:
        return 0;
    case core::export_::RasterOutputColorSpace::AdobeRgb:
        return 1;
    case core::export_::RasterOutputColorSpace::DisplayP3:
        return 2;
    }
    return std::nullopt;
}

// 목적: wire enum을 RasterOutputColorSpace로 변환
// 입력: wireValue: payload 값, value: 성공 시 domain color space 출력
// 출력: 알려진 color space인지 여부
[[nodiscard]] bool decodeOutputColorSpace(const std::uint8_t wireValue, core::export_::RasterOutputColorSpace& value)
{
    switch (wireValue)
    {
    case 0:
        value = core::export_::RasterOutputColorSpace::Srgb;
        return true;
    case 1:
        value = core::export_::RasterOutputColorSpace::AdobeRgb;
        return true;
    case 2:
        value = core::export_::RasterOutputColorSpace::DisplayP3;
        return true;
    default:
        return false;
    }
}

// 목적: 검증된 RasterExportOptions를 stable scalar 순서로 기록
// 입력: writer: output cursor, options: raster option, error: enum 실패 정보 출력
// 출력: 모든 field 기록 성공 여부
[[nodiscard]] bool writeOutputOptions(PayloadWriter& writer,
                                      const core::export_::RasterExportOptions& options,
                                      PayloadError& error)
{
    const std::optional<std::uint8_t> format = encodeRasterFormat(options.format);
    const std::optional<std::uint8_t> compression = encodeTiffCompression(options.tiffCompression);
    const std::optional<std::uint8_t> colorSpace = encodeOutputColorSpace(options.outputColorSpace);
    if (!format.has_value() || !compression.has_value() || !colorSpace.has_value())
    {
        error = makePayloadError(PayloadErrorCode::InvalidEnumValue,
                                 QStringLiteral("Raster output option contains an unsupported enum value."));
        return false;
    }
    if (!std::in_range<std::int32_t>(options.jpegQuality) || !std::in_range<std::int32_t>(options.pngCompression) ||
        !std::in_range<std::int32_t>(options.maximumDimension))
    {
        error = makePayloadError(PayloadErrorCode::InvalidOutputOptions,
                                 QStringLiteral("Raster output integer exceeds the wire range."));
        return false;
    }

    writer.writeUnsigned8(*format);
    writer.writeSigned32(static_cast<std::int32_t>(options.jpegQuality));
    writer.writeSigned32(static_cast<std::int32_t>(options.pngCompression));
    writer.writeUnsigned8(*compression);
    writer.writeSigned32(static_cast<std::int32_t>(options.maximumDimension));
    writer.writeUnsigned8(*colorSpace);
    writer.writeUnsigned8(options.includeMetadata ? 1 : 0);
    return true;
}

// 목적: stable scalar 순서에서 RasterExportOptions 복원
// 입력: reader: input cursor, options/error: 성공 또는 실패 정보 출력
// 출력: 모든 field와 enum/bool을 읽었으면 true
[[nodiscard]] bool readOutputOptions(PayloadReader& reader,
                                     core::export_::RasterExportOptions& options,
                                     PayloadError& error)
{
    std::uint8_t format = 0;
    std::uint8_t compression = 0;
    std::uint8_t colorSpace = 0;
    std::uint8_t includeMetadata = 0;
    std::int32_t jpegQuality = 0;
    std::int32_t pngCompression = 0;
    std::int32_t maximumDimension = 0;
    if (!reader.readUnsigned8(format) || !reader.readSigned32(jpegQuality) || !reader.readSigned32(pngCompression) ||
        !reader.readUnsigned8(compression) || !reader.readSigned32(maximumDimension) ||
        !reader.readUnsigned8(colorSpace) || !reader.readUnsigned8(includeMetadata))
    {
        error = makePayloadError(PayloadErrorCode::MalformedPayload,
                                 QStringLiteral("Raster output option payload is truncated."));
        return false;
    }
    if (!decodeRasterFormat(format, options.format) || !decodeTiffCompression(compression, options.tiffCompression) ||
        !decodeOutputColorSpace(colorSpace, options.outputColorSpace) || includeMetadata > 1)
    {
        error = makePayloadError(PayloadErrorCode::InvalidEnumValue,
                                 QStringLiteral("Raster output option contains an invalid enum or bool value."));
        return false;
    }
    options.jpegQuality = static_cast<int>(jpegQuality);
    options.pngCompression = static_cast<int>(pngCompression);
    options.maximumDimension = static_cast<int>(maximumDimension);
    options.includeMetadata = includeMetadata != 0;
    return true;
}

// 목적: RenderStats의 고정 stage 값을 순서대로 기록
// 입력: writer: output cursor, stats: stage별 nanosecond 값
// 출력: 없음
void writeRenderStats(PayloadWriter& writer, const core::measurement::RenderStats& stats)
{
    for (const StatsMember member : RenderStatsMembers)
    {
        writer.writeUnsigned64(stats.*member);
    }
}

// 목적: 고정 stage 순서에서 RenderStats 복원
// 입력: reader: input cursor, stats: 성공 시 timing 출력
// 출력: 모든 stage 값이 존재하면 true
[[nodiscard]] bool readRenderStats(PayloadReader& reader, core::measurement::RenderStats& stats)
{
    for (const StatsMember member : RenderStatsMembers)
    {
        if (!reader.readUnsigned64(stats.*member))
        {
            return false;
        }
    }
    return true;
}

// 목적: CoreErrorCode를 protocol에서 안정적인 wire enum으로 변환
// 입력: value: domain error code
// 출력: 알려진 error의 wire 값 또는 unknown
[[nodiscard]] std::optional<std::uint16_t> encodeCoreErrorCode(const core::types::ErrorCode value)
{
    switch (value)
    {
    case core::types::ErrorCode::Unknown:
        return 0;
    case core::types::ErrorCode::InvalidArgument:
        return 1;
    case core::types::ErrorCode::NotFound:
        return 2;
    case core::types::ErrorCode::PermissionDenied:
        return 3;
    case core::types::ErrorCode::UnsupportedFormat:
        return 4;
    case core::types::ErrorCode::ThumbnailUnavailable:
        return 5;
    case core::types::ErrorCode::DecodeFailed:
        return 6;
    case core::types::ErrorCode::DatabaseError:
        return 7;
    case core::types::ErrorCode::Conflict:
        return 8;
    case core::types::ErrorCode::Cancelled:
        return 9;
    }
    return std::nullopt;
}

// 목적: wire enum을 CoreErrorCode로 변환
// 입력: wireValue: payload 값, value: 성공 시 domain error 출력
// 출력: 알려진 error code인지 여부
[[nodiscard]] bool decodeCoreErrorCode(const std::uint16_t wireValue, core::types::ErrorCode& value)
{
    switch (wireValue)
    {
    case 0:
        value = core::types::ErrorCode::Unknown;
        return true;
    case 1:
        value = core::types::ErrorCode::InvalidArgument;
        return true;
    case 2:
        value = core::types::ErrorCode::NotFound;
        return true;
    case 3:
        value = core::types::ErrorCode::PermissionDenied;
        return true;
    case 4:
        value = core::types::ErrorCode::UnsupportedFormat;
        return true;
    case 5:
        value = core::types::ErrorCode::ThumbnailUnavailable;
        return true;
    case 6:
        value = core::types::ErrorCode::DecodeFailed;
        return true;
    case 7:
        value = core::types::ErrorCode::DatabaseError;
        return true;
    case 8:
        value = core::types::ErrorCode::Conflict;
        return true;
    case 9:
        value = core::types::ErrorCode::Cancelled;
        return true;
    default:
        return false;
    }
}

// 목적: encoded payload가 envelope 상한 안인지 확인하고 성공 Result 생성
// 입력: bytes: 완성된 payload
// 출력: bounded byte array 또는 payload 상한 오류
[[nodiscard]] EncodePayloadResult finishEncoding(QByteArray bytes)
{
    if (bytes.size() > static_cast<qsizetype>(protocol::MaximumPayloadBytes))
    {
        return EncodePayloadResult::failure(makePayloadError(
            PayloadErrorCode::PayloadTooLarge, QStringLiteral("Render payload exceeds the protocol limit.")));
    }
    return EncodePayloadResult::success(std::move(bytes));
}

// 목적: direct decoder 호출도 protocol envelope와 같은 payload 상한 적용
// 입력: bytes: decode할 payload 전체
// 출력: 상한 초과 시 PayloadTooLarge 오류
[[nodiscard]] std::optional<PayloadError> validateDecodeSize(const QByteArrayView bytes)
{
    if (bytes.size() > static_cast<qsizetype>(protocol::MaximumPayloadBytes))
    {
        return makePayloadError(PayloadErrorCode::PayloadTooLarge,
                                QStringLiteral("Render payload exceeds the protocol limit."));
    }
    return std::nullopt;
}

// 목적: decoder가 payload 전체를 정확히 소비했는지 확인
// 입력: reader: decode 후 cursor, error: trailing byte 실패 정보 출력
// 출력: unread byte가 없으면 true
[[nodiscard]] bool validateFinished(const PayloadReader& reader, PayloadError& error)
{
    if (reader.isFinished())
    {
        return true;
    }
    error =
        makePayloadError(PayloadErrorCode::TrailingBytes, QStringLiteral("Render payload contains trailing bytes."));
    return false;
}

}  // namespace

// 목적: resolved render request를 versioned binary payload로 직렬화
// 입력: payload: root-relative path, DevelopParams와 raster output option
// 출력: wire payload 또는 value/size contract 오류
EncodePayloadResult encodeRenderRequestPayload(const RenderRequestPayload& payload)
{
    const core::develop::DevelopParamsValidationResult developValidation =
        core::develop::validateDevelopParams(payload.developParams);
    if (developValidation.hasError())
    {
        return EncodePayloadResult::failure(
            makePayloadError(PayloadErrorCode::InvalidDevelopParams, developValidation.error().message));
    }
    const core::export_::RasterExportResult outputValidation =
        core::export_::validateRasterExportOptions(payload.outputOptions);
    if (outputValidation.hasError())
    {
        return EncodePayloadResult::failure(
            makePayloadError(PayloadErrorCode::InvalidOutputOptions, outputValidation.error().message));
    }

    PayloadWriter writer;
    PayloadError error;
    writePayloadVersion(writer);
    if (!writer.writeString(payload.sourceRelativePath, MaximumWirePathBytes, error) ||
        !writer.writeString(payload.outputRelativePath, MaximumWirePathBytes, error) ||
        !writeDevelopParams(writer, developValidation.value(), error) ||
        !writeOutputOptions(writer, payload.outputOptions, error))
    {
        return EncodePayloadResult::failure(std::move(error));
    }
    return finishEncoding(writer.takeBytes());
}

// 목적: versioned binary payload를 resolved render request 값으로 역직렬화
// 입력: bytes: RenderRequest frame의 payload 전체
// 출력: 검증된 request payload 또는 schema/value 오류
DecodeRenderRequestResult decodeRenderRequestPayload(const QByteArrayView bytes)
{
    if (const std::optional<PayloadError> sizeError = validateDecodeSize(bytes); sizeError.has_value())
    {
        return DecodeRenderRequestResult::failure(*sizeError);
    }
    PayloadReader reader(bytes);
    PayloadError error;
    RenderRequestPayload payload;
    if (!readPayloadVersion(reader, error) ||
        !reader.readString(MaximumWirePathBytes, payload.sourceRelativePath, error) ||
        !reader.readString(MaximumWirePathBytes, payload.outputRelativePath, error) ||
        !readDevelopParams(reader, payload.developParams, error) ||
        !readOutputOptions(reader, payload.outputOptions, error) || !validateFinished(reader, error))
    {
        return DecodeRenderRequestResult::failure(std::move(error));
    }

    const core::develop::DevelopParamsValidationResult developValidation =
        core::develop::validateDevelopParams(payload.developParams);
    if (developValidation.hasError())
    {
        return DecodeRenderRequestResult::failure(
            makePayloadError(PayloadErrorCode::InvalidDevelopParams, developValidation.error().message));
    }
    const core::export_::RasterExportResult outputValidation =
        core::export_::validateRasterExportOptions(payload.outputOptions);
    if (outputValidation.hasError())
    {
        return DecodeRenderRequestResult::failure(
            makePayloadError(PayloadErrorCode::InvalidOutputOptions, outputValidation.error().message));
    }
    payload.developParams = developValidation.value();
    return DecodeRenderRequestResult::success(std::move(payload));
}

// 목적: render 성공 artifact와 stage timing을 versioned payload로 직렬화
// 입력: payload: root-relative artifact path, byte size와 RenderStats
// 출력: wire payload 또는 string/size 오류
EncodePayloadResult encodeRenderSucceededPayload(const RenderSucceededPayload& payload)
{
    PayloadWriter writer;
    PayloadError error;
    writePayloadVersion(writer);
    if (!writer.writeString(payload.outputRelativePath, MaximumWirePathBytes, error))
    {
        return EncodePayloadResult::failure(std::move(error));
    }
    writer.writeUnsigned64(payload.byteSize);
    writeRenderStats(writer, payload.stats);
    return finishEncoding(writer.takeBytes());
}

// 목적: versioned render 성공 payload를 artifact와 timing으로 역직렬화
// 입력: bytes: RenderSucceeded frame의 payload 전체
// 출력: 성공 payload 또는 schema/string 오류
DecodeRenderSucceededResult decodeRenderSucceededPayload(const QByteArrayView bytes)
{
    if (const std::optional<PayloadError> sizeError = validateDecodeSize(bytes); sizeError.has_value())
    {
        return DecodeRenderSucceededResult::failure(*sizeError);
    }
    PayloadReader reader(bytes);
    PayloadError error;
    RenderSucceededPayload payload;
    if (!readPayloadVersion(reader, error) ||
        !reader.readString(MaximumWirePathBytes, payload.outputRelativePath, error) ||
        !reader.readUnsigned64(payload.byteSize) || !readRenderStats(reader, payload.stats))
    {
        if (error.message.isEmpty())
        {
            error = makePayloadError(PayloadErrorCode::MalformedPayload,
                                     QStringLiteral("Render success payload is truncated."));
        }
        return DecodeRenderSucceededResult::failure(std::move(error));
    }
    if (!validateFinished(reader, error))
    {
        return DecodeRenderSucceededResult::failure(std::move(error));
    }
    return DecodeRenderSucceededResult::success(std::move(payload));
}

// 목적: render 실패 원인과 완료 stage timing을 versioned payload로 직렬화
// 입력: payload: CoreError와 RenderStats
// 출력: wire payload 또는 error/string/size 오류
EncodePayloadResult encodeRenderFailedPayload(const RenderFailedPayload& payload)
{
    const std::optional<std::uint16_t> errorCode = encodeCoreErrorCode(payload.cause.code);
    if (!errorCode.has_value())
    {
        return EncodePayloadResult::failure(
            makePayloadError(PayloadErrorCode::InvalidEnumValue,
                             QStringLiteral("Core error code is not supported by the payload schema.")));
    }

    PayloadWriter writer;
    PayloadError error;
    writePayloadVersion(writer);
    writer.writeUnsigned16(*errorCode);
    if (!writer.writeString(payload.cause.message, MaximumWireMessageBytes, error))
    {
        return EncodePayloadResult::failure(std::move(error));
    }
    writeRenderStats(writer, payload.stats);
    return finishEncoding(writer.takeBytes());
}

// 목적: versioned render 실패 payload를 CoreError와 timing으로 역직렬화
// 입력: bytes: RenderFailed frame의 payload 전체
// 출력: 실패 payload 또는 schema/error/string 오류
DecodeRenderFailedResult decodeRenderFailedPayload(const QByteArrayView bytes)
{
    if (const std::optional<PayloadError> sizeError = validateDecodeSize(bytes); sizeError.has_value())
    {
        return DecodeRenderFailedResult::failure(*sizeError);
    }
    PayloadReader reader(bytes);
    PayloadError error;
    RenderFailedPayload payload;
    std::uint16_t errorCode = 0;
    if (!readPayloadVersion(reader, error) || !reader.readUnsigned16(errorCode) ||
        !reader.readString(MaximumWireMessageBytes, payload.cause.message, error) ||
        !readRenderStats(reader, payload.stats))
    {
        if (error.message.isEmpty())
        {
            error = makePayloadError(PayloadErrorCode::MalformedPayload,
                                     QStringLiteral("Render failure payload is truncated."));
        }
        return DecodeRenderFailedResult::failure(std::move(error));
    }
    if (!decodeCoreErrorCode(errorCode, payload.cause.code))
    {
        return DecodeRenderFailedResult::failure(makePayloadError(
            PayloadErrorCode::InvalidEnumValue, QStringLiteral("Render failure contains an invalid Core error code.")));
    }
    if (!validateFinished(reader, error))
    {
        return DecodeRenderFailedResult::failure(std::move(error));
    }
    return DecodeRenderFailedResult::success(std::move(payload));
}

// 목적: bounded scheduler 거절 이유를 versioned payload로 직렬화
// 입력: payload: 사용자에게 전달할 짧은 busy 진단
// 출력: wire payload 또는 string/size 오류
EncodePayloadResult encodeServerBusyPayload(const ServerBusyPayload& payload)
{
    PayloadWriter writer;
    PayloadError error;
    writePayloadVersion(writer);
    if (!writer.writeString(payload.message, MaximumWireMessageBytes, error))
    {
        return EncodePayloadResult::failure(std::move(error));
    }
    return finishEncoding(writer.takeBytes());
}

// 목적: versioned ServerBusy payload를 진단 값으로 역직렬화
// 입력: bytes: ServerBusy frame의 payload 전체
// 출력: busy payload 또는 schema/string 오류
DecodeServerBusyResult decodeServerBusyPayload(const QByteArrayView bytes)
{
    if (const std::optional<PayloadError> sizeError = validateDecodeSize(bytes); sizeError.has_value())
    {
        return DecodeServerBusyResult::failure(*sizeError);
    }
    PayloadReader reader(bytes);
    PayloadError error;
    ServerBusyPayload payload;
    if (!readPayloadVersion(reader, error) || !reader.readString(MaximumWireMessageBytes, payload.message, error) ||
        !validateFinished(reader, error))
    {
        return DecodeServerBusyResult::failure(std::move(error));
    }
    return DecodeServerBusyResult::success(std::move(payload));
}

// 목적: accepted render의 resource admission 거절과 retry advice를 직렬화
// 입력: payload: 사용자 진단과 optional retry delay
// 출력: wire payload 또는 string/range/size 오류
EncodePayloadResult encodeResourceBusyPayload(const ResourceBusyPayload& payload)
{
    if (payload.retryAfter.has_value() && payload.retryAfter->count() < 0)
    {
        return EncodePayloadResult::failure(makePayloadError(
            PayloadErrorCode::MalformedPayload, QStringLiteral("Resource busy retry delay cannot be negative.")));
    }

    PayloadWriter writer;
    PayloadError error;
    writePayloadVersion(writer);
    if (!writer.writeString(payload.message, MaximumWireMessageBytes, error))
    {
        return EncodePayloadResult::failure(std::move(error));
    }
    writer.writeUnsigned8(payload.retryAfter.has_value() ? 1U : 0U);
    if (payload.retryAfter.has_value())
    {
        writer.writeUnsigned64(static_cast<std::uint64_t>(payload.retryAfter->count()));
    }
    return finishEncoding(writer.takeBytes());
}

// 목적: versioned ResourceBusy payload를 구조적 admission 결과로 역직렬화
// 입력: bytes: ResourceBusy frame의 payload 전체
// 출력: resource busy payload 또는 schema/string/range 오류
DecodeResourceBusyResult decodeResourceBusyPayload(const QByteArrayView bytes)
{
    if (const std::optional<PayloadError> sizeError = validateDecodeSize(bytes); sizeError.has_value())
    {
        return DecodeResourceBusyResult::failure(*sizeError);
    }

    PayloadReader reader(bytes);
    PayloadError error;
    ResourceBusyPayload payload;
    std::uint8_t hasRetryAfter = 0;
    if (!readPayloadVersion(reader, error) || !reader.readString(MaximumWireMessageBytes, payload.message, error) ||
        !reader.readUnsigned8(hasRetryAfter) || hasRetryAfter > 1U)
    {
        if (error.message.isEmpty())
        {
            error = makePayloadError(PayloadErrorCode::MalformedPayload,
                                     QStringLiteral("Resource busy payload is truncated or invalid."));
        }
        return DecodeResourceBusyResult::failure(std::move(error));
    }
    if (hasRetryAfter == 1U)
    {
        std::uint64_t retryAfterMilliseconds = 0;
        if (!reader.readUnsigned64(retryAfterMilliseconds) ||
            retryAfterMilliseconds > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            return DecodeResourceBusyResult::failure(makePayloadError(
                PayloadErrorCode::MalformedPayload, QStringLiteral("Resource busy retry delay is invalid.")));
        }
        payload.retryAfter = std::chrono::milliseconds(static_cast<std::int64_t>(retryAfterMilliseconds));
    }
    if (!validateFinished(reader, error))
    {
        return DecodeResourceBusyResult::failure(std::move(error));
    }
    return DecodeResourceBusyResult::success(std::move(payload));
}

}  // namespace flexraw::worker::runtime
