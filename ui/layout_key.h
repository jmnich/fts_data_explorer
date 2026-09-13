#pragma once

// Per-workspace layout snapshot naming. A workspace layout is keyed by the
// session's stable key (a workspace path, or "multi-workspace.h5#sourceId")
// and hashed into a filename-safe suffix:
//   <imgui.ini>.layout.workspace.<16hex>      (+ ".sel" sidecar)

#include <cstdio>
#include <functional>
#include <string>

// Snapshot key suffix ("<16hex>"). pruneStaleWorkspaceLayouts parses the SAME
// suffix out of snapshot file names ("imgui.ini.layout.workspace.<hex>[/.sel]")
// and compares it against this form — changing it requires updating that scan,
// or every layout snapshot is silently deleted at exit (bugfix 2026-09-13).
inline std::string workspaceLayoutHex(const std::string& key) {
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016zx", std::hash<std::string>{}(key));
    return std::string(hex);
}

inline std::string workspaceLayoutName(const std::string& key) {
    return "workspace." + workspaceLayoutHex(key);
}
