#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "client_error.h"
#include "client_result.h"

namespace flexraw::core::client
{

struct WorkerProfileId
{
    std::string value;

    bool operator==(const WorkerProfileId&) const = default;
};

struct WorkerProfileSnapshot
{
    WorkerProfileId id;
    std::string displayName;
    std::string host;
    std::uint16_t port{0};
    bool enabled{true};
    std::string expectedSourceStorageId;
    std::string expectedOutputStorageId;

    bool operator==(const WorkerProfileSnapshot&) const = default;
};

struct CreateWorkerProfileCommand
{
    std::string displayName;
    std::string host;
    std::uint16_t port{0};
    bool enabled{true};
    std::string expectedSourceStorageId;
    std::string expectedOutputStorageId;
};

struct UpdateWorkerProfileCommand
{
    WorkerProfileId id;
    std::string displayName;
    std::string host;
    std::uint16_t port{0};
    bool enabled{true};
    std::string expectedSourceStorageId;
    std::string expectedOutputStorageId;
};

struct SetWorkerProfileEnabledCommand
{
    WorkerProfileId id;
    bool enabled{true};
};

using WorkerProfileListResult = ClientResult<std::vector<WorkerProfileSnapshot>, ClientError>;
using WorkerProfileResult = ClientResult<WorkerProfileSnapshot, ClientError>;
using WorkerProfileRemoveResult = ClientResult<WorkerProfileId, ClientError>;

class IWorkerProfileClient
{
public:
    // 목적: implementation별 application-level Worker profile resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IWorkerProfileClient() = default;

    // 목적: 저장 순서가 보존된 현재 Worker profile snapshot 목록 조회
    // 입력: 없음
    // 출력: profile 목록 또는 저장소 오류
    [[nodiscard]] virtual WorkerProfileListResult listWorkerProfiles() = 0;

    // 목적: application-level Worker profile을 새 stable identity로 생성
    // 입력: command: 이름, endpoint, 활성화 상태와 expected storage binding
    // 출력: 생성된 normalized profile 또는 validation·저장소 오류
    [[nodiscard]] virtual WorkerProfileResult createWorkerProfile(const CreateWorkerProfileCommand& command) = 0;

    // 목적: stable identity를 유지하며 Worker profile 속성 갱신
    // 입력: command: 대상 identity와 교체할 전체 profile 속성
    // 출력: 갱신된 normalized profile 또는 validation·not-found·저장소 오류
    [[nodiscard]] virtual WorkerProfileResult updateWorkerProfile(const UpdateWorkerProfileCommand& command) = 0;

    // 목적: 저장된 Worker profile과 그 application-level identity 제거
    // 입력: id: 제거할 stable profile identity
    // 출력: 제거된 identity 또는 validation·not-found·저장소 오류
    [[nodiscard]] virtual WorkerProfileRemoveResult removeWorkerProfile(const WorkerProfileId& id) = 0;

    // 목적: 다른 profile 속성을 유지하며 Worker 사용 가능 정책만 변경
    // 입력: command: 대상 identity와 새 enabled 상태
    // 출력: 갱신된 profile 또는 validation·not-found·저장소 오류
    [[nodiscard]] virtual WorkerProfileResult setWorkerProfileEnabled(
        const SetWorkerProfileEnabledCommand& command) = 0;
};

}  // namespace flexraw::core::client
