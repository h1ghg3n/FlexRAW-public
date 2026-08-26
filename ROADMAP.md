# FlexRAW Roadmap

## Status

Current milestone: **M1.4 — MCP Contract Proof**

이 문서는 가까운 개발 범위와 현재 구현 위치만 추적합니다. 장기 아이디어나 아직 구체화되지 않은 기능 목록은 포함하지 않습니다.

상태 표기는 다음 의미를 사용합니다.

- `DONE`: 구현과 현재 기준 검증 완료
- `CURRENT`: 현재 진행 위치
- `IN PROGRESS`: 현재 개발 중인 milestone
- `PLANNED`: 다음 범위로 확정됐지만 아직 구현하지 않음

## Milestones

| Milestone | Scope | Status |
|---|---|---|
| M1.1 | Persistent Editor | DONE |
| M1.2 | Catalog Navigation & Project | DONE |
| M1.3 | Qt-free Client Boundary | DONE |
| M1.4 | MCP Contract Proof | IN PROGRESS |
| M1.5 | Export & Acceptance | PLANNED |

## M1.3 — Qt-free Client Boundary

기존 Qt Widgets GUI는 유지하면서 안정화된 Catalog/Editor client 의미를 Qt-free contract로 옮긴 단계입니다.

| Phase | Scope | Status |
|---|---|---|
| Phase 1 | Qt-free client target + `DisplayFrame` v1 | DONE |
| Phase 2 | Project list/create contract | DONE |
| Phase 3 | Catalog / Folder / Project photo paging | DONE |
| Phase 4 | Project mutation / membership | DONE |
| Phase 5 | Single-Photo Editor command / state | DONE |
| Phase 6 | Editor event / subscription lifetime | DONE |
| Phase 7 | Preview / scan / source verification Activity projection | DONE |

M1.3에서 추가한 `flexraw_core_client` target은 Qt target이나 Qt object lifetime을 노출하지 않습니다. Qt GUI adapter는
같은 contract를 실제 Catalog/Editor workflow에 연결하며 다음 항목을 검증합니다.

- owned `DisplayFrame`과 Catalog/Folder/Project bounded paging
- Project CRUD와 Photo membership
- Single-Photo Editor command, immutable state와 event subscription
- serialized delivery와 unsubscribe/shutdown 이후 callback 차단
- owner lifecycle을 보존하는 Preview, folder scan과 source verification Activity

## Next

### Current — M1.4 MCP Contract Proof

M1.3에서 정리한 Qt-free client contract를 local headless consumer에서도 실제 사용할 수 있는지 검증합니다.

- Catalog/Project bounded query
- 명시적인 state-changing command 하나
- typed error와 cancellation/lifetime
- adapter-owned serialized delivery와 정상 shutdown

범용 plugin framework, remote execution manager와 전체 Editor automation은 이 milestone에 포함하지 않습니다.

### M1.5 — Export & Acceptance

이미 구현된 Local/Remote/Auto Export를 기반으로 남은 product acceptance와 사용자-facing Export 범위를 정리합니다.

---

이 roadmap은 구현 상태를 설명하기 위한 문서입니다. `PLANNED` 항목은 현재 구현된 기능을 의미하지 않으며, 실제 코드와 검증 결과가 항상 최종 기준입니다.
