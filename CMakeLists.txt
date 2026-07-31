cmake_minimum_required(VERSION 3.25)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RACEBOX_RELEASE_VERSION LIMIT_COUNT 1)
string(STRIP "${RACEBOX_RELEASE_VERSION}" RACEBOX_RELEASE_VERSION)
if(NOT RACEBOX_RELEASE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)\\.([0-9][0-9][0-9])$")
  message(FATAL_ERROR "VERSION must use major.minor.patch.build with a three-digit build number")
endif()
set(RACEBOX_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(RACEBOX_VERSION_MINOR "${CMAKE_MATCH_2}")
set(RACEBOX_VERSION_PATCH "${CMAKE_MATCH_3}")
set(RACEBOX_VERSION_BUILD_PADDED "${CMAKE_MATCH_4}")
string(REGEX REPLACE "^0+" "" RACEBOX_VERSION_BUILD "${RACEBOX_VERSION_BUILD_PADDED}")
if(RACEBOX_VERSION_BUILD STREQUAL "")
  set(RACEBOX_VERSION_BUILD 0)
endif()

project(RaceBoxViewerNative VERSION
  ${RACEBOX_VERSION_MAJOR}.${RACEBOX_VERSION_MINOR}.${RACEBOX_VERSION_PATCH}.${RACEBOX_VERSION_BUILD}
  LANGUAGES C CXX)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(resources/racebox_version.h.in generated/racebox_version.h @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
include(CTest)

FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.8-docking
  GIT_SHALLOW TRUE)
FetchContent_Declare(implot
  GIT_REPOSITORY https://github.com/epezent/implot.git
  GIT_TAG d65a2bef53d32502407de3a4be80f191e2f412d7)
FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE)
FetchContent_Declare(miniz_src
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 77d0dce8627735138c51770d1799a1ef48f2117d)
FetchContent_MakeAvailable(imgui implot json miniz_src)

add_library(imgui_native STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui_native PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui_native PUBLIC IMGUI_ENABLE_DOCKING IMGUI_DEFINE_MATH_OPERATORS)

add_library(implot_native STATIC
  ${implot_SOURCE_DIR}/implot.cpp
  ${implot_SOURCE_DIR}/implot_items.cpp)
target_include_directories(implot_native PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot_native PUBLIC imgui_native)

add_library(racebox_core STATIC
  src/core/parsers.cpp
  src/core/analysis.cpp
  src/core/crew_chief.cpp
  src/core/driver_analysis.cpp
  src/core/imu_analysis.cpp
  src/core/insight_evidence.cpp
  src/core/persistence.cpp
  src/core/race_day.cpp
  src/core/settings.cpp)
