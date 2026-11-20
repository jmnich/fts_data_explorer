#pragma once

#include <string>

#include "config.h"
#include "update_service.h"

struct AppState;

// Auto-update UI state (Help -> Check for updates + the startup check).
// Owns the background UpdateService worker; the frame loop polls its phase
// through tickUpdateDialog and joins it through the service itself
// (IMGUI_GUIDE §13).
struct UpdateDialogState {
    UpdateService service;
    bool open = false;          // a modal should be/shown
    bool manual = false;        // started from the menu; ignores cooldown/skip
    bool autoStarted = false;   // the one-shot startup check fired
    bool offerDismissed = false;// "Later" pressed for this offer
    bool skipRequested = false; // "Skip this version" pressed
    bool notesExpanded = false; // full release notes vs the first lines
    int  lastPhase = 0;         // UpdatePhase int, for transition detection
    double upToDateUntil = 0.0; // manual "up to date" toast deadline
};

// Startup (GUI only): stale-file cleanup + one background check when
// APP_IS_RELEASE, config.autoUpdateCheck, cooldown elapsed and the release is
// not the skipped version. Call once after the window is up.
void startupUpdateCheck(AppState& s);
// Per-frame poll: opens/closes the modal, arms needsRedraw, persists
// skipped_version and last_check on transitions.
void tickUpdateDialog(AppState& s);
// Draws the offer/progress/ready/failed/up-to-date modals (no-op when idle).
void renderUpdateDialog(AppState& s);
// Help -> "Check for updates…": starts a manual check (or just opens).
void openUpdateDialogManual(AppState& s);
// Redraw gate: worker in flight or modal open.
bool updateDialogInFlight(const AppState& s);
