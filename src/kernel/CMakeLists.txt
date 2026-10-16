cmake_minimum_required(VERSION 3.20)

# Xbox Kernel Replacement Layer
# Replaces 147 Xbox kernel imports with Win32 equivalents.
# Memory layout, file I/O, threading, synchronization, crypto, HAL.

set(KERNEL_SOURCES
    kernel_path.c
    kernel_pool.c
    kernel_rtl.c
    kernel_memory.c
    kernel_file.c
    kernel_thread.c
    kernel_sync.c
    kernel_hal.c
    kernel_xbox.c
    kernel_ob.c
    kernel_io.c
    kernel_crypto.c
    kernel_thunks.c
    kernel_bridge.c
    xbox_memory_layout.c
    guest_vmem.c
    icall_feedback.c
    recomp_trace.c
    nv2a_pb_scan.c
    nv2a_pb_exec.c
    nv2a_vsh_interp.c
    nv2a_combiner.c
    nv2a_gpu_d3d11.c
)

add_library(xbox_kernel STATIC ${KERNEL_SOURCES})

# See src/platform/CMakeLists.txt: resolve src/ relative to this file, not via
# CMAKE_SOURCE_DIR, which is the consuming project's root under add_subdirectory().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

target_include_directories(xbox_kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XBOXRECOMP_SRC_DIR}
)

if(WIN32)
    # platform is an INTERFACE target on Windows: it contributes the src/
    # include path that "platform/xbox_winnt.h" resolves against.
    target_link_libraries(xbox_kernel PUBLIC platform kernel32 bcrypt
                          d3d11 dxgi dxguid d3dcompiler)
else()
    # Linux: platform layer (POSIX host primitives) + OpenSSL for crypto.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(xbox_kernel PUBLIC platform OpenSSL::Crypto)

    # The Direct3D 11 GPU backend on DXVK Native (nv2a_gpu_d3d11.c). Point
    # XBOX_DXVK_NATIVE_INCLUDE at DXVK Native's headers (the include/dxvk
    # directory of a dxvk-native release) to build it in. Only that one file
    # sees them -- their Win32 types would clash with the platform layer's --
    # and nothing is linked: DXVK and vkd3d-utils are opened at run time.
    set(XBOX_DXVK_NATIVE_INCLUDE "" CACHE PATH
        "DXVK Native headers (include/dxvk) for the GPU backend on Linux")
    if(XBOX_DXVK_NATIVE_INCLUDE)
        find_package(SDL2 REQUIRED)
        get_target_property(_sdl2_inc SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
        set_source_files_properties(nv2a_gpu_d3d11.c PROPERTIES
            COMPILE_DEFINITIONS XBOX_DXVK_NATIVE
            INCLUDE_DIRECTORIES "${XBOX_DXVK_NATIVE_INCLUDE};${_sdl2_inc}"
            COMPILE_OPTIONS "-Wno-unused-variable;-Wno-unused-const-variable")
        target_link_libraries(xbox_kernel PUBLIC SDL2::SDL2 ${CMAKE_DL_LIBS})
        message(STATUS "xbox_kernel: GPU backend on DXVK Native (${XBOX_DXVK_NATIVE_INCLUDE})")
    endif()
endif()

if(MSVC)
    target_compile_options(xbox_kernel PRIVATE /W4 /WX- /O2 /GS- /Gy)
    target_compile_definitions(xbox_kernel PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xbox_kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_compile_definitions(xbox_kernel PRIVATE $<$<CONFIG:Debug>:_DEBUG>)
