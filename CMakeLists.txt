cmake_minimum_required(VERSION 3.14)  # FetchContent requires 3.14+
project(fts_data_explorer)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
)

# Add FFTW subdirectory - build it as part of our project
add_subdirectory(fftw-3.3.10)

# Find required packages
find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)



# ImGui is now handled by FetchContent
include_directories(
    ${OPENGL_INCLUDE_DIR}
    ${GLFW_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${implot_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/fftw-3.3.10/api  # FFTW headers
)

# Add executable
add_executable(fts_data_explorer
    main.cpp
    app_state.cpp
    spectrum.cpp
    spectral_toolbox.cpp
    adapters/csv_adapter.cpp
    ${EXTRA_SOURCES}
)



# Link libraries
# Note: ImPlot is header-only, so we don't link to a library
# Instead, we include the source files directly in our compilation
target_link_libraries(fts_data_explorer
    ${OPENGL_LIBRARIES}
    ${GLFW_LIBRARIES}
    glfw
    GL
    m
)

# Add compile definitions for imgui
target_compile_definitions(fts_data_explorer PRIVATE
    IMGUI_IMPL_OPENGL_LOADER_GLAD=0
)

# Link FFTW to our executable - use the targets created by the FFTW build
target_link_libraries(fts_data_explorer
    ${OPENGL_LIBRARIES}
    ${GLFW_LIBRARIES}
    glfw
    GL
    m
    fftw3  # Use the FFTW target that was built
)

# Install targets (optional)
install(TARGETS fts_data_explorer DESTINATION bin)
