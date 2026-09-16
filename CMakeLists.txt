cmake_minimum_required(VERSION 3.25)
project(flexraw VERSION 0.1.0 LANGUAGES CXX)

set(FLEXRAW_VERSION_PRERELEASE "alpha.5")
set(FLEXRAW_VERSION_STRING "${PROJECT_VERSION}-${FLEXRAW_VERSION_PRERELEASE}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FLEXRAW_BUILD_DESKTOP "Build the Flexraw desktop application" ON)
option(FLEXRAW_BUILD_WORKER "Build the bounded render worker" OFF)
option(FLEXRAW_BUILD_MCP "Build the local STDIO MCP second consumer" ${FLEXRAW_BUILD_DESKTOP})
option(FLEXRAW_BUILD_TESTS "Build Flexraw test targets" ON)
option(FLEXRAW_BUILD_UI_TESTS "Build Qt Widgets UI tests" ON)
option(FLEXRAW_BUILD_PORTABLE_TESTS "Build Qt-free product contract tests" ON)
option(FLEXRAW_BUILD_BENCHMARKS "Build opt-in runtime measurement executables" OFF)
option(FLEXRAW_WINDOWS_DEBUG_CONSOLE "Show the Windows desktop debug console" OFF)

if(NOT FLEXRAW_BUILD_DESKTOP AND NOT FLEXRAW_BUILD_WORKER AND NOT FLEXRAW_BUILD_MCP AND
   NOT (FLEXRAW_BUILD_TESTS AND FLEXRAW_BUILD_PORTABLE_TESTS))
    message(FATAL_ERROR
        "Enable Desktop, Worker, MCP, or FLEXRAW_BUILD_TESTS with FLEXRAW_BUILD_PORTABLE_TESTS."
    )
endif()

if(MSVC)
    add_compile_options(/permissive- /W4)
endif()

if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER OR FLEXRAW_BUILD_MCP)
    set(FLEXRAW_QT_COMPONENTS Core Gui)
    if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_WORKER)
        list(APPEND FLEXRAW_QT_COMPONENTS Network)
    endif()
    if(FLEXRAW_BUILD_DESKTOP OR FLEXRAW_BUILD_MCP)
        list(APPEND FLEXRAW_QT_COMPONENTS Concurrent Sql)
    endif()
    if(FLEXRAW_BUILD_DESKTOP)
        list(APPEND FLEXRAW_QT_COMPONENTS Widgets)
    endif()
    list(REMOVE_DUPLICATES FLEXRAW_QT_COMPONENTS)
    find_package(Qt6 REQUIRED COMPONENTS ${FLEXRAW_QT_COMPONENTS})
    qt_standard_project_setup()
endif()

add_subdirectory(src)

if(FLEXRAW_BUILD_DESKTOP)
qt_add_executable(flexraw src/main.cpp)

if(WIN32)
    if(FLEXRAW_WINDOWS_DEBUG_CONSOLE)
        set_target_properties(flexraw PROPERTIES WIN32_EXECUTABLE FALSE)
    else()
        set_target_properties(flexraw PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
endif()

target_link_libraries(flexraw PRIVATE
    flexraw_app
    flexraw_core_util
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

if(WIN32)
    qt_generate_deploy_app_script(
        TARGET flexraw
        OUTPUT_SCRIPT flexraw_deploy_script
        INCLUDE_PLUGIN_TYPES platforms imageformats
    )
    add_custom_command(TARGET flexraw POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DQT_DEPLOY_PREFIX=$<TARGET_FILE_DIR:flexraw>
            -DQT_DEPLOY_BIN_DIR=.
            -DQT_DEPLOY_LIB_DIR=.
            -DQT_DEPLOY_PLUGINS_DIR=plugins
            -P ${flexraw_deploy_script}
        WORKING_DIRECTORY $<TARGET_FILE_DIR:flexraw>
        COMMENT "Deploying Qt runtime dependencies"
    )

    set(FLEXRAW_IMAGEFORMAT_RELEASE_RUNTIME_DLL_NAMES
        jasper.dll
        jpeg62.dll
        tiff.dll
        liblzma.dll
        libsharpyuv.dll
        libwebp.dll
        libwebpdemux.dll
        libwebpmux.dll
    )
    set(FLEXRAW_IMAGEFORMAT_DEBUG_RUNTIME_DLL_NAMES
        jasperd.dll
        jpeg62.dll
        tiffd.dll
        liblzma.dll
        libsharpyuv.dll
        libwebp.dll
        libwebpdemux.dll
        libwebpmux.dll
    )
    set(FLEXRAW_IMAGEFORMAT_RUNTIME_DLLS)
    list(LENGTH FLEXRAW_IMAGEFORMAT_RELEASE_RUNTIME_DLL_NAMES FLEXRAW_IMAGEFORMAT_RUNTIME_DLL_COUNT)
    math(EXPR FLEXRAW_IMAGEFORMAT_RUNTIME_DLL_LAST_INDEX "${FLEXRAW_IMAGEFORMAT_RUNTIME_DLL_COUNT} - 1")
    foreach(FLEXRAW_IMAGEFORMAT_RUNTIME_DLL_INDEX RANGE ${FLEXRAW_IMAGEFORMAT_RUNTIME_DLL_LAST_INDEX})
        list(GET FLEXRAW_IMAGEFORMAT_RELEASE_RUNTIME_DLL_NAMES
            ${FLEXRAW_IMAGEFORMAT_RUNTIME_DLL_INDEX}
            FLEXRAW_IMAGEFORMAT_RELEASE_RUNTIME_DLL_NAME
        )
        list(GET FLEXRAW_IMAGEFORMAT_DEBUG_RUNTIME_DLL_NAMES
            ${FLEXRAW_IMAGEFORMAT_RUNTIME_DLL_INDEX}
            FLEXRAW_IMAGEFORMAT_DEBUG_RUNTIME_DLL_NAME
        )
        set(FLEXRAW_IMAGEFORMAT_RELEASE_RUNTIME_DLL
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/${FLEXRAW_IMAGEFORMAT_RELEASE_RUNTIME_DLL_NAME}"
        )
        set(FLEXRAW_IMAGEFORMAT_DEBUG_RUNTIME_DLL
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin/${FLEXRAW_IMAGEFORMAT_DEBUG_RUNTIME_DLL_NAME}"
        )
        if(NOT EXISTS "${FLEXRAW_IMAGEFORMAT_RELEASE_RUNTIME_DLL}")
            message(FATAL_ERROR
                "Required Release image format dependency is missing: ${FLEXRAW_IMAGEFORMAT_RELEASE_RUNTIME_DLL}"
            )
        endif()
        if(NOT EXISTS "${FLEXRAW_IMAGEFORMAT_DEBUG_RUNTIME_DLL}")
            message(FATAL_ERROR
                "Required Debug image format dependency is missing: ${FLEXRAW_IMAGEFORMAT_DEBUG_RUNTIME_DLL}"
            )
        endif()
        list(APPEND FLEXRAW_IMAGEFORMAT_RUNTIME_DLLS
            "$<IF:$<CONFIG:Debug>,${FLEXRAW_IMAGEFORMAT_DEBUG_RUNTIME_DLL},${FLEXRAW_IMAGEFORMAT_RELEASE_RUNTIME_DLL}>"
        )
    endforeach()

    add_custom_command(TARGET flexraw POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FLEXRAW_IMAGEFORMAT_RUNTIME_DLLS}
            $<TARGET_FILE_DIR:flexraw>
        COMMENT "Deploying vcpkg image format plugin dependencies"
    )
endif()
endif()

if(FLEXRAW_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FLEXRAW_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
