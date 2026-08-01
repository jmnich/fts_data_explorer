cmake_minimum_required(VERSION 3.14)  # FetchContent requires 3.14+
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
    adapters/csv_adapter.cpp
    adapters/adapter_registry.cpp
    file_browser.cpp
    welcome.cpp
    about.cpp
    ${PLATFORM_SOURCES}
    ${EXTRA_SOURCES}
)

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
