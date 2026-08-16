// Cross-store (.cross.h5) — embedded multi-workspace format (M2.4).
#include "cross_store.h"
#include "app_state.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>

#include "hdf/h5_store.h"
#include "hdf/hdf5_util.h"

namespace {

constexpr int kManifestVersion = 2;

// Mirrors workspaceFileList's read-priority (corrected > uncorrected >
// spectra originals) so @summary's memberCount needs no workspace_reader link.
size_t sourceMemberCount(const Workspace& ws) {
    if (!ws.correctedIfg.members.empty()) return ws.correctedIfg.members.size();
    if (!ws.uncorrectedIfg.members.empty()) return ws.uncorrectedIfg.members.size();
    size_t n = 0;
    for (const auto& m : ws.spectra.members)
        if (m.kind == MemberKind::Original) ++n;
    return n;
}

std::string slugFromPath(const std::string& p) {
    std::string stem = std::filesystem::path(p).stem().string();
    std::string out;
    for (char c : stem) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
            out += c;
        else
            out += '_';
    }
    return out.empty() ? "source" : out;
}

std::string sourcePrefix(const std::string& id) {
    return "sources/" + id;
}

// H5Ovisit3 callback: accumulate an object's storage footprint — object
// header (hdr.space.total) + B-tree/heap metadata (meta_size) + the dataset
// data itself (H5Dget_storage_size). H5O_info2_t carries no sizes in
// HDF5 1.14, so native info is fetched per object via the root-relative name.
struct VisitAccum {
    hid_t root;
    uint64_t total = 0;
};

herr_t h5VisitAccumSize(hid_t, const char* name, const H5O_info2_t* info,
                        void* opData) {
    auto* accum = static_cast<VisitAccum*>(opData);
    H5O_native_info_t ni;
    if (H5Oget_native_info_by_name(accum->root, name, &ni, H5O_NATIVE_INFO_ALL,
                                   H5P_DEFAULT) >= 0) {
        accum->total += static_cast<uint64_t>(ni.hdr.space.total) +
                        ni.meta_size.obj.index_size + ni.meta_size.obj.heap_size +
                        ni.meta_size.attr.index_size + ni.meta_size.attr.heap_size;
    }
    if (info->type == H5O_TYPE_DATASET) {
        H5DatasetGuard ds(H5Dopen2(accum->root, name, H5P_DEFAULT));
        if (ds.id >= 0) accum->total += static_cast<uint64_t>(H5Dget_storage_size(ds.id));
    }
    return 0;
}

// HDF5 storage footprint of a group subtree. HDF5 has no direct per-group
// size API — this is the standard approximation (within a few % of the true
// on-disk delta, excluding shared-metadata/free-space bookkeeping). Returns 0
// on any failure (missing group, walk error).
uint64_t h5GroupStorage(hid_t file, const std::string& groupPath) {
    H5GroupGuard g(H5Gopen2(file, groupPath.c_str(), H5P_DEFAULT));
    if (g.id < 0) return 0;
    VisitAccum accum{g.id, 0};
    if (H5Ovisit3(g.id, H5_INDEX_NAME, H5_ITER_NATIVE, h5VisitAccumSize, &accum,
                  H5O_INFO_ALL) < 0)
        return 0;
    return accum.total;
}

std::string experimentPrefix(const std::string& id) {
    return "experiments/" + id;
}

// Collect the link names of `group` (dataset enumeration for results/).
struct LinkNames {
    std::vector<std::string> names;
    static herr_t visit(hid_t, const char* name, const H5L_info_t*, void* op) {
        static_cast<LinkNames*>(op)->names.push_back(name);
        return 0;
    }
};

nlohmann::json readManifest(hid_t file) {
    if (!H5Lexists(file, "archive.json", H5P_DEFAULT))
        throw H5Error("cross: missing archive.json (not a .cross.h5)");
    nlohmann::json j = nlohmann::json::parse(h5ReadVlenString(file, "archive.json"),
                                             nullptr, false);
    if (j.is_discarded() || !j.is_object())
        throw H5Error("cross: archive.json is not a JSON object");
    const int version = j.value("version", 0);
    if (version != kManifestVersion)
        throw H5Error("cross: unsupported archive version " + std::to_string(version) +
                      " (expected " + std::to_string(kManifestVersion) + ")");
    return j;
}

