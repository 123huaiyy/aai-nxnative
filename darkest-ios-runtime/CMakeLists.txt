cmake_minimum_required(VERSION 3.25)
project(DarkestIOS LANGUAGES C CXX OBJC OBJCXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "Configure with the iPhoneOS or iPhoneSimulator SDK")
endif()
if(NOT EXISTS "${SWARC}/src/x86/runtime.h")
    message(FATAL_ERROR "Set SWARC to the patched SwARc source directory")
endif()

include(FetchContent)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.zip
    URL_HASH SHA256=5f399fdfbc040169ef4418a3cb99ccc5a0641681f7ccc1da66eeec3c97fa4076)
FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git GIT_TAG 16.6.0 GIT_SHALLOW ON)
FetchContent_Declare(spirv_cross
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Cross.git
    GIT_TAG vulkan-sdk-1.4.341.0 GIT_SHALLOW ON)
FetchContent_MakeAvailable(SDL3 glslang spirv_cross)
include("${SWARC}/cmake/softfloat.cmake")
include("${SWARC}/cmake/coremath.cmake")

set(core_sources
    runtime/log.cpp runtime/env.cpp runtime/guest_memory.cpp runtime/guest_alloc.cpp
    runtime/lazy_mem.cpp runtime/platform.cpp runtime/pacing.cpp runtime/write_watch.cpp
    runtime/gfx/format.cpp runtime/gfx/device.cpp runtime/gfx/present.cpp
    runtime/gfx/gpu_device.cpp runtime/gfx/shader_compile.cpp
    runtime/hlsl/preprocess.cpp runtime/hlsl/parse.cpp runtime/hlsl/check.cpp
    runtime/hlsl/glsl.cpp runtime/hlsl/hlsl.cpp runtime/hlsl/bytecode.cpp
    x86/x87.cpp x86/sse.cpp x86/sse_ext.cpp x86/atomic128.cpp
    x86/dispatch.cpp x86/process.cpp x86/hle.cpp x86/present.cpp x86/watchdog.cpp)
set(windows_sources
    pe_image.cpp unwind.cpp process.cpp hle.cpp kernel32.cpp winmm_timer.cpp
    kernel32_file.cpp kernel32_sync.cpp kernel32_seh.cpp seh64.cpp kernel32_misc.cpp
    advapi32.cpp user32_input.cpp user32_window.cpp gdi32.cpp shell_net.cpp ws2_32.cpp com.cpp
    ucrt.cpp ucrt_stdio.cpp ucrt_time.cpp vcruntime.cpp vcruntime_eh.cpp msvcp140.cpp
    sdl2.cpp sdl2_video.cpp)
list(TRANSFORM core_sources PREPEND "${SWARC}/src/")
list(TRANSFORM windows_sources PREPEND "${SWARC}/platforms/win32/src/win32/")
add_library(DarkestRuntime SHARED ${core_sources} ${windows_sources} ios_window.mm ios_gl.cpp shader_es.cpp)
target_compile_features(DarkestRuntime PUBLIC cxx_std_20)
target_include_directories(DarkestRuntime PUBLIC "${SWARC}/src" "${SWARC}/platforms/win32/src")
target_compile_options(DarkestRuntime PRIVATE -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
    -mno-red-zone -ffp-contract=off -fno-fast-math)
target_compile_definitions(DarkestRuntime PRIVATE RECOMP_IOS_GLES=1 RECOMP_IOS_COMPACT_WIN64=1)
target_link_libraries(DarkestRuntime PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,SDL3-static>" softfloat coremath
    glslang SPIRV glslang-default-resource-limits spirv-cross-msl "-framework UIKit" "-framework OpenGLES")
set_target_properties(DarkestRuntime PROPERTIES FRAMEWORK TRUE
    MACOSX_FRAMEWORK_IDENTIFIER org.aai.darkest.runtime
    INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR TRUE)

# These headers use their installed include directories in SwARc. The
# dependencies above are compiled in-tree for iOS.
set(dependency_headers "${CMAKE_CURRENT_BINARY_DIR}/dependency-include")
file(MAKE_DIRECTORY "${dependency_headers}/glslang")
file(CREATE_LINK "${glslang_SOURCE_DIR}/SPIRV" "${dependency_headers}/glslang/SPIRV" SYMBOLIC)
file(CREATE_LINK "${spirv_cross_SOURCE_DIR}" "${dependency_headers}/spirv_cross" SYMBOLIC)
target_include_directories(DarkestRuntime PRIVATE "${dependency_headers}")

