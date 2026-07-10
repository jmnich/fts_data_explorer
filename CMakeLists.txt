cmake_minimum_required(VERSION 3.14)  # FetchContent requires 3.14+
project(fts_data_explorer)

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

# Find required packages
find_package(OpenGL REQUIRED)
find_package(X11 REQUIRED)



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

# Add executable
add_executable(fts_data_explorer
    main.cpp
    app_state.cpp
    spectrum.cpp
    average_spectrum.cpp
    snr_spectrum.cpp
    allan_variance.cpp
    stability.cpp
    spectral_toolbox.cpp
    apodization.cpp
    export.cpp
    glibc_compat.cpp
    adapters/csv_adapter.cpp
    file_browser.cpp
    welcome.cpp
    ${EXTRA_SOURCES}
)

# Statically link C++ runtime and apply --wrap flags (target-specific only)
target_link_options(fts_data_explorer PRIVATE
    -static-libgcc
    -static-libstdc++
    ${GLIBCOMPAT_WRAP}
)

# Avoid __isoc23_* functions (GLIBC_2.38) and fortified functions
# that use newer GLIBC symbol versions. Target-specific only.
# Also force C11 for C sources since GCC/glibc 2.43 defaults to C23.
target_compile_options(fts_data_explorer PRIVATE
    -U_GNU_SOURCE
    -D_DEFAULT_SOURCE
    -D_FORTIFY_SOURCE=0
    -U_FORTIFY_SOURCE
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
)



# Link libraries
# Note: ImPlot is header-only, so we don't link to a library
# Instead, we include the source files directly in our compilation
target_link_libraries(fts_data_explorer
    ${OPENGL_LIBRARIES}
    glfw
    GL
    fftw3  # Use the FFTW target that was built
    X11
    m
)

# Link pthread and dl explicitly for compatibility with older glibc
target_link_libraries(fts_data_explorer
    pthread
    dl
)



# Add compile definitions for imgui
target_compile_definitions(fts_data_explorer PRIVATE
    IMGUI_IMPL_OPENGL_LOADER_GLAD=0
    IMGUI_USER_CONFIG="imconfig_custom.h"
)

# Install targets (optional)
install(TARGETS fts_data_explorer DESTINATION bin)
