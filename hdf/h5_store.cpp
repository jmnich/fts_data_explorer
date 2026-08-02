#include "h5_store.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "hdf5_util.h"

namespace {

constexpr const char* kFormat = "unified-spectral-data-container";

// igm_uncorrected_x is stored as fp32 on disk (matches the python parser);
// every other type group is fp64. Original-data protection tolerances follow
// this dtype mapping (see phase0.md "Locked decisions").
constexpr double kTolFp32 = 1e-6;
constexpr double kTolFp64 = 1e-12;

const char* kindStr(MemberKind k) {
    return k == MemberKind::Original ? "original" : "derivative";
}
MemberKind kindFromStr(const std::string& s) {
    return s == "original" ? MemberKind::Original : MemberKind::Derivative;
}

std::string utcNowIso() {
    return h5UtcNowIso();
}

bool fileExists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void fail(const std::string& what) {
    throw H5Error(what + ": " + h5LastError());
}

std::vector<std::string> listChildren(hid_t obj) {
    std::vector<std::string> names;
    H5G_info_t info{};
    if (H5Gget_info(obj, &info) < 0) fail("listChildren: H5Gget_info");
    for (hsize_t i = 0; i < info.nlinks; ++i) {
        char buf[512];
        ssize_t len = H5Lget_name_by_idx(obj, ".", H5_INDEX_NAME, H5_ITER_INC, i,
                                         buf, sizeof(buf), H5P_DEFAULT);
        if (len > 0) names.emplace_back(buf, static_cast<size_t>(len));
    }
    return names;
}

H5O_type_t linkType(hid_t obj, const char* name) {
    H5O_info2_t info{};
    if (H5Oget_info_by_name3(obj, name, &info, H5O_INFO_BASIC, H5P_DEFAULT) < 0)
        fail(std::string("linkType: ") + name);
    return info.type;
}

// Tolerate legacy files that predate @schema (missing -> expected); strict on mismatch.
std::string readSchemaAttr(hid_t group, const char* expected) {
    if (!h5HasAttr(group, "schema")) return expected;
    std::string s = h5ReadAttrString(group, "schema");
    if (s != expected)
        throw H5Error(std::string("schema mismatch: got '") + s + "', expected '" + expected + "'");
    return s;
}

// Per-dataset attrs for flat groups (kind/columns/units/timestamp on the dataset).
void readDatasetAttrs(hid_t parent, const char* dsName, MemberBase& m) {
    H5DatasetGuard ds(H5Dopen2(parent, dsName, H5P_DEFAULT));
    if (ds.id < 0) fail(std::string("readDatasetAttrs: H5Dopen2 ") + dsName);
    m.kind = h5HasAttr(ds.id, "kind") ? kindFromStr(h5ReadAttrString(ds.id, "kind"))
                                      : MemberKind::Derivative;
    m.timestamp = h5HasAttr(ds.id, "timestamp") ? h5ReadAttrString(ds.id, "timestamp") : "";
    m.columns = h5HasAttr(ds.id, "columns") ? h5ReadAttrStringArray(ds.id, "columns")
                                            : std::vector<std::string>{};
    m.units = h5HasAttr(ds.id, "units") ? h5ReadAttrStringArray(ds.id, "units")
                                        : std::vector<std::string>{};
}

// Per-member-group attrs for sub-group types (kind/origin/config on the <id>/ group).
void readMemberGroupAttrs(hid_t memberGroup, MemberBase& m) {
    m.kind = h5HasAttr(memberGroup, "kind") ? kindFromStr(h5ReadAttrString(memberGroup, "kind"))
                                            : MemberKind::Derivative;
    m.origin = h5HasAttr(memberGroup, "origin") ? h5ReadAttrString(memberGroup, "origin") : "";
    m.config = h5HasAttr(memberGroup, "config") ? h5ReadAttrString(memberGroup, "config") : "";
    m.timestamp = h5HasAttr(memberGroup, "timestamp")
                      ? h5ReadAttrString(memberGroup, "timestamp") : "";
}

void writeDatasetAttrs(hid_t parent, const char* dsName, const MemberBase& m) {
    H5DatasetGuard ds(H5Dopen2(parent, dsName, H5P_DEFAULT));
    if (ds.id < 0) fail(std::string("writeDatasetAttrs: H5Dopen2 ") + dsName);
    h5WriteAttrString(ds.id, "kind", kindStr(m.kind));
    if (!m.timestamp.empty()) h5WriteAttrString(ds.id, "timestamp", m.timestamp);
    if (!m.columns.empty()) h5WriteAttrStringArray(ds.id, "columns", m.columns);
    if (!m.units.empty()) h5WriteAttrStringArray(ds.id, "units", m.units);
}

void writeMemberGroupAttrs(hid_t memberGroup, const MemberBase& m) {
    h5WriteAttrString(memberGroup, "kind", kindStr(m.kind));
    h5WriteAttrString(memberGroup, "origin", m.origin);
    h5WriteAttrString(memberGroup, "config", m.config);
    if (!m.timestamp.empty()) h5WriteAttrString(memberGroup, "timestamp", m.timestamp);
}

// ---- load helpers ----

MemberGroup<InterferogramMember> loadFlatIfg(hid_t file, const char* path,
                                             const char* schema, bool corrected) {
    MemberGroup<InterferogramMember> g;
    if (!H5Lexists(file, path, H5P_DEFAULT)) return g;
    H5GroupGuard group(H5Gopen2(file, path, H5P_DEFAULT));
    if (group.id < 0) fail(std::string("loadFlatIfg: H5Gopen2 ") + path);
    g.schema = readSchemaAttr(group.id, schema);
    g.origin = h5HasAttr(group.id, "origin") ? h5ReadAttrString(group.id, "origin") : "";
    g.config = h5HasAttr(group.id, "config") ? h5ReadAttrString(group.id, "config") : "";
    for (const auto& name : listChildren(group.id)) {
        if (linkType(group.id, name.c_str()) != H5O_TYPE_DATASET) continue;
        InterferogramMember m;
        m.id = name;
        m.corrected = corrected;
        readDatasetAttrs(group.id, name.c_str(), m);
        h5Read2ColDataset(group.id, name.c_str(), m.col0, m.col1);
        g.members.push_back(std::move(m));
    }
    return g;
}

MemberGroup<TwoColumnMember> loadTwoColSub(hid_t file, const char* path, const char* schema) {
    MemberGroup<TwoColumnMember> g;
    if (!H5Lexists(file, path, H5P_DEFAULT)) return g;
    H5GroupGuard group(H5Gopen2(file, path, H5P_DEFAULT));
    if (group.id < 0) fail(std::string("loadTwoColSub: H5Gopen2 ") + path);
    g.schema = readSchemaAttr(group.id, schema);
    g.origin = h5HasAttr(group.id, "origin") ? h5ReadAttrString(group.id, "origin") : "";
    g.config = h5HasAttr(group.id, "config") ? h5ReadAttrString(group.id, "config") : "";
    for (const auto& name : listChildren(group.id)) {
        if (linkType(group.id, name.c_str()) != H5O_TYPE_GROUP) continue;
        H5GroupGuard mg(H5Gopen2(group.id, name.c_str(), H5P_DEFAULT));
        if (mg.id < 0) fail("loadTwoColSub: H5Gopen2 member");
        TwoColumnMember m;
        m.id = name;
        readMemberGroupAttrs(mg.id, m);
        if (H5Lexists(mg.id, "data", H5P_DEFAULT)) {
            h5Read2ColDataset(mg.id, "data", m.x, m.y);
            H5DatasetGuard ds(H5Dopen2(mg.id, "data", H5P_DEFAULT));
            if (h5HasAttr(ds.id, "columns"))
                m.columns = h5ReadAttrStringArray(ds.id, "columns");
            if (h5HasAttr(ds.id, "units"))
                m.units = h5ReadAttrStringArray(ds.id, "units");
        }
        g.members.push_back(std::move(m));
    }
    return g;
}

MemberGroup<AllanMember> loadAllanSub(hid_t file, const char* path, const char* schema) {
    MemberGroup<AllanMember> g;
    if (!H5Lexists(file, path, H5P_DEFAULT)) return g;
    H5GroupGuard group(H5Gopen2(file, path, H5P_DEFAULT));
    if (group.id < 0) fail("loadAllanSub: H5Gopen2 " + std::string(path));
    g.schema = readSchemaAttr(group.id, schema);
    for (const auto& name : listChildren(group.id)) {
        if (linkType(group.id, name.c_str()) != H5O_TYPE_GROUP) continue;
        H5GroupGuard mg(H5Gopen2(group.id, name.c_str(), H5P_DEFAULT));
        AllanMember m;
        m.id = name;
        readMemberGroupAttrs(mg.id, m);
        if (H5Lexists(mg.id, "surface_data", H5P_DEFAULT)) {
            hsize_t dims[2] = {};
            h5Read2DRaw(mg.id, "surface_data", m.surface, dims);
            H5DatasetGuard ds(H5Dopen2(mg.id, "surface_data", H5P_DEFAULT));
            if (h5HasAttr(ds.id, "columns"))
                m.columns = h5ReadAttrStringArray(ds.id, "columns");
        }
        if (H5Lexists(mg.id, "wavelengths", H5P_DEFAULT))
            h5ReadFp64Vector(mg.id, "wavelengths", m.wavelengths);
        if (H5Lexists(mg.id, "taus", H5P_DEFAULT))
            h5ReadFp64Vector(mg.id, "taus", m.taus);
        g.members.push_back(std::move(m));
    }
    return g;
}

MemberGroup<T100Member> loadT100Sub(hid_t file, const char* path, const char* schema) {
    MemberGroup<T100Member> g;
    if (!H5Lexists(file, path, H5P_DEFAULT)) return g;
    H5GroupGuard group(H5Gopen2(file, path, H5P_DEFAULT));
    if (group.id < 0) fail("loadT100Sub: H5Gopen2 " + std::string(path));
    g.schema = readSchemaAttr(group.id, schema);
    for (const auto& name : listChildren(group.id)) {
        if (linkType(group.id, name.c_str()) != H5O_TYPE_GROUP) continue;
        H5GroupGuard mg(H5Gopen2(group.id, name.c_str(), H5P_DEFAULT));
        T100Member m;
        m.id = name;
        readMemberGroupAttrs(mg.id, m);
        if (H5Lexists(mg.id, "reference", H5P_DEFAULT)) {
            h5Read2ColDataset(mg.id, "reference", m.reference.x, m.reference.y);
            readDatasetAttrs(mg.id, "reference", m.reference);
        }
        if (H5Lexists(mg.id, "stddev", H5P_DEFAULT)) {
            h5Read2ColDataset(mg.id, "stddev", m.stddev.x, m.stddev.y);
            readDatasetAttrs(mg.id, "stddev", m.stddev);
        }
        if (H5Lexists(mg.id, "transmittance", H5P_DEFAULT)) {
            H5GroupGuard tg(H5Gopen2(mg.id, "transmittance", H5P_DEFAULT));
            for (const auto& cname : listChildren(tg.id)) {
                H5GroupGuard cg(H5Gopen2(tg.id, cname.c_str(), H5P_DEFAULT));
                T100Member::Curve c;
                c.fileId = cname;
                if (H5Lexists(cg.id, "data", H5P_DEFAULT))
                    h5Read2ColDataset(cg.id, "data", c.x, c.y);
                m.curves.push_back(std::move(c));
            }
        }
        g.members.push_back(std::move(m));
    }
    return g;
}

// ---- save helpers ----

void writeRoot(hid_t file, const Workspace& ws) {
    h5WriteAttrString(file, "format", kFormat);
    h5WriteAttrString(file, "created",
                      ws.created.empty() ? utcNowIso() : ws.created);
    h5WriteVlenString(file, "workspace.json", ws.workspaceJson.dump());
    h5WriteVlenString(file, "measurement_config.json", ws.measurementConfig.dump());
    h5WriteVlenString(file, "measurement_comment.txt", ws.measurementComment);
    h5WriteVlenString(file, "tags", ws.tags);
}

void writeFlatIfg(hid_t file, const char* path, const char* schema,
                  const MemberGroup<InterferogramMember>& g, bool fp32) {
    H5GroupGuard group(H5Gcreate2(file, path, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    if (group.id < 0) fail(std::string("writeFlatIfg: H5Gcreate2 ") + path);
    h5WriteAttrString(group.id, "schema", schema);
    if (!g.origin.empty()) h5WriteAttrString(group.id, "origin", g.origin);
    if (!g.config.empty()) h5WriteAttrString(group.id, "config", g.config);
    for (const auto& m : g.members) {
        h5Write2ColDataset(group.id, m.id.c_str(), m.col0, m.col1, fp32);
        writeDatasetAttrs(group.id, m.id.c_str(), m);
    }
}

void writeTwoColSub(hid_t file, const char* path, const char* schema,
                    const MemberGroup<TwoColumnMember>& g) {
    H5GroupGuard group(H5Gcreate2(file, path, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    if (group.id < 0) fail(std::string("writeTwoColSub: H5Gcreate2 ") + path);
    h5WriteAttrString(group.id, "schema", schema);
    for (const auto& m : g.members) {
        H5GroupGuard mg(H5Gcreate2(group.id, m.id.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
        if (mg.id < 0) fail("writeTwoColSub: H5Gcreate2 member");
        writeMemberGroupAttrs(mg.id, m);
        h5Write2ColDataset(mg.id, "data", m.x, m.y);
        writeDatasetAttrs(mg.id, "data", m);
    }
}

void writeAllanSub(hid_t file, const char* path, const char* schema,
                   const MemberGroup<AllanMember>& g) {
    H5GroupGuard group(H5Gcreate2(file, path, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    if (group.id < 0) fail("writeAllanSub: H5Gcreate2 " + std::string(path));
    h5WriteAttrString(group.id, "schema", schema);
    for (const auto& m : g.members) {
        H5GroupGuard mg(H5Gcreate2(group.id, m.id.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
        if (mg.id < 0) fail("writeAllanSub: H5Gcreate2 member");
        writeMemberGroupAttrs(mg.id, m);
        hsize_t rows = static_cast<hsize_t>(m.taus.size());
        hsize_t cols = static_cast<hsize_t>(m.wavelengths.size());
        if (m.surface.size() != static_cast<size_t>(rows * cols))
            throw H5Error("writeAllanSub: surface size != taus*wavelengths for " + m.id);
        h5Write2DRaw(mg.id, "surface_data", m.surface, rows, cols);
        H5DatasetGuard sd(H5Dopen2(mg.id, "surface_data", H5P_DEFAULT));
        if (!m.columns.empty()) h5WriteAttrStringArray(sd.id, "columns", m.columns);
        h5WriteFp64Vector(mg.id, "wavelengths", m.wavelengths);
        H5DatasetGuard wl(H5Dopen2(mg.id, "wavelengths", H5P_DEFAULT));
        h5WriteAttrString(wl.id, "units", "um");
        h5WriteFp64Vector(mg.id, "taus", m.taus);
        H5DatasetGuard ts(H5Dopen2(mg.id, "taus", H5P_DEFAULT));
        h5WriteAttrString(ts.id, "units", "s");
    }
}

void writeT100Sub(hid_t file, const char* path, const char* schema,
                  const MemberGroup<T100Member>& g) {
    H5GroupGuard group(H5Gcreate2(file, path, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
    if (group.id < 0) fail("writeT100Sub: H5Gcreate2 " + std::string(path));
    h5WriteAttrString(group.id, "schema", schema);
    for (const auto& m : g.members) {
        H5GroupGuard mg(H5Gcreate2(group.id, m.id.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
        if (mg.id < 0) fail("writeT100Sub: H5Gcreate2 member");
        writeMemberGroupAttrs(mg.id, m);
        h5Write2ColDataset(mg.id, "reference", m.reference.x, m.reference.y);
        writeDatasetAttrs(mg.id, "reference", m.reference);
        h5Write2ColDataset(mg.id, "stddev", m.stddev.x, m.stddev.y);
        writeDatasetAttrs(mg.id, "stddev", m.stddev);
        if (!m.curves.empty()) {
            H5GroupGuard tg(H5Gcreate2(mg.id, "transmittance", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
            for (const auto& c : m.curves) {
                H5GroupGuard cg(H5Gcreate2(tg.id, c.fileId.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
                h5Write2ColDataset(cg.id, "data", c.x, c.y);
                H5DatasetGuard ds(H5Dopen2(cg.id, "data", H5P_DEFAULT));
                h5WriteAttrStringArray(ds.id, "columns", {"x", "T%"});
                h5WriteAttrStringArray(ds.id, "units", {"cm-1", "%"});
            }
        }
    }
}

// ---- original-data protection ----

bool vecEqual(const std::vector<double>& a, const std::vector<double>& b, double tol) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::fabs(a[i] - b[i]) > tol) return false;
    return true;
}

bool curveEqual(const T100Member::Curve& a, const T100Member::Curve& b, double tol) {
    return a.fileId == b.fileId && vecEqual(a.x, b.x, tol) && vecEqual(a.y, b.y, tol);
}

// For every kind="original" member in `existing` (the current file on disk),
// the same member must exist in `ws` with equal data (spec rule 1).
template <typename T, typename F>
void checkOriginalGroup(const MemberGroup<T>& existing, const MemberGroup<T>& ws,
                        const std::string& groupName, double tol, F dataEqual) {
    for (const auto& e : existing.members) {
        if (e.kind != MemberKind::Original) continue;
        auto it = std::find_if(ws.members.begin(), ws.members.end(),
                               [&](const T& m) { return m.id == e.id; });
        if (it == ws.members.end())
            throw H5Error("save: original-data protected: deleted /" + groupName + "/" + e.id);
        if (it->kind != MemberKind::Original)
            throw H5Error("save: original-data protected: kind changed /" + groupName + "/" + e.id);
        if (!dataEqual(e, *it, tol))
            throw H5Error("save: original-data protected: modified /" + groupName + "/" + e.id);
    }
}

void verifyOriginalsUnchanged(const Workspace& existing, const Workspace& ws) {
    checkOriginalGroup(existing.uncorrectedIfg, ws.uncorrectedIfg, "igm_uncorrected_x", kTolFp32,
        [](const InterferogramMember& a, const InterferogramMember& b, double t) {
            return vecEqual(a.col0, b.col0, t) && vecEqual(a.col1, b.col1, t);
        });
    checkOriginalGroup(existing.correctedIfg, ws.correctedIfg, "igm_corrected_x", kTolFp64,
        [](const InterferogramMember& a, const InterferogramMember& b, double t) {
            return vecEqual(a.col0, b.col0, t) && vecEqual(a.col1, b.col1, t);
        });
    checkOriginalGroup(existing.spectra, ws.spectra, "spectra", kTolFp64,
        [](const TwoColumnMember& a, const TwoColumnMember& b, double t) {
            return vecEqual(a.x, b.x, t) && vecEqual(a.y, b.y, t);
        });
    checkOriginalGroup(existing.allanWerle, ws.allanWerle, "allan_werle", kTolFp64,
        [](const AllanMember& a, const AllanMember& b, double t) {
            return vecEqual(a.taus, b.taus, t) && vecEqual(a.wavelengths, b.wavelengths, t) &&
                   vecEqual(a.surface, b.surface, t);
        });
    checkOriginalGroup(existing.t100, ws.t100, "t100", kTolFp64,
        [](const T100Member& a, const T100Member& b, double t) {
            bool ref = vecEqual(a.reference.x, b.reference.x, t) &&
                       vecEqual(a.reference.y, b.reference.y, t);
            bool sd = vecEqual(a.stddev.x, b.stddev.x, t) &&
                      vecEqual(a.stddev.y, b.stddev.y, t);
            if (a.curves.size() != b.curves.size()) return false;
            for (size_t i = 0; i < a.curves.size(); ++i)
                if (!curveEqual(a.curves[i], b.curves[i], t)) return false;
            return ref && sd;
        });
}

std::string joinDangling(const std::vector<std::string>& dangling) {
    std::string out;
    for (size_t i = 0; i < dangling.size(); ++i)
        out += (i ? ", " : "") + dangling[i];
    return out;
}

}  // namespace

Workspace H5Store::load(const std::string& path) {
    H5FileGuard file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    if (file.id < 0) throw H5Error("load: cannot open '" + path + "'");
    Workspace ws;

    if (!h5HasAttr(file.id, "format"))
        throw H5Error("load: missing root @format");
    ws.format = h5ReadAttrString(file.id, "format");
    if (ws.format != kFormat)
        throw H5Error("load: @format mismatch: '" + ws.format + "'");
    ws.created = h5HasAttr(file.id, "created") ? h5ReadAttrString(file.id, "created") : "";

    auto readRoot = [&](const char* name, const std::string& fallback) {
        return H5Lexists(file.id, name, H5P_DEFAULT) ? h5ReadVlenString(file.id, name) : fallback;
    };
    auto parseJson = [](const std::string& s) {
        nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
        return j.is_discarded() ? nlohmann::json::object() : j;
    };
    ws.measurementConfig = parseJson(readRoot("measurement_config.json", "{}"));
    ws.measurementComment = readRoot("measurement_comment.txt", "");
    ws.tags = readRoot("tags", "");
    ws.workspaceJson = parseJson(readRoot("workspace.json", "{}"));

    ws.uncorrectedIfg = loadFlatIfg(file.id, "igm_uncorrected_x", "interferogram", false);
    ws.correctedIfg = loadFlatIfg(file.id, "igm_corrected_x", "interferogram", true);
    ws.spectra = loadTwoColSub(file.id, "spectra", "spectrum/v1");
    ws.averageSpectra = loadTwoColSub(file.id, "average_spectra", "average_spectrum/v1");
    ws.snrSpectra = loadTwoColSub(file.id, "snr_spectra", "snr_spectrum/v1");
    ws.allanWerle = loadAllanSub(file.id, "allan_werle", "allan_werle/v1");
    ws.t100 = loadT100Sub(file.id, "t100", "t100/v1");
    return ws;
}

void H5Store::save(const std::string& path, const Workspace& ws) {
    std::vector<std::string> dangling = ws.danglingInputs();
    if (!dangling.empty())
        throw H5Error("save: dangling inputs (rule 10): " + joinDangling(dangling));

    Workspace existing;
    if (fileExists(path)) {
        existing = load(path);  // also validates @format/@schema
        verifyOriginalsUnchanged(existing, ws);
    }

    std::string tmp = path + ".tmp";
    try {
        H5FileGuard file(H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
        if (file.id < 0) fail("save: H5Fcreate");
        writeRoot(file.id, ws);
        writeFlatIfg(file.id, "igm_uncorrected_x", "interferogram", ws.uncorrectedIfg, true);
        writeFlatIfg(file.id, "igm_corrected_x", "interferogram", ws.correctedIfg, false);
        writeTwoColSub(file.id, "spectra", "spectrum/v1", ws.spectra);
        writeTwoColSub(file.id, "average_spectra", "average_spectrum/v1", ws.averageSpectra);
        writeTwoColSub(file.id, "snr_spectra", "snr_spectrum/v1", ws.snrSpectra);
        writeAllanSub(file.id, "allan_werle", "allan_werle/v1", ws.allanWerle);
        writeT100Sub(file.id, "t100", "t100/v1", ws.t100);
    } catch (...) {
        std::remove(tmp.c_str());
        throw;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        throw H5Error("save: rename failed for '" + path + "'");
    }
}

void H5Store::validate(const std::string& path) {
    Workspace ws = load(path);  // throws on @format/@schema violation
    H5FileGuard file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    if (file.id < 0) throw H5Error("validate: cannot open '" + path + "'");
    for (const char* r : {"workspace.json", "measurement_config.json",
                          "measurement_comment.txt", "tags"}) {
        if (!H5Lexists(file.id, r, H5P_DEFAULT))
            throw H5Error(std::string("validate: missing root dataset ") + r);
    }
    if (!ws.inputsAreValid())
        throw H5Error("validate: dangling inputs present");
}