find_program(HOST_GLSLANG NAMES glslang glslangValidator REQUIRED)
find_program(HOST_SPIRV_CROSS NAMES spirv-cross REQUIRED)
foreach(stage IN ITEMS vert frag)
    set(shader "${SWARC}/src/runtime/gfx/shaders/ff.${stage}")
    set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/ff_${stage}")
    if(stage STREQUAL "vert")
        set(symbol kFfVert)
    else()
        set(symbol kFfFrag)
    endif()
    add_custom_command(OUTPUT "${output}.h"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${HOST_GLSLANG}" -V --quiet -DMETAL -o "${output}.spv" "${shader}"
        COMMAND "${HOST_SPIRV_CROSS}" --msl --msl-version 20100
            --msl-decoration-binding --msl-no-clip-distance-user-varying
            "${output}.spv" --output "${output}.metal"
        COMMAND "${CMAKE_COMMAND}" "-DIN=${output}.metal" "-DOUT=${output}.h"
            "-DNAME=${symbol}" -P "${SWARC}/cmake/embed_text.cmake"
        DEPENDS "${shader}" VERBATIM)
    target_sources(DarkestRuntime PRIVATE "${output}.h")
endforeach()
target_include_directories(DarkestRuntime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders")

set(DARKEST_GAME_ARCHIVE "" CACHE FILEPATH "Locally generated ARM64 archive for the original game")
if(DARKEST_GAME_ARCHIVE)
    add_executable(Darkest MACOSX_BUNDLE main.cpp game_code.cpp)
    target_compile_features(Darkest PRIVATE cxx_std_20)
    target_include_directories(Darkest PRIVATE "${sdl3_SOURCE_DIR}/include")
    target_link_libraries(Darkest PRIVATE "${DARKEST_GAME_ARCHIVE}" DarkestRuntime "-framework UIKit")
    set_target_properties(Darkest PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
        INSTALL_RPATH "@executable_path/Frameworks" BUILD_WITH_INSTALL_RPATH TRUE)
    add_custom_command(TARGET Darkest POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:Darkest>/Frameworks"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_BUNDLE_DIR:DarkestRuntime>"
            "$<TARGET_BUNDLE_DIR:Darkest>/Frameworks/DarkestRuntime.framework"
        VERBATIM)
endif()

option(DARKEST_RUNTIME_SMOKE "Build the iOS simulator runtime test" OFF)
if(DARKEST_RUNTIME_SMOKE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(fixture "${CMAKE_CURRENT_BINARY_DIR}/smoke.exe")
    add_custom_command(OUTPUT "${fixture}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/make_smoke_pe.py" "${fixture}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/make_smoke_pe.py"
        VERBATIM)
    add_executable(DarkestRuntimeSmoke MACOSX_BUNDLE runtime_smoke.cpp "${fixture}")
    target_compile_features(DarkestRuntimeSmoke PRIVATE cxx_std_20)
    target_include_directories(DarkestRuntimeSmoke PRIVATE "${sdl3_SOURCE_DIR}/include")
    target_link_libraries(DarkestRuntimeSmoke PRIVATE DarkestRuntime "-framework UIKit")
    set_source_files_properties("${fixture}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(DarkestRuntimeSmoke PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER org.aai.darkest.runtime-smoke
        MACOSX_BUNDLE_BUNDLE_NAME DarkestRuntimeSmoke
        MACOSX_BUNDLE_SHORT_VERSION_STRING 1.0
        MACOSX_BUNDLE_BUNDLE_VERSION 1
        INSTALL_RPATH "@executable_path/Frameworks" BUILD_WITH_INSTALL_RPATH TRUE)
    add_custom_command(TARGET DarkestRuntimeSmoke POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_BUNDLE_DIR:DarkestRuntimeSmoke>/Frameworks"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "$<TARGET_BUNDLE_DIR:DarkestRuntime>"
            "$<TARGET_BUNDLE_DIR:DarkestRuntimeSmoke>/Frameworks/DarkestRuntime.framework"
        VERBATIM)
endif()
