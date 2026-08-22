#pragma once

#include "photo_identity.h"

namespace flexraw::core::catalog
{

enum class SourceAvailability
{
    Available,
    Missing,
    Unreadable,
};

enum class SourceBindingState
{
    FingerprintPending = 0,
    Available = 1,
    Missing = 2,
    VerificationRequired = 3,
    IdentityUnverified = 4,
    ReplacementDetected = 5,
    Unreadable = 6,
    Unlinked = 7,
};

struct SourceObservation
{
    SourceAvailability availability{SourceAvailability::Available};
    types::SourceFingerprint fingerprint;
};

// 목적: persisted source binding state enum이 지원 범위인지 확인
// 입력: value: SQLite source_binding_state 정수값
// 출력: SourceBindingState로 변환 가능하면 true
[[nodiscard]] bool isSourceBindingState(int value) noexcept;

// 목적: baseline fingerprint와 현재 source 관찰값으로 binding state 판정
// 입력: baseline: catalog에 저장된 fingerprint, observation: 현재 locator의 가용성과 fingerprint
// 출력: 처리 가능·추가 검증 필요·교체 감지 등의 source binding state
[[nodiscard]] SourceBindingState evaluateSourceBinding(const types::SourceFingerprint& baseline,
                                                       const SourceObservation& observation) noexcept;

// 목적: source binding state에서 decode·develop·export 작업을 시작해도 되는지 확인
// 입력: state: 현재 source binding state
// 출력: 확인된 source 또는 import 직후 fingerprint 생성 중인 source면 true
[[nodiscard]] bool allowsSourceProcessing(SourceBindingState state) noexcept;

// 목적: source binding state가 사용자에게 relink·identity 선택 UI를 제공해야 하는지 확인
// 입력: state: 현재 source binding state
// 출력: source 사용 불가나 identity 결정이 필요하면 true, automatic verification 상태면 false
[[nodiscard]] bool requiresSourceResolution(SourceBindingState state) noexcept;

}  // namespace flexraw::core::catalog
