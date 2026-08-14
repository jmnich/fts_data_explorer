#pragma once

// Phase 4 (M4.4): per-tab-type dock-layout persistence. Each tab type keeps
// its own ImGui ini snapshot (`<imgui.ini>.layout.<type>` next to imgui.ini);
// switching tab types saves the outgoing type's layout and restores the
// incoming one. No snapshot yet = the current imgui.ini state is the fallback
// (pre-existing layouts keep working). Snapshots are written on switch away
// and reloaded on switch back — including across app restarts.

// ActiveTabKind → "session" / "workspace" / "environment".
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

// Delete the type's snapshot file (e.g. after a layout-version bump that
// renamed windows — the stale DockIds must not be restorable).
void resetTabLayout(const char* type);
