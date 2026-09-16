#include <cstddef>
#include <exception>
#include <iostream>
#include <string>

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>

#include "mcp_application_context.h"
#include "mcp_catalog_tools.h"
#include "mcp_editor_tools.h"
#include "mcp_project_tools.h"
#include "mcp_server.h"
#include "mcp_source_resolution_tools.h"

namespace
{

// 목적: QString process argument를 byte 길이가 보존된 UTF-8 string으로 변환
// 입력: value: Qt command-line text
// 출력: 같은 Unicode text를 가진 UTF-8 bytes
[[nodiscard]] std::string toStdString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

// 목적: 기존 Catalog를 명시적으로 열고 local STDIO MCP server 실행
// 입력: --catalog <path>를 포함한 process arguments
// 출력: 정상 EOF는 0, startup 또는 transport 실패는 non-zero process code
int runMcpProcess(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("flexraw-mcp"));
    QCoreApplication::setApplicationVersion(QStringLiteral(FLEXRAW_VERSION_STRING));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Flexraw local STDIO MCP server"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption catalogOption(QStringList{QStringLiteral("c"), QStringLiteral("catalog")},
                                           QStringLiteral("Open an existing Flexraw Catalog."),
                                           QStringLiteral("path"));
    const QCommandLineOption allowWriteOption(QStringLiteral("allow-write"),
                                              QStringLiteral("Enable explicitly approved write-capable tools."));
    parser.addOption(catalogOption);
    parser.addOption(allowWriteOption);
    parser.process(application);
    if (!parser.isSet(catalogOption) || parser.value(catalogOption).isEmpty())
    {
        std::cerr << "flexraw-mcp requires --catalog <path>\n";
        return 64;
    }

    flexraw::app::mcp::McpApplicationContext context;
    const flexraw::core::client::CatalogSessionResult opened =
        context.openExistingCatalog(toStdString(parser.value(catalogOption)));
    if (opened.hasError())
    {
        std::cerr << "Unable to open the Flexraw Catalog: " << opened.error().technicalMessage << '\n';
        return 66;
    }

    flexraw::mcp::McpCatalogTools catalogTools(context.catalogPhotoClient());
    flexraw::mcp::McpProjectTools projectTools(context.catalogProjectClient(), parser.isSet(allowWriteOption));
    flexraw::mcp::McpEditorTools editorTools(context.editorClient(), parser.isSet(allowWriteOption));
    flexraw::mcp::McpSourceResolutionTools sourceTools(
        context.sourceResolutionClient(), context.sourceResolutionEventSource(), parser.isSet(allowWriteOption));
    if (!sourceTools.ready())
    {
        std::cerr << "Unable to subscribe to Source Resolution events: " << sourceTools.startupDiagnostic() << '\n';
        return 70;
    }
    flexraw::mcp::McpServer server(
        catalogTools, projectTools, editorTools, sourceTools, FLEXRAW_VERSION_STRING, std::cin, std::cout, std::cerr);
    return server.run();
}

}  // namespace

// 목적: MCP Composition 생성 예외를 stderr diagnostic과 software failure code로 정규화
// 입력: argc: process argument 수, argv: process argument 값
// 출력: 정상 MCP code 또는 예상하지 못한 startup 실패 70
int main(int argc, char* argv[])
{
    try
    {
        return runMcpProcess(argc, argv);
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Unable to start flexraw-mcp: " << exception.what() << '\n';
    }
    catch (...)
    {
        std::cerr << "Unable to start flexraw-mcp: unknown exception\n";
    }

    return 70;
}
