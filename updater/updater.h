#pragma once

#include <string>
#include <vector>

namespace updater {

// One-shot startup: removes stale update files and starts the automatic
// background check (honors its own cooldown/skip settings). Call once, as
// early as possible on the GUI path, before the window is created.
void startup();

// Menu entry: force a check now (ignores cooldown and the skipped version).
void requestManualCheck();

// Per-frame poll of the background worker (atomics only; safe before
// ImGui::NewFrame). Keeps the idle redraw gate informed.
void tick();

// Draws the updater modal. Must be called first after ImGui::NewFrame(),
// before any other screen (the home screen included). Returns true when a
// modal was drawn this frame.
bool render();

// One-shot: true once, on the frame after the user chose "Restart Now" and
// the swap succeeded. The caller closes the window through its own path.
bool takeCloseRequest();

// True while the worker is in flight or the modal is up (redraw gate).
bool busy();

// True when the user chose "Restart Now" and applyStaged() succeeded.
bool restartArmed();

// Relaunches the (already replaced) executable with the original argv.
// Used by main() after the frame loop has exited.
bool relaunch(const std::vector<std::string>& args);

// Settings menu toggle, persisted in the updater's own config file.
bool autoCheckEnabled();
void setAutoCheckEnabled(bool enabled);

} // namespace updater
