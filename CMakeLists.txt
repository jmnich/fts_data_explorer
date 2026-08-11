cmake_minimum_required(VERSION 3.18)  # FetchContent 3.14+; HDF5 1.14.3 requires 3.18
project(fts_data_explorer)

# -- Version header fallback ---------------------------------------------------
# If build_script.sh hasn't generated version.h yet, create a dev fallback
# from the VERSION file so bare 'cmake' invocations still work.
set(VERSION_FILE "${CMAKE_SOURCE_DIR}/VERSION")
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/version.h")
    if(EXISTS "${VERSION_FILE}")
        file(READ "${VERSION_FILE}" RAW_VERSION)
        string(STRIP "${RAW_VERSION}" RAW_VERSION)
    else()
        set(RAW_VERSION "0.0.0")
    endif()
    file(WRITE "${CMAKE_SOURCE_DIR}/version.h"
        "#pragma once\n"
        "#define APP_VERSION \"${RAW_VERSION} (dev)\"\n"
        "#define APP_IS_RELEASE 0\n")
    message(STATUS "Generated fallback version.h: ${RAW_VERSION} (dev)")
endif()

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GLIBC compat wrappers — these --wrap flags redirect calls to new GLIBC symbol
# versions to our wrappers in glibc_compat.cpp that use GLIBC_2.2.5 compatible versions
set(GLIBCOMPAT_WRAP
    -Wl,--wrap,__libc_start_main
    -Wl,--wrap,sqrtf
    -Wl,--wrap,acosf
    -Wl,--wrap,atan2f
    -Wl,--wrap,fmodf
    -Wl,--wrap,exp
    -Wl,--wrap,log
    -Wl,--wrap,log2
    -Wl,--wrap,pow
    -Wl,--wrap,logf
    -Wl,--wrap,powf
    -Wl,--wrap,pthread_key_create
    -Wl,--wrap,pthread_key_delete
    -Wl,--wrap,pthread_getspecific
    -Wl,--wrap,pthread_setspecific
    -Wl,--wrap,pthread_once
    -Wl,--wrap,dlopen
    -Wl,--wrap,dlsym
    -Wl,--wrap,dlclose
    # Safety net for C23 functions from dependencies
    -Wl,--wrap,__isoc23_sscanf
    -Wl,--wrap,__isoc23_strtoul
    -Wl,--wrap,__isoc23_strtol
    -Wl,--wrap,__isoc23_strtoll
    -Wl,--wrap,__isoc23_strtoull
    # Re-versioned math (GLIBC_2.44) and C++ runtime (GLIBCXX_3.4.32/CXXABI_1.3.15)
    # that would raise the minimum glibc/libstdc++ above Ubuntu 22.04
    -Wl,--wrap,cosh
    -Wl,--wrap,sinh
    -Wl,--wrap,__cxa_call_terminate
)

# Download and include Dear ImGui
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

# Download and include ImPlot for scientific plotting
FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG master
)
FetchContent_MakeAvailable(implot)

# Download and include ImPlot3D for 3D plotting
FetchContent_Declare(
    implot3d
    GIT_REPOSITORY https://github.com/brenocq/implot3d.git
    GIT_TAG main
)
FetchContent_MakeAvailable(implot3d)

# Download tinyfiledialogs for cross-platform directory selection
FetchContent_Declare(
    tinyfiledialogs
    GIT_REPOSITORY https://github.com/native-toolkit/tinyfiledialogs.git
    GIT_TAG 2.9.3
)
FetchContent_MakeAvailable(tinyfiledialogs)

