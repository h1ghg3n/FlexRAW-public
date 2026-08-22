# core — 모듈별 STATIC library 구성
# flexraw_core 는 INTERFACE library 로, 모든 sub 모듈을 묶어 노출한다.
#
# 의존 방향:
#   types  ← util  ← (catalog/raw/develop/color/preview/ml/import/export/lens/hdr/pano/history/preset/liquify)

# 공통 의존성 (sub 모듈에서 직접 find_package 하지 않고 부모에서 한 번만)
find_package(libraw CONFIG REQUIRED)
find_package(lcms2 CONFIG REQUIRED)
find_package(exiv2 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui)

# ONNX Runtime — pre-built 바이너리 (vcpkg source build 회피)
if(FLEXRAW_BUILD_DESKTOP)
    find_package(OpenCV REQUIRED)

    if(NOT ONNXRUNTIME_ROOT AND DEFINED ENV{ONNXRUNTIME_ROOT})
        set(ONNXRUNTIME_ROOT $ENV{ONNXRUNTIME_ROOT})
    endif()
    if(ONNXRUNTIME_ROOT)
        add_library(onnxruntime SHARED IMPORTED)
        set_target_properties(onnxruntime PROPERTIES
            IMPORTED_IMPLIB "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
            IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/bin/onnxruntime.dll"
            INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_ROOT}/include"
        )
        set(ONNXRUNTIME_FOUND TRUE)
    else()
        message(STATUS "ONNXRUNTIME_ROOT not set — ML inference disabled")
        set(ONNXRUNTIME_FOUND FALSE)
    endif()
else()
    set(ONNXRUNTIME_FOUND FALSE)
endif()

# 기반 모듈 (다른 모듈이 의존)
add_subdirectory(measurement)
add_subdirectory(types)
add_subdirectory(util)

# Worker와 Desktop이 공유하는 resolved render dependency
add_subdirectory(raw)
add_subdirectory(develop)
add_subdirectory(color)
add_subdirectory(export)
add_subdirectory(render)

if(FLEXRAW_BUILD_DESKTOP)
    add_subdirectory(preset)
    add_subdirectory(catalog)
    add_subdirectory(preview)
    add_subdirectory(ml)
    add_subdirectory(import)
    add_subdirectory(lens)
    add_subdirectory(hdr)
    add_subdirectory(pano)
    add_subdirectory(history)
    add_subdirectory(liquify)

    # cross-module use case layer (domain leaf가 이 target에 역의존하면 안 됨)
    add_subdirectory(orchestration)

    # 메타 타겟 — Desktop용 모든 Core sub 모듈을 묶음
    add_library(flexraw_core INTERFACE)
    target_link_libraries(flexraw_core INTERFACE
        flexraw_core_measurement
        flexraw_core_types
        flexraw_core_util
        flexraw_core_catalog
        flexraw_core_raw
        flexraw_core_develop
        flexraw_core_color
        flexraw_core_preview
        flexraw_core_ml
        flexraw_core_import
        flexraw_core_export
        flexraw_core_render
        flexraw_core_lens
        flexraw_core_hdr
        flexraw_core_pano
        flexraw_core_history
        flexraw_core_preset
        flexraw_core_liquify
        flexraw_core_orchestration
    )
endif()
