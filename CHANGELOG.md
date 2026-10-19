# FlexRAW 변경 내역

## 0.2.0

### 제품 기능

- Catalog 사진의 선별·제외 상태와 batch Export source 선택을 보완했습니다.
- Catalog에 저장된 사진별 Develop 값과 현재 Editor의 in-memory Develop 값을 Export에 적용합니다.
- General, Workers와 Export Settings의 validation과 화면 구성을 다듬었습니다.
- 선택한 사진을 Local JPEG로 내보내고 진행 상태를 조회·취소하는 MCP 도구를 추가했습니다.

### Processing과 실행 구성

- RAW, Develop, Color와 PNG encode의 neutral processing target을 추가했습니다.
- RAW 한 장을 PNG로 처리하는 `flexraw-cli`를 추가했습니다.
- bounded Float32 NR/Clarity/Sharpening processing과 project-owned RGBA8 storage를 적용했습니다.
- Windows display profile/target 경계와 cross-platform path 처리를 보완했습니다.
- Worker admission의 memory claim lifetime과 예외 처리를 수정했습니다.

### 배포와 검증

- Windows Desktop runtime을 clean directory에 staging하는 script를 추가했습니다.
- staged DLL의 vcpkg owner와 license notice, source revision 및 artifact checksum을 검증하는 public package finalizer를 추가했습니다.
- Windows Desktop/MCP/Debug, Linux x64 portable processing과 Linux ARM64 native processing 검증을 통과했습니다.

### 현재 제한

- HSL processing contract와 scalar implementation은 포함하지만 조작 UI는 아직 제공하지 않습니다.
- 첫 binary package는 Windows Desktop을 대상으로 하며 MCP, Worker와 CLI는 source/build capability로 제공합니다.
- Worker는 인증/TLS가 없는 trusted-LAN 구성입니다.

## 0.2.0-alpha.1

- M1.7 product acceptance와 repository review를 마친 source snapshot입니다.
- frontend-neutral Product Contract, standalone MCP consumer와 bounded Render Worker runtime을 포함합니다.
