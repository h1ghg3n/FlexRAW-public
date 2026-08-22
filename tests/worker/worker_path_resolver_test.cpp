#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "worker_path_resolver.h"

namespace flexraw::worker::runtime
{
namespace
{

// 목적: path resolver test용 기존 source file 생성
// 입력: path: 생성할 file path
// 출력: file 생성 성공 여부
[[nodiscard]] bool createFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        return false;
    }
    return file.write("raw-fixture") == 11;
}

// 목적: QString path를 C++20 filesystem의 UTF-8 path로 변환
// 입력: path: Qt Unicode path
// 출력: platform filesystem path
[[nodiscard]] std::filesystem::path toFilesystemPath(const QString& path)
{
    const QByteArray utf8 = path.toUtf8();
    const auto* const first = reinterpret_cast<const char8_t*>(utf8.constData());
    return std::filesystem::path(std::u8string(first, first + utf8.size()));
}

// 목적: 임시 source/output root를 사용하는 resolver 생성
// 입력: sourceRoot/outputRoot: 유효한 temporary directory
// 출력: create 성공을 assertion한 resolver
[[nodiscard]] WorkerPathResolver makeResolver(const QTemporaryDir& sourceRoot, const QTemporaryDir& outputRoot)
{
    WorkerPathResolver::CreateResult result = WorkerPathResolver::create({sourceRoot.path(), outputRoot.path()});
    if (result.hasError())
    {
        ADD_FAILURE() << result.error().message.toStdString();
        throw std::runtime_error("Unable to create WorkerPathResolver test fixture.");
    }
    return result.value();
}

// 목적: resolve 실패와 예상 WorkerPathErrorCode를 안전하게 검증
// 입력: result: resolver 반환값, expectedCode: 예상 path 오류 분류
// 출력: Google Test assertion 결과
void expectPathError(const WorkerPathResolver::ResolveResult& result, const WorkerPathErrorCode expectedCode)
{
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(expectedCode, result.error().code);
}

TEST(WorkerPathResolverTest, ResolvesExistingSourceAndOutputParentInsideRoots)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    ASSERT_TRUE(QDir(sourceRoot.path()).mkpath(QStringLiteral("originals")));
    ASSERT_TRUE(QDir(outputRoot.path()).mkpath(QStringLiteral("exports")));
    ASSERT_TRUE(createFile(QDir(sourceRoot.path()).filePath(QStringLiteral("originals/input.CR3"))));
    const WorkerPathResolver resolver = makeResolver(sourceRoot, outputRoot);
    RenderRequestPayload payload;
    payload.sourceRelativePath = QStringLiteral("originals/input.CR3");
    payload.outputRelativePath = QStringLiteral("exports/output.jpg");

    const WorkerPathResolver::ResolveResult result = resolver.resolve(payload);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(QFileInfo(QDir(sourceRoot.path()).filePath(payload.sourceRelativePath)).canonicalFilePath(),
              result.value().sourcePath);
    EXPECT_EQ(QDir(QFileInfo(QDir(outputRoot.path()).filePath(QStringLiteral("exports"))).canonicalFilePath())
                  .filePath(QStringLiteral("output.jpg")),
              result.value().outputPath);
}

// 목적: Unicode 상대 경로가 Worker root 안에서 canonical path로 유지되는지 검증
// 입력: 한글 directory/file name을 가진 source와 output 상대 경로
// 출력: source/output 모두 손실 없이 local filesystem path로 해석됨
TEST(WorkerPathResolverTest, ResolvesUnicodePathsInsideRoots)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    ASSERT_TRUE(QDir(sourceRoot.path()).mkpath(QStringLiteral("원본")));
    ASSERT_TRUE(QDir(outputRoot.path()).mkpath(QStringLiteral("결과")));
    ASSERT_TRUE(createFile(QDir(sourceRoot.path()).filePath(QStringLiteral("원본/사진.ARW"))));
    const WorkerPathResolver resolver = makeResolver(sourceRoot, outputRoot);
    RenderRequestPayload payload;
    payload.sourceRelativePath = QStringLiteral("원본/사진.ARW");
    payload.outputRelativePath = QStringLiteral("결과/완성.jpg");

    const WorkerPathResolver::ResolveResult result = resolver.resolve(payload);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(QFileInfo(QDir(sourceRoot.path()).filePath(payload.sourceRelativePath)).canonicalFilePath(),
              result.value().sourcePath);
    EXPECT_EQ(QDir(QFileInfo(QDir(outputRoot.path()).filePath(QStringLiteral("결과"))).canonicalFilePath())
                  .filePath(QStringLiteral("완성.jpg")),
              result.value().outputPath);
}

TEST(WorkerPathResolverTest, RejectsAbsoluteParentAndNonPortablePaths)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    const WorkerPathResolver resolver = makeResolver(sourceRoot, outputRoot);
    RenderRequestPayload payload;
    payload.outputRelativePath = QStringLiteral("output.jpg");

    payload.sourceRelativePath = QDir(sourceRoot.path()).filePath(QStringLiteral("input.CR3"));
    expectPathError(resolver.resolve(payload), WorkerPathErrorCode::InvalidRelativePath);

    payload.sourceRelativePath = QStringLiteral("../input.CR3");
    expectPathError(resolver.resolve(payload), WorkerPathErrorCode::InvalidRelativePath);

    payload.sourceRelativePath = QStringLiteral("folder\\input.CR3");
    expectPathError(resolver.resolve(payload), WorkerPathErrorCode::InvalidRelativePath);

    payload.sourceRelativePath = QStringLiteral("C:/input.CR3");
    expectPathError(resolver.resolve(payload), WorkerPathErrorCode::InvalidRelativePath);

    ASSERT_TRUE(createFile(QDir(sourceRoot.path()).filePath(QStringLiteral("input.CR3"))));
    payload.sourceRelativePath = QStringLiteral("input.CR3");
    payload.outputRelativePath = QStringLiteral("../output.jpg");
    expectPathError(resolver.resolve(payload), WorkerPathErrorCode::InvalidRelativePath);
}

