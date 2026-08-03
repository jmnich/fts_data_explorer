#include "hdf5_util.h"

#include <ctime>
#include <cstring>
#include <iomanip>
#include <sstream>

std::string h5UtcNowIso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

std::string h5LastError() {
    H5E_auto2_t oldFn;
    void* oldData = nullptr;
    if (H5Eget_auto2(H5E_DEFAULT, &oldFn, &oldData) < 0) return "unknown HDF5 error";
    H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);  // silence callback spam
    std::string desc;
    int n = H5Eget_num(H5E_DEFAULT);
    std::ostringstream os;
    os << n << " HDF5 error(s)";
    H5Ewalk2(H5E_DEFAULT, H5E_WALK_UPWARD, [](unsigned, const H5E_error2_t* e, void* ctx) {
        auto* os = static_cast<std::ostringstream*>(ctx);
        *os << " | " << (e->desc ? e->desc : "");
        return 0;
    }, &os);
    H5Eclear2(H5E_DEFAULT);
    H5Eset_auto2(H5E_DEFAULT, oldFn, oldData);
    return os.str();
}

static void fail(const char* what) {
    throw H5Error(std::string(what) + ": " + h5LastError());
}

static hid_t vlenStrType() {
    hid_t t = H5Tcopy(H5T_C_S1);
    if (t < 0) fail("vlenStrType: H5Tcopy");
    if (H5Tset_size(t, H5T_VARIABLE) < 0 || H5Tset_cset(t, H5T_CSET_UTF8) < 0) {
        H5Tclose(t);
        fail("vlenStrType: H5Tset_size/cset");
    }
    return t;
}

static bool linkExists(hid_t loc, const char* name) {
    return H5Lexists(loc, name, H5P_DEFAULT) > 0;
}

std::string h5ReadVlenString(hid_t loc, const char* name) {
    if (!linkExists(loc, name)) fail("h5ReadVlenString: missing link");
    H5DatasetGuard ds(H5Dopen2(loc, name, H5P_DEFAULT));
    if (ds.id < 0) fail("h5ReadVlenString: H5Dopen2");
    char* buf = nullptr;
    H5TypeGuard mem(vlenStrType());
    if (H5Dread(ds.id, mem.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, &buf) < 0)
        fail("h5ReadVlenString: H5Dread");
    std::string out = buf ? buf : "";
    if (buf) H5free_memory(buf);
    return out;
}

