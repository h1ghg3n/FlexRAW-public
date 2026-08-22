# Flexraw Architecture

대상 snapshot: `0.1.0`

이 문서는 공개 source snapshot의 현재 구조를 설명한다. 구현된 경계와 아직 검증되지 않은 장기 방향을 구분하며,
상세 동작의 source of truth는 항상 해당 source와 test다.

## 1. 전체 구조

Flexraw는 하나의 Desktop process와 선택적인 Render Worker process로 구성된다. Desktop은 Catalog, Editor,
Preview와 Export use case를 소유한다. Worker는 Catalog나 UI를 알지 않고, 이미 해석된 단일 RAW render 요청만
처리한다.

```text
┌──────────────────────────── Flexraw Desktop ────────────────────────────┐
│                                                                        │
│  Qt Widgets UI                                                         │
│  MainWindow · Panels · CatalogEditorFacade · ExportDialog              │
│                              │ commands                                │
│                              ▼                                         │
│  Orchestration                                                         │
│  CatalogOrchestrator · EditorOrchestrator · PreviewOrchestrator        │
│  ExportOrchestrator                                                    │
│                              │ resolved values                         │
│                              ▼                                         │
│  Core domain / processing                                              │
│  Catalog · History · RAW · Develop · Color · Preview · Export · Render │
│                              │                                         │
│                              ▼                                         │
│  SQLite · Filesystem · Platform API/Adapter · Qt runtime               │
│                                                                        │
└───────────────────────────────┬────────────────────────────────────────┘
                                │ optional FRWK/TCP
                                │ portable relative paths + render intent
                                ▼
┌────────────────────────── flexraw-worker ──────────────────────────────┐
│ TCP Server → ProtocolSession → bounded JobScheduler → ResourceAdmission│
│                                                        │               │
│                                                        ▼               │
│                                              ResolvedRenderPipeline    │
│                                                        │               │
│                                                        ▼               │
│                                             shared output storage      │
└────────────────────────────────────────────────────────────────────────┘
```

굵은 원칙은 다음과 같다.

1. UI는 화면 state와 입력을 담당하고 processing algorithm을 소유하지 않는다.
2. Orchestrator는 cross-module 순서, async lifecycle, cancellation과 result assembly를 담당한다.
3. Domain module은 SQL, RAW decode, Develop, encode 같은 실제 작업을 담당한다.
4. `ApplicationContext`와 `WorkerApplicationContext`만 production object graph를 조립한다.
5. Worker는 Desktop의 Catalog, Preview, UI와 `ApplicationContext`에 의존하지 않는다.
6. 현재 Core/Orchestration에는 Qt type이 남아 있다. Qt-free Engine이나 client boundary는 아직 추출되지 않았다.

## 2. Dependency 방향

일반적인 dependency 방향은 위에서 아래다. Result와 event는 반대 방향으로 전달될 수 있지만, 아래 layer가 위
layer의 concrete type을 include하지는 않는다.

```text
Desktop executable
        │
        ▼
       app ────────────────┐
        │                  │
        ▼                  ▼
       ui              worker/client
        │                  │
        └────────┬─────────┘
                 ▼
          core/orchestration
                 │
                 ▼
       Core domain/processing leaves
                 │
                 ▼
              util
                 │
                 ▼
              types


Worker executable
        │
        ▼
   worker/app
        │
        ├── network ── protocol
        ├── runtime
        ├── admission ── platform/api
        └── core/render
                 │
                 ▼
        raw · develop · color · export
```

Worker target은 Desktop용 `flexraw_core` aggregate를 link하지 않고 필요한 leaf target만 명시적으로 link한다. 이
규칙은 Worker가 Catalog/UI dependency를 우연히 끌어오는 것을 막는다.

## 3. Desktop Composition Root

`src/app/application_context.*`가 Desktop production object의 생성, 주입과 lifetime을 소유한다.

