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

project(RaceBoxTelemetryViewer VERSION
  ${RACEBOX_VERSION_MAJOR}.${RACEBOX_VERSION_MINOR}.${RACEBOX_VERSION_PATCH}.${RACEBOX_VERSION_BUILD}
  LANGUAGES C CXX)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(resources/racebox_version.h.in generated/racebox_version.h @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)
include(CTest)

set(RACEBOX_WINDOWS_APP_DEFAULT OFF)
if(WIN32)
  set(RACEBOX_WINDOWS_APP_DEFAULT ON)
endif()
option(RACEBOX_BUILD_WINDOWS_APP
  "Build the Win32/DX11 app, Windows storage/network integrations, and portable package"
  ${RACEBOX_WINDOWS_APP_DEFAULT})
option(RACEBOX_BUILD_CLI "Build the platform-neutral telemetry CLI" ON)

if(RACEBOX_BUILD_WINDOWS_APP AND NOT WIN32)
  message(FATAL_ERROR
    "RACEBOX_BUILD_WINDOWS_APP requires Windows. Configure with "
    "-DRACEBOX_BUILD_WINDOWS_APP=OFF for the portable domain/application/CLI targets.")
endif()

if(RACEBOX_BUILD_CLI OR RACEBOX_BUILD_WINDOWS_APP)
  FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(json)
endif()

if(RACEBOX_BUILD_WINDOWS_APP)
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.8-docking
    GIT_SHALLOW TRUE)
  FetchContent_Declare(implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG d65a2bef53d32502407de3a4be80f191e2f412d7)
  FetchContent_Declare(miniz_src
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 77d0dce8627735138c51770d1799a1ef48f2117d)
  FetchContent_MakeAvailable(imgui implot miniz_src)

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
else()
  message(STATUS
    "RaceBox portable core-only build: Win32/DX11 UI, storage, Crew Chief transport, "
    "Windows-only tests, and portable packaging are disabled.")
endif()

