#include "remote_render_request_mapper.h"

#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace flexraw::worker::client
{
namespace
{

// 목적: remote path mapping 오류를 지정 분류와 text로 생성
// 입력: code: 오류 분류, message: 진단 text
// 출력: 구성된 RemoteRenderPathError
[[nodiscard]] RemoteRenderPathError makePathError(const RemoteRenderPathErrorCode code, QString message)
{
    return {code, std::move(message)};
}

// 목적: local mapping root가 기존 absolute directory인지 확인하고 canonical path 반환
// 입력: path: source/output root 후보, label: 오류 메시지용 root 이름
// 출력: canonical root 또는 configuration 오류
[[nodiscard]] core::types::Result<QString, RemoteRenderPathError> validateRoot(const QString& path,
                                                                               const QString& label)
{
    const QString cleanedPath = QDir::cleanPath(path);
    if (path.trimmed().isEmpty() || !QDir::isAbsolutePath(cleanedPath))
    {
        return core::types::Result<QString, RemoteRenderPathError>::failure(
            makePathError(RemoteRenderPathErrorCode::InvalidRoot,
                          label + QStringLiteral(" local root must be an absolute directory.")));
    }

    const QFileInfo rootInfo(cleanedPath);
    if (!rootInfo.exists() || !rootInfo.isDir())
    {
        return core::types::Result<QString, RemoteRenderPathError>::failure(makePathError(
            RemoteRenderPathErrorCode::InvalidRoot, label + QStringLiteral(" local root is not a directory.")));
    }

    const QString canonicalPath = rootInfo.canonicalFilePath();
    if (canonicalPath.isEmpty())
    {
        return core::types::Result<QString, RemoteRenderPathError>::failure(makePathError(
            RemoteRenderPathErrorCode::InvalidRoot, label + QStringLiteral(" local root cannot be canonicalized.")));
    }
    return core::types::Result<QString, RemoteRenderPathError>::success(QDir::cleanPath(canonicalPath));
}

// 목적: canonical candidate가 canonical root 자신 또는 descendant인지 확인
// 입력: rootPath: canonical directory, candidatePath: canonical file/directory
// 출력: relative traversal 없이 root 안에 있으면 true
[[nodiscard]] bool isInsideRoot(const QString& rootPath, const QString& candidatePath)
{
    const QString relative = QDir(rootPath).relativeFilePath(candidatePath);
    return !QDir::isAbsolutePath(relative) && relative != QStringLiteral("..") &&
           !relative.startsWith(QStringLiteral("../"));
}

// 목적: canonical local path를 Worker protocol의 portable relative path로 변환
// 입력: rootPath: canonical local root, candidatePath: root 안의 canonical candidate
// 출력: slash-separated relative path 또는 protocol portability 오류
[[nodiscard]] core::types::Result<QString, RemoteRenderPathError> makePortableRelativePath(const QString& rootPath,
                                                                                           const QString& candidatePath)
{
    const QString relative = QDir::fromNativeSeparators(QDir(rootPath).relativeFilePath(candidatePath));
    if (relative.isEmpty() || relative.contains(QChar::Null) || relative.contains(QLatin1Char('\\')) ||
        relative.contains(QLatin1Char(':')) || QDir::isAbsolutePath(relative))
    {
        return core::types::Result<QString, RemoteRenderPathError>::failure(
            makePathError(RemoteRenderPathErrorCode::NonPortablePath,
                          QStringLiteral("Mapped Worker path is not a portable relative path.")));
    }

    for (const QChar character : relative)
    {
        if (character.unicode() < 0x20U)
        {
            return core::types::Result<QString, RemoteRenderPathError>::failure(
                makePathError(RemoteRenderPathErrorCode::NonPortablePath,
                              QStringLiteral("Mapped Worker path contains a control character.")));
        }
    }

    const QStringList segments = relative.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString& segment : segments)
    {
        if (segment.isEmpty() || segment == QStringLiteral(".") || segment == QStringLiteral(".."))
        {
            return core::types::Result<QString, RemoteRenderPathError>::failure(
                makePathError(RemoteRenderPathErrorCode::NonPortablePath,
                              QStringLiteral("Mapped Worker path contains an invalid segment.")));
        }
    }

    if (QDir::cleanPath(relative) != relative)
    {
        return core::types::Result<QString, RemoteRenderPathError>::failure(
            makePathError(RemoteRenderPathErrorCode::NonPortablePath,
                          QStringLiteral("Mapped Worker path is not in canonical relative form.")));
    }
    return core::types::Result<QString, RemoteRenderPathError>::success(relative);
}

// 목적: local source file을 source root 기준 portable relative path로 변환
// 입력: rootPath: canonical source root, sourcePath: Desktop absolute source path
// 출력: source relative path 또는 not-found/root escape 오류
[[nodiscard]] core::types::Result<QString, RemoteRenderPathError> mapSourcePath(const QString& rootPath,
                                                                                const QString& sourcePath)
{
    const QString cleanedPath = QDir::cleanPath(sourcePath);
    if (sourcePath.trimmed().isEmpty() || !QDir::isAbsolutePath(cleanedPath))
    {
        return core::types::Result<QString, RemoteRenderPathError>::failure(makePathError(
            RemoteRenderPathErrorCode::InvalidPath, QStringLiteral("Remote render source path must be absolute.")));
    }

    const QFileInfo sourceInfo(cleanedPath);
    if (!sourceInfo.exists() || !sourceInfo.isFile())
    {
        return core::types::Result<QString, RemoteRenderPathError>::failure(makePathError(
            RemoteRenderPathErrorCode::SourceNotFound, QStringLiteral("Remote render source file does not exist.")));
    }

    const QString canonicalPath = sourceInfo.canonicalFilePath();
    if (canonicalPath.isEmpty() || !isInsideRoot(rootPath, canonicalPath))
    {
        return core::types::Result<QString, RemoteRenderPathError>::failure(
            makePathError(RemoteRenderPathErrorCode::SourceOutsideRoot,
                          QStringLiteral("Remote render source path is outside the configured local root.")));
    }
    return makePortableRelativePath(rootPath, canonicalPath);
}

// 목적: local output file을 output root 기준 portable relative path로 변환
// 입력: rootPath: canonical output root, outputPath: Desktop absolute output path
// 출력: output relative path 또는 parent/root escape 오류
[[nodiscard]] core::types::Result<QString, RemoteRenderPathError> mapOutputPath(const QString& rootPath,
                                                                                const QString& outputPath)
{
    const QString cleanedPath = QDir::cleanPath(outputPath);
    if (outputPath.trimmed().isEmpty() || !QDir::isAbsolutePath(cleanedPath))
    {
        return core::types::Result<QString, RemoteRenderPathError>::failure(makePathError(
            RemoteRenderPathErrorCode::InvalidPath, QStringLiteral("Remote render output path must be absolute.")));
    }

    const QFileInfo outputInfo(cleanedPath);
    if (outputInfo.fileName().isEmpty())
    {
        return core::types::Result<QString, RemoteRenderPathError>::failure(makePathError(
            RemoteRenderPathErrorCode::InvalidPath, QStringLiteral("Remote render output path must name a file.")));
    }

    const QFileInfo parentInfo(outputInfo.absolutePath());
    if (!parentInfo.exists() || !parentInfo.isDir())
    {
        return core::types::Result<QString, RemoteRenderPathError>::failure(
            makePathError(RemoteRenderPathErrorCode::OutputParentNotFound,
                          QStringLiteral("Remote render output parent directory does not exist.")));
    }

    const QString canonicalParent = parentInfo.canonicalFilePath();
    if (canonicalParent.isEmpty() || !isInsideRoot(rootPath, canonicalParent))
    {
        return core::types::Result<QString, RemoteRenderPathError>::failure(
            makePathError(RemoteRenderPathErrorCode::OutputOutsideRoot,
                          QStringLiteral("Remote render output path is outside the configured local root.")));
    }
    if (outputInfo.exists() && (!outputInfo.isFile() || outputInfo.isSymLink()))
    {
        return core::types::Result<QString, RemoteRenderPathError>::failure(
            makePathError(RemoteRenderPathErrorCode::OutputOutsideRoot,
                          QStringLiteral("Remote render output must be a regular file and not a symbolic link.")));
    }

    return makePortableRelativePath(rootPath, QDir(canonicalParent).filePath(outputInfo.fileName()));
}

}  // namespace

