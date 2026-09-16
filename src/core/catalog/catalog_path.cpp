#include "catalog_path.h"

#include <filesystem>
#include <system_error>

#include <QDir>
#include <QFileInfo>

namespace flexraw::core::catalog
{

// 목적: Catalog locator를 비교와 database open에 사용할 절대 경로로 정규화
// 입력: catalogPath: caller 또는 settings가 전달한 Catalog 경로
// 출력: outer whitespace가 있거나 비어 있으면 빈 문자열, 아니면 clean absolute path
QString normalizeCatalogPath(const QString& catalogPath)
{
    if (catalogPath.isEmpty() || catalogPath != catalogPath.trimmed())
    {
        return {};
    }
    return QDir::cleanPath(QFileInfo(catalogPath).absoluteFilePath());
}

// 목적: 두 기존 Catalog locator가 같은 filesystem entry를 가리키는지 판정
// 입력: firstPath/secondPath: 비교할 Catalog 경로
// 출력: clean absolute path가 같거나 기존 file identity가 같으면 true
bool catalogPathsReferToSameFile(const QString& firstPath, const QString& secondPath)
{
    const QString normalizedFirst = normalizeCatalogPath(firstPath);
    const QString normalizedSecond = normalizeCatalogPath(secondPath);
    if (normalizedFirst.isEmpty() || normalizedSecond.isEmpty())
    {
        return false;
    }
    if (normalizedFirst == normalizedSecond)
    {
        return true;
    }

    const QFileInfo firstInfo(normalizedFirst);
    const QFileInfo secondInfo(normalizedSecond);
    if (!firstInfo.exists() || !secondInfo.exists())
    {
        return false;
    }

    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(
        firstInfo.filesystemAbsoluteFilePath(), secondInfo.filesystemAbsoluteFilePath(), error);
    return !error && equivalent;
}

}  // namespace flexraw::core::catalog
