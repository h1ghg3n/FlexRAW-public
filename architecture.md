# FlexRAW Architecture

이 문서는 공개 source snapshot의 실제 구성과 주요 runtime 흐름을 설명합니다. 미래 목표와 현재 구현을 구분하며, directory
이름만 존재하는 기능을 구현된 capability로 간주하지 않습니다.

## 1. 전체 구성

FlexRAW는 하나의 범용 framework를 먼저 만든 뒤 기능을 끼워 넣는 방식이 아니라, 실제 product workflow를 vertical slice로
구현하고 검증된 의미만 공통 경계로 승격하는 방식을 사용합니다.

```text
                         Product Contract
                                ^
                                |
              +-----------------+-----------------+
              |                                   |
       Qt Desktop Adapter                   MCP Adapter
              |                                   |
              +------------ Composition ----------+
                                |
                        Product Runtime
                  Catalog / Editor authority
                                |
                         Orchestration
                                |
        catalog / raw / develop / color / preview / export

TCP Server Adapter -> Worker Runtime Port -> bounded Worker Runtime
                                                |
                                      ResolvedRenderPipeline
```

Desktop, MCP와 Worker는 서로 다른 executable입니다. Adapter끼리는 직접 의존하지 않으며 각 Composition Root가 필요한
Runtime과 concrete dependency를 생성하고 명시적으로 주입합니다.

## 2. Repository 영역

| 경로 | 책임 |
|---|---|
| `src/app` | Desktop와 MCP Composition Root를 구성합니다. |
| `src/runtime` | Catalog/Editor authority를 공유하는 Product Runtime owner를 제공합니다. |
| `src/core/client` | Frontend-neutral command/state/event contract를 제공합니다. |
| `src/core/orchestration` | Use case 순서, async lifecycle, cancellation, revision과 result assembly를 담당합니다. |
| `src/core/catalog` | Catalog model, SQLite persistence와 source binding을 담당합니다. |
| `src/core/raw`, `develop`, `color` | RAW decode와 pixel processing 의미를 담당합니다. |
| `src/core/preview` | Preview pipeline을 제공합니다. |
| `src/core/export`, `render` | Raster option, encode와 resolved render seam을 제공합니다. |
| `src/platform` | OS별 memory와 path identity 의미를 격리합니다. |
| `src/ui` | Qt Widgets presentation과 Product Contract adapter를 제공합니다. |
| `src/mcp` | STDIO transport와 MCP tool projection을 제공합니다. |
| `src/worker` | Remote client, wire protocol, bounded runtime와 TCP server를 제공합니다. |

Core leaf와 UI leaf는 각각 `STATIC` library이며, parent `flexraw_core`와 `flexraw_ui`는 필요한 leaf를 모으는
`INTERFACE` target입니다. Core와 Runtime은 UI 또는 MCP Adapter target에 의존하지 않습니다.

## 3. Composition과 lifetime

Concrete implementation 선택과 object lifetime은 Composition Root가 소유합니다. Core consumer가 `ApplicationContext`나
global service bag을 받지 않으며, 필요한 dependency를 constructor로 전달받습니다.

Desktop `ApplicationContext`는 다음 순서의 lifetime을 보장합니다.

```text
Platform service / persistence / pipeline
        -> Product Runtime와 Orchestrator
        -> Qt Adapter와 MainWindow
```

소멸 시에는 역순으로 callback 입구와 Adapter를 먼저 닫고, background operation을 취소·대기한 뒤 dependency를 파괴합니다.
MCP process도 EOF에서 subscription을 먼저 닫고 Runtime을 종료합니다.

현재 Product Runtime은 Catalog session, Catalog/Project, Editor와 Source Resolution authority 및 주입된 capability를 사용하는
Preview Orchestrator를 소유합니다. Desktop은 실제 Preview pipeline과 presentation Adapter를 조립하며 MCP는 unavailable Preview
capability를 주입합니다. Thumbnail과 Export의 ownership은 Desktop use case에 남아 있고 실제 두 번째 consumer가 필요로 하는
vertical slice에서만 Runtime으로 점진적으로 이동합니다.

## 4. Product Contract

`src/core/client`는 Qt Widget, `QObject`, SQL connection과 transport type을 노출하지 않는 product-facing boundary입니다.
주요 surface는 다음과 같습니다.

- Catalog session create/open/current state
- Catalog/Folder/Project bounded page와 Project mutation
- Editor selection, Develop update, adjustment begin/end, undo/redo와 save
- Preview presentation과 `DisplayFrame`
- Catalog thumbnail window
- Source Resolution command/event
- Worker profile와 health snapshot
- Export request/progress/cancel/result
- Catalog startup과 Export defaults

