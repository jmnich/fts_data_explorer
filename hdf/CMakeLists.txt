add_library(fts_hdf STATIC
    hdf5_util.cpp
    workspace.cpp
    h5_store.cpp
)
target_include_directories(fts_hdf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fts_hdf PUBLIC
    nlohmann_json::nlohmann_json
    hdf5-static      # static HDF5 C library target from FetchContent
)

add_executable(fts_hdf_roundtrip hdf_roundtrip_test.cpp)
target_link_libraries(fts_hdf_roundtrip PRIVATE fts_hdf)

# HDF5 caches output dirs to its own build tree; pin ours to the project dirs.
set_target_properties(fts_hdf PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set_target_properties(fts_hdf_roundtrip PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
