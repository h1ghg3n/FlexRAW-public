#pragma once

#include "error.h"
#include "operation_types.h"
#include "photo_identity.h"
#include "result.h"

namespace flexraw::core::catalog
{

using SourceFingerprintResult = types::Result<types::SourceFingerprint, types::CoreError>;

// 목적: source identity fast path에 사용할 file size와 수정 시각 조회
// 입력: locator: 조회할 현재 source path
// 출력: content hash가 없는 metadata fingerprint 또는 file 접근 오류
[[nodiscard]] SourceFingerprintResult inspectSourceMetadata(const types::SourceLocator& locator);

// 목적: background source identity baseline·verification에 사용할 SHA-256 fingerprint 생성
// 입력: locator: hash할 source path, cancellationToken: chunk 사이에 확인할 cancellation state
// 출력: metadata와 SHA-256를 포함한 fingerprint 또는 file 변경·취소·IO 오류
[[nodiscard]] SourceFingerprintResult calculateSourceFingerprint(const types::SourceLocator& locator,
                                                                 const types::CancellationToken& cancellationToken);

}  // namespace flexraw::core::catalog
