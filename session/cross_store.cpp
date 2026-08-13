// Cross-store (.cross.h5) — embedded multi-workspace format (M2.4).
#include "cross_store.h"
#include "app_state.h"

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

// Rewrite the @summary attribute of a source group.
void writeSourceSummary(hid_t file, const std::string& id, const Workspace& ws,
                        const std::string& name, const std::string& originPath) {
    H5GroupGuard g(H5Gopen2(file, sourcePrefix(id).c_str(), H5P_DEFAULT));
    if (g.id < 0) throw H5Error("cross: source group '" + id + "' missing");
    nlohmann::json summary = {
        {"id", id},
        {"name", name},
        {"originPath", originPath},
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
            writeSourceSummary(file.id, id, ws, name, srcPath);
            appendSourceToManifest(file.id, {{"id", id}, {"name", name},
                                             {"originPath", srcPath},
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
            sum.originPath = e.value("originPath", "");
            sum.memberCount = e.value("memberCount", 0u);
            sum.createdIso = e.value("createdIso", "");
            st.sources.push_back(std::move(sum));
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
        writeSourceSummary(file.id, sourceId, ws, name, "");
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
