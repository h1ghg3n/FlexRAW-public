#include "source_binding.h"

namespace flexraw::core::catalog
{

// 목적: persisted source binding state enum이 지원 범위인지 확인
// 입력: value: SQLite source_binding_state 정수값
// 출력: SourceBindingState로 변환 가능하면 true
bool isSourceBindingState(int value) noexcept
{
    return value >= static_cast<int>(SourceBindingState::FingerprintPending) &&
           value <= static_cast<int>(SourceBindingState::Unlinked);
}

// 목적: baseline fingerprint와 현재 source 관찰값으로 binding state 판정
// 입력: baseline: catalog에 저장된 fingerprint, observation: 현재 locator의 가용성과 fingerprint
// 출력: 처리 가능·추가 검증 필요·교체 감지 등의 source binding state
SourceBindingState evaluateSourceBinding(const types::SourceFingerprint& baseline,
                                         const SourceObservation& observation) noexcept
{
    if (observation.availability == SourceAvailability::Missing)
    {
        return SourceBindingState::Missing;
    }

    if (observation.availability == SourceAvailability::Unreadable)
    {
        return SourceBindingState::Unreadable;
    }

    const bool metadataMatches = baseline.sizeBytes == observation.fingerprint.sizeBytes &&
                                 baseline.modifiedAtMs == observation.fingerprint.modifiedAtMs;
    const bool baselineHasHash = types::hasSourceContentHash(baseline);
    const bool observationHasHash = types::hasSourceContentHash(observation.fingerprint);

    if (!baselineHasHash)
    {
        return metadataMatches ? SourceBindingState::FingerprintPending : SourceBindingState::IdentityUnverified;
    }

    if (!observationHasHash)
    {
        return metadataMatches ? SourceBindingState::Available : SourceBindingState::VerificationRequired;
    }

    return baseline.sha256 == observation.fingerprint.sha256 ? SourceBindingState::Available
                                                             : SourceBindingState::ReplacementDetected;
}

// 목적: source binding state에서 decode·develop·export 작업을 시작해도 되는지 확인
// 입력: state: 현재 source binding state
// 출력: 확인된 source 또는 import 직후 fingerprint 생성 중인 source면 true
bool allowsSourceProcessing(SourceBindingState state) noexcept
{
    return state == SourceBindingState::Available || state == SourceBindingState::FingerprintPending;
}

// 목적: source binding state가 사용자에게 relink·identity 선택 UI를 제공해야 하는지 확인
// 입력: state: 현재 source binding state
// 출력: source 사용 불가나 identity 결정이 필요하면 true, automatic verification 상태면 false
bool requiresSourceResolution(SourceBindingState state) noexcept
{
    return state == SourceBindingState::Missing || state == SourceBindingState::IdentityUnverified ||
           state == SourceBindingState::ReplacementDetected || state == SourceBindingState::Unreadable ||
           state == SourceBindingState::Unlinked;
}

}  // namespace flexraw::core::catalog