`ClientError`는 frontend-independent category와 diagnostic message를 전달합니다. Export failure, Worker health와 Source state처럼
행동에 영향을 주는 domain 의미는 하나의 global error enum으로 평탄화하지 않습니다.

## 5. Catalog와 source identity

Catalog는 SQLite를 사용하며 `PhotoId`를 안정적인 identity로 유지합니다. Source file 위치는 변경될 수 있고 Develop revision은
optimistic save conflict를 검출합니다.

```text
Folder scan
   -> normalized source locator
   -> existing file identity 확인
   -> 기존 PhotoId 재사용 또는 새 Photo 등록
   -> Editor session과 persisted Develop state 연결
```

입력 path의 앞뒤 whitespace를 조용히 제거하지 않습니다. Existing source는 filesystem identity로 비교하며, 아직 생성되지 않은
Export destination은 현재 OS Adapter가 제공하는 transient comparison key를 사용합니다. Windows는 parent directory의
case-sensitive 설정을 반영하고, Linux는 filesystem evidence로 exact-name 비교가 안전한지 확인합니다. 의미를 확정할 수 없으면
fail closed합니다.

Source가 없어지거나 같은 path의 content가 바뀌더라도 Photo identity와 Develop state를 즉시 버리지 않습니다. Missing,
replacement와 unreadable 상태를 기록하고 사용자가 replacement 수용, 새 Photo 등록 또는 relink를 선택할 수 있도록 합니다.

## 6. Editor와 Preview

Editor는 현재 선택, Develop state, undo/redo, persisted baseline과 preview scheduling policy를 소유합니다. Preview Orchestrator는
background request lifecycle을 소유합니다.

```text
User input
   -> Qt Adapter
   -> EditorOrchestrator
      - state/history 갱신
      - interactive throttle와 final debounce
      - superseded request 취소
      - immutable PreviewRequest 생성
   -> PreviewOrchestrator
   -> FilePreviewPipeline
   -> current-result 검증
   -> DisplayFrame publication
```

Cancellation은 불필요해진 계산을 줄이고, stale-result validation은 늦게 끝난 이전 결과가 최신 UI를 덮지 못하도록 합니다.
두 방어를 서로 대체 가능한 것으로 취급하지 않습니다.

Preview frame은 owned BGRA8/sRGB `DisplayFrame`으로 Product Contract를 통과한 뒤 Qt Adapter에서 `QImage` view로 복원합니다.
Histogram과 clipping은 현재 standard preview 결과와 함께 계산합니다.

## 7. Shared processing path

Preview와 Export는 lifecycle과 output precision이 다르지만 RAW decode, Develop parameter, color와 encode 의미를 공유합니다.
Worker가 사용하는 가장 좁은 synchronous seam은 `ResolvedRenderPipeline`입니다.

```text
ResolvedRenderRequest
   -> validation
   -> RAW/raster decode
   -> source conversion
   -> Develop
   -> color transform
   -> raster encode와 atomic write
   -> ResolvedRenderResult + stage stats
```

이 pipeline에는 Catalog lookup, UI, network, batch queue와 process-wide scheduling policy가 들어가지 않습니다. Cancellation은
주요 stage 경계와 output 처리에서 cooperative하게 확인합니다.

## 8. Export placement

`ExportOrchestrator`가 Desktop Export scheduling authority입니다. 하나의 immutable Export intent에 Local, Remote 또는 Auto
placement를 적용합니다.

```text
Export request
   -> item preparation
   -> stable pending queue
      -> Local slot  -> FileExportPipeline
      -> Remote slot -> Worker client adapter
```

Local과 Remote slot은 각각 bounded입니다. Remote handshake는 직렬화하지만 accepted render는 별도 execution slot에서 진행할 수
있습니다. Workload가 시작되지 않았다는 사실이 확실한 rejection만 Auto에서 requeue할 수 있습니다. 실행 여부가 불명확한
disconnect나 timeout은 `Ambiguous` terminal이며 Local에서 자동 재실행하지 않습니다.

Output은 temporary artifact에 작성한 뒤 성공한 경우에만 final path로 교체합니다. Source/output collision은 OS별 path identity
service를 사용해 생성 전에 차단합니다.

## 9. Worker runtime과 TCP Adapter

`src/worker` 전체를 Server Adapter로 취급하지 않습니다.

| 영역 | 책임 |
|---|---|
| protocol/network | framing, parsing, socket/session lifetime과 wire projection을 담당합니다. |
| runtime | job identity, state, cancellation, exact terminal과 bounded queue를 담당합니다. |
| admission | memory estimate와 local/router admission을 담당합니다. |
| processing | `ResolvedRenderPipeline`을 호출합니다. |
| app | concrete dependency와 startup/shutdown을 조립합니다. |