void writeManifest(hid_t file, const nlohmann::json& manifest) {
    if (H5Lexists(file, "archive.json", H5P_DEFAULT))
        H5Ldelete(file, "archive.json", H5P_DEFAULT);
    h5WriteVlenString(file, "archive.json", manifest.dump());
}

// Rewrite the @summary attribute of a source group. The original file path
// is deliberately NOT stored (nothing in the archive should reference the
// pre-embedding location).
void writeSourceSummary(hid_t file, const std::string& id, const Workspace& ws,
                        const std::string& name) {
    H5GroupGuard g(H5Gopen2(file, sourcePrefix(id).c_str(), H5P_DEFAULT));
    if (g.id < 0) throw H5Error("cross: source group '" + id + "' missing");
    nlohmann::json summary = {
        {"id", id},
        {"name", name},
        {"memberCount", sourceMemberCount(ws)},
        {"createdIso", h5UtcNowIso()},
    };
    h5WriteAttrString(g.id, "summary", summary.dump());
}

// Atomic archive mutation: copy `path` to a temp sibling, run `mutate` on the
// temp, rename over the original (delete-then-rename fallback for Windows,
// where rename-over-existing fails). A crash leaves the original intact and
// at most a stale .tmp behind.
bool atomicMutate(const std::string& path,
                  const std::function<void(const std::string& tmp)>& mutate,
                  std::string& err, bool slowSave = false) {
    const std::string tmp = path + ".tmp";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);   // stale temp from a crash
    std::filesystem::copy_file(path, tmp, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        err = "cross: copy to temp failed: " + ec.message();
        return false;
    }
    if (slowSave) std::this_thread::sleep_for(std::chrono::seconds(2));  // kill-test window
    try {
        mutate(tmp);
    } catch (const std::exception& e) {
        std::filesystem::remove(tmp);
        err = e.what();
        return false;
    }
    // HDF5 1.14.3 quirk: after a mutating close, a SUBSEQUENT open of the same
    // file (without an intervening open/close) can read stale vlen blob-ids
    // from the metadata cache — the file on disk is fine, but the next open
    // misreads it ("global heap object size does not match"). One RDONLY
    // open+close finalizes the pending metadata-cache state before the rename
    // (empirically verified: without it, two mutations in a row corrupt the
    // second one's manifest read; with it, all mutation sequences pass).
    {
        H5FileGuard heal(H5Fopen(tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
        if (heal.id < 0) {
            std::filesystem::remove(tmp);
            err = "cross: post-mutate reopen failed";
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        // Windows: rename over an existing target is not allowed.
        std::error_code ec2;
        std::filesystem::remove(path, ec2);
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) {
        std::filesystem::remove(tmp);
        err = "cross: rename failed: " + ec.message();
        return false;
    }
    return true;
}

void appendSourceToManifest(hid_t file, const nlohmann::json& entry) {
    nlohmann::json manifest = readManifest(file);
    manifest["sources"].push_back(entry);
    writeManifest(file, manifest);
}

}  // namespace

bool crossIsCrossFile(const std::string& path) {
    H5FileGuard file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    return file.id >= 0 && H5Lexists(file.id, "archive.json", H5P_DEFAULT);
}

bool crossCreate(const std::string& path, std::string& err) {
    const std::string tmp = path + ".tmp";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    try {
        H5FileGuard file(H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT));
        if (file.id < 0) throw H5Error("crossCreate: H5Fcreate failed");
        nlohmann::json manifest = {{"version", kManifestVersion}, {"sources", nlohmann::json::array()}};
        writeManifest(file.id, manifest);
    } catch (const std::exception& e) {
        std::filesystem::remove(tmp);
        err = e.what();
        return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        err = "crossCreate: rename failed: " + ec.message();
        return false;
    }
    return true;
}

