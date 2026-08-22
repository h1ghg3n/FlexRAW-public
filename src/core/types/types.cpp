#include <utility>

#include "operation_types.h"
#include "photo_identity.h"

namespace flexraw::core::types
{

// 목적: PhotoId가 catalog에서 발급된 양의 SQLite identity인지 확인
// 입력: photoId: 확인할 catalog-local photo identity
// 출력: 0보다 큰 identity면 true
bool isValidPhotoId(PhotoId photoId) noexcept
{
    return photoId.value > 0;
}

// 목적: SourceFingerprint가 완전한 SHA-256 content hash를 보유하는지 확인
// 입력: fingerprint: 확인할 source fingerprint
// 출력: SHA-256 digest가 정확히 32 byte면 true
bool hasSourceContentHash(const SourceFingerprint& fingerprint) noexcept
{
    return fingerprint.sha256.size() == Sha256DigestSize;
}

// 목적: SourceFingerprint가 catalog에 저장 가능한 형태인지 확인
// 입력: fingerprint: file metadata와 optional SHA-256를 포함한 fingerprint
// 출력: 음수가 아닌 metadata와 없거나 32 byte인 hash면 true
bool isValidSourceFingerprint(const SourceFingerprint& fingerprint) noexcept
{
    return fingerprint.sizeBytes >= 0 && fingerprint.modifiedAtMs >= 0 &&
           (fingerprint.sha256.isEmpty() || hasSourceContentHash(fingerprint));
}

// 목적: CancellationSource가 소유한 공유 state를 읽는 token 생성
// 입력: state: source와 공유할 atomic cancellation state
// 출력: 읽기 전용 cancellation token
CancellationToken::CancellationToken(std::shared_ptr<std::atomic_bool> state) : m_state(std::move(state)) {}

// 목적: 공유 cancellation state가 취소 요청을 받았는지 확인
// 입력: 없음
// 출력: 취소가 요청됐으면 true
bool CancellationToken::isCancellationRequested() const noexcept
{
    return m_state->load(std::memory_order_acquire);
}

// 목적: 취소되지 않은 새 request cancellation state 생성
// 입력: 없음
// 출력: 독립 cancellation source
CancellationSource::CancellationSource() : m_state(std::make_shared<std::atomic_bool>(false)) {}

// 목적: worker와 pipeline에 전달할 읽기 전용 token 생성
// 입력: 없음
// 출력: source와 state를 공유하는 token
CancellationToken CancellationSource::token() const
{
    return CancellationToken(m_state);
}

// 목적: 공유 state에 cancellation 요청을 원자적으로 기록
// 입력: 없음
// 출력: 이후 모든 공유 token이 취소 상태를 관찰
void CancellationSource::requestCancellation() const noexcept
{
    m_state->store(true, std::memory_order_release);
}

}  // namespace flexraw::core::types
