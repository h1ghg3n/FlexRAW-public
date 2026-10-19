#include "source_path.h"

#include <QDir>

namespace flexraw::core::catalog
{
namespace
{

// 목적: 저장된 source locator를 host OS와 무관한 separator와 root 규칙으로 정규화
// 입력: path: 외부 공백 검증을 마친 lexical source path
// 출력: drive root와 UNC prefix를 보존한 정규화 path
QString normalizeLexicalSourcePath(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const bool driveRoot =
        path.size() >= 3 && path[0].isLetter() && path[1] == QLatin1Char(':') && path[2] == QLatin1Char('/');
    if (driveRoot)
    {
        return path.left(2) + QDir::cleanPath(path.mid(2));
    }
    const bool uncRoot = path.startsWith(QStringLiteral("//")) && !path.startsWith(QStringLiteral("///"));
    QString normalized = QDir::cleanPath(path);
    if (uncRoot && !normalized.startsWith(QStringLiteral("//")))
    {
        normalized.prepend(QLatin1Char('/'));
    }
    return normalized;
}

}  // namespace

// 목적: 존재 여부와 무관하게 source folder path를 비교 가능한 lexical path로 정규화
// 입력: folderPath: query 또는 source record에서 얻은 folder path
// 출력: separator와 중복 segment를 정리한 path, 유효하지 않으면 빈 문자열
QString normalizeSourceFolderPath(const QString& folderPath)
{
    if (folderPath.isEmpty() || folderPath != folderPath.trimmed())
    {
        return {};
    }
    const QString normalized = normalizeLexicalSourcePath(folderPath);
    return normalized == QStringLiteral(".") ? QString{} : normalized;
}

// 목적: source file path에서 exact-folder query용 parent path 파생
// 입력: sourcePath: catalog에 저장할 source file locator
// 출력: 정규화된 parent path, parent를 파생할 수 없으면 빈 문자열
QString sourceParentPath(const QString& sourcePath)
{
    if (sourcePath.isEmpty() || sourcePath != sourcePath.trimmed())
    {
        return {};
    }
    const QString normalizedSource = normalizeLexicalSourcePath(sourcePath);
    if (normalizedSource.isEmpty() || normalizedSource == QStringLiteral("."))
    {
        return {};
    }

    const qsizetype separator = normalizedSource.lastIndexOf(QLatin1Char('/'));
    if (separator < 0)
    {
        return {};
    }
    const bool rootParent = separator == 0 || (separator == 2 && normalizedSource[1] == QLatin1Char(':'));
    return normalizeSourceFolderPath(normalizedSource.left(separator + (rootParent ? 1 : 0)));
}

}  // namespace flexraw::core::catalog
