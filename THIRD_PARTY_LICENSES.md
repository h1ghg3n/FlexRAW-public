# Third-Party License Inventory

Last audited: 2026-09-13

This file records the dependency and license inventory for the current Flexraw source tree. It is not
the complete notice bundle for a binary release. The exact release bundle must be generated from the
packages, plugins, codecs, models, and other artifacts that are actually distributed. Source publication
does not by itself certify a binary notice bundle.

Rows marked as inactive future candidates are retained as implementation and license checkpoints; they are not current build or
distribution dependencies.

Third-party components retain their own copyright and license terms. Neither Flexraw's public
AGPL license nor a separate private/commercial license can relicense those components.

## Current Dependencies

| Component | Current use | License | Distribution note |
|---|---|---|---|
| Qt 6 | Core, Gui, Network, Widgets, Sql, Concurrent and image format plugins by build profile | Module-specific open-source terms, primarily `LGPL-3.0-only` with GPL alternatives; commercial terms are separate | Audit every deployed Qt module, plugin, and bundled third-party notice. Public and commercial profiles must each use one valid Qt licensing path. |
| Exiv2 | Export metadata copy/include/exclude; directly linked by Desktop and Worker | `GPL-2.0-or-later` | The public AGPL profile selects a GPLv3-compatible route. The current Exiv2 project does not offer a new commercial license; a proprietary distributed profile must replace/exclude Exiv2 unless separately documented legacy rights are legally verified to cover the exact shipped code. Internal use does not grant redistribution rights. |
| LibRaw | RAW decode; directly linked | `LGPL-2.1-only OR CDDL-1.0` | The public AGPL build selects `LGPL-2.1-only`. Include the applicable source and notice material in each release. |
| Little CMS | ICC/color transforms; directly linked | `MIT` | Preserve the MIT copyright and permission notice. |
| spdlog | Logging; directly linked | `MIT` | Preserve the MIT notice. |
| fmt | Transitive formatting dependency of spdlog | `MIT` | Preserve the fmt MIT notice when its code or binary is included. |
| GNU libiconv/libcharset | Windows runtime dependency used by the Exiv2 build | `LGPL-2.1-or-later` for libraries and headers | The separate `iconv` program is GPL-covered and is not intended to be distributed by Flexraw. |
| OpenCV | Inactive future candidate; not declared, linked, or bundled by the current build profiles | `Apache-2.0` | Reintroduce only with the first real OpenCV vertical slice. If shipped later, include the Apache license, copyright, and any upstream NOTICE material for deployed modules. |
| Lensfun | Inactive future candidate; no Lensfun API or database is built or bundled | `LGPL-3.0-only` for libraries; `CC-BY-SA-3.0` for the lens database | Reintroduce only when lens correction is implemented. If the database is shipped later, include its attribution and ShareAlike terms separately. Lensfun applications are GPL-3.0 and are not intended to be bundled. |
| Qt Image Formats | Broad Desktop image-format post-build deployment; the exact plugin/codec subset is not yet a clean release package | Qt module terms plus codec-specific terms | Treat deployed plugins and codec libraries as release artifacts, remove unused plugins only after runtime smoke, and collect notices for the final staged subset. |
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
- Exiv2 commercial-license status: <https://github.com/Exiv2/exiv2/wiki/Commercial-License>
- LibRaw: <https://www.libraw.org/about>
- Little CMS: <https://github.com/mm2/Little-CMS>
- spdlog: <https://github.com/gabime/spdlog/blob/v1.x/LICENSE>
- GNU libiconv: <https://www.gnu.org/software/libiconv/>
- OpenCV: <https://github.com/opencv/opencv>
- Lensfun: <https://github.com/lensfun/lensfun>
- GoogleTest: <https://github.com/google/googletest/blob/main/LICENSE>
- ONNX Runtime: <https://github.com/microsoft/onnxruntime/blob/main/LICENSE>

This inventory is an engineering compliance record, not legal advice. A binary release must be audited
against its final staged artifacts and should receive legal review when the distribution model,
dependency set, or contributor model changes.
