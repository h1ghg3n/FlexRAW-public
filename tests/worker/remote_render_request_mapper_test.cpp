#include <stdexcept>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "remote_render_request_mapper.h"

namespace flexraw::worker::client
{
namespace
{

// 목적: path mapping test에 사용할 빈 regular file 생성
// 입력: path: 기존 parent 아래 생성할 absolute file path
// 출력: file 생성 성공 여부
[[nodiscard]] bool createFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("raw") == 3;
}

// 목적: path mapping 대상 resolved render request 생성
// 입력: sourcePath/outputPath: local absolute file path
// 출력: 식별 가능한 develop/output 값을 포함한 request
[[nodiscard]] core::render::ResolvedRenderRequest makeRequest(const QString& sourcePath, const QString& outputPath)
{
    core::render::ResolvedRenderRequest request;
    request.sourcePath = sourcePath;
    request.outputPath = outputPath;
    request.developParams.exposureEv = 0.75F;
    request.outputOptions.jpegQuality = 91;
    return request;
}

// 목적: 유효한 temporary root pair로 request mapper 생성
// 입력: sourceRoot/outputRoot: 기존 temporary directory
// 출력: 검증된 RemoteRenderRequestMapper
[[nodiscard]] RemoteRenderRequestMapper makeMapper(const QTemporaryDir& sourceRoot, const QTemporaryDir& outputRoot)
{
    RemoteRenderRequestMapper::CreateResult result =
        RemoteRenderRequestMapper::create({sourceRoot.path(), outputRoot.path()});
    if (result.hasError())
    {
        throw std::runtime_error("Unable to create remote render request mapper fixture.");
    }
    return result.value();
}

TEST(RemoteRenderRequestMapperTest, MapsUnicodeAbsolutePathsAndPreservesProcessingValues)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    ASSERT_TRUE(QDir(sourceRoot.path()).mkpath(QStringLiteral("originals/한글")));
    ASSERT_TRUE(QDir(outputRoot.path()).mkpath(QStringLiteral("exports/결과")));

    const QString sourcePath = QDir(sourceRoot.path()).filePath(QStringLiteral("originals/한글/사진.ARW"));
    const QString outputPath = QDir(outputRoot.path()).filePath(QStringLiteral("exports/결과/사진.jpg"));
    ASSERT_TRUE(createFile(sourcePath));

    const RemoteRenderRequestMapper mapper = makeMapper(sourceRoot, outputRoot);
    const RemoteRenderRequestMapper::MapResult result = mapper.map(makeRequest(sourcePath, outputPath));

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(QStringLiteral("originals/한글/사진.ARW"), result.value().sourceRelativePath);
    EXPECT_EQ(QStringLiteral("exports/결과/사진.jpg"), result.value().outputRelativePath);
    EXPECT_FLOAT_EQ(0.75F, result.value().developParams.exposureEv);
    EXPECT_EQ(91, result.value().outputOptions.jpegQuality);
}

TEST(RemoteRenderRequestMapperTest, RejectsRelativeAndMissingRoots)
{
    QTemporaryDir outputRoot;
    ASSERT_TRUE(outputRoot.isValid());

    const RemoteRenderRequestMapper::CreateResult relative =
        RemoteRenderRequestMapper::create({QStringLiteral("relative-source"), outputRoot.path()});
    ASSERT_TRUE(relative.hasError());
    EXPECT_EQ(RemoteRenderPathErrorCode::InvalidRoot, relative.error().code);

    const RemoteRenderRequestMapper::CreateResult missing = RemoteRenderRequestMapper::create(
        {QDir(outputRoot.path()).filePath(QStringLiteral("missing")), outputRoot.path()});
    ASSERT_TRUE(missing.hasError());
    EXPECT_EQ(RemoteRenderPathErrorCode::InvalidRoot, missing.error().code);
}

TEST(RemoteRenderRequestMapperTest, RejectsSourceOutsideRootAndMissingSource)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    QTemporaryDir outsideRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    ASSERT_TRUE(outsideRoot.isValid());

    const RemoteRenderRequestMapper mapper = makeMapper(sourceRoot, outputRoot);
    const QString outputPath = QDir(outputRoot.path()).filePath(QStringLiteral("output.jpg"));
    const RemoteRenderRequestMapper::MapResult missing =
        mapper.map(makeRequest(QDir(sourceRoot.path()).filePath(QStringLiteral("missing.ARW")), outputPath));
    ASSERT_TRUE(missing.hasError());
    EXPECT_EQ(RemoteRenderPathErrorCode::SourceNotFound, missing.error().code);

    const QString outsideSource = QDir(outsideRoot.path()).filePath(QStringLiteral("outside.ARW"));
    ASSERT_TRUE(createFile(outsideSource));
    const RemoteRenderRequestMapper::MapResult outside = mapper.map(makeRequest(outsideSource, outputPath));
    ASSERT_TRUE(outside.hasError());
    EXPECT_EQ(RemoteRenderPathErrorCode::SourceOutsideRoot, outside.error().code);
}

TEST(RemoteRenderRequestMapperTest, RejectsMissingAndOutsideOutputParent)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    QTemporaryDir outsideRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());
    ASSERT_TRUE(outsideRoot.isValid());

    const QString sourcePath = QDir(sourceRoot.path()).filePath(QStringLiteral("input.ARW"));
    ASSERT_TRUE(createFile(sourcePath));
    const RemoteRenderRequestMapper mapper = makeMapper(sourceRoot, outputRoot);

    const RemoteRenderRequestMapper::MapResult missingParent =
        mapper.map(makeRequest(sourcePath, QDir(outputRoot.path()).filePath(QStringLiteral("missing/output.jpg"))));
    ASSERT_TRUE(missingParent.hasError());
    EXPECT_EQ(RemoteRenderPathErrorCode::OutputParentNotFound, missingParent.error().code);

    const RemoteRenderRequestMapper::MapResult outside =
        mapper.map(makeRequest(sourcePath, QDir(outsideRoot.path()).filePath(QStringLiteral("output.jpg"))));
    ASSERT_TRUE(outside.hasError());
    EXPECT_EQ(RemoteRenderPathErrorCode::OutputOutsideRoot, outside.error().code);
}

TEST(RemoteRenderRequestMapperTest, RejectsNonPortableOutputName)
{
    QTemporaryDir sourceRoot;
    QTemporaryDir outputRoot;
    ASSERT_TRUE(sourceRoot.isValid());
    ASSERT_TRUE(outputRoot.isValid());

    const QString sourcePath = QDir(sourceRoot.path()).filePath(QStringLiteral("input.ARW"));
    ASSERT_TRUE(createFile(sourcePath));
    const RemoteRenderRequestMapper mapper = makeMapper(sourceRoot, outputRoot);

    const RemoteRenderRequestMapper::MapResult result =
        mapper.map(makeRequest(sourcePath, QDir(outputRoot.path()).filePath(QStringLiteral("bad:name.jpg"))));

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(RemoteRenderPathErrorCode::NonPortablePath, result.error().code);
}

}  // namespace
}  // namespace flexraw::worker::client
