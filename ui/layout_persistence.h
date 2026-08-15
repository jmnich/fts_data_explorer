#pragma once

// Phase 4 (M4.4): per-tab-type dock-layout persistence. Each tab type keeps
// its own ImGui ini snapshot (`<imgui.ini>.layout.<type>` next to imgui.ini);
// switching tab types saves the outgoing type's layout and restores the
// incoming one. No snapshot yet = the current imgui.ini state is the fallback
// (pre-existing layouts keep working). Snapshots are written on switch away
// and reloaded on switch back — including across app restarts.
//
// Alongside the ini snapshot, a small sidecar (`...layout.<type>.sel`) records
// the SELECTED window per dock node ("0x<NodeID>=<WindowName>" per leaf node
// with >= 2 windows). ImGui's own ini restore does not reliably re-apply the
// snapshot's Selected= to nodes that already exist (DockContextBuildNodesFrom
// Settings skips duplicates, imgui.cpp:18426), and several internal paths
// override the selection on the restore frame (Windows[0] fallback
// imgui.cpp:19535, AutoSelectNewTabs imgui_widgets.cpp:10731). The app
// re-asserts the saved selection pre-DockSpace via takeRestoredNodeSelection.

#include <map>
#include <string>

#include <imgui.h>

// ActiveTabKind → "session" / "workspace" / "experiment".
const char* tabTypeName(int activeTabKind);

// Snapshot the CURRENT ImGui settings (docks + window layout) to the type's
// file. Safe to call mid-frame (SaveIniSettingsToMemory is read-only); the
// returned buffer is copied into the file immediately — it aliases ImGui's
// internal settings buffer, which later calls may resize or free.
void saveTabLayout(const char* type);

// Restore the type's snapshot into ImGui. No-op when no snapshot exists
// (the current layout / imgui.ini stays authoritative). Loads replace the
// full settings state — windows created after this call pick up their saved
// dock ids.
void restoreTabLayout(const char* type);

// Per-workspace layout snapshots (bugfix 2026-08-15): each dataset keeps its
// OWN docking arrangement, keyed by the session's stable `key` (workspace
// path, or "cross.h5#sourceId"). A shared "workspace" snapshot would make
// every workspace reflect the last one's layout.
void saveWorkspaceLayout(const std::string& key);
void restoreWorkspaceLayout(const std::string& key);

// Delete the type's snapshot file (e.g. after a layout-version bump that
// renamed windows — the stale DockIds must not be restorable).
void resetTabLayout(const char* type);

// Consume the node→selected-window map captured by the last restoreTabLayout.
// The caller re-applies it (pre-DockSpace) because ImGui's own restore does
// not reliably honor the snapshot's per-node selection. Returns-and-clears.
std::map<ImGuiID, std::string> takeRestoredNodeSelection();
