#pragma once

#include <memory>

#include <QHash>
#include <QObject>
#include <QThreadPool>

#include "catalog_contracts.h"
#include "source_fingerprint.h"

template<typename T> class QFutureWatcher;

namespace flexraw::core::catalog
{
class CatalogDatabase;
class CatalogDevelopRepository;
class CatalogFolderImporter;
class CatalogPhotoRepository;
}  // namespace flexraw::core::catalog

namespace flexraw::core::orchestration
{

class CatalogOrchestrator final : public QObject
{
    Q_OBJECT

public:
    // 목적: catalog-session state와 직렬 background fingerprint pool 생성
    // 입력: parent: Qt 부모 object
    // 출력: catalog가 열리지 않은 CatalogOrchestrator 객체
    explicit CatalogOrchestrator(QObject* parent = nullptr);

    // 목적: active fingerprint 작업을 취소하고 catalog connection 정리
    // 입력: 없음
    // 출력: worker와 database resource가 남지 않음
    ~CatalogOrchestrator() override;

    // 목적: 현재 catalog-session immutable state 반환
    // 입력: 없음
    // 출력: open 여부와 canonical catalog path
    [[nodiscard]] CatalogSessionState state() const;

    // 목적: catalog database를 열고 migration과 repository session 구성
    // 입력: catalogPath: 생성하거나 열 catalog file 경로
    // 출력: 열린 session state 또는 validation·migration 오류
    [[nodiscard]] CatalogSessionResult openCatalog(const QString& catalogPath);

    // 목적: active fingerprint 작업을 취소하고 현재 catalog session 종료
    // 입력: 없음
    // 출력: 닫힌 session state
    [[nodiscard]] CatalogSessionState closeCatalog();

    // 목적: 열린 catalog에서 optional exact-folder scope와 stable cursor 기반 bounded photo page 조회
    // 입력: request: page 크기, 방향, optional exclusive cursor와 folder scope
    // 출력: 표시 순서의 bounded page 또는 session·validation·database 오류
    [[nodiscard]] CatalogPhotoPageResult queryPhotos(const catalog::CatalogPhotoPageRequest& request) const;

    // 목적: folder 사진을 catalog에 등록하고 pending fingerprint 작업 예약
    // 입력: folderPath: scan할 folder 경로
    // 출력: import 수와 identity·background request 목록 또는 오류
    [[nodiscard]] CatalogImportResult importFolder(const QString& folderPath);

    // 목적: worker에서 완료한 folder scan 결과를 열린 catalog에 저장하고 fingerprint 작업 예약
    // 입력: entries: UI thread 밖에서 scan·분류가 끝난 지원 photo 목록
    // 출력: import 수와 identity·background request 목록 또는 오류
    [[nodiscard]] CatalogImportResult importScannedEntries(const QVector<catalog::CatalogEntry>& entries);

    // 목적: Editor activation 대상 photo를 active Catalog에 등록하거나 기존 identity로 resolve
    // 입력: entry: folder scan에서 얻은 단일 supported photo
    // 출력: stable catalog-local PhotoId 또는 session·database 오류
    [[nodiscard]] CatalogPhotoRegistrationResult registerPhoto(const catalog::CatalogEntry& entry);

    // 목적: PhotoId의 develop state와 현재 source binding을 resolve
    // 입력: photoId: 선택하거나 조회할 catalog-local identity
    // 출력: processing 가능 여부와 optional verification request를 포함한 state
    [[nodiscard]] CatalogPhotoStateResult resolvePhoto(types::PhotoId photoId);

    // 목적: caller baseline revision과 일치할 때 PhotoId의 develop state 저장
    // 입력: photoId: 저장 대상, params: 새 develop 값, expectedRevision: load 당시 persisted revision
    // 출력: 증가한 persisted revision을 포함한 photo state 또는 conflict
    [[nodiscard]] CatalogPhotoStateResult saveDevelopState(types::PhotoId photoId,
                                                           const types::DevelopParams& params,
                                                           types::DevelopRevision expectedRevision);

    // 목적: 교체·identity 미확인 source를 기존 PhotoId의 새 baseline으로 수용
    // 입력: photoId: 기존 develop state를 유지할 identity
    // 출력: background hash request ID 또는 현재 state 오류
    [[nodiscard]] CatalogSourceSubmissionResult acceptReplacement(types::PhotoId photoId);

    // 목적: 교체·identity 미확인 source를 새 PhotoId로 등록
    // 입력: photoId: source binding을 해제할 기존 identity
    // 출력: background hash request ID 또는 현재 state 오류
    [[nodiscard]] CatalogSourceSubmissionResult registerReplacementAsNew(types::PhotoId photoId);

    // 목적: 기존 PhotoId를 content match가 확인된 다른 locator에 재연결
    // 입력: photoId: 유지할 identity, locator: 검증할 새 source 위치
    // 출력: background hash request ID 또는 현재 state 오류
    [[nodiscard]] CatalogSourceSubmissionResult relinkSource(types::PhotoId photoId,
                                                             const types::SourceLocator& locator);