```text
ApplicationContext
│
├─ PreviewOrchestrator
│  └─ FilePreviewPipeline
│
├─ ISystemMemoryProbe
│  └─ current Windows/Linux Platform Adapter
│
├─ ExportOrchestrator
│  ├─ FileExportPipeline                    (Local)
│  └─ RemoteExportExecutionAdapter
│     └─ RemoteRenderExecutor               (Remote)
│
├─ ManagedCatalogSession
│  └─ CatalogOrchestrator
│     ├─ CatalogDatabase
│     ├─ CatalogPhotoRepository
│     ├─ CatalogDevelopRepository
│     └─ CatalogFolderImporter
│
├─ EditorOrchestrator
│  ├─ uses PreviewOrchestrator
│  └─ uses CatalogOrchestrator
│
├─ CatalogEditorFacade
│  ├─ uses CatalogOrchestrator
│  └─ uses EditorOrchestrator
│
└─ MainWindow
   ├─ uses CatalogEditorFacade
   ├─ uses CatalogOrchestrator
   └─ uses ExportOrchestrator
```

Owned object는 dependency보다 나중에 생성되고 먼저 파괴된다. 따라서 `MainWindow`와 Facade가 먼저 연결을 끊고,
Editor가 active timer/request를 정리한 뒤 Preview/Catalog dependency가 파괴된다. Background 작업을 가진 객체의
destructor는 신규 접수를 막고 cancellation을 전달한 뒤 thread 종료를 기다린다.

`CatalogEditorFacade`는 현재 Qt GUI를 위한 transitional facade다. `QObject`, Qt signal과 Qt value type을 사용하므로
Qt-free public API로 간주하지 않는다. `MainWindow`가 일부 Catalog/Export Orchestrator를 직접 받는 연결도 migration
중인 product boundary다.

## 4. Catalog와 Photo Identity

Catalog는 SQLite-backed persistent registry다. `PhotoId`는 content hash나 path가 아니라 **Catalog 내부에서 발급되는
immutable identity**다.

```text
Folder entry
    │ source path
    ▼
Editor activation
    │
    ├─ existing record ────────────────┐
    │                                 │
    └─ registerPhoto() → new PhotoId  │
                                      ▼
                              Editor session
                              ├─ DevelopParams
                              ├─ persisted revision
                              ├─ undo / redo history
                              └─ source-binding state
```

Identity 관련 값의 의미는 분리된다.

| 값 | 의미 |
|---|---|
| `PhotoId` | active Catalog 안에서 안정적인 SQLite identity |
| `SourceLocator` | 현재 source file의 변경 가능한 위치 |
| `SourceFingerprint` | size, timestamp와 optional SHA-256 관찰값 |
| `DevelopRevision` | optimistic save conflict를 검출하는 persisted revision |

Slider 조작은 Editor의 in-memory `DevelopParams`와 history를 바꾸지만 즉시 DB에 저장하지 않는다. 명시적인 save
flow가 `EditorOrchestrator → CatalogOrchestrator → Repository`로 내려가며, load 당시 revision과 DB revision이 다르면
덮어쓰지 않고 conflict를 반환한다.

Source path가 없어지거나 같은 path의 content가 바뀌어도 Photo identity와 Develop state는 보존한다.

```text
┌─ Processing allowed ──────────────────────────────────────┐
│ FingerprintPending · Available                            │
├─ Automatic verification in progress ─────────────────────-┤
│ VerificationRequired                                      │
├─ User resolution required ────────────────────────────────┤
│ Missing · IdentityUnverified · ReplacementDetected        │
│ Unreadable · Unlinked                                     │
└───────────────────────────────────────────────────────────┘
```

Processing 가능 여부와 사용자 resolution 필요 여부는 state machine의 domain policy가 판단한다. GUI는 기존 source
수용, 새 Photo 등록, relink command를 노출하지만 state correctness를 소유하지 않는다.

Catalog photo 목록은 stable cursor를 사용하는 bounded page로 조회한다. Catalog open이 모든 Photo와 thumbnail을 RAM에
적재한다는 의미는 아니다.

## 5. Editor와 Preview

Editor는 현재 선택, Develop state, undo/redo, persisted baseline과 preview scheduling policy를 소유한다. Preview
Orchestrator는 실제 background request lifecycle을 소유한다.

```text
User input
   │
   ▼
MainWindow / CatalogEditorFacade
   │ update DevelopParams
   ▼
EditorOrchestrator
   ├─ validate and update state/history
   ├─ interactive throttle / final debounce
   ├─ cancel superseded request
   └─ create immutable PreviewRequest
                    │
                    ▼
          PreviewOrchestrator
          ├─ RequestId
          ├─ CancellationSource
          └─ dedicated QThread
                    │
                    ▼
            FilePreviewPipeline
            RAW/cache → Develop → analysis → display frame
                    │
                    ▼
          request result / warning / terminal event
                    │
                    ▼
EditorOrchestrator current-result check
                    │
          current result only
                    ▼
                   UI
```

