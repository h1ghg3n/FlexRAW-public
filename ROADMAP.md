# FlexRAW Roadmap

이 문서는 공개 source에서 확인할 수 있는 현재 기능과 가까운 제품 개발 범위를 요약합니다. 세부 구현 상태는 실제 source와
test를 기준으로 합니다.

## 0.2.0 현재 상태

| 영역 | 현재 상태 |
|---|---|
| Windows Desktop | Catalog, Editor, Preview, Settings와 Local/Remote/Auto Export 제공 |
| Catalog | Folder import, Project, 선별·제외, source replacement/relink와 사진별 Develop 저장 지원 |
| Processing | RAW decode, 기본 Develop, color transform와 JPEG/PNG/TIFF output 지원 |
| Batch Export | 현재 선택 및 Catalog에 저장된 사진별 Develop 값 적용 |
| Terminal CLI | RAW 한 장을 neutral processing 경로로 처리해 PNG로 기록 |
| MCP | Catalog/Project/Editor와 선택 사진의 bounded Local JPEG Export 제공 |
| Render Worker | Windows, Linux x64와 Linux ARM64에서 제한된 trusted-LAN workflow 검증 |
| Portable processing | Windows x64, Linux x64와 Linux ARM64에서 no-Qt build/test 제공 |

## 다음 제품 범위

### 편집과 표시

- HSL 조작 UI와 product workflow 연결
- crop, rotate, straighten과 before/after 비교
- monitor별 ICC와 high-bit-depth display 검증
- 정밀/full-resolution preview 단계

### Catalog workflow

- automatic save와 conflict 처리
- rating, flag, label과 filter
- adjustment copy/apply와 preset workflow 보완
- disk thumbnail cache

### Export와 외부 사용

- 고급 JPEG/TIFF/PNG option
- destination과 artifact publication 확장
- MCP Export 결과 조회 범위 보완
- Worker deployment와 storage workflow 보완

### Platform

- Linux Desktop 검증
- macOS frontend와 platform integration
- Windows/Linux ARM64 package 검증 확대

## 현재 지원하지 않는 범위

- Worker 인증/TLS와 public internet 운영
- 자동 Worker discovery와 multi-worker scheduling
- Lensfun lens correction
- HDR/panorama와 ML inference
- 독립 설치형 processing engine 또는 plugin SDK

현재 지원 범위와 설치·실행 방법은 [README.md](README.md), source 구조는 [architecture.md](architecture.md)를 참고해 주세요.
