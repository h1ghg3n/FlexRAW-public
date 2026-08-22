#include <QByteArray>

#include <gtest/gtest.h>

#include "source_binding.h"

namespace flexraw::core::catalog
{
namespace
{

// 목적: source binding test에서 사용할 고정 SHA-256 digest 생성
// 입력: marker: digest byte를 구분할 값
// 출력: marker로 채워 32 byte digest
[[nodiscard]] QByteArray makeDigest(char marker)
{
    return QByteArray(types::Sha256DigestSize, marker);
}

TEST(SourceBindingTest, AllowsProcessingWhileInitialFingerprintIsPending)
{
    EXPECT_TRUE(allowsSourceProcessing(SourceBindingState::FingerprintPending));
    EXPECT_TRUE(allowsSourceProcessing(SourceBindingState::Available));
    EXPECT_FALSE(allowsSourceProcessing(SourceBindingState::ReplacementDetected));
}

TEST(SourceBindingTest, UsesMetadataAsCheapMatchWhenBaselineHashExists)
{
    const types::SourceFingerprint baseline{1024, 1234, makeDigest('a')};
    const SourceObservation observation{SourceAvailability::Available, {1024, 1234, {}}};

    EXPECT_EQ(SourceBindingState::Available, evaluateSourceBinding(baseline, observation));
}

TEST(SourceBindingTest, RequestsHashWhenMetadataChangedAndBaselineHashExists)
{
    const types::SourceFingerprint baseline{1024, 1234, makeDigest('a')};
    const SourceObservation observation{SourceAvailability::Available, {2048, 5678, {}}};

    EXPECT_EQ(SourceBindingState::VerificationRequired, evaluateSourceBinding(baseline, observation));
}

TEST(SourceBindingTest, DetectsReplacementFromDifferentContentHashes)
{
    const types::SourceFingerprint baseline{1024, 1234, makeDigest('a')};
    const SourceObservation observation{SourceAvailability::Available, {2048, 5678, makeDigest('b')}};

    EXPECT_EQ(SourceBindingState::ReplacementDetected, evaluateSourceBinding(baseline, observation));
    EXPECT_TRUE(requiresSourceResolution(SourceBindingState::ReplacementDetected));
}

TEST(SourceBindingTest, AcceptsMetadataChangesWhenContentHashMatches)
{
    const QByteArray digest = makeDigest('a');
    const types::SourceFingerprint baseline{1024, 1234, digest};
    const SourceObservation observation{SourceAvailability::Available, {1024, 5678, digest}};

    EXPECT_EQ(SourceBindingState::Available, evaluateSourceBinding(baseline, observation));
}

TEST(SourceBindingTest, LeavesChangedLegacySourceIdentityUnverified)
{
    const types::SourceFingerprint baseline{1024, 1234, {}};
    const SourceObservation observation{SourceAvailability::Available, {2048, 5678, makeDigest('a')}};

    EXPECT_EQ(SourceBindingState::IdentityUnverified, evaluateSourceBinding(baseline, observation));
    EXPECT_TRUE(requiresSourceResolution(SourceBindingState::IdentityUnverified));
}

TEST(SourceBindingTest, ReportsMissingAndUnreadableSourcesWithoutComparingFingerprint)
{
    const types::SourceFingerprint baseline{1024, 1234, makeDigest('a')};

    EXPECT_EQ(SourceBindingState::Missing, evaluateSourceBinding(baseline, {SourceAvailability::Missing, {}}));
    EXPECT_EQ(SourceBindingState::Unreadable, evaluateSourceBinding(baseline, {SourceAvailability::Unreadable, {}}));
    EXPECT_TRUE(requiresSourceResolution(SourceBindingState::Missing));
    EXPECT_TRUE(requiresSourceResolution(SourceBindingState::Unreadable));
    EXPECT_TRUE(requiresSourceResolution(SourceBindingState::Unlinked));
    EXPECT_FALSE(requiresSourceResolution(SourceBindingState::VerificationRequired));
}

}  // namespace
}  // namespace flexraw::core::catalog
