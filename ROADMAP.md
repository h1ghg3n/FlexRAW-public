# FlexRAW Roadmap

이 문서는 현재 구현 상태와 다음 개발 gate를 설명합니다. 아직 구현되지 않은 아이디어를 현재 기능처럼 표시하지 않으며,
실제 source와 검증 결과를 최종 기준으로 사용합니다.

## 현재 상태

현재 checkpoint는 **M1.7 Local Product Acceptance & Freeze 완료**이며, 검토된 source snapshot 공개와
`0.2.0-alpha.1` version 갱신도 완료했습니다. Binary publication은 별도 검증과 승인이 필요한 gate입니다.

| Milestone | 범위 | 상태 |
|---|---|---|
| M1.1 | Persistent Editor | 완료 |
| M1.2 | Catalog Navigation & Project | 완료 |
| M1.3 | Qt-free Catalog/Editor Client Boundary | 완료 |
| M1.4 | Frontend-neutral Product Surface | 완료 |
| M1.5 | Qt-free Contract Build와 ARM64 lane | 완료 |
| M1.6 | Local STDIO MCP second consumer proof | 완료 |
| Post-M1.6 | Product Runtime / Adapter bridge | 완료 |
| M1.7 | Product acceptance, full review와 correction | 완료 |
| Public source publication | 검토된 M1.7 source snapshot 공개 | 완료 |
| Version checkpoint | Desktop/Worker/MCP `0.2.0-alpha.1` 표기 갱신 | 완료 |
| Binary publication | clean package, notice와 source 제공 검증 | 별도 승인 필요 |

## 완료된 구조 변화

### Frontend-neutral Product Contract

Catalog, Project, Editor, Preview presentation, thumbnail, source resolution, Worker profile/health, Export와 product setting의
현재 use case를 Qt Widget type 없이 표현할 수 있도록 정리했습니다. Qt Desktop은 이 contract의 실제 consumer입니다.

### Portable contract build

Desktop과 Worker를 끈 상태에서 Product Contract와 deterministic contract test를 Windows, WSL x64와 Linux ARM64에서
build할 수 있습니다. 여기서 portable은 무설치 package가 아니라 frontend package 없이 contract를 검증할 수 있다는
뜻입니다.

### MCP second consumer

Standalone local STDIO MCP process가 같은 Catalog/Editor contract를 사용합니다. Read surface는 기본으로 제공하고 mutation은
`--allow-write` opt-in이 있을 때만 광고합니다. EOF에서는 subscription을 먼저 닫고 Runtime을 종료합니다.

### Runtime / Adapter bridge

Desktop과 MCP Composition Root는 같은 `ProductRuntime` owner type을 사용합니다. TCP Server는 concrete scheduler가 아니라
`flexraw_worker_runtime_port`를 소비하며, wire identity는 `ProtocolSession`에서 Runtime identity로 변환합니다.

### M1.7 acceptance와 correction

Windows Desktop, MCP-only, Worker-only와 Qt-free contract build를 다시 검증했습니다. Catalog/Editor/Preview/Export lifetime,
Worker cancellation, path identity와 public license 경계를 검토하고 필요한 correction을 반영했습니다.

## 완료된 source 공개와 version 갱신

검토된 M1.7 source, test, build metadata와 공개용 문서를 반영했으며 Desktop/Worker/MCP의 version을
`0.2.0-alpha.1`로 동기화했습니다. Private fixture, credential, 내부 작업 문서와 local infrastructure 설정은 포함하지 않습니다.

Source 공개와 version 표기 완료는 version tag나 binary artifact 발행을 의미하지 않습니다.

## 후속 개발과 별도 publication gate

다음 항목은 완료된 M1.7을 다시 여는 작업이 아닙니다. 실제 workflow와 measurement에 필요한 vertical slice를 선택하며,
binary publication 승인은 product feature 개발을 시작하기 위한 선행 조건이 아닙니다.

### 1. Binary publication gate

Clean staging, 실제 배포 DLL/plugin/codec 목록, Qt와 third-party notice, source 제공 방법을 별도로 검증한 뒤에만
binary publication을 진행합니다. Version tag와 binary artifact 발행은 별도 승인 범위로 유지합니다.

### 2. Product interaction 보완

향후 vertical slice 후보는 다음과 같습니다.

- Catalog 등록파일 제외와 관련 사용자 확인 flow
- automatic save policy와 conflict UX
- zoom/pan, before/after와 geometry control
- rating, flag, label, filter와 adjustment copy/apply
- MCP Export 또는 결과 artifact를 다루는 bounded tool surface

각 기능은 먼저 하나의 실제 workflow로 구현하고, 두 번째 consumer에서 의미가 같다는 evidence가 생긴 뒤에만 공통
abstraction으로 승격합니다.

### 3. Processing과 resource 확장

Disk thumbnail cache, high-precision preview, wider color-management path, advanced Export option, Lensfun과 multi-image processing을
실제 measurement와 함께 점진적으로 추가할 수 있습니다.

Shared CPU pool, global worker count, priority/fairness, CPU reservation과 intra-image parallelism은 별도 설계 승인 없이
production policy로 확정하지 않습니다.

### 4. Engine extraction

검증된 RAW/develop/color/encode path를 먼저 monorepo 내부의 좁은 target으로 분리합니다. 독립 CLI, 다른 native frontend
또는 두 번째 product가 같은 contract를 실제로 사용할 때 installable package나 별도 repository 분리를 검토합니다.

미래 재사용 가능성만으로 generic scheduler, executor, plugin framework 또는 image abstraction을 선행하지 않습니다.

## 유지할 원칙

- Product Runtime이 authoritative state와 operation lifecycle을 소유합니다.
- Adapter는 business state를 복제하지 않으며 Adapter끼리 직접 의존하지 않습니다.
- Pipeline은 계산의 순서를 소유하지만 process-wide scheduling policy를 소유하지 않습니다.
- Identity, revision, cancellation, exact terminal, failure와 shutdown 의미를 구조 변경 중에도 보존합니다.
- Directory 대이동보다 target dependency와 lifetime boundary를 먼저 정리합니다.
- Qt 제거 자체를 목적으로 wrapper를 만들지 않고 실제 consumer와 build blocker가 있는 경계부터 이동합니다.