TCP `ProtocolSession`은 concrete `JobScheduler` 대신 `flexraw_worker_runtime_port` target의 `IRenderWorkerRuntime` interface를
소비합니다. Wire `JobId`와 Runtime job identity의 mapping은 session 내부에만 존재합니다.

Scheduler는 bounded running slot과 waiting queue를 사용합니다. Queue capacity와 resource admission은 서로 다른 제한입니다.
Queued/running cancellation은 terminal을 정확히 한 번만 반환해야 하며 shutdown은 신규 접수 차단, active session 종료,
cooperative cancellation, worker 대기 순서로 진행합니다.

Worker protocol은 native struct memory를 전송하지 않고 fixed-width big-endian frame과 strict UTF-8 payload를 사용합니다. 현재
shared-storage 방식과 trusted LAN을 전제로 하며 authentication, authorization과 TLS를 제공하지 않습니다.

## 10. MCP Adapter

MCP executable은 하나의 `ProductRuntime`을 소유하고 Product Contract를 JSON-RPC/MCP tool로 projection합니다. Tool layer가
별도의 Catalog 또는 Editor business state를 만들지 않습니다.

기본 surface는 bounded Catalog/Project read, Editor state와 Source Resolution event poll입니다. Mutation tool은
`--allow-write`가 있을 때만 등록합니다. Request 처리, Runtime callback과 stdout write를 하나의 owner context로 직렬화해
response byte가 섞이지 않도록 합니다.

MCP에는 Preview frame, Export, Worker control과 generic filesystem access가 아직 포함되지 않습니다.

## 11. Thread와 async boundary

| 실행 context | 주요 책임 |
|---|---|
| Desktop GUI event loop | Widget state, Adapter event와 command 호출을 담당합니다. |
| Preview dedicated thread | Synchronous Preview Pipeline을 실행합니다. |
| Thumbnail serial pool | Visible/adjacent decode와 stale window 폐기를 담당합니다. |
| Fingerprint pool | Source hash와 verification을 담당합니다. |
| Export preparation/local/remote pool | Item 준비와 Local/Remote execution을 담당합니다. |
| Worker network event loop | Socket과 `ProtocolSession`을 소유합니다. |
| Worker scheduler pool | Admitted render를 실행합니다. |

Cross-thread callback은 immutable value를 전달하고 owner context로 serialize합니다. Async owner가 borrowed dependency보다 오래
살지 않도록 Composition Root의 member order와 destructor wait를 검증합니다.

현재 Preview, Export와 Worker의 compute pool을 하나의 global scheduler로 통합하지 않습니다. Shared executor, priority,
fairness, CPU reservation과 intra-image parallelism은 measurement와 별도 설계 승인이 필요한 후속 범위입니다.

## 12. Build와 test boundary

주요 build 조합은 다음과 같습니다.

- Windows Desktop Release
- Windows MCP-only Release
- Windows Worker-only Release
- Windows Qt-free Product Contract
- WSL x64 Qt-free Product Contract
- Linux ARM64 Qt-free Product Contract

Linux ARM64 preset은 `arm64-linux` triplet을 사용하는 native build용입니다. 특정 장비 이름을 build target으로 사용하지
않으며, 장비별 측정 결과와 모든 Linux ARM64 환경의 지원 보장은 구분합니다.

Test는 ownership 경계에 따라 `tests/core`, `tests/orchestration`, `tests/worker`, `tests/ui`, `tests/app`, `tests/mcp`,
`tests/platform`과 `tests/portable`로 나뉩니다. 실제 RAW 또는 OS capability가 필요한 test는 fixture가 없을 때 이유를 명시하고
skip하며, 일반 unit test가 fixture 부재를 성공으로 가장하지 않습니다.

## 13. 의도적인 한계

현재 snapshot은 다음 항목을 완료된 architecture로 주장하지 않습니다.

- Core implementation과 Product Runtime 전체의 Qt-free 전환
- GUI와 MCP를 한 process에서 실행하는 combined composition
- 독립 installable processing engine 또는 plugin SDK
- RAW upload, artifact download와 cross-storage synchronization
- Worker discovery, durable retry/resume와 public network security
- 완전한 display ICC, high-bit-depth preview와 advanced Export feature
- ML/HDR/Panorama directory가 암시하는 모든 미래 기능

새 abstraction은 실제 consumer 또는 concrete build blocker가 있을 때만 추가합니다. 코드 모양이 비슷하다는 이유만으로
ownership, lifetime, cancellation과 failure semantics가 다른 책임을 하나로 합치지 않습니다.
