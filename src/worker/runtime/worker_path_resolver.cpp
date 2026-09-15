#include "worker_path_resolver.h"

#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include "develop.h"
#include "export.h"

namespace flexraw::worker::runtime
{
namespace
{

// 목적: Worker path 오류를 지정 분류와 text로 생성
// 입력: code: path 오류 분류, message: 진단 text
// 출력: 구성된 WorkerPathError
[[nodiscard]] WorkerPathError makePathError(const WorkerPathErrorCode code, QString message)
{
    return {code, std::move(message)};
}

// 목적: Worker configuration root가 존재하는 directory인지 확인하고 canonical path 반환
// 입력: path: source 또는 output root 후보, label: 오류 메시지용 root 이름
// 출력: canonical directory path 또는 root 오류
[[nodiscard]] core::types::Result<QString, WorkerPathError> validateRoot(const QString& path, const QString& label)
{
    if (path.isEmpty() || path != path.trimmed())
    {
        return core::types::Result<QString, WorkerPathError>::failure(makePathError(
            WorkerPathErrorCode::InvalidRoot, label + QStringLiteral(" root is empty or contains outer whitespace.")));
    }

    const QFileInfo rootInfo(QDir::cleanPath(path));
    if (!rootInfo.exists() || !rootInfo.isDir())
    {
        return core::types::Result<QString, WorkerPathError>::failure(
            makePathError(WorkerPathErrorCode::InvalidRoot, label + QStringLiteral(" root is not a directory.")));
    }

    const QString canonicalPath = rootInfo.canonicalFilePath();
    if (canonicalPath.isEmpty())
    {
        return core::types::Result<QString, WorkerPathError>::failure(
            makePathError(WorkerPathErrorCode::InvalidRoot, label + QStringLiteral(" root cannot be canonicalized.")));
    }
    return core::types::Result<QString, WorkerPathError>::success(QDir::cleanPath(canonicalPath));
}

// 목적: wire path가 platform-independent canonical relative path인지 확인
// 입력: path: UTF-8 payload에서 복원한 source 또는 output path
// 출력: slash-separated normalized path 또는 path contract 오류
[[nodiscard]] core::types::Result<QString, WorkerPathError> validateRelativePath(const QString& path)
{
    if (path.isEmpty() || path.contains(QChar::Null) || path.contains(QLatin1Char('\\')) ||
        path.contains(QLatin1Char(':')) || QDir::isAbsolutePath(path))
    {
        return core::types::Result<QString, WorkerPathError>::failure(
            makePathError(WorkerPathErrorCode::InvalidRelativePath,
                          QStringLiteral("Worker path must be a non-empty portable relative path.")));
    }

    for (const QChar character : path)
    {
        if (character.unicode() < 0x20U)
        {
            return core::types::Result<QString, WorkerPathError>::failure(makePathError(
                WorkerPathErrorCode::InvalidRelativePath, QStringLiteral("Worker path contains a control character.")));
        }
    }

    const QStringList segments = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString& segment : segments)
    {
        if (segment.isEmpty() || segment == QStringLiteral(".") || segment == QStringLiteral(".."))
        {
            return core::types::Result<QString, WorkerPathError>::failure(
                makePathError(WorkerPathErrorCode::InvalidRelativePath,
                              QStringLiteral("Worker path must not contain empty, dot, or parent segments.")));
        }
    }

    const QString normalized = QDir::cleanPath(path);
    if (normalized != path)
    {
        return core::types::Result<QString, WorkerPathError>::failure(
            makePathError(WorkerPathErrorCode::InvalidRelativePath,
                          QStringLiteral("Worker path is not in canonical relative form.")));
    }
    return core::types::Result<QString, WorkerPathError>::success(normalized);
}

// 목적: canonical candidate가 canonical root 자신 또는 그 descendant인지 확인
// 입력: rootPath: canonical directory, candidatePath: canonical file/directory
// 출력: relative traversal 없이 root 안에 있으면 true
[[nodiscard]] bool isInsideRoot(const QString& rootPath, const QString& candidatePath)
{
    const QString relative = QDir(rootPath).relativeFilePath(candidatePath);
    return !QDir::isAbsolutePath(relative) && relative != QStringLiteral("..") &&
           !relative.startsWith(QStringLiteral("../"));
}

// 목적: source relative path를 존재하는 root-contained regular file로 해석
// 입력: rootPath: canonical source root, relativePath: 검증된 portable path
// 출력: canonical source file 또는 not-found/escape 오류
[[nodiscard]] core::types::Result<QString, WorkerPathError> resolveSourcePath(const QString& rootPath,
                                                                              const QString& relativePath)
{
    const QFileInfo sourceInfo(QDir(rootPath).filePath(relativePath));
    if (!sourceInfo.exists() || !sourceInfo.isFile())
    {
        return core::types::Result<QString, WorkerPathError>::failure(
            makePathError(WorkerPathErrorCode::SourceNotFound, QStringLiteral("Worker source file does not exist.")));
    }

    const QString canonicalPath = sourceInfo.canonicalFilePath();
    if (canonicalPath.isEmpty() || !isInsideRoot(rootPath, canonicalPath))
    {
        return core::types::Result<QString, WorkerPathError>::failure(makePathError(
            WorkerPathErrorCode::SourceOutsideRoot, QStringLiteral("Worker source path escapes the configured root.")));
    }
    return core::types::Result<QString, WorkerPathError>::success(canonicalPath);
}

// 목적: output relative path를 root-contained existing parent와 file path로 해석
// 입력: rootPath: canonical output root, relativePath: 검증된 portable path
// 출력: canonical parent 아래 output path 또는 parent/escape 오류
[[nodiscard]] core::types::Result<QString, WorkerPathError> resolveOutputPath(const QString& rootPath,
                                                                              const QString& relativePath)
{
    const QFileInfo requestedInfo(QDir(rootPath).filePath(relativePath));
    const QFileInfo parentInfo(requestedInfo.absolutePath());
    if (!parentInfo.exists() || !parentInfo.isDir())
    {
        return core::types::Result<QString, WorkerPathError>::failure(
            makePathError(WorkerPathErrorCode::OutputParentNotFound,
                          QStringLiteral("Worker output parent directory does not exist.")));
    }

    const QString canonicalParent = parentInfo.canonicalFilePath();
    if (canonicalParent.isEmpty() || !isInsideRoot(rootPath, canonicalParent))
    {
        return core::types::Result<QString, WorkerPathError>::failure(makePathError(
            WorkerPathErrorCode::OutputOutsideRoot, QStringLiteral("Worker output path escapes the configured root.")));
    }
    if (requestedInfo.exists() && (!requestedInfo.isFile() || requestedInfo.isSymLink()))
    {
        return core::types::Result<QString, WorkerPathError>::failure(
            makePathError(WorkerPathErrorCode::OutputOutsideRoot,
                          QStringLiteral("Worker output must be a regular file and must not be a symbolic link.")));
    }

    return core::types::Result<QString, WorkerPathError>::success(
        QDir(canonicalParent).filePath(requestedInfo.fileName()));
}

}  // namespace

