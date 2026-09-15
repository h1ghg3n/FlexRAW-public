#pragma once

#include <optional>

#include <QHash>
#include <QString>
#include <QVector>

#include "develop_params.h"
#include "operation_types.h"

namespace flexraw::core::history
{

class DevelopHistory final
{
public:
    // 목적: 지정 사진의 현재 develop 상태를 반환하고 없으면 기본값으로 초기화
    // 입력: photoPath: 사진별 history를 구분할 정규화된 파일 경로
    // 출력: 현재 DevelopParams 값
    [[nodiscard]] types::DevelopParams selectPhoto(const QString& photoPath);

    // 목적: 지정 사진의 history를 저장된 develop state로 최초 초기화하고 현재 값 반환
    // 입력: photoKey: 사진별 history identity, initialParams: history가 없을 때 사용할 persisted baseline
    // 출력: 기존 session 값 또는 새로 초기화된 DevelopParams 값
    [[nodiscard]] types::DevelopParams selectPhoto(const QString& photoKey, const types::DevelopParams& initialParams);

    // 목적: 연속된 UI 조작을 하나의 undo 단계로 시작
    // 입력: photoPath: 조작을 시작할 사진 경로
    // 출력: 없음
    void beginEdit(const QString& photoPath);

    // 목적: 사진의 현재 develop 상태를 갱신하고 필요한 undo 기준점을 기록
    // 입력: photoPath: 갱신할 사진 경로, params: 새 develop parameter 값
    // 출력: 변경이 적용되면 true
    [[nodiscard]] bool update(const QString& photoPath, const types::DevelopParams& params);

    // 목적: 현재 연속 UI 조작을 마쳐 다음 조작을 별도 undo 단계로 분리
    // 입력: photoPath: 조작을 끝낼 사진 경로
    // 출력: 없음
    void endEdit(const QString& photoPath);

    // 목적: 사진의 마지막 develop 변경을 되돌린 상태 반환
    // 입력: photoPath: undo할 사진 경로
    // 출력: 되돌린 DevelopParams 또는 가능한 undo가 없으면 빈 값
    [[nodiscard]] std::optional<types::DevelopParams> undo(const QString& photoPath);

    // 목적: 사진의 마지막으로 되돌린 develop 변경을 다시 적용한 상태 반환
    // 입력: photoPath: redo할 사진 경로
    // 출력: 다시 적용한 DevelopParams 또는 가능한 redo가 없으면 빈 값
    [[nodiscard]] std::optional<types::DevelopParams> redo(const QString& photoPath);

    // 목적: 사진에 적용 가능한 undo 단계 존재 여부 확인
    // 입력: photoPath: 확인할 사진 경로
    // 출력: undo 가능 여부
    [[nodiscard]] bool canUndo(const QString& photoPath) const;

    // 목적: 사진에 적용 가능한 redo 단계 존재 여부 확인
    // 입력: photoPath: 확인할 사진 경로
    // 출력: redo 가능 여부
    [[nodiscard]] bool canRedo(const QString& photoPath) const;

    // 목적: 지정 사진의 현재 develop state revision 반환
    // 입력: photoPath: revision을 확인할 사진 경로
    // 출력: 사진별 단조 증가 DevelopRevision, history가 없으면 0
    [[nodiscard]] types::DevelopRevision revision(const QString& photoPath) const;

    // 목적: 지정 사진의 session-local develop state와 undo/redo history 폐기
    // 입력: photoPath: 폐기할 사진별 history identity
    // 출력: 없음; 다른 사진 history는 유지
    void discardPhoto(const QString& photoPath);

private:
    struct PhotoHistory
    {
        types::DevelopParams currentParams;
        QVector<types::DevelopParams> undoStack;
        QVector<types::DevelopParams> redoStack;
        types::DevelopRevision revision{0};
        bool editInProgress{false};
        bool editRecorded{false};
    };

    // 목적: 사진 경로의 history state를 반환하고 없으면 생성
    // 입력: photoPath: history를 조회할 사진 경로
    // 출력: 해당 사진의 수정 가능한 history state
    [[nodiscard]] PhotoHistory& stateFor(const QString& photoPath);

    QHash<QString, PhotoHistory> m_photoHistories;
};

}  // namespace flexraw::core::history
