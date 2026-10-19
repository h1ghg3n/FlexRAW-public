find_package(GTest CONFIG REQUIRED)

if(WIN32)
    find_program(FLEXRAW_POWERSHELL_EXECUTABLE NAMES pwsh REQUIRED)
    add_test(
        NAME WindowsReleaseStagingScriptTests
        COMMAND ${FLEXRAW_POWERSHELL_EXECUTABLE}
            -NoProfile
            -File ${PROJECT_SOURCE_DIR}/tests/scripts/stage_windows_release_tests.ps1
            -ScriptPath ${PROJECT_SOURCE_DIR}/scripts/stage-windows-release.ps1
            -WorkingRoot ${CMAKE_BINARY_DIR}/staging-script-tests
    )
    add_test(
        NAME WindowsPublicReleaseFinalizationScriptTests
        COMMAND ${FLEXRAW_POWERSHELL_EXECUTABLE}
            -NoProfile
            -File ${PROJECT_SOURCE_DIR}/tests/scripts/finalize_windows_public_release_tests.ps1
            -ScriptPath ${PROJECT_SOURCE_DIR}/scripts/finalize-windows-public-release.ps1
            -WorkingRoot ${CMAKE_BINARY_DIR}/public-release-finalization-script-tests
    )
endif()

add_library(flexraw_test_support INTERFACE)
target_include_directories(flexraw_test_support INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/support
)
target_link_libraries(flexraw_test_support INTERFACE
    flexraw_platform_api
)

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER OR FLEXRAW_BUILD_MCP)
    set(FLEXRAW_VCPKG_RELEASE_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    set(FLEXRAW_VCPKG_DEBUG_ROOT "${FLEXRAW_VCPKG_RELEASE_ROOT}/debug")
    set(FLEXRAW_VCPKG_RUNTIME_ROOT
        "$<IF:$<CONFIG:Debug>,${FLEXRAW_VCPKG_DEBUG_ROOT},${FLEXRAW_VCPKG_RELEASE_ROOT}>"
    )
    set(FLEXRAW_QT_PLUGIN_PATH "${FLEXRAW_VCPKG_RUNTIME_ROOT}/Qt6/plugins")
    set(FLEXRAW_VCPKG_BIN_PATH "${FLEXRAW_VCPKG_RUNTIME_ROOT}/bin")
endif()

if(FLEXRAW_BUILD_PORTABLE_TESTS)
    add_subdirectory(portable)
endif()

if(FLEXRAW_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER)
    add_subdirectory(platform)
endif()

if(FLEXRAW_BUILD_DESKTOP)
    add_subdirectory(core)
    add_subdirectory(orchestration)
    if(FLEXRAW_BUILD_UI_TESTS)
        add_subdirectory(ui)
    endif()
    add_subdirectory(app)
endif()

if(FLEXRAW_BUILD_MCP)
    add_subdirectory(mcp)
endif()

if(FLEXRAW_BUILD_WORKER)
    add_subdirectory(worker)
endif()