void h5WriteVlenString(hid_t loc, const char* name, const std::string& value) {
    H5SpaceGuard space(H5Screate(H5S_SCALAR));
    if (space.id < 0) fail("h5WriteVlenString: H5Screate");
    H5TypeGuard type(vlenStrType());
    H5DatasetGuard ds(H5Dcreate2(loc, name, type.id, space.id,
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    if (ds.id < 0) fail("h5WriteVlenString: H5Dcreate2");
    const char* cstr = value.c_str();
    if (H5Dwrite(ds.id, type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, &cstr) < 0)
        fail("h5WriteVlenString: H5Dwrite");
}

bool h5HasAttr(hid_t obj, const char* name) {
    return H5Aexists(obj, name) > 0;
}

std::string h5ReadAttrString(hid_t obj, const char* name) {
    H5AttrGuard attr(H5Aopen(obj, name, H5P_DEFAULT));
    if (attr.id < 0) fail("h5ReadAttrString: H5Aopen");
    char* buf = nullptr;
    H5TypeGuard mem(vlenStrType());
    if (H5Aread(attr.id, mem.id, &buf) < 0) fail("h5ReadAttrString: H5Aread");
    std::string out = buf ? buf : "";
    if (buf) H5free_memory(buf);
    return out;
}

void h5WriteAttrString(hid_t obj, const char* name, const std::string& value) {
    H5SpaceGuard space(H5Screate(H5S_SCALAR));
    if (space.id < 0) fail("h5WriteAttrString: H5Screate");
    H5TypeGuard type(vlenStrType());
    H5AttrGuard attr(H5Acreate2(obj, name, type.id, space.id,
                                H5P_DEFAULT, H5P_DEFAULT));
    if (attr.id < 0) fail("h5WriteAttrString: H5Acreate2");
    const char* cstr = value.c_str();
    if (H5Awrite(attr.id, type.id, &cstr) < 0) fail("h5WriteAttrString: H5Awrite");
}

std::vector<std::string> h5ReadAttrStringArray(hid_t obj, const char* name) {
    H5AttrGuard attr(H5Aopen(obj, name, H5P_DEFAULT));
    if (attr.id < 0) fail("h5ReadAttrStringArray: H5Aopen");
    H5SpaceGuard space(H5Aget_space(attr.id));
    int rank = H5Sget_simple_extent_ndims(space.id);
    hsize_t dims[8] = {};
    if (rank < 0 || rank > 8) fail("h5ReadAttrStringArray: bad rank");
    H5Sget_simple_extent_dims(space.id, dims, nullptr);
    hsize_t n = 1;
    for (int i = 0; i < rank; ++i) n *= dims[i];

    std::vector<char*> buf(n);
    std::vector<std::string> out;
    out.reserve(n);
    H5TypeGuard mem(vlenStrType());
    if (H5Aread(attr.id, mem.id, buf.data()) < 0) fail("h5ReadAttrStringArray: H5Aread");
    for (hsize_t i = 0; i < n; ++i) {
        out.emplace_back(buf[i] ? buf[i] : "");
        if (buf[i]) H5free_memory(buf[i]);
    }
    return out;
}

void h5WriteAttrStringArray(hid_t obj, const char* name,
                            const std::vector<std::string>& values) {
    hsize_t dims[1] = {static_cast<hsize_t>(values.size())};
    H5SpaceGuard space(H5Screate_simple(1, dims, nullptr));
    if (space.id < 0) fail("h5WriteAttrStringArray: H5Screate_simple");
    H5TypeGuard type(vlenStrType());
    H5AttrGuard attr(H5Acreate2(obj, name, type.id, space.id,
                                H5P_DEFAULT, H5P_DEFAULT));
    if (attr.id < 0) fail("h5WriteAttrStringArray: H5Acreate2");
    std::vector<const char*> cstrs;
    cstrs.reserve(values.size());
    for (const auto& s : values) cstrs.push_back(s.c_str());
    if (H5Awrite(attr.id, type.id, cstrs.data()) < 0)
        fail("h5WriteAttrStringArray: H5Awrite");
}

void h5Read2ColDataset(hid_t loc, const char* name,
                       std::vector<double>& colA, std::vector<double>& colB) {
    H5DatasetGuard ds(H5Dopen2(loc, name, H5P_DEFAULT));
    if (ds.id < 0) fail("h5Read2ColDataset: H5Dopen2");
    H5SpaceGuard space(H5Dget_space(ds.id));
    int rank = H5Sget_simple_extent_ndims(space.id);
    hsize_t dims[2] = {};
    H5Sget_simple_extent_dims(space.id, dims, nullptr);
    if (rank != 2 || dims[1] != 2)
        throw H5Error(std::string("h5Read2ColDataset: ") + name +
                      " not [N,2] (rank " + std::to_string(rank) + ")");

    H5TypeGuard fileType(H5Dget_type(ds.id));
    bool isFp32 = H5Tequal(fileType.id, H5T_IEEE_F32LE) > 0 ||
                  H5Tequal(fileType.id, H5T_IEEE_F32BE) > 0;
    hsize_t n = dims[0];
    std::vector<double> raw(n * 2);
    if (isFp32) {
        std::vector<float> tmp(n * 2);
        if (H5Dread(ds.id, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, tmp.data()) < 0)
            fail("h5Read2ColDataset: H5Dread fp32");
        for (hsize_t i = 0; i < n * 2; ++i) raw[i] = tmp[i];
    } else {
        if (H5Dread(ds.id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, raw.data()) < 0)
            fail("h5Read2ColDataset: H5Dread fp64");
    }
    colA.assign(n, 0.0);
    colB.assign(n, 0.0);
    for (hsize_t i = 0; i < n; ++i) {
        colA[i] = raw[i * 2];
        colB[i] = raw[i * 2 + 1];
    }
}

void h5Write2ColDataset(hid_t loc, const char* name,
                        const std::vector<double>& colA, const std::vector<double>& colB,
                        bool isFp32) {
    if (colA.size() != colB.size())
        throw H5Error(std::string("h5Write2ColDataset: ") + name + " columns differ in length");
    hsize_t dims[2] = {static_cast<hsize_t>(colA.size()), 2};
    H5SpaceGuard space(H5Screate_simple(2, dims, nullptr));
    if (space.id < 0) fail("h5Write2ColDataset: H5Screate_simple");
    H5TypeGuard type(isFp32 ? H5Tcopy(H5T_IEEE_F32LE) : H5Tcopy(H5T_IEEE_F64LE));
    H5DatasetGuard ds(H5Dcreate2(loc, name, type.id, space.id,
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    if (ds.id < 0) fail("h5Write2ColDataset: H5Dcreate2");
    if (isFp32) {
        std::vector<float> tmp(colA.size() * 2);
        for (size_t i = 0; i < colA.size(); ++i) {
            tmp[i * 2] = static_cast<float>(colA[i]);
            tmp[i * 2 + 1] = static_cast<float>(colB[i]);
        }
        if (H5Dwrite(ds.id, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, tmp.data()) < 0)
            fail("h5Write2ColDataset: H5Dwrite fp32");
    } else {
        std::vector<double> tmp(colA.size() * 2);
        for (size_t i = 0; i < colA.size(); ++i) {
            tmp[i * 2] = colA[i];
            tmp[i * 2 + 1] = colB[i];
        }
        if (H5Dwrite(ds.id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, tmp.data()) < 0)
            fail("h5Write2ColDataset: H5Dwrite fp64");
    }
}

void h5ReadFp64Vector(hid_t loc, const char* name, std::vector<double>& out) {
    H5DatasetGuard ds(H5Dopen2(loc, name, H5P_DEFAULT));
    if (ds.id < 0) fail("h5ReadFp64Vector: H5Dopen2");
    H5SpaceGuard space(H5Dget_space(ds.id));
    int rank = H5Sget_simple_extent_ndims(space.id);
    hsize_t dims[1] = {};
    H5Sget_simple_extent_dims(space.id, dims, nullptr);
    if (rank != 1) throw H5Error(std::string("h5ReadFp64Vector: ") + name + " not 1-D");
    out.resize(dims[0]);
    if (H5Dread(ds.id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, out.data()) < 0)
        fail("h5ReadFp64Vector: H5Dread");
}

void h5WriteFp64Vector(hid_t loc, const char* name, const std::vector<double>& data) {
    hsize_t dims[1] = {static_cast<hsize_t>(data.size())};
    H5SpaceGuard space(H5Screate_simple(1, dims, nullptr));
    if (space.id < 0) fail("h5WriteFp64Vector: H5Screate_simple");
    H5TypeGuard type(H5Tcopy(H5T_IEEE_F64LE));
    H5DatasetGuard ds(H5Dcreate2(loc, name, type.id, space.id,
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    if (ds.id < 0) fail("h5WriteFp64Vector: H5Dcreate2");
    if (H5Dwrite(ds.id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 data.data()) < 0)
        fail("h5WriteFp64Vector: H5Dwrite");
}

void h5Read2DRaw(hid_t loc, const char* name, std::vector<double>& out, hsize_t* dimsOut) {
    H5DatasetGuard ds(H5Dopen2(loc, name, H5P_DEFAULT));
    if (ds.id < 0) fail("h5Read2DRaw: H5Dopen2");
    H5SpaceGuard space(H5Dget_space(ds.id));
    int rank = H5Sget_simple_extent_ndims(space.id);
    hsize_t dims[2] = {};
    H5Sget_simple_extent_dims(space.id, dims, nullptr);
    if (rank != 2) throw H5Error(std::string("h5Read2DRaw: ") + name + " not rank-2");
    out.resize(static_cast<size_t>(dims[0]) * static_cast<size_t>(dims[1]));
    if (H5Dread(ds.id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, out.data()) < 0)
        fail("h5Read2DRaw: H5Dread");
    dimsOut[0] = dims[0];
    dimsOut[1] = dims[1];
}

void h5Write2DRaw(hid_t loc, const char* name, const std::vector<double>& data,
                  hsize_t rows, hsize_t cols) {
    hsize_t dims[2] = {rows, cols};
    H5SpaceGuard space(H5Screate_simple(2, dims, nullptr));
    if (space.id < 0) fail("h5Write2DRaw: H5Screate_simple");
    H5TypeGuard type(H5Tcopy(H5T_IEEE_F64LE));
    H5DatasetGuard ds(H5Dcreate2(loc, name, type.id, space.id,
                                 H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    if (ds.id < 0) fail("h5Write2DRaw: H5Dcreate2");
    if (H5Dwrite(ds.id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                 data.data()) < 0)
        fail("h5Write2DRaw: H5Dwrite");
}
