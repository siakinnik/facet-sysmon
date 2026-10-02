cmake_minimum_required(VERSION 3.18)
project(facet-sysmon VERSION 0.1.0 LANGUAGES CXX)
set(SYSMON_VERSION_SUFFIX "alpha")  # pre-release tag; empty for final releases; keep manifest.json in sync

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(SYSMON_STATIC "Link fully static (portable release binaries)" OFF)

if(SYSMON_VERSION_SUFFIX)
    set(SYSMON_VERSION "${PROJECT_VERSION}-${SYSMON_VERSION_SUFFIX}")
else()
    set(SYSMON_VERSION "${PROJECT_VERSION}")
endif()

# The plugin SDK lives in facet-core: a local checkout next to this project
# (or FACET_CORE_DIR) is used when present, otherwise it is downloaded.
# Charts need the arc/poly/polyline canvas ops of facet-core 0.2.
set(FACET_CORE_DIR "" CACHE PATH "facet-core checkout providing the SDK (default: ../facet-core, else download)")
set(FACET_CORE_REF "v0.3.0-alpha" CACHE STRING "facet-core git ref to download the SDK from")
if(NOT FACET_CORE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../facet-core/sdk/CMakeLists.txt")
    set(FACET_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../facet-core")
endif()
if(FACET_CORE_DIR)
    add_subdirectory(${FACET_CORE_DIR}/sdk ${CMAKE_BINARY_DIR}/facet_sdk)
else()
    include(FetchContent)
    FetchContent_Declare(facet_core
        GIT_REPOSITORY https://github.com/siakinnik/facet-core.git
        GIT_TAG ${FACET_CORE_REF}
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR sdk)
    FetchContent_MakeAvailable(facet_core)
endif()

add_library(sysmon_stats STATIC src/stats.cpp)
target_include_directories(sysmon_stats PUBLIC src)
target_compile_options(sysmon_stats PRIVATE -Wall -Wextra)

add_executable(sysmon src/main.cpp src/i18n/ru.cpp)
target_link_libraries(sysmon PRIVATE sysmon_stats facet_sdk)
target_compile_options(sysmon PRIVATE -Wall -Wextra)
target_compile_definitions(sysmon PRIVATE SYSMON_VERSION="${SYSMON_VERSION}")
if(SYSMON_STATIC)
    target_link_options(sysmon PRIVATE -static -s)
endif()

# The build dir doubles as a plugin dir: FACET_PLUGIN_PATH=<build>
set(PLUGIN_OUT ${CMAKE_BINARY_DIR}/sysmon.plugin)
set_target_properties(sysmon PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PLUGIN_OUT})
facet_plugin_manifest(${CMAKE_CURRENT_SOURCE_DIR}/manifest.json ${PLUGIN_OUT}/manifest.json)

enable_testing()
add_executable(stats_test tests/stats_test.cpp)
target_link_libraries(stats_test PRIVATE sysmon_stats)
add_test(NAME stats COMMAND stats_test)

install(TARGETS sysmon RUNTIME DESTINATION lib/facet/plugins/sysmon)
install(FILES ${PLUGIN_OUT}/manifest.json DESTINATION lib/facet/plugins/sysmon)