bool crossCreateFromDataset(AppState&, const std::string& path,
                            const std::string& srcPath, std::string& err) {
    if (!crossCreate(path, err)) return false;
    std::string newId;
    return crossAddSource(path, srcPath, newId, err);
}

bool crossAddSource(const std::string& path, const std::string& srcPath,
                    std::string& newId, std::string& err, bool slowSave) {
    try {
        Workspace ws = H5Store::load(srcPath);   // throws H5Error on invalid input

        // Unique id within the archive (makeUniqueId's "_0001" suffix scheme).
        std::vector<std::string> existingIds;
        {
            H5FileGuard file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
            if (file.id < 0) throw H5Error("crossAddSource: cannot open '" + path + "'");
            for (const auto& e : readManifest(file.id).value("sources", nlohmann::json::array()))
                existingIds.push_back(e.value("id", ""));
        }
        const std::string id = makeUniqueId(slugFromPath(srcPath), existingIds);
        newId = id;

        const std::string prefix = sourcePrefix(id);
        const std::string name = std::filesystem::path(srcPath).stem().string();
        if (!atomicMutate(path, [&](const std::string& tmp) {
            H5Store::saveGroup(tmp, prefix, ws);            // embed the content
            H5FileGuard file(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
            if (file.id < 0) throw H5Error("crossAddSource: reopen failed");
            writeSourceSummary(file.id, id, ws, name);
            appendSourceToManifest(file.id, {{"id", id}, {"name", name},
                                             {"memberCount", sourceMemberCount(ws)},
                                             {"createdIso", h5UtcNowIso()}});
        }, err, slowSave)) {
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();   // never throws into the UI callers
        return false;
    }
}

bool crossRemoveSource(const std::string& path, const std::string& id, std::string& err) {
    return atomicMutate(path, [&](const std::string& tmp) {
        H5FileGuard file(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
        if (file.id < 0) throw H5Error("crossRemoveSource: cannot open temp");
        // Parent-aware existence check: H5Lexists on a slash path whose parent
        // group is missing fails with an error stack + auto-print.
        const std::string prefix = sourcePrefix(id);
        const size_t slash = prefix.find('/');
        if (H5Lexists(file.id, prefix.substr(0, slash).c_str(), H5P_DEFAULT) > 0 &&
            H5Lexists(file.id, prefix.c_str(), H5P_DEFAULT) > 0)
            H5Ldelete(file.id, prefix.c_str(), H5P_DEFAULT);
        nlohmann::json manifest = readManifest(file.id);
        auto& sources = manifest["sources"];
        sources.erase(std::remove_if(sources.begin(), sources.end(),
                                     [&](const nlohmann::json& e) {
                                         return e.value("id", "") == id;
                                     }),
                      sources.end());
        writeManifest(file.id, manifest);
    }, err);
}

// Best-effort re-walk of every embedded source group after an archive save,
// so the Session-tab sizes follow the on-disk state.
void crossRefreshSourceSizes(SessionTabState& st, const std::string& path) {
    try {
        H5FileGuard file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
        if (file.id < 0) return;
        for (auto& sum : st.sources)
            sum.sizeBytes = h5GroupStorage(file.id, sourcePrefix(sum.id));
    } catch (...) {
        // Best-effort: leave stale cached sizes on failure.
    }
}

// Same for persisted experiments — sizes shown in Active Experiments follow
// the on-disk state (transient instances have no group: sizeBytes stays 0).
void crossRefreshExperimentSizes(AppState& s, const std::string& path) {
    try {
        H5FileGuard file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
        if (file.id < 0) return;
        for (auto& env : s.experiments) {
            if (env->id.empty()) { env->sizeBytes = 0; continue; }
            env->sizeBytes = h5GroupStorage(file.id, experimentPrefix(env->id));
        }
    } catch (...) {
        // Best-effort: leave stale cached sizes on failure.
    }
}

bool crossLoadInto(SessionTabState& st, const std::string& path, std::string& err) {
    try {
        H5FileGuard file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
        if (file.id < 0) throw H5Error("crossLoad: cannot open '" + path + "'");
        const nlohmann::json manifest = readManifest(file.id);
        st.sources.clear();
        st.sourceCache.clear();   // source workspaces may have changed
        for (const auto& e : manifest.value("sources", nlohmann::json::array())) {
            SourceSummary sum;
            sum.id = e.value("id", "");
            sum.name = e.value("name", sum.id);
            sum.memberCount = e.value("memberCount", 0u);
            sum.createdIso = e.value("createdIso", "");
            sum.open = e.value("open", false);
            sum.sizeBytes = h5GroupStorage(file.id, sourcePrefix(sum.id));
            st.sources.push_back(std::move(sum));
        }
        // Tab-strip order (bugfix 2026-08-14): "tabOrder" lists stable keys
        // in strip order — "ws:<sourceId>" → openTabIds, "exp:<id>" →
        // experimentTabOrder; the RAW list is kept so the full interleave can
        // be restored into AppState::tabStripOrder on load. Legacy files
        // store "openTabs" (source ids) or per-source "open" booleans; fall
        // back in sources order.
        st.openTabIds.clear();
        st.experimentTabOrder.clear();
        st.tabOrder.clear();
        auto to = manifest.find("tabOrder");
        if (to != manifest.end() && to->is_array()) {
            for (const auto& k : *to) {
                if (!k.is_string()) continue;
                const std::string s = k.get<std::string>();
                st.tabOrder.push_back(s);
                if (s.rfind("ws:", 0) == 0) st.openTabIds.push_back(s.substr(3));
                else if (s.rfind("exp:", 0) == 0) st.experimentTabOrder.push_back(s.substr(4));
            }
        } else {
            auto ot = manifest.find("openTabs");
            if (ot != manifest.end() && ot->is_array()) {
                for (const auto& id : *ot)
                    if (id.is_string()) st.openTabIds.push_back(id.get<std::string>());
            } else {
                for (const auto& src : st.sources)
                    if (src.open) st.openTabIds.push_back(src.id);
            }
        }
        st.multiWorkspaceOpen = true;
        st.multiWorkspacePath = path;
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool crossLoad(AppState& s, const std::string& path, std::string& err) {
    return crossLoadInto(s.sessionTab, path, err);
}

Workspace crossLoadSource(const std::string& crossPath, const std::string& sourceId,
                          std::string& err) {
    try {
        return H5Store::loadGroup(crossPath, sourcePrefix(sourceId));
    } catch (const std::exception& e) {
        err = e.what();
        return Workspace{};
    }
}

void crossSaveSource(const std::string& crossPath, const std::string& sourceId,
                     const Workspace& ws, std::string& err) {
    const std::string prefix = sourcePrefix(sourceId);
    const std::string name = std::filesystem::path(sourceId).stem().string();
    const bool ok = atomicMutate(crossPath, [&](const std::string& tmp) {
        H5Store::saveGroup(tmp, prefix, ws);
        H5FileGuard file(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
        if (file.id < 0) throw H5Error("crossSaveSource: reopen failed");
        writeSourceSummary(file.id, sourceId, ws, name);
        // Refresh the manifest entry in the same save (member count etc.).
        nlohmann::json manifest = readManifest(file.id);
        for (auto& e : manifest["sources"]) {
            if (e.value("id", "") == sourceId) {
                e["memberCount"] = sourceMemberCount(ws);
                e["name"] = name;
            }
        }
        writeManifest(file.id, manifest);
    }, err);
    if (!ok) throw H5Error(err);
}
// Persist the tab-strip's EXACT visual order (bugfix 2026-08-14): the
// manifest "tabOrder" array lists stable keys in strip order —
// "ws:<sourceId>" for embedded workspace tabs, "exp:<experimentId>" for
// experiments — interleaved exactly as shown, so reopening rebuilds the same
// strip (including workspaces dragged to the right of experiment tabs).
// Written on explicit saves (Ctrl+S / exit Save All / project-switch save) —
// every write is a full-file copy.
void crossSaveTabOrder(const std::string& path,
                       const std::vector<std::string>& tabOrder,
                       std::string& err) {
    const bool ok = atomicMutate(path, [&](const std::string& tmp) {
        H5FileGuard file(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
        if (file.id < 0) throw H5Error("crossSaveTabOrder: cannot open temp");
        nlohmann::json manifest = readManifest(file.id);
        manifest["tabOrder"] = tabOrder;
        writeManifest(file.id, manifest);
    }, err);
    if (!ok) throw H5Error(err);
}

// AppState-level helper: the ids of the currently-open embedded source tabs,
// IN sessions[] order (the order the strip shows them in).
std::vector<std::string> openEmbeddedSourceIds(const AppState& s) {
    std::vector<std::string> ids;
    for (const auto& sess : s.sessions) {
        const size_t hash = sess->key.find('#');
        if (hash != std::string::npos)
            ids.push_back(sess->key.substr(hash + 1));
    }
    return ids;
}

// Reduce the captured strip order to what a .cross.h5 can restore: embedded
// workspace tabs ("ws:<sourceId>") + experiments with a persisted id
// ("exp:<id>"). Standalone workspace tabs and unsaved experiments can't be
// reopened — they are dropped (they re-append at the end after a reload).
// Falls back to the sessions-only order when no strip has rendered yet.
std::vector<std::string> persistableTabOrder(const AppState& s) {
    std::vector<std::string> out;
    const auto& order = s.tabStripOrder;
    if (order.empty()) {
        for (const auto& id : openEmbeddedSourceIds(s)) out.push_back("ws:" + id);
        return out;
    }
    for (const auto& k : order) {
        if (k.rfind("ws:", 0) == 0) {
            const std::string key = k.substr(3);
            const size_t hash = key.find('#');
            if (hash != std::string::npos)
                out.push_back("ws:" + key.substr(hash + 1));
        } else if (k.rfind("exp:", 0) == 0) {
            const std::string nameOrKey = k.substr(4);
            for (const auto& e : s.experiments) {
                // Match the rename-stable stripKey (live captures), the
                // persisted id, or the instance name (legacy captures).
                if (!e->id.empty() &&
                    (e->id == nameOrKey || e->instanceName == nameOrKey ||
                     e->stripKey == nameOrKey)) {
                    out.push_back("exp:" + e->id);
                    break;
                }
            }
        }
    }
    return out;
}

// ── Phase 4: experiments ────────────────────────────────────────────────────

bool crossExperimentWrite(const std::string& path, const std::string& expId,
                          const nlohmann::json& config,
                          const nlohmann::json& fingerprints,
                          const std::map<std::string, std::vector<double>>& results,
                          const nlohmann::json& stats, std::string& err) {
    const std::string prefix = experimentPrefix(expId);
    return atomicMutate(path, [&](const std::string& tmp) {
        H5FileGuard file(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
        if (file.id < 0) throw H5Error("crossExperimentWrite: cannot open temp");
        // Parent-aware existence: H5Lexists on a slash path with a missing
        // parent errors out (hdf5 quirk fixed in Phase 2).
        const bool haveParent = H5Lexists(file.id, "experiments", H5P_DEFAULT) > 0;
        if (haveParent && H5Lexists(file.id, prefix.c_str(), H5P_DEFAULT) > 0)
            H5Ldelete(file.id, prefix.c_str(), H5P_DEFAULT);
        if (!haveParent &&
            H5Gcreate2(file.id, "experiments", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT) < 0)
            throw H5Error("crossExperimentWrite: cannot create experiments/");
        H5GroupGuard g(H5Gcreate2(file.id, prefix.c_str(), H5P_DEFAULT,
                                  H5P_DEFAULT, H5P_DEFAULT));
        if (g.id < 0) throw H5Error("crossExperimentWrite: cannot create group '" + expId + "'");
        h5WriteVlenString(g.id, "config.json", config.dump());
        h5WriteVlenString(g.id, "fingerprint.json", fingerprints.dump());
        if (!results.empty()) {
            H5GroupGuard rg(H5Gcreate2(g.id, "results", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
            if (rg.id < 0) throw H5Error("crossExperimentWrite: cannot create results/");
            for (const auto& [name, vec] : results)
                h5WriteFp64Vector(rg.id, name.c_str(), vec);
        }
        h5WriteVlenString(g.id, "stats.json", stats.dump());
        // Manifest entry (append or replace in the same save).
        nlohmann::json manifest = readManifest(file.id);
        if (manifest.find("experiments") == manifest.end())
            manifest["experiments"] = nlohmann::json::array();
        nlohmann::json entry = {
            {"id", expId},
            {"name", config.value("name", expId)},
            {"type", config.value("type", "absorbance")},
            {"createdIso", h5UtcNowIso()},
        };
        auto& exps = manifest["experiments"];
        bool found = false;
        for (auto& e : exps)
            if (e.value("id", "") == expId) { e = entry; found = true; break; }
        if (!found) exps.push_back(entry);
        writeManifest(file.id, manifest);
    }, err);
}

bool crossExperimentRemove(const std::string& path, const std::string& expId,
                           std::string& err) {
    return atomicMutate(path, [&](const std::string& tmp) {
        H5FileGuard file(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
        if (file.id < 0) throw H5Error("crossExperimentRemove: cannot open temp");
        const std::string prefix = experimentPrefix(expId);
        if (H5Lexists(file.id, "experiments", H5P_DEFAULT) > 0 &&
            H5Lexists(file.id, prefix.c_str(), H5P_DEFAULT) > 0)
            H5Ldelete(file.id, prefix.c_str(), H5P_DEFAULT);
        nlohmann::json manifest = readManifest(file.id);
        if (manifest.find("experiments") != manifest.end()) {
            auto& exps = manifest["experiments"];
            exps.erase(std::remove_if(exps.begin(), exps.end(),
                                      [&](const nlohmann::json& e) {
                                          return e.value("id", "") == expId;
                                      }),
                       exps.end());
        }
        writeManifest(file.id, manifest);
    }, err);
}

bool crossExperimentList(const std::string& path,
                         std::vector<nlohmann::json>& entries, std::string& err) {
    try {
        H5FileGuard file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
        if (file.id < 0) throw H5Error("crossExperimentList: cannot open '" + path + "'");
        const nlohmann::json manifest = readManifest(file.id);
        entries.clear();
        for (const auto& e : manifest.value("experiments", nlohmann::json::array()))
            entries.push_back(e);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool crossExperimentRead(const std::string& path, const std::string& expId,
                         nlohmann::json& config, nlohmann::json& fingerprints,
                         std::map<std::string, std::vector<double>>& results,
                         nlohmann::json& stats, std::string& err) {
    try {
        H5FileGuard file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
        if (file.id < 0) throw H5Error("crossExperimentRead: cannot open '" + path + "'");
        const std::string prefix = experimentPrefix(expId);
        H5GroupGuard g(H5Gopen2(file.id, prefix.c_str(), H5P_DEFAULT));
        if (g.id < 0) throw H5Error("crossExperimentRead: experiment group '" + expId + "' missing");
        config = nlohmann::json::parse(h5ReadVlenString(g.id, "config.json"), nullptr, false);
        if (config.is_discarded()) config = nlohmann::json::object();
        fingerprints = nlohmann::json::parse(h5ReadVlenString(g.id, "fingerprint.json"),
                                             nullptr, false);
        if (fingerprints.is_discarded()) fingerprints = nlohmann::json::object();
        stats = nlohmann::json::parse(h5ReadVlenString(g.id, "stats.json"), nullptr, false);
        if (stats.is_discarded()) stats = nlohmann::json::object();
        results.clear();
        if (H5Lexists(g.id, "results", H5P_DEFAULT) > 0) {   // parent-aware (no error spam)
            H5GroupGuard rg(H5Gopen2(g.id, "results", H5P_DEFAULT));
            if (rg.id >= 0) {
                LinkNames links;
                hsize_t idx = 0;
                H5Literate(rg.id, H5_INDEX_NAME, H5_ITER_INC, &idx, LinkNames::visit, &links);
                for (const auto& name : links.names) {
                    std::vector<double> vec;
                    h5ReadFp64Vector(rg.id, name.c_str(), vec);
                    results[name] = std::move(vec);
                }
            }
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

// ── Batch-processing recipes (M-batch) ──────────────────────────────────────
// Root group "recipes/", one vlen-string dataset per recipe (content = the
// recipe JSON). Additive to the cross file: the manifest is untouched, and
// H5Store::validate is never invoked on cross files.

bool crossRecipeWrite(const std::string& path, const std::string& name,
                      const nlohmann::json& recipe, std::string& err) {
    if (name.empty() || name.find('/') != std::string::npos) {
        err = "recipes: invalid recipe name";
        return false;
    }
    const std::string payload = recipe.dump();
    return atomicMutate(path, [&](const std::string& tmp) {
        H5FileGuard f(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
        if (f.id < 0) throw H5Error("recipes: open failed");
        // atomicMutate copies the WHOLE file to tmp, so an existing recipes/
        // group survives the copy — open it instead of recreating
        // (H5Gcreate2 would fail "name already exists" on every write after
        // the first).
        const bool exists = H5Lexists(f.id, "recipes", H5P_DEFAULT) > 0;
        hid_t gid = exists ? H5Gopen2(f.id, "recipes", H5P_DEFAULT)
                           : H5Gcreate2(f.id, "recipes", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        if (gid < 0) throw H5Error("recipes: group open/create failed");
        H5GroupGuard g(gid);
        if (H5Lexists(g.id, name.c_str(), H5P_DEFAULT))
            H5Ldelete(g.id, name.c_str(), H5P_DEFAULT);
        h5WriteVlenString(g.id, name.c_str(), payload);
    }, err);
}

bool crossRecipeList(const std::string& path, std::vector<std::string>& names,
                     std::string& err) {
    try {
        H5FileGuard f(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
        if (f.id < 0) { err = "recipes: open failed"; return false; }
        names.clear();
        if (!H5Lexists(f.id, "recipes", H5P_DEFAULT)) return true;   // absent group = no recipes
        H5GroupGuard g(H5Gopen2(f.id, "recipes", H5P_DEFAULT));
        if (g.id < 0) { err = "recipes: group open failed"; return false; }
        struct Ctx { std::vector<std::string>* names; };
        Ctx ctx{&names};
        auto visit = [](hid_t g, const char* n, const H5L_info_t*, void* op) -> herr_t {
            auto* c = static_cast<Ctx*>(op);
            c->names->emplace_back(n);
            return 0;
        };
        if (H5Literate(g.id, H5_INDEX_NAME, H5_ITER_INC, nullptr, visit, &ctx) < 0) {
            err = "recipes: iterate failed";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool crossRecipeRead(const std::string& path, const std::string& name,
                     nlohmann::json& out, std::string& err) {
    try {
        H5FileGuard f(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
        if (f.id < 0) { err = "recipes: open failed"; return false; }
        H5GroupGuard g(H5Gopen2(f.id, "recipes", H5P_DEFAULT));
        if (g.id < 0) { err = "recipes: group open failed"; return false; }
        if (!H5Lexists(g.id, name.c_str(), H5P_DEFAULT)) {
            err = "recipes: '" + name + "' not found";
            return false;
        }
        out = nlohmann::json::parse(h5ReadVlenString(g.id, name.c_str()), nullptr, false);
        if (out.is_discarded() || !out.is_object()) {
            err = "recipes: '" + name + "' is not a JSON object";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool crossRecipeRemove(const std::string& path, const std::string& name,
                       std::string& err) {
    return atomicMutate(path, [&](const std::string& tmp) {
        H5FileGuard f(H5Fopen(tmp.c_str(), H5F_ACC_RDWR, H5P_DEFAULT));
        if (f.id < 0) throw H5Error("recipes: open failed");
        if (H5Lexists(f.id, "recipes", H5P_DEFAULT) > 0) {
            H5GroupGuard g(H5Gopen2(f.id, "recipes", H5P_DEFAULT));
            if (g.id >= 0 && H5Lexists(g.id, name.c_str(), H5P_DEFAULT))
                H5Ldelete(g.id, name.c_str(), H5P_DEFAULT);
        }
    }, err);
}