TEST(WorkerPathResolverTest, RejectsMissingSourceAndOutputParent)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    const WorkerPathResolver resolver = makeResolver(sourceRoot, outputRoot);
    RenderRequestPayload payload;
    payload.sourceRelativePath = QStringLiteral("missing.CR3");
    payload.outputRelativePath = QStringLiteral("output.jpg");

    const WorkerPathResolver::ResolveResult missingSource = resolver.resolve(payload);
    ASSERT_TRUE(missingSource.hasError());
    EXPECT_EQ(WorkerPathErrorCode::SourceNotFound, missingSource.error().code);

    ASSERT_TRUE(createFile(QDir(sourceRoot.path()).filePath(QStringLiteral("input.CR3"))));
    payload.sourceRelativePath = QStringLiteral("input.CR3");
    payload.outputRelativePath = QStringLiteral("missing/output.jpg");
    const WorkerPathResolver::ResolveResult missingOutputParent = resolver.resolve(payload);
    ASSERT_TRUE(missingOutputParent.hasError());
    EXPECT_EQ(WorkerPathErrorCode::OutputParentNotFound, missingOutputParent.error().code);
}

TEST(WorkerPathResolverTest, RejectsSymlinkEscapeWhenPlatformAllowsProbe)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outsideRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outsideRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    ASSERT_TRUE(createFile(QDir(outsideRoot.path()).filePath(QStringLiteral("outside.CR3"))));

    std::error_code linkError;
    const std::filesystem::path linkPath = toFilesystemPath(QDir(sourceRoot.path()).filePath(QStringLiteral("linked")));
    const std::filesystem::path targetPath = toFilesystemPath(outsideRoot.path());
    std::filesystem::create_directory_symlink(targetPath, linkPath, linkError);
    if (linkError)
    {
        GTEST_SKIP() << "Directory symlink creation is unavailable: " << linkError.message();
    }

    const WorkerPathResolver resolver = makeResolver(sourceRoot, outputRoot);
    RenderRequestPayload payload;
    payload.sourceRelativePath = QStringLiteral("linked/outside.CR3");
    payload.outputRelativePath = QStringLiteral("output.jpg");

    const WorkerPathResolver::ResolveResult result = resolver.resolve(payload);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(WorkerPathErrorCode::SourceOutsideRoot, result.error().code);
}

// 목적: output parent symlink가 configured root 밖으로 artifact를 우회하지 못하는지 검증
// 입력: output root 내부에서 외부 directory를 가리키는 symlink와 그 아래 상대 경로
// 출력: OutputOutsideRoot 오류로 request 거절
TEST(WorkerPathResolverTest, RejectsOutputParentSymlinkEscapeWhenPlatformAllowsProbe)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    QTemporaryDir outsideRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    ASSERT_TRUE(outsideRoot.isValid());
    ASSERT_TRUE(createFile(QDir(sourceRoot.path()).filePath(QStringLiteral("input.CR3"))));

    std::error_code linkError;
    const std::filesystem::path linkPath = toFilesystemPath(QDir(outputRoot.path()).filePath(QStringLiteral("linked")));
    const std::filesystem::path targetPath = toFilesystemPath(outsideRoot.path());
    std::filesystem::create_directory_symlink(targetPath, linkPath, linkError);
    if (linkError)
    {
        GTEST_SKIP() << "Directory symlink creation is unavailable: " << linkError.message();
    }

    const WorkerPathResolver resolver = makeResolver(sourceRoot, outputRoot);
    RenderRequestPayload payload;
    payload.sourceRelativePath = QStringLiteral("input.CR3");
    payload.outputRelativePath = QStringLiteral("linked/output.jpg");

    const WorkerPathResolver::ResolveResult result = resolver.resolve(payload);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(WorkerPathErrorCode::OutputOutsideRoot, result.error().code);
}

TEST(WorkerPathResolverTest, RejectsInvalidProcessingValuesBeforeResolution)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    const WorkerPathResolver resolver = makeResolver(sourceRoot, outputRoot);
    RenderRequestPayload payload;
    payload.sourceRelativePath = QStringLiteral("missing.CR3");
    payload.outputRelativePath = QStringLiteral("output.jpg");
    payload.outputOptions.jpegQuality = 0;

    const WorkerPathResolver::ResolveResult result = resolver.resolve(payload);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(WorkerPathErrorCode::InvalidRequestValue, result.error().code);
}

TEST(WorkerPathResolverTest, RejectsMissingConfiguredRoot)
{
    QTemporaryDir outputRoot;
    ASSERT_TRUE(outputRoot.isValid());

    const WorkerPathResolver::CreateResult result =
        WorkerPathResolver::create({QStringLiteral("missing-worker-root"), outputRoot.path()});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(WorkerPathErrorCode::InvalidRoot, result.error().code);
}

}  // namespace
}  // namespace flexraw::worker::runtime
