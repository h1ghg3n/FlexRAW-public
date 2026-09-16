# core — 모듈별 STATIC library 구성
# flexraw_core 는 INTERFACE library 로, 모든 sub 모듈을 묶어 노출한다.
#
# 의존 방향:
#   types  ← util  ← (catalog/raw/develop/color/preview/ml/import/export/lens/hdr/pano/history/preset/liquify)

# Product Contract는 Qt와 processing dependency 없이 독립적으로 구성한다.
add_subdirectory(client)

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER OR FLEXRAW_BUILD_MCP)
    # 공통 의존성 (sub 모듈에서 직접 find_package 하지 않고 부모에서 한 번만)
    find_package(libraw CONFIG REQUIRED)
    find_package(lcms2 CONFIG REQUIRED)
    find_package(exiv2 CONFIG REQUIRED)
    find_package(spdlog CONFIG REQUIRED)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui)

# 기반 모듈 (다른 모듈이 의존)
    add_subdirectory(measurement)
    add_subdirectory(types)
    add_subdirectory(util)

    # Worker/Desktop과 현재 bundled Orchestration을 사용하는 MCP가 공유하는 resolved render dependency
    add_subdirectory(raw)
    add_subdirectory(develop)
    add_subdirectory(color)
    add_subdirectory(export)
    add_subdirectory(render)

    if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_MCP)
        add_subdirectory(preset)
        add_subdirectory(catalog)
        add_subdirectory(preview)
        add_subdirectory(history)

        # cross-module use case layer (domain leaf가 이 target에 역의존하면 안 됨)
        add_subdirectory(orchestration)
    endif()

    if(FLEXRAW_BUILD_DESKTOP)
        # 메타 타겟 — Desktop용 모든 Core sub 모듈을 묶음
        add_library(flexraw_core INTERFACE)
        target_link_libraries(flexraw_core INTERFACE
            flexraw_core_measurement
            flexraw_core_types
            flexraw_core_client
            flexraw_core_util
            flexraw_core_catalog
            flexraw_core_raw
            flexraw_core_develop
            flexraw_core_color
            flexraw_core_preview
            flexraw_core_export
            flexraw_core_render
            flexraw_core_history
            flexraw_core_preset
            flexraw_core_orchestration
        )
    endif()
endif()