    // 목적: 지정 fingerprint request의 향후 mutation과 event publish 취소
    // 입력: requestId: 취소할 request identity
    // 출력: active request를 취소했으면 true
    bool cancelSourceRequest(types::RequestId requestId);

signals:
    // 목적: background fingerprint와 repository state transition 완료 전달
    // 입력: update: request와 갱신된 기존·optional 신규 photo record
    // 출력: 없음
    void sourceBindingUpdated(const CatalogSourceUpdate& update);

    // 목적: background fingerprint 또는 repository transition 실패 전달
    // 입력: issue: request·photo identity와 technical 오류
    // 출력: 없음
    void sourceBindingFailed(const CatalogIssue& issue);

    // 목적: accepted fingerprint request의 terminal cancellation 전달
    // 입력: requestId: 취소된 request identity
    // 출력: 없음
    void sourceBindingCancelled(types::RequestId requestId);

private:
    enum class FingerprintPurpose
    {
        EstablishBaseline,
        VerifySource,
        AcceptReplacement,
        RegisterReplacementAsNew,
        RelinkSource,
    };

    struct ActiveFingerprintJob
    {
        FingerprintPurpose purpose{FingerprintPurpose::VerifySource};
        types::PhotoId photoId;
        types::SourceLocator locator;
        catalog::CatalogEntry replacementEntry;
        types::CancellationSource cancellationSource;
        QFutureWatcher<catalog::SourceFingerprintResult>* watcher{nullptr};
    };

    // 목적: 열린 session repository 존재 여부 확인
    // 입력: 없음
    // 출력: 열려 있으면 빈 error, 아니면 session 오류
    [[nodiscard]] std::optional<types::CoreError> requireOpenCatalog() const;

    // 목적: photo record와 persisted develop state를 adapter용 snapshot으로 조립
    // 입력: photoId: 조회할 identity, requestId: optional active verification request
    // 출력: 조립된 state 또는 repository 오류
    [[nodiscard]] CatalogPhotoStateResult loadPhotoState(
        types::PhotoId photoId, std::optional<types::RequestId> requestId = std::nullopt) const;

    // 목적: current source metadata를 평가하고 필요하면 hash verification 예약
    // 입력: record: 평가할 persisted photo record
    // 출력: optional request ID 또는 metadata·repository 오류
    [[nodiscard]] types::Result<std::optional<types::RequestId>, types::CoreError> observeSource(
        const catalog::CatalogPhotoRecord& record);

    // 목적: source fingerprint 계산을 직렬 worker pool에 제출
    // 입력: purpose: 완료 후 transition, photoId: 대상, locator: hash source, replacementEntry: 신규 등록 정보
    // 출력: accepted request ID 또는 session·중복·ID 오류
    [[nodiscard]] CatalogSourceSubmissionResult submitFingerprintJob(FingerprintPurpose purpose,
                                                                     types::PhotoId photoId,
                                                                     types::SourceLocator locator,
                                                                     catalog::CatalogEntry replacementEntry = {});

    // 목적: worker fingerprint 결과를 repository state transition과 terminal event로 변환
    // 입력: requestId: 완료된 request, result: 계산된 fingerprint 또는 오류
    // 출력: updated·failed·cancelled terminal signal 하나
    void handleFingerprintFinished(types::RequestId requestId, const catalog::SourceFingerprintResult& result);

    // 목적: fingerprint purpose에 맞는 domain repository mutation 실행
    // 입력: job: active request context, fingerprint: 완전한 SHA-256 observation
    // 출력: optional 신규 PhotoId 또는 transition 오류
    [[nodiscard]] types::Result<std::optional<types::PhotoId>, types::CoreError> applyFingerprintResult(
        const ActiveFingerprintJob& job, const types::SourceFingerprint& fingerprint);

    // 목적: active fingerprint request를 모두 취소하고 worker 종료 대기
    // 입력: publishCancellation: request별 cancellation signal 발생 여부
    // 출력: active request map과 photo deduplication map이 비워짐
    void cancelAllSourceRequests(bool publishCancellation);

    std::unique_ptr<catalog::CatalogDatabase> m_database;
    std::unique_ptr<catalog::CatalogPhotoRepository> m_photoRepository;
    std::unique_ptr<catalog::CatalogDevelopRepository> m_developRepository;
    std::unique_ptr<catalog::CatalogFolderImporter> m_folderImporter;
    QThreadPool m_fingerprintPool;
    QHash<types::RequestId, ActiveFingerprintJob> m_activeJobs;
    QHash<qint64, types::RequestId> m_activePhotoRequests;
    types::RequestId m_nextRequestId{1};
    bool m_acceptingRequests{true};
};

}  // namespace flexraw::core::orchestration
