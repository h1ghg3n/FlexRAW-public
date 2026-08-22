#pragma once

#include <QString>

namespace flexraw::core::catalog
{

// 목적: 존재 여부와 무관하게 source folder path를 비교 가능한 lexical path로 정규화
// 입력: folderPath: query 또는 source record에서 얻은 folder path
// 출력: separator와 중복 segment를 정리한 path, 유효하지 않으면 빈 문자열
[[nodiscard]] QString normalizeSourceFolderPath(const QString& folderPath);

// 목적: source file path에서 exact-folder query용 parent path 파생
// 입력: sourcePath: catalog에 저장할 source file locator
// 출력: 정규화된 parent path, parent를 파생할 수 없으면 빈 문자열
[[nodiscard]] QString sourceParentPath(const QString& sourcePath);

}  // namespace flexraw::core::catalog