Preview correctness에는 서로 다른 두 방어가 있다.

1. **Cancellation**은 이미 가치가 없어진 계산을 가능한 한 일찍 중단한다.
2. **Stale-result validation**은 취소가 늦거나 결과 순서가 뒤바뀌어도 오래된 frame이 최신 UI를 덮지 못하게 한다.

현재 result 여부는 request, photo/source identity, Develop revision과 preview sequence 같은 값으로 확인한다. 사용자가
연속 조작하는 동안에는 latest-wins latency를 우선하고, 조작이 끝나면 final preview를 다시 요청한다.

## 6. Shared Processing Path

Preview와 Export는 서로 다른 lifecycle을 가지지만 같은 RAW/Develop/Color 의미를 사용한다. Worker가 재사용하는 가장
좁은 synchronous 경계는 `ResolvedRenderPipeline`이다.

```text
ResolvedRenderRequest
├─ absolute source path          process-local
├─ absolute output path          process-local
├─ resolved DevelopParams
└─ RasterExportOptions
             │
             ▼
      contract validation
             │
             ▼
        RAW decode
             │
             ▼
      source conversion
             │
             ▼
          Develop
             │
             ▼
       raster encode/write
             │
             ▼
ResolvedRenderResult
├─ artifact path / byte size
└─ per-stage RenderStats
```

Pipeline에는 Catalog lookup, UI, network, thread pool과 batch scheduling이 없다. Cancellation은 각 주요 stage 사이와
output 처리에 cooperative하게 전달된다. 실행 위치를 선택하는 layer가 processing intent를 변경해서는 안 된다.

## 7. Export Placement

`ExportOrchestrator`가 Desktop의 Export scheduling authority다. Local, Remote와 Auto는 별도 subsystem이 아니라 같은
immutable Export intent에 적용되는 placement policy다.

```text
Export request
    │
    ▼
item preparation
Catalog/default DevelopParams resolve
    │
    ▼
stable pending queue
    │
    ├──────── LocalOnly ───────▶ Local slots ───────▶ FileExportPipeline
    │
    ├──────── RemoteOnly ──────▶ Remote slots ──────▶ Remote adapter
    │
    └──────── Auto ────────────▶ eligible Local + Remote slots
```

Item lifecycle의 핵심은 execution owner가 최대 하나라는 점이다.

```text
                         ┌──────────────▶ RunningLocal ─────┐
                         │                                  │
Queued ──────────────────┤                                  ├─▶ terminal
                         │                                  │
                         └─▶ DispatchingRemote ─▶ RunningRemote
                                      │
                                      └─ NotStarted rejection
                                             │ cooldown
                                             └──────────────▶ Queued

terminal = Succeeded | Failed | Cancelled | Ambiguous
```

- Local/Remote slot은 각각 bounded다.
- Remote dispatch handshake는 직렬화하지만 accepted render 자체는 병렬 실행할 수 있다.
- `ServerBusy`, `ResourceBusy`, 접수 전 connection failure처럼 workload가 시작되지 않은 경우만 Auto에서 requeue할 수
  있다.
- 실행 시작 여부가 불명확한 timeout/disconnect는 `Ambiguous` terminal이며 Local에서 자동 재실행하지 않는다.
- Cancelled item은 다시 queue에 들어가지 않는다.
- UI의 enable/disable과 status는 Orchestrator state의 projection일 뿐 eligibility의 source of truth가 아니다.

## 8. Remote Shared-Storage Boundary

현재 Remote Render는 RAW byte upload나 artifact download를 수행하지 않는다. Desktop과 Worker가 같은 logical storage를
서로 다른 local path로 mount한 **operator-configured shared-storage 방식**이다.

```text
Desktop                                                    Worker
──────────────────────────────────────────────────────────────────────────
Z:\photos                                                  /mnt/photos
└─ .flexraw-storage.json                                   └─ configured
   storage_id = UUID                                          source root
└─ 2026\trip\a.ARW                                        └─ 2026/trip/a.ARW
        │                                                          ▲
        ▼                                                          │
SharedStorageLocator                                               │
├─ local root = Z:\photos                                          │
├─ storage ID = UUID                                               │
└─ relative = 2026/trip/a.ARW                                      │
        │                                                          │
        ▼                                                          │
Remote profile ID match                                            │
        │                                                          │
        ▼                                                          │
RemoteRenderRequestMapper                                          │
        │ wire: 2026/trip/a.ARW                                    │
        └──────────────── FRWK/TCP ────────────────────────────────┘
```

