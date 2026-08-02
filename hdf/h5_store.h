#pragma once

#include <stdexcept>
#include <string>

#include "workspace.h"

// Sole owner of HDF5 file I/O. The engine never touches HDF5.
class H5Store {
public:
    // Read an entire .h5 into RAM. File is detached afterwards; missing/empty
    // optional members are tolerated (spec rules 7-8), unknown keys preserved (rule 9).
    static Workspace load(const std::string& path);

    // Write the whole Workspace to disk, atomically (temp file + rename).
    // Enforces original-data immutability and inputs integrity — throws H5Error
    // on any violation and leaves the existing file untouched.
    static void save(const std::string& path, const Workspace& ws);

    // Full spec conformance walk over an existing file.
    static void validate(const std::string& path);
};
