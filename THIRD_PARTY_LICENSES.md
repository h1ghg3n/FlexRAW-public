# Third-Party License Inventory

Last reviewed for this public source snapshot: 2026-08-23

This is the dependency inventory for the current Flexraw public source snapshot. It is not a complete
binary notice bundle. Exact notices must be regenerated from the packages, plugins, codecs, databases,
models, and provider binaries that are actually distributed.

| Component | Planned/current use | License | Release requirement |
|---|---|---|---|
| Qt 6 | Core, Gui, Network, Widgets, Sql, Concurrent and image format plugins by profile | Module-specific open-source terms with separate commercial licensing | Public and private/commercial products must each select one valid Qt licensing path and preserve all required notices. |
| Exiv2 | Directly linked export metadata support in the public profile | `GPL-2.0-or-later` | Use the GPLv3-compatible route with Flexraw AGPLv3 code. A proprietary distributed profile must replace/exclude Exiv2 unless an actual upstream grant is independently verified. |
| LibRaw | Directly linked RAW decode | `LGPL-2.1-only OR CDDL-1.0` | Public Flexraw builds select `LGPL-2.1-only`; include the applicable source and notice material. |
| Little CMS | Directly linked ICC/color transforms | `MIT` | Preserve the MIT notice. |
| spdlog | Directly linked logging | `MIT` | Preserve the MIT notice. |
| fmt | Transitive spdlog dependency | `MIT` | Preserve the fmt MIT notice when included. |
| GNU libiconv/libcharset | Windows runtime dependency | `LGPL-2.1-or-later` for libraries and headers | Do not confuse the library terms with the separate GPL-covered `iconv` program. |
| OpenCV | Desktop preview and advanced processing modules | `Apache-2.0` | Include the Apache license, copyright, and upstream NOTICE material for deployed modules. |
| Lensfun | Planned desktop lens correction | `LGPL-3.0-only` library; `CC-BY-SA-3.0` database | Handle database attribution/ShareAlike separately; do not bundle Lensfun GPL applications. |
| Qt Image Formats | Desktop image format plugins | Qt and codec-specific terms | Collect notices for each deployed plugin and codec library. |
| GoogleTest | Build/test only | `BSD-3-Clause` | Exclude from runtime notices unless actually present in a release artifact. |
| ONNX Runtime | Optional ML backend | `MIT` | Include the matching `LICENSE` and `ThirdPartyNotices.txt` when enabled. |

No model weight artifact is staged here. A future model must be audited by exact file, not inferred from
the source repository name.

For a public binary release, enumerate the final package and copy the exact copyright/license files for
every deployed direct and transitive package into `licenses/`. The dependency graph can include codec
and utility packages that do not appear in the direct manifest.

Authoritative upstream sources:

- Qt: <https://www.qt.io/development/open-source-lgpl-obligations>
- Exiv2: <https://github.com/Exiv2/exiv2>
- LibRaw: <https://www.libraw.org/about>
- Little CMS: <https://github.com/mm2/Little-CMS>
- spdlog: <https://github.com/gabime/spdlog/blob/v1.x/LICENSE>
- GNU libiconv: <https://www.gnu.org/software/libiconv/>
- OpenCV: <https://github.com/opencv/opencv>
- Lensfun: <https://github.com/lensfun/lensfun>
- GoogleTest: <https://github.com/google/googletest/blob/main/LICENSE>
- ONNX Runtime: <https://github.com/microsoft/onnxruntime/blob/main/LICENSE>

This inventory is an engineering compliance record, not legal advice.
