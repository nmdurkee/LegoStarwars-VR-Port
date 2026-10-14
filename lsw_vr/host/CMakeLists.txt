cmake_minimum_required(VERSION 3.20)
project(lsw_vr_host CXX)

# lsw_vr_host.exe runs OpenXR for the mod in a separate 64-bit process: Meta's
# 32-bit OpenXR runtime crashes in xrCreateSession. Configure from an x64 MSVC
# environment; deploy lsw_vr_host.exe and openxr_loader.dll next to the game.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "lsw_vr_host must be built as 64-bit (use an x64 MSVC developer environment)")
endif()

set(OPENXR_SDK "C:/Users/nmdur/Tools/openxr_loader_windows-1.1.63" CACHE PATH "OpenXR loader SDK")

add_executable(lsw_vr_host
    main.cpp
    xr_screen.cpp
    xr_input.cpp
    ../src/log.cpp
    ../src/shared/frame_channel.cpp)
target_compile_features(lsw_vr_host PRIVATE cxx_std_17)
target_compile_definitions(lsw_vr_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(lsw_vr_host PRIVATE "${OPENXR_SDK}/include")
target_link_libraries(lsw_vr_host PRIVATE "${OPENXR_SDK}/x64/lib/openxr_loader.lib" d3d11 dxgi)
add_custom_command(TARGET lsw_vr_host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENXR_SDK}/x64/bin/openxr_loader.dll"
            $<TARGET_FILE_DIR:lsw_vr_host>)

# Live tuning window (keyboard), deployed next to the game.
add_executable(lsw_vr_tuner WIN32 ../tools/tuner.cpp)
target_compile_features(lsw_vr_tuner PRIVATE cxx_std_17)
target_compile_definitions(lsw_vr_tuner PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(lsw_vr_tuner PRIVATE user32 gdi32)
