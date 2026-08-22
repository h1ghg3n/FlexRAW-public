#include "catalog_command_service.h"

#include <utility>

#include <QChar>
#include <QStringList>

namespace flexraw::ui::cli
{
namespace
{

struct TokenizeResult
{
    bool valid{false};
    QStringList tokens;
};

// 목적: double quote로 묶인 Windows와 UNC 경로를 보존하며 command token 분리
// 입력: commandLine: 사용자가 입력한 전체 command 문자열
// 출력: quote 검증 상태와 분리된 token 목록
[[nodiscard]] TokenizeResult tokenizeCommand(const QString& commandLine)
{
    QStringList tokens;
    QString token;
    bool inQuotes = false;
    bool tokenStarted = false;

    for (const QChar character : commandLine)
    {
        if (character == QLatin1Char('"'))
        {
            inQuotes = !inQuotes;
            tokenStarted = true;
            continue;
        }

        if (character.isSpace() && !inQuotes)
        {
            if (tokenStarted)
            {
                tokens.append(token);
                token.clear();
                tokenStarted = false;
            }
            continue;
        }

        token.append(character);
        tokenStarted = true;
    }

    if (inQuotes)
    {
        return {};
    }

    if (tokenStarted)
    {
        tokens.append(token);
    }

    return {true, tokens};
}

// 목적: 인식된 catalog command의 parsing 실패 결과 생성
// 입력: message: 사용자에게 표시할 검증 오류
// 출력: catalog command로 인식된 실패 결과
[[nodiscard]] CatalogImportCommandParseResult parseFailure(QString message)
{
    CatalogImportCommandParseResult result;
    result.recognized = true;
    result.errorMessage = std::move(message);
    return result;
}

}  // namespace

CatalogImportCommandParseResult CatalogCommandService::parseImportCommand(const QString& commandLine)
{
    const QString trimmedCommand = commandLine.trimmed();
    qsizetype firstSeparator = 0;

    while (firstSeparator < trimmedCommand.size() && !trimmedCommand.at(firstSeparator).isSpace())
    {
        ++firstSeparator;
    }

    const QString commandName = trimmedCommand.first(firstSeparator);

    if (commandName.compare(QStringLiteral("catalog"), Qt::CaseInsensitive) != 0)
    {
        return {};
    }

    const TokenizeResult tokenized = tokenizeCommand(commandLine);

    if (!tokenized.valid)
    {
        return parseFailure(tr("Catalog command contains an unterminated quote."));
    }

    const QStringList& tokens = tokenized.tokens;

    if (tokens.size() < 2 || tokens.at(1).compare(QStringLiteral("import"), Qt::CaseInsensitive) != 0)
    {
        return parseFailure(tr("Usage: catalog import --catalog \"<catalog-file>\" --folder \"<photo-folder>\""));
    }

    CatalogImportCommand command;

    for (qsizetype index = 2; index < tokens.size(); index += 2)
    {
        const QString& option = tokens.at(index);

        if (index + 1 >= tokens.size())
        {
            return parseFailure(tr("Missing value for option: %1").arg(option));
        }

        const QString& value = tokens.at(index + 1);

        if (option.compare(QStringLiteral("--catalog"), Qt::CaseInsensitive) == 0)
        {
            if (!command.catalogPath.isEmpty())
            {
                return parseFailure(tr("Option --catalog may only be specified once."));
            }
            command.catalogPath = value;
            continue;
        }

        if (option.compare(QStringLiteral("--folder"), Qt::CaseInsensitive) == 0)
        {
            if (!command.folderPath.isEmpty())
            {
                return parseFailure(tr("Option --folder may only be specified once."));
            }
            command.folderPath = value;
            continue;
        }

        return parseFailure(tr("Unknown catalog import option: %1").arg(option));
    }

    if (command.catalogPath.isEmpty() || command.folderPath.isEmpty())
    {
        return parseFailure(tr("Both --catalog and --folder are required."));
    }

    CatalogImportCommandParseResult result;
    result.recognized = true;
    result.valid = true;
    result.command = std::move(command);
    return result;
}

}  // namespace flexraw::ui::cli
