cmake_minimum_required(VERSION 3.10)

project(hilbert_comparison_test)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include parent directory for spectral_toolbox.h
include_directories(
    ${PROJECT_SOURCE_DIR}/..
    ${PROJECT_SOURCE_DIR}/../fftw-3.3.10/api
)

# Find the pre-built FFTW library from main project
set(FFTW_LIBRARY "${PROJECT_SOURCE_DIR}/../build/fftw-3.3.10/libfftw3.so")
set(FFTW_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/../fftw-3.3.10/api")

# Check if FFTW library exists, if not try to find it
if(NOT EXISTS ${FFTW_LIBRARY})
    message(WARNING "FFTW library not found at expected path: ${FFTW_LIBRARY}")
    message(WARNING "Trying to find FFTW system-wide...")
    find_library(FFTW_LIBRARY NAMES fftw3)
    if(NOT FFTW_LIBRARY)
        message(FATAL_ERROR "FFTW library not found. Please build the main project first or install FFTW3 system-wide.")
    endif()
endif()

# Executable target
add_executable(hilbert_comparison_test 
    test_hilbert_comparison.cpp
    ${PROJECT_SOURCE_DIR}/../spectral_toolbox.cpp
)

# Link libraries - use the pre-built FFTW library
target_link_libraries(hilbert_comparison_test 
    ${FFTW_LIBRARY}
    m  # Math library for FFTW
)

# Copy required header file to build directory for easy access
configure_file(
    ${PROJECT_SOURCE_DIR}/../spectral_toolbox.h
    ${PROJECT_BINARY_DIR}/spectral_toolbox.h
    COPYONLY
)

# Add custom target for running the test
add_custom_target(run_test
    COMMAND hilbert_comparison_test
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    DEPENDS hilbert_comparison_test
    COMMENT "Running Hilbert transform comparison test"
)

# Add custom target for full workflow
add_custom_target(full_workflow
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target run_test
    COMMAND python ${PROJECT_SOURCE_DIR}/python_reference/hilbert_reference.py
    COMMAND python ${PROJECT_SOURCE_DIR}/comparison_scripts/plot_comparison.py
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    DEPENDS hilbert_comparison_test
    COMMENT "Running full comparison workflow: C++ test -> Python reference -> Visual debugging"
)

message(STATUS "Hilbert transform comparison test harness configured")
message(STATUS "Build with: cmake --build . --target hilbert_comparison_test")
message(STATUS "Run full workflow with: cmake --build . --target full_workflow")
message(STATUS "Or manually:")
message(STATUS "  1. ./hilbert_comparison_test")
message(STATUS "  2. python python_reference/hilbert_reference.py")
message(STATUS "  3. python comparison_scripts/plot_comparison.py")