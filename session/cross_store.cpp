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
