cmake_minimum_required(VERSION 3.20)
project(xr_smoke_test CXX)

# Build from an x86 (32-bit) or x64 MSVC developer environment.
set(OPENXR_SDK "C:/Users/nmdur/Tools/openxr_loader_windows-1.1.63" CACHE PATH "OpenXR loader SDK")
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(OPENXR_ARCH Win32)
else()
    set(OPENXR_ARCH x64)
endif()

add_executable(xr_smoke_test main.cpp)
target_compile_features(xr_smoke_test PRIVATE cxx_std_17)
target_include_directories(xr_smoke_test PRIVATE "${OPENXR_SDK}/include")
target_link_libraries(xr_smoke_test PRIVATE "${OPENXR_SDK}/${OPENXR_ARCH}/lib/openxr_loader.lib" d3d11 dxgi)

add_custom_command(TARGET xr_smoke_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENXR_SDK}/${OPENXR_ARCH}/bin/openxr_loader.dll" $<TARGET_FILE_DIR:xr_smoke_test>)
