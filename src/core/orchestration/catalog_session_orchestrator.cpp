#include "catalog_session_orchestrator.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include <QByteArray>
#include <QFileInfo>
#include <QString>

#include "catalog_orchestrator.h"
#include "catalog_path.h"
#include "client_error_projection.h"
#include "editor_client.h"

namespace flexraw::core::orchestration
{
namespace
{

// 목적: QString을 byte 길이가 보존된 UTF-8 client string으로 변환
// 입력: value: normalized absolute Catalog path 또는 diagnostics text
// 출력: Qt-free UTF-8 string
[[nodiscard]] std::string toClientString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

// 목적: Qt-free UTF-8 path를 transitional Catalog operation path로 변환
// 입력: value: byte 길이가 보존된 UTF-8 path
// 출력: 같은 Unicode path를 가진 QString
[[nodiscard]] QString fromClientString(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// 목적: Catalog session state를 Qt-free immutable snapshot으로 투영
// 입력: state: current internal open 여부와 normalized absolute path
// 출력: closed이면 빈 path, open이면 normalized absolute UTF-8 path를 가진 snapshot
[[nodiscard]] client::CatalogSessionSnapshot toClientSnapshot(const CatalogSessionState& state)
{
    return {state.isOpen, state.isOpen ? toClientString(state.catalogPath) : std::string{}};
}

// 목적: Catalog session command의 즉시 실패 result 생성
// 입력: code: 공통 오류 분류, message: log와 diagnostics용 설명
// 출력: Qt-free CatalogSessionResult failure
[[nodiscard]] client::CatalogSessionResult makeFailure(types::ErrorCode code, QString message)
{
    return client::CatalogSessionResult::failure(toClientError({code, std::move(message)}));
}

// 목적: open mode의 file existence 전제조건을 session mutation 전에 검증
// 입력: catalogPath: normalized path, openMode: existing open 또는 new create 의도
// 출력: 전제조건 위반 시 client 오류, 통과하면 빈 값
[[nodiscard]] std::optional<client::ClientError> validateOpenIntent(const QString& catalogPath,
                                                                    client::CatalogOpenMode openMode)
{
    if (catalogPath.isEmpty())
    {
        return toClientError({types::ErrorCode::InvalidArgument, QStringLiteral("Catalog path must not be empty.")});
    }

    const QFileInfo catalogInfo(catalogPath);
    if (catalogInfo.exists() && catalogInfo.isDir())
    {
        return toClientError(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Catalog path must not be a directory.")});
    }

    switch (openMode)
    {
    case client::CatalogOpenMode::OpenExisting:
        if (!catalogInfo.exists())
        {
            return toClientError({types::ErrorCode::NotFound, QStringLiteral("Catalog file does not exist.")});
        }
        return std::nullopt;
    case client::CatalogOpenMode::CreateNew:
        if (catalogInfo.exists())
        {
            return toClientError({types::ErrorCode::Conflict, QStringLiteral("Catalog file already exists.")});
        }
        return std::nullopt;
    }
    return toClientError({types::ErrorCode::InvalidArgument, QStringLiteral("Catalog open mode is invalid.")});
}

// 목적: session 전환 전에 unsaved Editor state를 보존하고 clean selection만 정리
// 입력: editorClient: authoritative Editor snapshot과 clear command owner
// 출력: dirty/Adjustment 또는 clear 실패 시 client 오류, 전환 가능하면 빈 값
[[nodiscard]] std::optional<client::ClientError> prepareEditorForSessionChange(client::IEditorClient& editorClient)
{
    const client::EditorSnapshot editor = editorClient.editorSnapshot();
    if (editor.dirty || editor.adjustmentActive)
    {
        return client::ClientError{client::ClientErrorCode::Conflict,
                                   "Unsaved Editor state blocks the Catalog session transition."};
    }
    if (!editor.hasSelection)
    {
        return std::nullopt;
    }

    const client::EditorResult cleared = editorClient.clearEditorSelection();
    return cleared.hasError() ? std::optional<client::ClientError>{cleared.error()} : std::nullopt;
}

}  // namespace

// 목적: Catalog resource owner와 Editor state owner를 하나의 session transition use case로 조립
// 입력: catalogOrchestrator: database session owner, editorClient: dirty/selection authority
// 출력: 두 dependency lifetime 안에서 동작하는 CatalogSessionOrchestrator
CatalogSessionOrchestrator::CatalogSessionOrchestrator(CatalogOrchestrator& catalogOrchestrator,
                                                       client::IEditorClient& editorClient) noexcept
    : m_catalogOrchestrator(&catalogOrchestrator), m_editorClient(&editorClient)
{}

// 목적: 현재 active Catalog session을 immutable Qt-free snapshot으로 조회
// 입력: 없음
// 출력: open 여부와 open 상태에서만 채워지는 normalized absolute UTF-8 catalog path
client::CatalogSessionSnapshot CatalogSessionOrchestrator::catalogSnapshot() const
{
    return toClientSnapshot(m_catalogOrchestrator->state());
}

// 목적: 명시적 create/open 및 optional clean-session 교체 정책으로 Catalog 활성화
// 입력: command: UTF-8 path, create/open mode와 active session replacement 정책
// 출력: 열린 normalized absolute snapshot 또는 validation·permission·database·conflict 오류
client::CatalogSessionResult CatalogSessionOrchestrator::openCatalog(const client::OpenCatalogCommand& command)
{
    const QString requestedPath = catalog::normalizeCatalogPath(fromClientString(command.catalogPath));
    if (const std::optional<client::ClientError> error = validateOpenIntent(requestedPath, command.openMode);
        error.has_value())
    {
        return client::CatalogSessionResult::failure(*error);
    }

    const CatalogSessionState current = m_catalogOrchestrator->state();
    if (current.isOpen && catalog::catalogPathsReferToSameFile(current.catalogPath, requestedPath))
    {
        return client::CatalogSessionResult::success(toClientSnapshot(current));
    }
    if (current.isOpen && command.replacementPolicy != client::CatalogReplacementPolicy::ReplaceCurrent)
    {
        return makeFailure(types::ErrorCode::Conflict, QStringLiteral("A different Catalog session is already open."));
    }
    if (const std::optional<client::ClientError> error = prepareEditorForSessionChange(*m_editorClient);
        error.has_value())
    {
        return client::CatalogSessionResult::failure(*error);
    }

    const QString previousPath = current.catalogPath;
    if (current.isOpen)
    {
        (void)m_catalogOrchestrator->closeCatalog();
    }

    const CatalogSessionResult opened = m_catalogOrchestrator->openCatalog(requestedPath);
    if (!opened.hasError())
    {
        return client::CatalogSessionResult::success(toClientSnapshot(opened.value()));
    }

    client::ClientError openError = toClientError(opened.error());
    if (current.isOpen)
    {
        const CatalogSessionResult restored = m_catalogOrchestrator->openCatalog(previousPath);
        if (restored.hasError())
        {
            openError.technicalMessage.append(" Previous Catalog restore also failed: ");
            openError.technicalMessage.append(toClientError(restored.error()).technicalMessage);
        }
    }
    return client::CatalogSessionResult::failure(std::move(openError));
}

// 목적: dirty Editor state를 보존하면서 clean Catalog session을 idempotent하게 종료
// 입력: 없음
// 출력: 닫힌 snapshot 또는 unsaved Editor conflict 오류
client::CatalogSessionResult CatalogSessionOrchestrator::closeCatalog()
{
    if (const std::optional<client::ClientError> error = prepareEditorForSessionChange(*m_editorClient);
        error.has_value())
    {
        return client::CatalogSessionResult::failure(*error);
    }
    return client::CatalogSessionResult::success(toClientSnapshot(m_catalogOrchestrator->closeCatalog()));
}

}  // namespace flexraw::core::orchestration