Desktop absolute path와 Worker absolute path는 wire를 통과하지 않는다. Marker의 `storage_id`는 logical storage identity일
뿐 credential이 아니다. Host, port, drive letter, mount path와 secret을 marker에 저장하지 않는다.

Remote preflight 순서는 다음과 같다.

```text
source/output marker 탐색
        → endpoint profile의 expected storage ID 확인
        → canonical root containment 확인
        → portable relative path mapping
        → TCP connection
```

Marker가 없거나 malformed이거나 profile과 ID가 다르면 network 연결 전에 typed failure로 끝난다. Local Export는 marker
유무와 관계없이 동작한다. Worker는 자신의 `--source-root`와 `--output-root` 안에서 relative path를 다시 canonicalize하고
root escape, missing source/output parent와 non-portable path를 거절한다.

현재 Worker startup은 marker UUID를 wire에서 검증하지 않는다. Desktop profile과 Worker root가 같은 storage layout을
가리키도록 구성하는 것은 trusted operator의 책임이다.

## 9. Worker Runtime

Worker는 `QCoreApplication` 기반의 headless executable이다.

```text
flexraw-worker
      │
      ▼
WorkerApplicationContext
├─ WorkerPathResolver
├─ ISystemMemoryProbe + current Platform Adapter
├─ ResolvedRenderPipeline
├─ PipelineRenderJobRunner
├─ ResourceAdmission
│  ├─ LocalMemoryAdmission
│  └─ ResourceRouterAdmission       configured mode
├─ AdmissionRenderJobRunner
├─ JobScheduler
│  ├─ private QThreadPool
│  ├─ bounded running slots
│  └─ bounded waiting queue
└─ WorkerServer
   └─ ProtocolSession per TCP connection
```

Network event-loop thread만 `QTcpSocket`과 session을 소유한다. Scheduler worker thread는 synchronous render를 수행하고,
completion은 queued delivery로 session context에 돌아온다.

Resource admission은 queue capacity와 별도다. Scheduler가 job을 수락했더라도 processing 시작 전에 local memory reserve
또는 외부 Resource Router lease를 확인할 수 있다. Lease mode에서는 workload 시작 전에 acquire하고, 실행 중 renew하며,
workload가 완전히 끝난 뒤 release한다.

종료도 bounded lifetime 계약을 따른다.

```text
신규 connection 차단
    → active session close / cancellation
    → scheduler 신규 submit 차단
    → queued/running cooperative cancellation
    → thread pool 종료 대기
    → processing dependency 파괴
```

## 10. Wire Protocol

Worker protocol은 native C++ struct memory를 그대로 전송하지 않는다.

| 항목 | 현재 계약 |
|---|---|
| Magic / version | `FRWK`, envelope `1.1` |
| Header | 24-byte, big-endian |
| Job identity | connection 안에서 unique한 unsigned `JobId` |
| Payload limit | frame당 1 MiB |
| Text | strict UTF-8 |
| Request path | portable slash-separated relative path |

Message flow는 다음과 같다.

```text
Client                                      Worker
  │                                            │
  ├──────────── RenderRequest ───────────────▶│
  │◀─────────── JobAccepted ──────────────────┤
  │                                            │
  │◀─────────── RenderSucceeded ──────────────┤
  │                  or                        │
  │◀─────────── RenderFailed/ResourceBusy ────┤
  │                                            │
  └──────────── CancelRequest ───────────────▶│

Queue가 가득 차면 JobAccepted 없이 ServerBusy를 반환한다.
```

Incremental parser는 잘린 header/payload를 다음 read와 결합하고, 한 read에 여러 frame이 합쳐져도 wire 순서대로
분리한다. Magic/version/size/path 위반은 terminal protocol error다.

현재 protocol에는 authentication과 TLS가 없다. 기본 loopback 또는 trusted operator network에서만 사용해야 하며,
untrusted Internet service용 security boundary가 아니다.

## 11. Thread와 Async Boundary

