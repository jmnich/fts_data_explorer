// Auto-update UI: modals for the GitHub-releases update flow. The worker and
// all network I/O live in io/update_service; this file only polls the phase
// (IMGUI_GUIDE §13) and draws the toast-style modals (§14/§15/§17).
#include "pthread_compat.h" // GCC 16+: declares pthread_cond_clockwait etc. before <mutex>
#include "update_dialog.h"

#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "app_state.h"
#include "config.h"
#include "open_default.h"
#include "popup_utils.h"
#include "wrap_text.h"
#include "imgui.h"
#include "version.h"

// Accent for the update modals. Deliberately NOT modalAccent() (defined in
// ui/app_loop.cpp — no cross-file symbol here): the active-button colour is
// the same family and always available.
static ImVec4 updateAccent() {
    return ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
}

// A modal button was consumed: render the closing/switching frame (§17).
static void wake(AppState& s) { s.needsRedraw = true; }

// Shared body of both check entry points: start a background check against
// the configured repo slug. Cooldown/skip filtering happens in the caller.
static void startUpdateCheck(AppState& s) {
    UpdateDialogState& st = s.updateDialog;
    st.open = false;
    st.notesExpanded = false;
    UpdateCheckConfig check;
    check.repoSlug = s.configPtr ? s.configPtr->updateRepoUrl : std::string();
    check.currentVersion = APP_VERSION;
    st.service.startCheck(check);
    st.lastPhase = static_cast<int>(st.service.phase());
    wake(s);   // keep polling while the worker runs (§17)
}

void startupUpdateCheck(AppState& s) {
    cleanupStaleUpdateFiles();
    UpdateDialogState& st = s.updateDialog;
    if (!APP_IS_RELEASE || st.autoStarted) return;
    st.autoStarted = true;
    const AppConfig* cfg = s.configPtr;
    if (!cfg) return;
    const long long now = static_cast<long long>(std::time(nullptr));
    if (cfg->autoUpdateCheck && now - cfg->lastUpdateCheckEpoch >= 86400) {
        // Silent until a result arrives: tickUpdateDialog only raises a modal
        // when the check actually finds an update.
        st.manual = false;
        startUpdateCheck(s);
    }
}

void openUpdateDialogManual(AppState& s) {
    UpdateDialogState& st = s.updateDialog;
    if (st.service.busy()) {
        // A check/download is already in flight: just surface its modal.
        st.open = true;
        st.manual = true;
        wake(s);
        return;
    }
    st.service.reset();   // safe while !busy(): joins a finished worker only
    st.manual = true;
    st.offerDismissed = false;
    startUpdateCheck(s);  // manual checks ignore cooldown and skip
}

bool updateDialogInFlight(const AppState& s) {
    return s.updateDialog.service.busy() || s.updateDialog.open;
}

// Check-result edge (Checking -> UpToDate/Available/Failed): persist the
// check timestamp (clamped one second into the future so a result landing
// inside the start second still counts as a full second elapsed) so the
// startup cooldown sees it.
static void persistCheckTime(AppState& s) {
    if (!s.configPtr || s.configFilePath.empty()) return;
    s.configPtr->lastUpdateCheckEpoch =
        static_cast<long long>(std::time(nullptr)) + 1;
    s.configPtr->saveToFile(s.configFilePath);
}

// "Skip this version" was pressed: persist the skipped version and drop the
// offer. Runs while the release info is still readable (reset() clears it).
static void applySkipRequest(AppState& s) {
    UpdateDialogState& st = s.updateDialog;
    if (s.configPtr) {
        s.configPtr->skippedUpdateVersion = st.service.release().version;
        if (!s.configFilePath.empty())
            s.configPtr->saveToFile(s.configFilePath);
    }
    st.service.reset();
    st.skipRequested = false;
    st.open = false;
    wake(s);
}

