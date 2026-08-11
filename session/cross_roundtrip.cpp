// fts_cross_roundtrip — CLI driver for the .cross.h5 cross-store (M2.4),
// driven by playground/multi_workspace_roundtrip.py (h5py can read the file
// structure but cannot call the C++ cross-store API).
//
// Usage:
//   fts_cross_roundtrip create <path>
//   fts_cross_roundtrip add <cross.h5> <src.h5> [--slow-save]
//   fts_cross_roundtrip remove <cross.h5> <id>
//   fts_cross_roundtrip list <cross.h5>
//   fts_cross_roundtrip load <cross.h5> <id>
//   fts_cross_roundtrip sniff <path>
//   fts_cross_roundtrip save-source <cross.h5> <id> <src.h5>
// Exit 0 = success, 1 = failure (message on stderr).

#include <cstdio>
#include <cstring>
#include <string>

#include "app_state.h"
#include "cross_store.h"
#include "hdf/h5_store.h"

namespace {

int fail(const std::string& msg) {
    std::fprintf(stderr, "%s\n", msg.c_str());
    return 1;
}

int cmdCreate(const std::string& path) {
    std::string err;
    if (!crossCreate(path, err)) return fail(err);
    std::printf("created %s\n", path.c_str());
    return 0;
}

int cmdAdd(const std::vector<std::string>& args) {
    bool slow = false;
    std::string crossPath, srcPath;
    for (const auto& a : args) {
        if (a == "--slow-save") slow = true;
        else if (crossPath.empty()) crossPath = a;
        else if (srcPath.empty()) srcPath = a;
    }
    if (crossPath.empty() || srcPath.empty())
        return fail("usage: add <cross.h5> <src.h5> [--slow-save]");
    std::string err, newId;
    if (!crossAddSource(crossPath, srcPath, newId, err, slow)) return fail(err);
    std::printf("%s\n", newId.c_str());
    if (slow) std::printf("slow-save: 2s window for the kill test\n");
    return 0;
}

int cmdRemove(const std::string& crossPath, const std::string& id) {
    std::string err;
    if (!crossRemoveSource(crossPath, id, err)) return fail(err);
    std::printf("removed %s\n", id.c_str());
    return 0;
}

int cmdList(const std::string& crossPath) {
    SessionTabState st;
    std::string err;
    if (!crossLoadInto(st, crossPath, err)) return fail(err);
    for (const auto& src : st.sources)
        std::printf("%s\t%s\t%zu\t%s\n", src.id.c_str(), src.name.c_str(),
                    src.memberCount, src.originPath.c_str());
    return 0;
}

int cmdLoad(const std::string& crossPath, const std::string& id) {
    std::string err;
    Workspace ws = crossLoadSource(crossPath, id, err);
    if (!err.empty()) return fail(err);
    std::printf("format=%s\n", ws.format.c_str());
    std::printf("comment=%s\n", ws.measurementComment.c_str());
    std::printf("tags=%s\n", ws.tags.c_str());
    std::printf("uncorrected=%zu corrected=%zu spectra=%zu\n",
                ws.uncorrectedIfg.members.size(), ws.correctedIfg.members.size(),
                ws.spectra.members.size());
    std::printf("workspaceJson=%s\n", ws.workspaceJson.dump().c_str());
    return 0;
}

int cmdSniff(const std::string& path) {
    std::printf(crossIsCrossFile(path) ? "cross\n" : "workspace\n");
    return 0;
}

int cmdSaveSource(const std::string& crossPath, const std::string& id,
                  const std::string& srcPath) {
    Workspace ws;
    try {
        ws = H5Store::load(srcPath);
    } catch (const std::exception& e) {
        return fail(std::string("load source failed: ") + e.what());
    }
    std::string err;
    try {
        crossSaveSource(crossPath, id, ws, err);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    if (!err.empty()) return fail(err);
    std::printf("saved %s\n", id.c_str());
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fts_cross_roundtrip create|add|remove|list|load|sniff|save-source ...\n");
        return 1;
    }
    const std::string mode = argv[1];
    if (mode == "create" && argc == 3) return cmdCreate(argv[2]);
    if (mode == "add") {
        std::vector<std::string> args(argv + 2, argv + argc);
        return cmdAdd(args);
    }
    if (mode == "remove" && argc == 4) return cmdRemove(argv[2], argv[3]);
    if (mode == "list" && argc == 3) return cmdList(argv[2]);
    if (mode == "load" && argc == 4) return cmdLoad(argv[2], argv[3]);
    if (mode == "sniff" && argc == 3) return cmdSniff(argv[2]);
    if (mode == "save-source" && argc == 5) return cmdSaveSource(argv[2], argv[3], argv[4]);
    std::fprintf(stderr, "bad arguments\n");
    return 1;
}