# Download nlohmann/json for headless mode config parsing
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# Download and build HDF5 (static C library, no optional features).
# The pinned tag is hdf5-1_14_3 (the 'hdf5-1.14.3' spelling does not exist upstream).
# On MinGW the cross-configure must skip HDF5's try_run() probes (x86-64 safe values)
# and declare vasprintf via _GNU_SOURCE; see phase0.md.
# Phase 5: HDF5 is unconditional — .h5 is the only runtime input, the
# FTS_BUILD_HDF5=OFF escape hatch was retired with the adapter system.
set(HDF5_BUILD_SHARED_LIBS   OFF CACHE BOOL "Build HDF5 as static library")
set(HDF5_BUILD_STATIC_LIBS   ON  CACHE BOOL "Build HDF5 as static library")
set(HDF5_BUILD_CPP_LIB       OFF CACHE BOOL "Disable HDF5 C++ library")
set(HDF5_BUILD_HL_LIB        OFF CACHE BOOL "Disable HDF5 high-level library")
set(HDF5_BUILD_TOOLS         OFF CACHE BOOL "Disable HDF5 command-line tools")
set(HDF5_BUILD_EXAMPLES      OFF CACHE BOOL "Disable HDF5 examples")
set(BUILD_TESTING            OFF CACHE BOOL "Disable HDF5 tests")
set(HDF5_ENABLE_Z_LIB_SUPPORT  OFF CACHE BOOL "Disable zlib")
set(HDF5_ENABLE_SZIP_SUPPORT   OFF CACHE BOOL "Disable szip")
set(HDF5_ENABLE_THREADSAFE     OFF CACHE BOOL "Disable threadsafe (single-writer discipline)")

# HDF5's CMake reads the generic BUILD_SHARED_LIBS/BUILD_STATIC_LIBS vars and, if
# unset, defaults BUILD_SHARED_LIBS=ON which leaks into FFTW/GLFW configured later.
# Pin them first so everything stays static.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)
set(BUILD_STATIC_LIBS ON  CACHE BOOL "Build static libraries" FORCE)

if(MINGW)
    # Cross-compile: replace HDF5's try_run() probes with x86-64 MinGW-safe results.
    # Both <var>_RUN and <var>_RUN__TRYRUN_OUTPUT must be set or CMake re-runs the probe.
    foreach(v IN ITEMS H5_LDOUBLE_TO_LONG_SPECIAL H5_LONG_TO_LDOUBLE_SPECIAL
                       H5_LDOUBLE_TO_LLONG_ACCURATE H5_LLONG_TO_LDOUBLE_CORRECT
                       H5_DISABLE_SOME_LDOUBLE_CONV HAVE_IOEO)
        set(${v}_RUN__TRYRUN_OUTPUT PLEASE_FILL_OUT-NOTFOUND CACHE STRING "cross-compile result")
    endforeach()
    set(H5_LDOUBLE_TO_LONG_SPECIAL_RUN   0 CACHE STRING "MinGW: no special ldouble conversion")
    set(H5_LONG_TO_LDOUBLE_SPECIAL_RUN   0 CACHE STRING "MinGW: no special ldouble conversion")
    set(H5_LDOUBLE_TO_LLONG_ACCURATE_RUN 1 CACHE STRING "MinGW: accurate ldouble<->llong")
    set(H5_LLONG_TO_LDOUBLE_CORRECT_RUN  1 CACHE STRING "MinGW: correct llong->ldouble")
    set(H5_DISABLE_SOME_LDOUBLE_CONV_RUN 0 CACHE STRING "MinGW: no power9 conversion bug")
    set(HAVE_IOEO_EXITCODE               0 CACHE STRING "InitOnceExecuteOnce works on Windows x64")
    # Declare vasprintf (H5_HAVE_VASPRINTF is detected at link time, but MinGW's
    # headers only declare it under _GNU_SOURCE).
    set(HDF5_SAVED_C_FLAGS "${CMAKE_C_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D_GNU_SOURCE")
endif()

FetchContent_Declare(
    hdf5
    GIT_REPOSITORY https://github.com/HDFGroup/hdf5.git
    GIT_TAG hdf5-1_14_3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(hdf5)

if(MINGW)
    set(CMAKE_C_FLAGS "${HDF5_SAVED_C_FLAGS}")
endif()

add_subdirectory(hdf)  # fts_hdf static library

# HDF5 caches CMAKE_*_OUTPUT_DIRECTORY to its own build tree; restore the
# project dirs so fts_data_explorer keeps landing in ${CMAKE_BINARY_DIR}.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} CACHE PATH "Executables dir" FORCE)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} CACHE PATH "Libraries dir" FORCE)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} CACHE PATH "Archives dir" FORCE)

# Add tinyfiledialogs source directly (matching imgui/implot pattern)
list(APPEND EXTRA_SOURCES
    ${tinyfiledialogs_SOURCE_DIR}/tinyfiledialogs.c
)

# Add ImGui core and backend source files to the executable
list(APPEND EXTRA_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot3d_SOURCE_DIR}/implot3d.cpp
    ${implot3d_SOURCE_DIR}/implot3d_items.cpp
)

