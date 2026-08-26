#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "catalog_photo_client.h"
#include "client_error.h"
#include "client_identity.h"
#include "client_result.h"

namespace flexraw::core::client
{

enum class EditorWhiteBalanceMode : std::uint8_t
{
    AsShot,
    Custom,
};

struct EditorDevelopParams
{
    float exposureEv{0.0F};
    float contrast{0.0F};
    float highlights{0.0F};
    float shadows{0.0F};
    float whites{0.0F};
    float blacks{0.0F};
    float saturation{0.0F};
    float vibrance{0.0F};
    EditorWhiteBalanceMode whiteBalanceMode{EditorWhiteBalanceMode::AsShot};
    float whiteBalanceTemperatureKelvin{6500.0F};
    float whiteBalanceTint{0.0F};
    float clarity{0.0F};
    float dehaze{0.0F};
    float sharpeningAmount{0.0F};
    float sharpeningRadius{1.0F};
    float sharpeningDetail{0.0F};
    float sharpeningMasking{0.0F};
    float luminanceNoiseReduction{0.0F};
    float colorNoiseReduction{0.0F};
    float toneCurveShadows{0.0F};
    float toneCurveDarks{0.0F};
    float toneCurveLights{0.0F};
    float toneCurveHighlights{0.0F};
    float pointCurveBlack{0.0F};
    float pointCurveShadows{0.0F};
    float pointCurveMidtones{0.0F};
    float pointCurveHighlights{0.0F};
    float pointCurveWhite{0.0F};

    bool operator==(const EditorDevelopParams&) const = default;
};

struct SessionDevelopRevision
{
    std::uint64_t value{0};

    bool operator==(const SessionDevelopRevision&) const = default;
};

struct PersistedDevelopRevision
{
    std::uint64_t value{0};

    bool operator==(const PersistedDevelopRevision&) const = default;
};

struct EditorSourceSnapshot
{
    std::string path;
    std::string extension;
    std::string displayName;
    CatalogFileKind kind{CatalogFileKind::Unknown};

    bool operator==(const EditorSourceSnapshot&) const = default;
};

struct EditorSourceResolutionCapabilities
{
    bool canAcceptReplacement{false};
    bool canRegisterReplacementAsNew{false};
    bool canRelinkSource{false};

    bool operator==(const EditorSourceResolutionCapabilities&) const = default;
};

struct EditorSnapshot
{
    bool hasSelection{false};
    ClientPhotoId photoId;
    SessionDevelopRevision developRevision;
    PersistedDevelopRevision persistedRevision;
    std::optional<EditorSourceSnapshot> source;
    EditorDevelopParams params;
    bool sourceProcessingAllowed{false};
    std::optional<CatalogSourceState> sourceState;
    EditorSourceResolutionCapabilities sourceResolution;
    bool dirty{false};
    bool adjustmentActive{false};
    bool canUndo{false};
    bool canRedo{false};

    bool operator==(const EditorSnapshot&) const = default;
};

struct SelectEditorPhotoCommand
{
    ClientPhotoId photoId;
};

struct UpdateDevelopParamsCommand
{
    EditorDevelopParams params;
};

using EditorResult = ClientResult<EditorSnapshot, ClientError>;

class IEditorClient
{
public:
    // 목적: implementation별 resource를 올바른 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IEditorClient() = default;

    // 목적: 현재 Editor session의 immutable Qt-free state 조회
    // 입력: 없음
    // 출력: selection, Develop state, source capability와 history snapshot
    [[nodiscard]] virtual EditorSnapshot editorSnapshot() const = 0;

    // 목적: active Catalog의 stable PhotoId를 현재 Editor session으로 선택
    // 입력: command: 선택할 fixed-width Photo identity
    // 출력: 선택 후 snapshot 또는 validation·Catalog·Develop load 오류
    [[nodiscard]] virtual EditorResult selectPhoto(const SelectEditorPhotoCommand& command) = 0;

    // 목적: 현재 Editor selection과 진행 중 Adjustment를 정리
    // 입력: 없음
    // 출력: 선택되지 않은 snapshot
    [[nodiscard]] virtual EditorResult clearEditorSelection() = 0;

    // 목적: 현재 Photo의 Develop parameter를 검증하고 session state에 반영
    // 입력: command: Qt-free Develop parameter 전체 값
    // 출력: 변경 후 snapshot 또는 selection·source·validation 오류
    [[nodiscard]] virtual EditorResult updateDevelopParams(const UpdateDevelopParamsCommand& command) = 0;

    // 목적: 연속 Develop parameter 조작을 하나의 undo 단위로 시작
    // 입력: 없음
    // 출력: Adjustment가 활성화된 snapshot 또는 현재 state 오류
    [[nodiscard]] virtual EditorResult beginAdjustment() = 0;

    // 목적: 현재 연속 Adjustment를 종료하고 undo 단위를 확정
    // 입력: 없음
    // 출력: Adjustment가 종료된 snapshot 또는 현재 state 오류
    [[nodiscard]] virtual EditorResult endAdjustment() = 0;

    // 목적: 현재 Photo의 마지막 Develop Adjustment를 되돌림
    // 입력: 없음
    // 출력: 되돌린 snapshot 또는 selection·history 상태 오류
    [[nodiscard]] virtual EditorResult undoDevelop() = 0;

    // 목적: 현재 Photo에서 마지막으로 되돌린 Develop Adjustment를 다시 적용
    // 입력: 없음
    // 출력: 다시 적용한 snapshot 또는 selection·history 상태 오류
    [[nodiscard]] virtual EditorResult redoDevelop() = 0;

    // 목적: dirty Develop state를 optimistic persisted revision으로 저장
    // 입력: 없음
    // 출력: 저장된 baseline과 revision snapshot 또는 conflict·database 오류
    [[nodiscard]] virtual EditorResult saveDevelopState() = 0;
};

}  // namespace flexraw::core::client