void tickUpdateDialog(AppState& s) {
    UpdateDialogState& st = s.updateDialog;
    UpdateService& svc = st.service;
    if (st.skipRequested) {
        applySkipRequest(s);
        st.lastPhase = static_cast<int>(svc.phase());
        return;
    }
    const UpdatePhase prev = static_cast<UpdatePhase>(st.lastPhase);
    const UpdatePhase phase = svc.phase();
    st.lastPhase = static_cast<int>(phase);
    const bool checkEnded = prev == UpdatePhase::Checking && phase != prev;
    if (checkEnded) persistCheckTime(s);

    // Automatic offer: a version the user skipped before never offers again.
    // reset() lands on Idle, so the persist above cannot mis-fire for it.
    if (phase == UpdatePhase::Available && !st.manual &&
        s.configPtr && !s.configPtr->skippedUpdateVersion.empty() &&
        svc.release().version == s.configPtr->skippedUpdateVersion) {
        svc.reset();
    }

    // Offer is silent once dismissed; a manual re-check always re-offers.
    const bool offer = phase == UpdatePhase::Available &&
                       (st.manual || !st.offerDismissed);
    st.open = offer || phase == UpdatePhase::Downloading ||
              phase == UpdatePhase::Verifying ||
              phase == UpdatePhase::ReadyToApply ||
              (phase == UpdatePhase::Failed && st.manual);
    if (phase == UpdatePhase::RestartArmed) st.open = false;
    if (checkEnded && phase == UpdatePhase::UpToDate && st.manual)
        st.upToDateUntil = ImGui::GetTime() + 5.0;
    if (st.open || updateDialogInFlight(s) || st.upToDateUntil > 0.0)
        wake(s);
}

// ── Modal ───────────────────────────────────────────────────────────────────
// ONE popup id for the whole updater, content-switched by phase. Per-phase
// popups leaked into ImGui's popup stack (§19): a phase transition stopped
// calling BeginPopupModal on the still-open popup, leaving it invisible and
// blocking all input. The single popup always gets a BeginPopupModal frame,
// and any frame whose state says "nothing to show" closes it from inside the
// body (the self-closing path; OpenPopup is idempotent while the same popup
// is open, so re-opening per frame is safe). Toast framing via
// beginModal/drawModalAccentFrame/endModal; title inside the body
// (NoTitleBar, §14); Enter gated by the modal-scope `wasOpen` latch (§15).

static const char* kUpdatePopup = "Updater##updateModal";

// Release notes: wrapped first lines, with a Show more/less toggle when the
// text was actually clamped (the +1-line probe detects the clamp). The
// height change it causes is absorbed by the modal's AlwaysAutoResize.
static void renderNotes(AppState& s, const std::string& notes) {
    UpdateDialogState& st = s.updateDialog;
    const int maxLines = st.notesExpanded ? 60 : 10;
    const float width = ImGui::GetContentRegionAvail().x;
    const std::vector<std::string> lines = wrapToLines(notes, width, maxLines);
    const bool clamped = !st.notesExpanded &&
                         wrapToLines(notes, width, maxLines + 1).size() > lines.size();
    for (const std::string& line : lines)
        ImGui::TextUnformatted(line.c_str());
    if (clamped && ImGui::SmallButton("Show more")) {
        st.notesExpanded = true;
        wake(s);
    }
    if (st.notesExpanded && ImGui::SmallButton("Show less")) {
        st.notesExpanded = false;
        wake(s);
    }
}

// Bodies draw content + button row only: no OpenPopup/beginModal/EndPopup —
// the single modal wrapper in renderUpdateDialog owns all framing. The
// `pressed ==` branches act AND close the popup; the wrapper's !shouldShow
// branch is the backstop for anything tick() dismisses.