function(racebox_configure_cpp_target target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# Platform-neutral telemetry, synchronization, map math, IMU, and deterministic
# analysis. This is the scientific core shared by the Windows and future macOS
# applications.
add_library(racebox_domain STATIC
  src/core/parsers.cpp
  src/core/analysis.cpp
  src/core/driver_analysis.cpp
  src/core/imu_analysis.cpp
  src/core/insight_evidence.cpp
  src/core/source_identity.cpp)
target_include_directories(racebox_domain PUBLIC include)
racebox_configure_cpp_target(racebox_domain)
add_library(racebox::domain ALIAS racebox_domain)

# Portable application state and OS-service contracts. The current native UI
# remains unchanged while v2 adopts this layer one workflow at a time.
add_library(racebox_application STATIC
  src/application/state.cpp)
target_include_directories(racebox_application PUBLIC include)
target_link_libraries(racebox_application PUBLIC racebox_domain)
racebox_configure_cpp_target(racebox_application)
add_library(racebox::application ALIAS racebox_application)

if(RACEBOX_BUILD_CLI)
  add_executable(racebox_cli src/cli/main.cpp)
  target_link_libraries(racebox_cli PRIVATE racebox_domain nlohmann_json::nlohmann_json)
  racebox_configure_cpp_target(racebox_cli)
endif()

if(RACEBOX_BUILD_WINDOWS_APP)
  # Existing Windows implementations are isolated from the portable targets.
  add_library(racebox_platform_windows STATIC
    src/core/settings.cpp)
  target_include_directories(racebox_platform_windows PUBLIC include)
  target_link_libraries(racebox_platform_windows PUBLIC shell32 ole32)
  target_compile_definitions(racebox_platform_windows PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  racebox_configure_cpp_target(racebox_platform_windows)

  add_library(racebox_storage_windows STATIC
    src/core/persistence.cpp
    src/core/race_day.cpp)
  target_include_directories(racebox_storage_windows PUBLIC include)
  target_link_libraries(racebox_storage_windows
    PUBLIC racebox_domain nlohmann_json::nlohmann_json
    PRIVATE miniz racebox_platform_windows)
  racebox_configure_cpp_target(racebox_storage_windows)

  add_library(racebox_integrations_windows STATIC
    src/core/crew_chief.cpp)
  target_include_directories(racebox_integrations_windows PUBLIC include)
  target_link_libraries(racebox_integrations_windows
    PUBLIC racebox_domain nlohmann_json::nlohmann_json
    PRIVATE winhttp)
  target_compile_definitions(racebox_integrations_windows PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  racebox_configure_cpp_target(racebox_integrations_windows)

  # Windows compatibility aggregate: existing UI and Windows-only tests keep
  # linking racebox_core while v2 adopts the separated targets incrementally.
  add_library(racebox_core INTERFACE)
  target_link_libraries(racebox_core INTERFACE
    racebox_domain
    racebox_application
    racebox_storage_windows
    racebox_integrations_windows
    racebox_platform_windows)

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
  racebox_configure_cpp_target(RaceBoxViewer)
  set_target_properties(RaceBoxViewer PROPERTIES OUTPUT_NAME RaceBoxTelemetryViewer)
endif()

if(BUILD_TESTING)
  # These tests exercise only platform-neutral targets and are available in
  # both the Windows build and the portable core-only configure path.
  add_executable(racebox_driver_analysis_tests tests/driver_analysis_tests.cpp)
  target_link_libraries(racebox_driver_analysis_tests PRIVATE racebox_domain)
  target_compile_definitions(racebox_driver_analysis_tests PRIVATE GOLDEN_DIR="${CMAKE_SOURCE_DIR}/golden")
  racebox_configure_cpp_target(racebox_driver_analysis_tests)
  add_test(NAME racebox_driver_analysis COMMAND racebox_driver_analysis_tests)

  add_executable(racebox_insight_evidence_tests tests/insight_evidence_tests.cpp)
  target_link_libraries(racebox_insight_evidence_tests PRIVATE racebox_domain)
  target_compile_definitions(racebox_insight_evidence_tests PRIVATE GOLDEN_DIR="${CMAKE_SOURCE_DIR}/golden")
  racebox_configure_cpp_target(racebox_insight_evidence_tests)
  add_test(NAME racebox_insight_evidence_addon COMMAND racebox_insight_evidence_tests)

  add_executable(racebox_plot_order_tests tests/plot_order_tests.cpp)
  target_include_directories(racebox_plot_order_tests PRIVATE include)
  racebox_configure_cpp_target(racebox_plot_order_tests)
  add_test(NAME racebox_plot_order COMMAND racebox_plot_order_tests)

  add_executable(racebox_plot_decimation_tests tests/plot_decimation_tests.cpp)
  target_include_directories(racebox_plot_decimation_tests PRIVATE include)
  racebox_configure_cpp_target(racebox_plot_decimation_tests)
  add_test(NAME racebox_plot_decimation COMMAND racebox_plot_decimation_tests)

  add_executable(racebox_imu_analysis_tests tests/imu_analysis_tests.cpp)
  target_link_libraries(racebox_imu_analysis_tests PRIVATE racebox_domain)
  racebox_configure_cpp_target(racebox_imu_analysis_tests)
  add_test(NAME racebox_imu_analysis COMMAND racebox_imu_analysis_tests)

  add_executable(racebox_application_tests tests/application_state_tests.cpp)
  target_link_libraries(racebox_application_tests PRIVATE racebox_application)
  racebox_configure_cpp_target(racebox_application_tests)
  add_test(NAME racebox_application_state COMMAND racebox_application_tests)

  add_executable(racebox_source_identity_tests tests/source_identity_tests.cpp)
  target_link_libraries(racebox_source_identity_tests PRIVATE racebox_domain)
  racebox_configure_cpp_target(racebox_source_identity_tests)
  add_test(NAME racebox_source_identity COMMAND racebox_source_identity_tests)

  if(RACEBOX_BUILD_WINDOWS_APP)
    add_executable(racebox_tests tests/core_tests.cpp)
    target_link_libraries(racebox_tests PRIVATE racebox_core)
    target_compile_definitions(racebox_tests PRIVATE GOLDEN_DIR="${CMAKE_SOURCE_DIR}/golden")
    racebox_configure_cpp_target(racebox_tests)
    add_test(NAME racebox_core_golden COMMAND racebox_tests)

    add_executable(racebox_dx11_tests tests/dx11_tests.cpp)
    target_link_libraries(racebox_dx11_tests PRIVATE d3d11 dxgi)
    target_compile_definitions(racebox_dx11_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    racebox_configure_cpp_target(racebox_dx11_tests)
    add_test(NAME racebox_dx11_warp COMMAND racebox_dx11_tests)

    add_executable(racebox_memory_tests tests/memory_tests.cpp)
    target_link_libraries(racebox_memory_tests PRIVATE racebox_core psapi)
    target_compile_definitions(racebox_memory_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    racebox_configure_cpp_target(racebox_memory_tests)
    add_test(NAME racebox_one_million_samples COMMAND racebox_memory_tests)

    add_executable(racebox_ui_preferences_tests
      tests/ui_preferences_tests.cpp
      src/app/ui_preferences.cpp)
    target_include_directories(racebox_ui_preferences_tests PRIVATE include src/app)
    target_link_libraries(racebox_ui_preferences_tests PRIVATE nlohmann_json::nlohmann_json)
    target_compile_definitions(racebox_ui_preferences_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    racebox_configure_cpp_target(racebox_ui_preferences_tests)
    add_test(NAME racebox_ui_preferences COMMAND racebox_ui_preferences_tests)

    add_executable(racebox_crew_chief_tests tests/crew_chief_tests.cpp)
    target_link_libraries(racebox_crew_chief_tests PRIVATE racebox_core)
    target_compile_definitions(racebox_crew_chief_tests PRIVATE GOLDEN_DIR="${CMAKE_SOURCE_DIR}/golden")
    racebox_configure_cpp_target(racebox_crew_chief_tests)
    add_test(NAME racebox_crew_chief COMMAND racebox_crew_chief_tests)

    add_executable(racebox_race_day_tests tests/race_day_tests.cpp)
    target_link_libraries(racebox_race_day_tests PRIVATE racebox_core)
    target_compile_definitions(racebox_race_day_tests PRIVATE GOLDEN_DIR="${CMAKE_SOURCE_DIR}/golden")
    racebox_configure_cpp_target(racebox_race_day_tests)
    add_test(NAME racebox_race_day COMMAND racebox_race_day_tests)
  endif()
endif()

if(RACEBOX_BUILD_WINDOWS_APP)
  set(RACEBOX_INSTALL_TARGETS RaceBoxViewer)
  if(TARGET racebox_cli)
    list(APPEND RACEBOX_INSTALL_TARGETS racebox_cli)
  endif()
  install(TARGETS ${RACEBOX_INSTALL_TARGETS} RUNTIME DESTINATION .)
  install(FILES VERSION DESTINATION .)
  install(FILES README.md DESTINATION .)
  install(FILES HANDOFF.md DESTINATION .)
  install(FILES docs/race-day-crew-chief.md DESTINATION docs)
  install(FILES docs/v2-architecture.md DESTINATION docs)
  install(FILES "Start RaceBox Demo.cmd" DESTINATION .)
  install(FILES assets/rrr-map.png assets/rrr-map-original.png DESTINATION assets)
  install(FILES golden/session.vbo golden/session.csv golden/sanwa.csv DESTINATION demo)
  install(FILES ${imgui_SOURCE_DIR}/LICENSE.txt DESTINATION licenses RENAME imgui-LICENSE.txt)
  install(FILES ${implot_SOURCE_DIR}/LICENSE DESTINATION licenses RENAME implot-LICENSE.txt)
  install(FILES ${miniz_src_SOURCE_DIR}/LICENSE DESTINATION licenses RENAME miniz-LICENSE.txt)
  install(FILES ${json_SOURCE_DIR}/LICENSE.MIT DESTINATION licenses RENAME nlohmann-json-LICENSE.MIT)
endif()
