# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/home/guowa/Documents/Repos/fts_data_explorer/build/_deps/implot-src")
  file(MAKE_DIRECTORY "/home/guowa/Documents/Repos/fts_data_explorer/build/_deps/implot-src")
endif()
file(MAKE_DIRECTORY
  "/home/guowa/Documents/Repos/fts_data_explorer/build/_deps/implot-build"
  "/home/guowa/Documents/Repos/fts_data_explorer/build/_deps/implot-subbuild/implot-populate-prefix"
  "/home/guowa/Documents/Repos/fts_data_explorer/build/_deps/implot-subbuild/implot-populate-prefix/tmp"
  "/home/guowa/Documents/Repos/fts_data_explorer/build/_deps/implot-subbuild/implot-populate-prefix/src/implot-populate-stamp"
  "/home/guowa/Documents/Repos/fts_data_explorer/build/_deps/implot-subbuild/implot-populate-prefix/src"
  "/home/guowa/Documents/Repos/fts_data_explorer/build/_deps/implot-subbuild/implot-populate-prefix/src/implot-populate-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/guowa/Documents/Repos/fts_data_explorer/build/_deps/implot-subbuild/implot-populate-prefix/src/implot-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/guowa/Documents/Repos/fts_data_explorer/build/_deps/implot-subbuild/implot-populate-prefix/src/implot-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
