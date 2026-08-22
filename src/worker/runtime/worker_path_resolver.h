#pragma once

#include <QString>

#include "render_contracts.h"
#include "render_message_contracts.h"
#include "result.h"

namespace flexraw::worker::runtime
{

struct WorkerRootConfiguration
{
    QString sourceRoot;
    QString outputRoot;
};

enum class WorkerPathErrorCode
{
    InvalidRoot,
    InvalidRelativePath,
    SourceNotFound,
    SourceOutsideRoot,
    OutputParentNotFound,
    OutputOutsideRoot,
    InvalidRequestValue,
};

struct WorkerPathError
{
    WorkerPathErrorCode code{WorkerPathErrorCode::InvalidRelativePath};
    QString message;
};

class WorkerPathResolver final
{
public:
    using CreateResult = core::types::Result<WorkerPathResolver, WorkerPathError>;
    using ResolveResult = core::types::Result<core::render::ResolvedRenderRequest, WorkerPathError>;

    // 목적: Worker가 접근할 canonical source/output root 검증
    // 입력: configuration: 기존 directory인 source root와 output root
    // 출력: root-bound resolver 또는 root configuration 오류
    [[nodiscard]] static CreateResult create(const WorkerRootConfiguration& configuration);

    // 목적: wire request의 상대 경로를 Worker root 안의 local render request로 해석
    // 입력: payload: portable relative path와 processing value
    // 출력: canonical source/output path를 가진 request 또는 escape/value 오류
    [[nodiscard]] ResolveResult resolve(const RenderRequestPayload& payload) const;

    // 목적: 검증된 canonical source root 확인
    // 입력: 없음
    // 출력: resolver가 보유한 source root
    [[nodiscard]] const QString& sourceRoot() const noexcept;

    // 목적: 검증된 canonical output root 확인
    // 입력: 없음
    // 출력: resolver가 보유한 output root
    [[nodiscard]] const QString& outputRoot() const noexcept;

private:
    // 목적: 이미 검증된 canonical root로 resolver 구성
    // 입력: sourceRoot/outputRoot: 존재하는 canonical directory
    // 출력: root-bound resolver
    WorkerPathResolver(QString sourceRoot, QString outputRoot);

    QString m_sourceRoot;
    QString m_outputRoot;
};

}  // namespace flexraw::worker::runtime