// 목적: 검증된 canonical local root로 mapper 구성
// 입력: sourceRoot/outputRoot: Worker root와 동일 relative layout을 갖는 local root
// 출력: request mapping에 사용할 mapper
RemoteRenderRequestMapper::RemoteRenderRequestMapper(QString sourceRoot, QString outputRoot)
    : m_sourceRoot(std::move(sourceRoot)), m_outputRoot(std::move(outputRoot))
{}

// 목적: Desktop local root와 Worker root의 relative-path 대응 경계 생성
// 입력: roots: Worker source/output root에 각각 대응하는 기존 absolute local directory
// 출력: canonical local root를 보유한 mapper 또는 configuration 오류
RemoteRenderRequestMapper::CreateResult RemoteRenderRequestMapper::create(const RemoteRenderLocalRoots& roots)
{
    core::types::Result<QString, RemoteRenderPathError> sourceRoot =
        validateRoot(roots.sourceRoot, QStringLiteral("Source"));
    if (sourceRoot.hasError())
    {
        return CreateResult::failure(sourceRoot.error());
    }

    core::types::Result<QString, RemoteRenderPathError> outputRoot =
        validateRoot(roots.outputRoot, QStringLiteral("Output"));
    if (outputRoot.hasError())
    {
        return CreateResult::failure(outputRoot.error());
    }
    return CreateResult::success(RemoteRenderRequestMapper(sourceRoot.value(), outputRoot.value()));
}

// 목적: local absolute render request를 Worker wire용 root-relative request로 변환
// 입력: request: 기존 source file과 output parent를 사용하는 resolved render request
// 출력: portable relative path를 가진 remote request 또는 path mapping 오류
RemoteRenderRequestMapper::MapResult RemoteRenderRequestMapper::map(
    const core::render::ResolvedRenderRequest& request) const
{
    core::types::Result<QString, RemoteRenderPathError> sourcePath = mapSourcePath(m_sourceRoot, request.sourcePath);
    if (sourcePath.hasError())
    {
        return MapResult::failure(sourcePath.error());
    }

    core::types::Result<QString, RemoteRenderPathError> outputPath = mapOutputPath(m_outputRoot, request.outputPath);
    if (outputPath.hasError())
    {
        return MapResult::failure(outputPath.error());
    }

    return MapResult::success({sourcePath.value(), outputPath.value(), request.developParams, request.outputOptions});
}

}  // namespace flexraw::worker::client
