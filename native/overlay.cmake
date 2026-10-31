find_package(Python3 REQUIRED COMPONENTS Interpreter)
pkg_check_modules(FONT REQUIRED IMPORTED_TARGET SDL2_ttf)
add_custom_command(OUTPUT "${port}/overlay_helpers.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/overlay_sources.py" "${UU_UPSTREAM}/source" "${port}/overlay_helpers.inc"
    DEPENDS android_drawer.py android_settings.py overlay_sources.py privacy_ui.py virtual_display_ui.py tablet_pages.inc custom_resolution_ui.py resolution_editor.inc "${UU_UPSTREAM}/source/main.cpp" "${UU_UPSTREAM}/source/live_frontend.inc" "${UU_UPSTREAM}/source/tv_frontend.inc")
target_sources(youyou_core PRIVATE overlay.cpp "${port}/overlay_helpers.inc" "${UU_UPSTREAM}/source/native_display.cpp"
    "${UU_UPSTREAM}/source/runtime_profile.cpp" "${UU_UPSTREAM}/source/live_telemetry.cpp")
target_link_libraries(youyou_core PRIVATE PkgConfig::FONT)
target_include_directories(youyou_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