# Configure FFTW to build static libraries
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build FFTW as static library")
set(BUILD_TESTS OFF CACHE BOOL "Disable FFTW tests")
set(ENABLE_THREADS ON CACHE BOOL "Enable pthread support in FFTW")

# Add FFTW subdirectory - build it as part of our project
add_subdirectory(fftw-3.3.10)

# Download and include GLFW
set(GLFW_USE_STATIC_LIBS ON CACHE BOOL "Use static GLFW libraries")
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.3.8
)
FetchContent_MakeAvailable(glfw)

# Generate asset headers from binary assets (via xxd, requires vim-common)
set(ASSET_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${ASSET_GENERATED_DIR}")
execute_process(
    COMMAND xxd -i assets/interferogram_curve.png
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_FILE "${ASSET_GENERATED_DIR}/interferogram_curve.h"
    ERROR_VARIABLE XXD_ERR
)
if(NOT XXD_ERR STREQUAL "")
    message(FATAL_ERROR "Failed to generate asset header: ${XXD_ERR}")
endif()

execute_process(
    COMMAND xxd -i assets/icon.png
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_FILE "${ASSET_GENERATED_DIR}/icon.h"
    ERROR_VARIABLE XXD_ERR
)
if(NOT XXD_ERR STREQUAL "")
    message(FATAL_ERROR "Failed to generate icon header: ${XXD_ERR}")
endif()

# Find required packages
find_package(OpenGL REQUIRED)
if(NOT WIN32)
    find_package(X11 REQUIRED)
endif()



# Fix case-sensitive header includes on MinGW cross-compilation
if(MINGW)
    include_directories(${CMAKE_SOURCE_DIR}/cmake/mingw-case-fix/include)
endif()

# ImGui is now handled by FetchContent
include_directories(
    ${OPENGL_INCLUDE_DIR}
    ${glfw_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${implot_SOURCE_DIR}
    ${implot3d_SOURCE_DIR}
    ${tinyfiledialogs_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/fftw-3.3.10/api  # FFTW headers
    ${CMAKE_BINARY_DIR}/generated        # xxd-generated asset headers
)

# Platform-specific sources
set(PLATFORM_SOURCES
    glibc_compat.cpp
)
if(WIN32)
    set(PLATFORM_SOURCES)  # glibc_compat.cpp is Linux/GLIBC-specific
endif()

# Add executable
add_executable(fts_data_explorer
    main.cpp
    app_state.cpp
    headless.cpp
    spectrum.cpp
    average_spectrum.cpp
    snr_spectrum.cpp
    allan_variance.cpp
    t100.cpp
    spectral_toolbox.cpp
    apodization.cpp
    export.cpp
    theme.cpp
    converter.cpp
    conversion_screen.cpp
    app_dirs.cpp
    file_browser.cpp
    welcome.cpp
    about.cpp
    ui/window.cpp
    ui/app_loop.cpp
    ui/menu_bar.cpp
    panels/files_panel.cpp
    panels/interferogram_view.cpp
    panels/metadata_panel.cpp
    session/workspace_session.cpp
    session/cross_store.cpp
    session/session_tab.cpp
    ${PLATFORM_SOURCES}
    ${EXTRA_SOURCES}
)

# fts_hdf integration (unconditional — .h5 is the only runtime input)
target_sources(fts_data_explorer PRIVATE workspace_reader.cpp)
target_link_libraries(fts_data_explorer fts_hdf)
target_compile_definitions(fts_data_explorer PRIVATE FTS_BUILD_HDF5=1)

# Platform-specific compile options
if(WIN32)
    target_compile_options(fts_data_explorer PRIVATE
        $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    )
else()
    # Avoid __isoc23_* functions (GLIBC_2.38) and fortified functions
    # that use newer GLIBC symbol versions.
    target_compile_options(fts_data_explorer PRIVATE
        -U_GNU_SOURCE
        -D_DEFAULT_SOURCE
        -D_FORTIFY_SOURCE=0
        -U_FORTIFY_SOURCE
        -march=x86-64-v3
        $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    )
endif()

# Platform-specific linker options
if(NOT WIN32)
    target_link_options(fts_data_explorer PRIVATE
        ${GLIBCOMPAT_WRAP}
    )
else()
    target_link_options(fts_data_explorer PRIVATE
        -static
    )
endif()

# Link libraries
target_link_libraries(fts_data_explorer
    ${OPENGL_LIBRARIES}
    glfw
    fftw3
    fftw3_threads
    nlohmann_json::nlohmann_json
)

# Platform-specific libraries
if(NOT WIN32)
    target_link_libraries(fts_data_explorer
        GL
        X11
        m
        pthread
        dl
    )
else()
    target_link_libraries(fts_data_explorer
        pthread
    )
endif()



# Add compile definitions for imgui
target_compile_definitions(fts_data_explorer PRIVATE
    IMGUI_IMPL_OPENGL_LOADER_GLAD=0
    IMGUI_USER_CONFIG="imconfig_custom.h"
)

# Strip .note.gnu.property to remove inherited x86-64-v4 ISA requirement from
# system libraries (glibc/libstdc++ compiled with AVX-512), allowing the binary
# to run on CPUs that only support up to x86-64-v3 (e.g. Zen 2).
if(NOT WIN32)
    add_custom_command(TARGET fts_data_explorer POST_BUILD
        COMMAND objcopy -R .note.gnu.property "$<TARGET_FILE:fts_data_explorer>"
        COMMENT "Stripping .note.gnu.property (x86 ISA requirements from system libs)"
    )
endif()

# Install targets (optional)
install(TARGETS fts_data_explorer DESTINATION bin)

# ── fts_session_roundtrip (M2.1/M2.7): park/resume equality harness ─────────
# Python cannot reach AppState, so the session-level round-trip test is C++.
# Links the app's non-GUI state + the panel render objects (they reference
# ImGui/ImPlot/GLFW symbols even though this binary never opens a window).
add_executable(fts_session_roundtrip
    session/session_roundtrip.cpp
    app_state.cpp
    session/workspace_session.cpp
    spectrum.cpp
    average_spectrum.cpp
    snr_spectrum.cpp
    allan_variance.cpp
    t100.cpp
    export.cpp
    spectral_toolbox.cpp
    apodization.cpp
    workspace_reader.cpp
    file_browser.cpp
    theme.cpp
    ${PLATFORM_SOURCES}
    ${EXTRA_SOURCES}
)
target_link_libraries(fts_session_roundtrip
    fts_hdf
    fftw3
    fftw3_threads
    ${OPENGL_LIBRARIES}
    glfw
    nlohmann_json::nlohmann_json
)
if(NOT WIN32)
    target_link_libraries(fts_session_roundtrip GL X11 m pthread dl)
endif()
target_compile_definitions(fts_session_roundtrip PRIVATE
    FTS_BUILD_HDF5=1
    IMGUI_IMPL_OPENGL_LOADER_GLAD=0
    IMGUI_USER_CONFIG="imconfig_custom.h"
)
target_include_directories(fts_session_roundtrip PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${implot_SOURCE_DIR}
    ${implot3d_SOURCE_DIR}
    ${tinyfiledialogs_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/fftw-3.3.10/api
)
if(NOT WIN32)
    target_link_options(fts_session_roundtrip PRIVATE ${GLIBCOMPAT_WRAP})
    target_compile_options(fts_session_roundtrip PRIVATE
        -U_GNU_SOURCE -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -U_FORTIFY_SOURCE
        -march=x86-64-v3
    )
endif()

# ── fts_cross_roundtrip (M2.4): cross-store CLI driver for the h5py suite ───
# Modes: create/add/remove/list/load/sniff/save-source (+ --slow-save kill test).
# Links only the hdf layer + cross_store (no ImGui/GLFW).
add_executable(fts_cross_roundtrip
    session/cross_roundtrip.cpp
    session/cross_store.cpp
    ${PLATFORM_SOURCES}
)
target_link_libraries(fts_cross_roundtrip
    fts_hdf
    nlohmann_json::nlohmann_json
)
target_compile_definitions(fts_cross_roundtrip PRIVATE
    FTS_BUILD_HDF5=1
    IMGUI_USER_CONFIG="imconfig_custom.h"
)
target_include_directories(fts_cross_roundtrip PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/fftw-3.3.10/api
)
if(NOT WIN32)
    target_link_options(fts_cross_roundtrip PRIVATE ${GLIBCOMPAT_WRAP})
    target_compile_options(fts_cross_roundtrip PRIVATE
        -U_GNU_SOURCE -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -U_FORTIFY_SOURCE
        -march=x86-64-v3
    )
endif()
