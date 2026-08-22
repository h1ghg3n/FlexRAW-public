#pragma once

#include <QByteArrayView>

#include "render_message_contracts.h"

namespace flexraw::worker::runtime
{

// 목적: resolved render request를 versioned binary payload로 직렬화
// 입력: payload: root-relative path, DevelopParams와 raster output option
// 출력: wire payload 또는 value/size contract 오류
[[nodiscard]] EncodePayloadResult encodeRenderRequestPayload(const RenderRequestPayload& payload);

// 목적: versioned binary payload를 resolved render request 값으로 역직렬화
// 입력: bytes: RenderRequest frame의 payload 전체
// 출력: 검증된 request payload 또는 schema/value 오류
[[nodiscard]] DecodeRenderRequestResult decodeRenderRequestPayload(QByteArrayView bytes);

// 목적: render 성공 artifact와 stage timing을 versioned payload로 직렬화
// 입력: payload: root-relative artifact path, byte size와 RenderStats
// 출력: wire payload 또는 string/size 오류
[[nodiscard]] EncodePayloadResult encodeRenderSucceededPayload(const RenderSucceededPayload& payload);

// 목적: versioned render 성공 payload를 artifact와 timing으로 역직렬화
// 입력: bytes: RenderSucceeded frame의 payload 전체
// 출력: 성공 payload 또는 schema/string 오류
[[nodiscard]] DecodeRenderSucceededResult decodeRenderSucceededPayload(QByteArrayView bytes);

// 목적: render 실패 원인과 완료 stage timing을 versioned payload로 직렬화
// 입력: payload: CoreError와 RenderStats
// 출력: wire payload 또는 error/string/size 오류
[[nodiscard]] EncodePayloadResult encodeRenderFailedPayload(const RenderFailedPayload& payload);

// 목적: versioned render 실패 payload를 CoreError와 timing으로 역직렬화
// 입력: bytes: RenderFailed frame의 payload 전체
// 출력: 실패 payload 또는 schema/error/string 오류
[[nodiscard]] DecodeRenderFailedResult decodeRenderFailedPayload(QByteArrayView bytes);

// 목적: bounded scheduler 거절 이유를 versioned payload로 직렬화
// 입력: payload: 사용자에게 전달할 짧은 busy 진단
// 출력: wire payload 또는 string/size 오류
[[nodiscard]] EncodePayloadResult encodeServerBusyPayload(const ServerBusyPayload& payload);

// 목적: versioned ServerBusy payload를 진단 값으로 역직렬화
// 입력: bytes: ServerBusy frame의 payload 전체
// 출력: busy payload 또는 schema/string 오류
[[nodiscard]] DecodeServerBusyResult decodeServerBusyPayload(QByteArrayView bytes);

// 목적: accepted render의 resource admission 거절과 retry advice를 직렬화
// 입력: payload: 사용자 진단과 optional retry delay
// 출력: wire payload 또는 string/range/size 오류
[[nodiscard]] EncodePayloadResult encodeResourceBusyPayload(const ResourceBusyPayload& payload);

// 목적: versioned ResourceBusy payload를 구조적 admission 결과로 역직렬화
// 입력: bytes: ResourceBusy frame의 payload 전체
// 출력: resource busy payload 또는 schema/string/range 오류
[[nodiscard]] DecodeResourceBusyResult decodeResourceBusyPayload(QByteArrayView bytes);

}  // namespace flexraw::worker::runtime
