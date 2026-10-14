cmake_minimum_required(VERSION 3.20)
project(lsw_vr CXX)

# The game is a 32-bit DX9 process: configure from an x86 MSVC environment.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "lsw_vr must be built as 32-bit (use an x86 MSVC developer environment)")
endif()

include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4)
FetchContent_MakeAvailable(minhook)

# Game-side VR output (capture + host launcher), shared by the mod and the
# standalone harness. The OpenXR side is the separate 64-bit project in host/.
add_library(lsw_vr_output STATIC
    src/log.cpp
    src/shared/frame_channel.cpp
    src/vr/capture.cpp
    src/vr/launcher.cpp)
target_compile_features(lsw_vr_output PUBLIC cxx_std_17)
target_compile_definitions(lsw_vr_output PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)

# Proxy d3d9.dll: placed next to LEGOStarWarsSaga.exe, it loads ahead of the
# system DLL and forwards to it while hooking the device.
add_library(lsw_vr SHARED
    src/proxy.cpp
    src/hooks.cpp
    src/arm_ik.cpp
    src/camera.cpp
    src/game.cpp
    src/head_tracking.cpp
    src/skeleton.cpp
    src/vr/vr_controls.cpp
    src/vr/vr_force.cpp
    src/vr/vr_frame.cpp
    src/vr/vr_interaction.cpp
    src/vr/sim.cpp
    d3d9.def)
set_target_properties(lsw_vr PROPERTIES OUTPUT_NAME d3d9)
target_link_libraries(lsw_vr PRIVATE lsw_vr_output minhook)

# Standalone VR output test (no game needed): vr_harness.exe [seconds]
add_executable(vr_harness tools/vr_harness.cpp tools/sim_stub.cpp)
target_link_libraries(vr_harness PRIVATE lsw_vr_output d3d9)
# Its own folder, so it loads the system d3d9.dll rather than the mod's proxy.
set_target_properties(vr_harness PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/harness")