static void renderOfferBody(AppState& s, int& focus, bool& wasOpen,
                            const ImVec4& accent) {
    UpdateDialogState& st = s.updateDialog;
    const ReleaseInfo info = st.service.release();
    // NoTitleBar: the title moves into the body so removing the header
    // loses no information.
    ImGui::Text("Update Available");
    ImGui::Spacing();
    ImGui::TextWrapped("Version %s is available. You are running %s.",
                       info.version.c_str(), APP_VERSION);
    if (!info.notes.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        renderNotes(s, info.notes);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const int pressed = modalButtonRow(
        {"Download && Install", "Skip This Version", "Later"}, focus, wasOpen, accent);
    if (pressed == 0) {
        st.service.startDownload();   // next frame renders the progress body
        wake(s);
    } else if (pressed == 1) {
        st.skipRequested = true;      // persisted by tickUpdateDialog
        ImGui::CloseCurrentPopup();
    } else if (pressed == 2 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        st.offerDismissed = true;     // silent until the next manual check
        st.open = false;
        ImGui::CloseCurrentPopup();
        wake(s);
    }
}

static void renderProgressBody(AppState& s, int& focus, bool& wasOpen,
                               const ImVec4& accent) {
    UpdateDialogState& st = s.updateDialog;
    ImGui::Text("Updating");
    ImGui::Spacing();
    const double frac = st.service.progressFraction();
    if (frac >= 0.0)
        ImGui::ProgressBar(static_cast<float>(frac), ImVec2(-1, 0));
    else
        ImGui::TextUnformatted("…");
    ImGui::TextWrapped("%s", st.service.statusText().c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const int pressed = modalButtonRow({"Cancel"}, focus, wasOpen, accent);
    if (pressed == 0) {
        st.service.cancelDownload();  // phase falls back to Idle
        ImGui::CloseCurrentPopup();   // tick() clears open; close it now
        wake(s);
    }
}

static void renderReadyBody(AppState& s, int& focus, bool& wasOpen,
                            const ImVec4& accent) {
    UpdateDialogState& st = s.updateDialog;
    ImGui::Text("Update Ready");
    ImGui::Spacing();
    ImGui::TextWrapped("The update has been downloaded and verified.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const int pressed =
        modalButtonRow({"Restart Now", "Later"}, focus, wasOpen, accent);
    if (pressed == 0) {
        // Swap the staged binary in; the existing unsaved-changes flow
        // still intercepts the close, and main.cpp relaunches when the
        // updater armed (service.restartArmed()).
        if (st.service.applyStaged())
            glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
        ImGui::CloseCurrentPopup();
        wake(s);
    } else if (pressed == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        // Modal only: the staged file stays for this session; the next
        // launch's stale-file cleanup discards it.
        st.open = false;
        st.manual = false;
        ImGui::CloseCurrentPopup();
        wake(s);
    }
}

static void renderFailedBody(AppState& s, int& focus, bool& wasOpen,
                             const ImVec4& accent) {
    UpdateDialogState& st = s.updateDialog;
    const std::string repo = s.configPtr ? s.configPtr->updateRepoUrl : std::string();
    const std::string url = "https://github.com/" + repo + "/releases/latest";
    ImGui::Text("Update Failed");
    ImGui::Spacing();
    ImGui::TextWrapped("%s", st.service.errorText().c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const int pressed =
        modalButtonRow({"Open Releases Page", "OK"}, focus, wasOpen, accent);
    if (pressed == 0) {
        openWithDefaultBrowser(std::filesystem::path(url));  // modal stays up
    } else if (pressed == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        st.service.reset();
        st.open = false;
        st.manual = false;
        ImGui::CloseCurrentPopup();
        wake(s);
    }
}

static void renderUpToDateBody(AppState& s, int& focus, bool& wasOpen,
                               const ImVec4& accent) {
    ImGui::TextWrapped("Up to date — you are running %s.", APP_VERSION);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (modalButtonRow({"OK"}, focus, wasOpen, accent) == 0 ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        s.updateDialog.upToDateUntil = 0.0;
        s.updateDialog.manual = false;
        ImGui::CloseCurrentPopup();
        wake(s);
    }
}

void renderUpdateDialog(AppState& s) {
    UpdateDialogState& st = s.updateDialog;
    if (st.upToDateUntil > 0.0 && ImGui::GetTime() >= st.upToDateUntil) {
        // Final frame after expiry so the toast is actually wiped (§17);
        // the still-open toast popup is closed by the !shouldShow path below.
        st.upToDateUntil = 0.0;
        st.manual = false;
        wake(s);
    }
    const bool shouldShow = st.open || st.upToDateUntil > 0.0;
    if (!shouldShow && !ImGui::IsPopupOpen(kUpdatePopup)) return;
    wake(s);   // keep frames flowing while the modal is up

    static int focus = 0;
    static bool wasOpen = false;
    const ImVec4 accent = updateAccent();
    ImGui::OpenPopup(kUpdatePopup);   // idempotent while already open
    beginModal(520.0f, accent);
    if (ImGui::BeginPopupModal(kUpdatePopup, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoTitleBar)) {
        if (!shouldShow) {
            ImGui::CloseCurrentPopup();   // state dismissed it: close the stale popup
        } else {
            switch (st.service.phase()) {
                case UpdatePhase::Available:    renderOfferBody(s, focus, wasOpen, accent); break;
                case UpdatePhase::Downloading:  // fall through
                case UpdatePhase::Verifying:
                    renderProgressBody(s, focus, wasOpen, accent); break;
                case UpdatePhase::ReadyToApply: renderReadyBody(s, focus, wasOpen, accent); break;
                case UpdatePhase::Failed:       renderFailedBody(s, focus, wasOpen, accent); break;
                default:
                    // UpToDate toast while live; anything else is a stale
                    // open (reset/cancel raced the tick) — self-close.
                    if (st.upToDateUntil > 0.0)
                        renderUpToDateBody(s, focus, wasOpen, accent);
                    else
                        ImGui::CloseCurrentPopup();
                    break;
            }
        }
        wasOpen = true;
        drawModalAccentFrame(accent);
        ImGui::EndPopup();
    } else {
        wasOpen = false;
    }
    endModal();
}
