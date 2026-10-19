# Third-Party License Inventory

Last audited: 2026-10-19

This file records the dependency and license inventory for the current Flexraw source tree. It is not
the complete notice bundle for a binary release. The exact release bundle must be generated from the
packages, plugins, codecs, models, and other artifacts that are actually distributed. Source publication
does not by itself certify a binary notice bundle.

Rows marked as inactive future candidates are retained as implementation and license checkpoints; they are not current build or
distribution dependencies.

Third-party components retain their own copyright and license terms. FlexRAW's license does not
alter or replace those terms.

## Current Dependencies

| Component | Current use | License | Distribution note |
|---|---|---|---|
| Qt 6 | Core, Gui, Network, Widgets, Sql, Concurrent and image format plugins by build profile | Module-specific open-source terms, primarily `LGPL-3.0-only` with GPL alternatives | Audit every deployed Qt module, plugin, and bundled third-party notice and comply with the applicable open-source terms. |
| Exiv2 | Export metadata copy/include/exclude; directly linked by Desktop and Worker | `GPL-2.0-or-later` | FlexRAW's `AGPL-3.0-or-later` distribution uses a GPLv3-compatible route. Include the corresponding source and license notices in each binary release. |
| LibRaw | RAW decode; directly linked | `LGPL-2.1-only OR CDDL-1.0` | The public AGPL build selects `LGPL-2.1-only`. Include the applicable source and notice material in each release. |
| Little CMS | ICC/color transforms; directly linked | `MIT` | Preserve the MIT copyright and permission notice. |
| libpng | Portable PNG8/sRGB encoding; directly linked by portable processing and Terminal CLI profiles | `libpng-2.0` | Preserve the libpng copyright and license notice. |
| spdlog | Logging; directly linked | `MIT` | Preserve the MIT notice. |
| fmt | Transitive formatting dependency of spdlog | `MIT` | Preserve the fmt MIT notice when its code or binary is included. |
| GNU libiconv/libcharset | Windows runtime dependency used by the Exiv2 build | `LGPL-2.1-or-later` for libraries and headers | The separate `iconv` program is GPL-covered and is not intended to be distributed by Flexraw. |
| OpenCV | Inactive future candidate; not declared, linked, or bundled by the current build profiles | `Apache-2.0` | Reintroduce only with the first real OpenCV vertical slice. If shipped later, include the Apache license, copyright, and any upstream NOTICE material for deployed modules. |
| Lensfun | Inactive future candidate; no Lensfun API or database is built or bundled | `LGPL-3.0-only` for libraries; `CC-BY-SA-3.0` for the lens database | Reintroduce only when lens correction is implemented. If the database is shipped later, include its attribution and ShareAlike terms separately. Lensfun applications are GPL-3.0 and are not intended to be bundled. |
| Qt Image Formats | Windows Desktop staging retains the `windeployqt` image-format plugin set while the product exposes JPEG/PNG/TIFF | Qt module terms plus codec-specific terms | The release finalizer maps staged DLLs to their vcpkg owners and collects the matching package notice/SPDX material. |
| GoogleTest | Build and test only | `BSD-3-Clause` | Do not include it in runtime notices unless a release artifact actually contains it. |
| ONNX Runtime | Inactive future candidate; no imported target, inference implementation, or binary is configured or bundled | `MIT` | Reintroduce only with the first real inference slice. When enabled in a release, include both `LICENSE` and the matching `ThirdPartyNotices.txt`. Provider-specific packages can add further notices. |

## ML Model Artifacts

No `.onnx`, `.pt`, `.pth`, `.engine`, or `.tflite` model artifact is currently stored in this repository.
Model names in design documents are candidates, not bundled components. Before adding any model, verify
the exact weight artifact's license, training-data restrictions, acceptable-use terms, attribution, and
redistribution rights. Add only the model that is actually shipped to the release notice bundle.

## Transitive Dependencies

The vcpkg graph can include codec and utility packages such as zlib, libjpeg-turbo, libpng, TIFF,
libwebp, expat, brotli, inih, and others. This list changes with the triplet, feature set, Qt build,
and package baseline, so this inventory intentionally does not pretend to be an exhaustive binary
manifest.

For every public binary release:

1. Freeze the source commit, vcpkg baseline, triplet, CMake profile, and optional backends.
2. Enumerate the DLLs, shared libraries, Qt plugins, databases, models, and resources in the package.
3. Copy the matching `vcpkg_installed/<triplet>/share/<port>/copyright` files for every deployed package.
4. Add license and notice files from dependencies managed outside vcpkg, including ONNX Runtime when it is actually enabled.
5. Verify that the assembled notice bundle matches the final package after deployment tooling runs.

## Authoritative Upstream Sources

- Qt licensing: <https://doc.qt.io/qt-6/licensing.html>
- Qt SBOM: <https://doc.qt.io/qt-6/sbom.html>
- Exiv2: <https://github.com/Exiv2/exiv2>
- LibRaw: <https://www.libraw.org/about>
- Little CMS: <https://github.com/mm2/Little-CMS>
- libpng: <https://github.com/pnggroup/libpng>
- spdlog: <https://github.com/gabime/spdlog/blob/v1.x/LICENSE>
- GNU libiconv: <https://www.gnu.org/software/libiconv/>
- OpenCV: <https://github.com/opencv/opencv>
- Lensfun: <https://github.com/lensfun/lensfun>
- GoogleTest: <https://github.com/google/googletest/blob/main/LICENSE>
- ONNX Runtime: <https://github.com/microsoft/onnxruntime/blob/main/LICENSE>

This inventory is an engineering compliance record, not legal advice. A binary release must be audited
against its final staged artifacts and should receive legal review when the distribution model,
dependency set, or contributor model changes.
