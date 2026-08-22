#pragma once

#include <QCoreApplication>
#include <QString>

namespace flexraw::ui::cli
{

struct CatalogImportCommand
{
    QString catalogPath;
    QString folderPath;
};

struct CatalogImportCommandParseResult
{
    bool recognized{false};
    bool valid{false};
    CatalogImportCommand command;
    QString errorMessage;
};

class CatalogCommandService final
{
    Q_DECLARE_TR_FUNCTIONS(CatalogCommandService)

public:
    // 목적: console 입력에서 catalog import command와 option을 분석
    // 입력: commandLine: 사용자가 입력한 전체 command 문자열
    // 출력: command 인식 여부, 검증 결과와 import 인자
    [[nodiscard]] static CatalogImportCommandParseResult parseImportCommand(const QString& commandLine);
};

}  // namespace flexraw::ui::cli