// 목적: 이미 검증된 canonical root로 resolver 구성
// 입력: sourceRoot/outputRoot: 존재하는 canonical directory
// 출력: root-bound resolver
WorkerPathResolver::WorkerPathResolver(QString sourceRoot, QString outputRoot)
    : m_sourceRoot(std::move(sourceRoot)), m_outputRoot(std::move(outputRoot))
{}

// 목적: Worker가 접근할 canonical source/output root 검증
// 입력: configuration: 기존 directory인 source root와 output root
// 출력: root-bound resolver 또는 root configuration 오류
WorkerPathResolver::CreateResult WorkerPathResolver::create(const WorkerRootConfiguration& configuration)
{
    core::types::Result<QString, WorkerPathError> sourceRoot =
        validateRoot(configuration.sourceRoot, QStringLiteral("Source"));
    if (sourceRoot.hasError())
    {
        return CreateResult::failure(sourceRoot.error());
    }

    core::types::Result<QString, WorkerPathError> outputRoot =
        validateRoot(configuration.outputRoot, QStringLiteral("Output"));
    if (outputRoot.hasError())
    {
        return CreateResult::failure(outputRoot.error());
    }
    return CreateResult::success(WorkerPathResolver(sourceRoot.value(), outputRoot.value()));
}

// 목적: Runtime request의 상대 경로를 Worker root 안의 local render request로 해석
// 입력: payload: transport에서 분리된 relative path와 processing value
// 출력: canonical source/output path를 가진 request 또는 escape/value 오류
WorkerPathResolver::ResolveResult WorkerPathResolver::resolve(const RenderWorkerRequest& payload) const
{
    const core::develop::DevelopParamsValidationResult developValidation =
        core::develop::validateDevelopParams(payload.developParams);
    if (developValidation.hasError())
    {
        return ResolveResult::failure(
            makePathError(WorkerPathErrorCode::InvalidRequestValue, developValidation.error().message));
    }
    const core::export_::RasterExportResult outputValidation =
        core::export_::validateRasterExportOptions(payload.outputOptions);
    if (outputValidation.hasError())
    {
        return ResolveResult::failure(
            makePathError(WorkerPathErrorCode::InvalidRequestValue, outputValidation.error().message));
    }

    const core::types::Result<QString, WorkerPathError> sourceRelative =
        validateRelativePath(payload.sourceRelativePath);
    if (sourceRelative.hasError())
    {
        return ResolveResult::failure(sourceRelative.error());
    }
    const core::types::Result<QString, WorkerPathError> outputRelative =
        validateRelativePath(payload.outputRelativePath);
    if (outputRelative.hasError())
    {
        return ResolveResult::failure(outputRelative.error());
    }

    const core::types::Result<QString, WorkerPathError> sourcePath =
        resolveSourcePath(m_sourceRoot, sourceRelative.value());
    if (sourcePath.hasError())
    {
        return ResolveResult::failure(sourcePath.error());
    }
    const core::types::Result<QString, WorkerPathError> outputPath =
        resolveOutputPath(m_outputRoot, outputRelative.value());
    if (outputPath.hasError())
    {
        return ResolveResult::failure(outputPath.error());
    }

    return ResolveResult::success(
        {sourcePath.value(), outputPath.value(), developValidation.value(), payload.outputOptions});
}

// 목적: 검증된 canonical source root 확인
// 입력: 없음
// 출력: resolver가 보유한 source root
const QString& WorkerPathResolver::sourceRoot() const noexcept
{
    return m_sourceRoot;
}

// 목적: 검증된 canonical output root 확인
// 입력: 없음
// 출력: resolver가 보유한 output root
const QString& WorkerPathResolver::outputRoot() const noexcept
{
    return m_outputRoot;
}

}  // namespace flexraw::worker::runtime