| 실행 context | 주요 책임 |
|---|---|
| Desktop GUI event loop | widget state, Facade event, Orchestrator command 호출 |
| Preview dedicated `QThread` | synchronous Preview Pipeline |
| Catalog fingerprint pool | SHA-256 baseline/verification과 source transition |
| Export preparation pool | request를 immutable item으로 해석 |
| Export Local pool | Local item processing |
| Export Remote pool | blocking Remote adapter/TCP execution |
| Worker network event loop | socket/session ownership과 frame delivery |
| Worker scheduler pool | admitted synchronous render execution |

Cross-thread callback은 immutable value를 전달하고 owner context로 serialize한다. Worker thread가 GUI widget이나 socket을
직접 조작하지 않는다. Request cancellation과 terminal result는 각각 정확히 한 번 수렴해야 한다.

## 12. CMake Target 구조

각 Core/UI leaf는 `STATIC` library이며, `flexraw_core`와 `flexraw_ui`는 Desktop용 aggregate `INTERFACE` target이다.

```text
flexraw.exe
└─ flexraw_app
   ├─ flexraw_ui_mainwindow
   ├─ flexraw_ui_facade
   ├─ flexraw_core_orchestration
   ├─ flexraw_core_preview
   ├─ flexraw_worker_export_adapter
   └─ flexraw_platform_current

flexraw-worker
└─ flexraw_worker_app
   ├─ flexraw_worker_network
   ├─ flexraw_worker_runtime
   ├─ flexraw_worker_admission
   ├─ flexraw_core_render
   └─ flexraw_platform_current
```

공개 `CMakePresets.json`의 기본 Debug/Release preset은 Desktop과 test를 만들고 Worker와 benchmark는 끈다. Worker는
`FLEXRAW_BUILD_WORKER=ON`인 별도 configure에서 opt-in으로 만들 수 있다.

## 13. Test 경계

Test는 다음 ownership 경계를 따라 분리한다.

```text
tests/core           domain algorithm · validation · persistence
tests/orchestration  request lifecycle · stale/cancel · placement
tests/worker         framing · parser · scheduler · admission · TCP
tests/ui             widget projection · GUI command wiring
tests/app            Composition Root · managed session lifecycle
```

실제 RAW나 OS symlink capability가 필요한 test는 fixture/capability가 없으면 명시적으로 skip한다. 일반 unit/integration
test가 fixture 부재를 success로 가장하지는 않는다.

## 14. 현재 제약

이 snapshot에서 다음은 의도적으로 완료된 architecture로 주장하지 않는다.

- Core와 frontend contract의 완전한 Qt-free 전환
- 독립 installable Engine package 또는 별도 Engine repository
- Project M:N persistence와 완성된 Library navigation
- RAW upload, artifact download와 cross-storage synchronization
- LAN discovery, multi-worker scheduling, durable retry/resume
- Remote protocol authentication, authorization과 TLS
- monitor별 wide-gamut/ICC presentation 전환
- 고급 Export frame/watermark와 전체 metadata policy
- ML/HDR/Panorama 등 디렉터리가 암시하는 모든 고급 product feature

새 abstraction은 현재 consumer가 실제로 요구하는 boundary에서만 추출한다. 특히 generic `ProcessingJob`, service
locator, global Orchestrator singleton 또는 범용 backend registry를 미리 만들지 않는다.

## 15. Source 안내

| 주제 | 시작 위치 |
|---|---|
| Desktop Composition Root | `src/app/application_context.*` |
| Managed Catalog startup | `src/app/managed_catalog_session.*` |
| Catalog/Editor GUI boundary | `src/ui/facade/catalog_editor_facade.*` |
| Catalog use case | `src/core/orchestration/catalog_orchestrator.*` |
| Editor/Preview use case | `src/core/orchestration/editor_orchestrator.*`, `preview_orchestrator.*` |
| Export placement | `src/core/orchestration/export_orchestrator.*` |
| Synchronous render seam | `src/core/render/resolved_render_pipeline.*` |
| Remote Desktop adapter | `src/worker/client/` |
| Worker protocol/runtime | `src/worker/protocol/`, `src/worker/runtime/`, `src/worker/network/` |
| Worker Composition Root | `src/worker/app/worker_application_context.*` |
| OS capability boundary | `src/platform/api/`, `src/platform/current/` |