target_include_directories(racebox_core PUBLIC include)
target_link_libraries(racebox_core PUBLIC nlohmann_json::nlohmann_json miniz)
target_link_libraries(racebox_core PUBLIC shell32 ole32 winhttp)
target_compile_definitions(racebox_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(racebox_core PRIVATE /W4 /permissive- /EHsc)

add_executable(racebox_cli src/cli/main.cpp)
target_link_libraries(racebox_cli PRIVATE racebox_core)

add_executable(RaceBoxViewer WIN32
  src/app/main.cpp
  src/app/native_app.cpp
  src/app/ui_preferences.cpp
  src/app/ui_theme.cpp
  src/app/file_dialog.cpp
  src/app/texture_loader.cpp
  resources/RaceBoxViewer.rc)
target_include_directories(RaceBoxViewer PRIVATE include src/app "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(RaceBoxViewer PRIVATE racebox_core imgui_native implot_native d3d11 dxgi d3dcompiler windowscodecs comdlg32 shell32 ole32 dwmapi dbghelp psapi)
target_compile_definitions(RaceBoxViewer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX IMGUI_DEFINE_MATH_OPERATORS)
target_compile_options(RaceBoxViewer PRIVATE /W4 /permissive- /EHsc)

add_executable(racebox_tests tests/core_tests.cpp)
target_link_libraries(racebox_tests PRIVATE racebox_core)
target_compile_definitions(racebox_tests PRIVATE GOLDEN_DIR=L"${CMAKE_SOURCE_DIR}/golden")
add_test(NAME racebox_core_golden COMMAND racebox_tests)

add_executable(racebox_driver_analysis_tests tests/driver_analysis_tests.cpp)
target_link_libraries(racebox_driver_analysis_tests PRIVATE racebox_core)
target_compile_definitions(racebox_driver_analysis_tests PRIVATE GOLDEN_DIR=L"${CMAKE_SOURCE_DIR}/golden")
add_test(NAME racebox_driver_analysis COMMAND racebox_driver_analysis_tests)

add_executable(racebox_insight_evidence_tests tests/insight_evidence_tests.cpp)
target_link_libraries(racebox_insight_evidence_tests PRIVATE racebox_core)
target_compile_definitions(racebox_insight_evidence_tests PRIVATE GOLDEN_DIR=L"${CMAKE_SOURCE_DIR}/golden")
add_test(NAME racebox_insight_evidence_addon COMMAND racebox_insight_evidence_tests)

add_executable(racebox_dx11_tests tests/dx11_tests.cpp)
target_link_libraries(racebox_dx11_tests PRIVATE d3d11 dxgi)
target_compile_definitions(racebox_dx11_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME racebox_dx11_warp COMMAND racebox_dx11_tests)

add_executable(racebox_memory_tests tests/memory_tests.cpp)
target_link_libraries(racebox_memory_tests PRIVATE racebox_core psapi)
target_compile_definitions(racebox_memory_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME racebox_one_million_samples COMMAND racebox_memory_tests)

add_executable(racebox_plot_order_tests tests/plot_order_tests.cpp)
target_include_directories(racebox_plot_order_tests PRIVATE include)
add_test(NAME racebox_plot_order COMMAND racebox_plot_order_tests)

add_executable(racebox_ui_preferences_tests
  tests/ui_preferences_tests.cpp
  src/app/ui_preferences.cpp)
target_include_directories(racebox_ui_preferences_tests PRIVATE include src/app)
target_link_libraries(racebox_ui_preferences_tests PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(racebox_ui_preferences_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(racebox_ui_preferences_tests PRIVATE /W4 /permissive- /EHsc)
add_test(NAME racebox_ui_preferences COMMAND racebox_ui_preferences_tests)

add_executable(racebox_imu_analysis_tests tests/imu_analysis_tests.cpp)
target_link_libraries(racebox_imu_analysis_tests PRIVATE racebox_core)
add_test(NAME racebox_imu_analysis COMMAND racebox_imu_analysis_tests)

add_executable(racebox_crew_chief_tests tests/crew_chief_tests.cpp)
target_link_libraries(racebox_crew_chief_tests PRIVATE racebox_core)
target_compile_definitions(racebox_crew_chief_tests PRIVATE GOLDEN_DIR=L"${CMAKE_SOURCE_DIR}/golden")
add_test(NAME racebox_crew_chief COMMAND racebox_crew_chief_tests)

add_executable(racebox_race_day_tests tests/race_day_tests.cpp)
target_link_libraries(racebox_race_day_tests PRIVATE racebox_core)
target_compile_definitions(racebox_race_day_tests PRIVATE GOLDEN_DIR=L"${CMAKE_SOURCE_DIR}/golden")
add_test(NAME racebox_race_day COMMAND racebox_race_day_tests)

install(TARGETS RaceBoxViewer racebox_cli RUNTIME DESTINATION .)
install(FILES VERSION DESTINATION .)
install(FILES README.md DESTINATION .)
install(FILES HANDOFF.md DESTINATION .)
install(FILES docs/race-day-crew-chief.md DESTINATION docs)
install(FILES "Start RaceBox Demo.cmd" DESTINATION .)
install(FILES assets/rrr-map.png assets/rrr-map-original.png DESTINATION assets)
install(FILES golden/session.vbo golden/session.csv golden/sanwa.csv DESTINATION demo)
install(FILES ${imgui_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME imgui-LICENSE.txt)
install(FILES ${implot_SOURCE_DIR}/LICENSE DESTINATION licenses RENAME implot-LICENSE.txt)
install(FILES ${miniz_src_SOURCE_DIR}/LICENSE DESTINATION licenses RENAME miniz-LICENSE.txt)
install(FILES ${json_SOURCE_DIR}/LICENSE.MIT DESTINATION licenses RENAME nlohmann-json-LICENSE.MIT)
