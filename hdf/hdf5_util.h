#pragma once

#include <hdf5.h>

#include <stdexcept>
#include <string>
#include <vector>

// Error type for all HDF5 layer failures. Thrown by H5Store and helpers.
class H5Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// RAII guards so no hid_t leaks. Not copyable; movable not needed (locals only).
struct H5FileGuard {
    hid_t id;
    H5FileGuard(hid_t i) : id(i) {}
    H5FileGuard(const H5FileGuard&) = delete;
    H5FileGuard& operator=(const H5FileGuard&) = delete;
    ~H5FileGuard() { if (id >= 0) H5Fclose(id); }
};
struct H5GroupGuard {
    hid_t id;
    H5GroupGuard(hid_t i) : id(i) {}
    H5GroupGuard(const H5GroupGuard&) = delete;
    H5GroupGuard& operator=(const H5GroupGuard&) = delete;
    ~H5GroupGuard() { if (id >= 0) H5Gclose(id); }
};
struct H5DatasetGuard {
    hid_t id;
    H5DatasetGuard(hid_t i) : id(i) {}
    H5DatasetGuard(const H5DatasetGuard&) = delete;
    H5DatasetGuard& operator=(const H5DatasetGuard&) = delete;
    ~H5DatasetGuard() { if (id >= 0) H5Dclose(id); }
};
struct H5AttrGuard {
    hid_t id;
    H5AttrGuard(hid_t i) : id(i) {}
    H5AttrGuard(const H5AttrGuard&) = delete;
    H5AttrGuard& operator=(const H5AttrGuard&) = delete;
    ~H5AttrGuard() { if (id >= 0) H5Aclose(id); }
};
struct H5TypeGuard {
    hid_t id;
    H5TypeGuard(hid_t i) : id(i) {}
    H5TypeGuard(const H5TypeGuard&) = delete;
    H5TypeGuard& operator=(const H5TypeGuard&) = delete;
    ~H5TypeGuard() { if (id >= 0) H5Tclose(id); }
};
struct H5SpaceGuard {
    hid_t id;
    H5SpaceGuard(hid_t i) : id(i) {}
    H5SpaceGuard(const H5SpaceGuard&) = delete;
    H5SpaceGuard& operator=(const H5SpaceGuard&) = delete;
    ~H5SpaceGuard() { if (id >= 0) H5Sclose(id); }
};

// A newline-joined (up to 5) description of the last HDF5 stack error.
std::string h5LastError();

// Current UTC time as "YYYY-MM-DDTHH:MM:SSZ" (portable gmtime_r/gmtime_s).
std::string h5UtcNowIso();

// ---- VLEN string helpers (all UTF-8, H5T_VARIABLE) ----
// Any failed call throws H5Error with `what`.

// Read the shape-(1,) VLEN string dataset `name` under `loc`.
std::string h5ReadVlenString(hid_t loc, const char* name);
// Create a shape-(1,) VLEN string dataset `name` under `loc` with `value`.
void h5WriteVlenString(hid_t loc, const char* name, const std::string& value);

// ---- Attribute helpers ----
bool h5HasAttr(hid_t obj, const char* name);
std::string h5ReadAttrString(hid_t obj, const char* name);
void h5WriteAttrString(hid_t obj, const char* name, const std::string& value);
std::vector<std::string> h5ReadAttrStringArray(hid_t obj, const char* name);
void h5WriteAttrStringArray(hid_t obj, const char* name,
                            const std::vector<std::string>& values);

// ---- Numeric dataset helpers ----
// Read the 2-col dataset `name` under `loc` into colA/colB (RAM fp64).
// Throws if the dataset is not rank-2 with exactly 2 columns.
void h5Read2ColDataset(hid_t loc, const char* name,
                       std::vector<double>& colA, std::vector<double>& colB);
// Create a [N,2] dataset from colA/colB. `isFp32` selects on-disk dtype.
void h5Write2ColDataset(hid_t loc, const char* name,
                        const std::vector<double>& colA, const std::vector<double>& colB,
                        bool isFp32 = false);
// Read a flat 1-D fp64 dataset `name` under `loc`.
void h5ReadFp64Vector(hid_t loc, const char* name, std::vector<double>& out);
// Write a flat 1-D fp64 dataset `name` under `loc`.
void h5WriteFp64Vector(hid_t loc, const char* name, const std::vector<double>& data);
// Read a rank-2 dataset `name` under `loc` as a flat row-major vector.
void h5Read2DRaw(hid_t loc, const char* name, std::vector<double>& out,
                 hsize_t* dimsOut);
// Write a flat row-major buffer as a [rows,cols] fp64 dataset.
void h5Write2DRaw(hid_t loc, const char* name, const std::vector<double>& data,
                  hsize_t rows, hsize_t cols);
