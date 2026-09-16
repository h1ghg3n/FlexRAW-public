#pragma once

#include "client_error.h"
#include "develop_params.h"
#include "editor_client.h"
#include "editor_contracts.h"
#include "error.h"

namespace flexraw::core::orchestration
{

// 목적: Core Develop parameter를 Qt-free Editor client 값으로 투영
// 입력: params: processing과 Editor session이 사용하는 현재 parameter
// 출력: 같은 필드 의미를 가진 Qt-free parameter 값
[[nodiscard]] client::EditorDevelopParams toClientDevelopParams(const types::DevelopParams& params) noexcept;

// 목적: Qt-free Editor client parameter를 Core Develop 값으로 복원
// 입력: params: client command가 전달한 parameter
// 출력: Core validation에 전달할 Develop parameter 값
[[nodiscard]] types::DevelopParams fromClientDevelopParams(const client::EditorDevelopParams& params) noexcept;

// 목적: transitional Editor state를 Qt-free immutable snapshot으로 투영
// 입력: state: 현재 Orchestration state, adjustmentActive: 연속 조작 진행 여부
// 출력: identity, revision, source와 history 의미를 보존한 client snapshot
[[nodiscard]] client::EditorSnapshot toClientEditorSnapshot(const EditorState& state, bool adjustmentActive);

// 목적: Qt-free Editor snapshot을 transitional Qt adapter state로 복원
// 입력: snapshot: client interface가 반환한 immutable state
// 출력: 기존 Qt GUI consumer가 사용할 EditorState 값
[[nodiscard]] EditorState fromClientEditorSnapshot(const client::EditorSnapshot& snapshot);

// 목적: Core 오류 분류와 진단문을 Qt-free client 오류로 투영
// 입력: error: Orchestration 또는 domain 오류
// 출력: 같은 분류와 UTF-8 technical message를 가진 client 오류
[[nodiscard]] client::ClientError toClientError(const types::CoreError& error);

// 목적: Qt-free client 오류를 transitional Qt adapter 오류로 복원
// 입력: error: client command가 반환한 오류
// 출력: 같은 분류와 QString technical message를 가진 Core 오류
[[nodiscard]] types::CoreError fromClientError(const client::ClientError& error);

}  // namespace flexraw::core::orchestration
