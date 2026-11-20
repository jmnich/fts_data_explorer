// Self-contained updater modal: the only ImGui-facing file of the updater
// module. It deliberately reuses no app code — the toast framing, the
// keyboard-navigable button row and the text wrapper below are local copies
// of the patterns from IMGUI_GUIDE §14/§15/§17 and the modal-styling rules,
// kept inside updater/ so the module stays standalone.
//
// Popup lifecycle (§19): ONE popup id serves every phase and the up-to-date
// toast. Every frame that finds the popup open calls BeginPopupModal on it;
// dismissing actions call CloseCurrentPopup from inside the body, and the
// !shouldShow branch is the backstop that closes a popup state has dismissed.
#include "updater_compat.h" // GCC 16+: declares pthread_cond_clockwait etc. before <mutex>
#include "updater_dialog.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>

#include "updater.h"
#include "updater_paths.h"
#include "version.h"

#if defined(_WIN32)
#include <shellapi.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace updater {

DialogState& state() {
    static DialogState instance;
    return instance;
}

namespace {

// Single popup id for all phases (per-phase popups leak ImGui's popup stack).
const char* const kPopup = "Updater##updaterModal";

long long nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Accent for the modal. The active-button colour is the same family as the
// app's modal accent and always available from the style.
ImVec4 accentColor() {
    return ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
}

// Toast-style modal framing: explicit centering (an AlwaysAutoResize modal
// without it opens at the top of the screen), pinned width so TextWrapped
// wraps correctly, rounded corners and a dark fill. Must be paired with
// endModalStyles() right after the BeginPopupModal if-block, even when it
// returned false.
void beginModalStyles(float width, const ImVec4& accent) {
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f),
                                        ImVec2(width, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, accent);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImVec4(30.0f / 255.0f, 30.0f / 255.0f,
                                 50.0f / 255.0f, 230.0f / 255.0f));
}

void endModalStyles() {
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// Accent frame around the modal body; call at the end of the body so it
// overlays the content edges.
void drawAccentFrame(const ImVec4& accent) {
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(accent);
    ImGui::GetWindowDrawList()->AddRect(
        pos, ImVec2(pos.x + size.x, pos.y + size.y), col, 8.0f,
        ImDrawFlags_None, 3.0f);
}

// Keyboard-navigable, centered button row. Left/Right/Up/Down move the focus
// highlight; Enter/Space activate it; hovering also moves the focus. `wasOpen`
// gates Enter/Space so the keypress that OPENED the popup does not trigger a
// button (§15). Returns the activated index, or -1.
int buttonRow(const std::vector<const char*>& labels, int& focus, bool wasOpen,
              const ImVec4& focusColor) {
    if (labels.empty()) return -1;
    if (focus < 0) focus = 0;
    if (focus >= (int)labels.size()) focus = (int)labels.size() - 1;

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && focus > 0) focus--;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) &&
        focus < (int)labels.size() - 1)
        focus++;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && focus > 0) focus--;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) &&
        focus < (int)labels.size() - 1)
        focus++;

    // Measure widths first so the row can be centered.
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    std::vector<float> widths;
    widths.reserve(labels.size());
    float total = 0.0f;
    for (const char* l : labels) {
        const float w = ImGui::CalcTextSize(l).x + pad.x * 2.0f;
        widths.push_back(w);
        total += w;
    }
    total += ImGui::GetStyle().ItemSpacing.x * (labels.size() - 1);
    const float startX = (ImGui::GetContentRegionAvail().x - total) * 0.5f;
    if (startX > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + startX);

    int activate = -1;
    const bool enter = wasOpen && (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                                   ImGui::IsKeyPressed(ImGuiKey_Space));
    for (int i = 0; i < (int)labels.size(); ++i) {
        if (i > 0) ImGui::SameLine();
        if (focus == i) ImGui::PushStyleColor(ImGuiCol_Button, focusColor);
        if (ImGui::Button(labels[i], ImVec2(widths[i], 0)) ||
            (enter && focus == i)) {
            activate = i;
        }
        if (focus == i) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) focus = i;
    }
    return activate;
}

// True when text[pos..] still carries a non-space character: trailing
// whitespace must not make a clamped wrap look incomplete.
bool hasContent(const std::string& text, size_t pos) {
    for (size_t k = pos; k < text.size(); ++k) {
        if (text[k] != ' ' && text[k] != '\t' && text[k] != '\n' &&
            text[k] != '\r')
            return true;
    }
    return false;
}

// Greedy word wrap into display lines: accumulate words while they fit the
// width; a word that alone exceeds the width is hard-broken at the fit
// point. Newlines in the text are honoured. When maxLines clamps the output,
// the last line ends in "…".
std::vector<std::string> wrapLines(const std::string& text, float width,
                                   size_t maxLines) {
    std::vector<std::string> lines;
    size_t i = 0;
    const size_t n = text.size();
    while (i < n && lines.size() < maxLines) {
        std::string line;
        while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
        while (i < n) {
            if (text[i] == '\n') {
                ++i;
                break;
            }
            const size_t start = i;
            while (i < n && text[i] != ' ' && text[i] != '\t' &&
                   text[i] != '\n')
                ++i;
            const std::string word = text.substr(start, i - start);
            std::string candidate = line.empty() ? word : line + " " + word;
            if (ImGui::CalcTextSize(candidate.c_str()).x > width &&
                !line.empty()) {
                i = start;  // word starts the next line
                break;
            }
            if (ImGui::CalcTextSize(candidate.c_str()).x > width) {
                // Single over-wide word: keep the longest fitting prefix and
                // hard-break the rest onto the next line.
                size_t keep = word.size() > 1 ? word.size() - 1 : 1;
                while (keep > 1 &&
                       ImGui::CalcTextSize(word.substr(0, keep).c_str()).x >
                           width)
                    --keep;
                candidate = line + word.substr(0, keep);
                i = start + keep;
                line = candidate;
                break;
            }
            line = candidate;
            while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
        }
        lines.push_back(line);
    }
    const bool clamped = hasContent(text, i);
    if (clamped && !lines.empty()) lines.back() += "\u2026";
    return lines;
}

// Open a URL in the user's browser without app helpers. POSIX: double-fork
// so the launcher is reaped by init (no zombies) — the first child sessions
// off and exits, the grandchild execs xdg-open; the parent waits on the
// first child only. Windows: ShellExecuteA.
bool openUrl(const std::string& url) {
#if defined(_WIN32)
    return ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr,
                         SW_SHOWNORMAL) > (HINSTANCE)32;
#else
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        const pid_t grand = fork();
        if (grand == 0) {
            execlp("xdg-open", "xdg-open", url.c_str(), (char*)nullptr);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, nullptr, 0);
    return pid > 0;
#endif
}

// Release notes: wrapped first lines, with a Show more/less toggle only when
// real content (not just trailing whitespace) was clamped. Expanded notes
// render in a scrollable child capped at 360 px so 60 lines cannot push the
// AlwaysAutoResize modal off screen.
void drawNotes(const std::string& notes, bool& expanded) {
    const size_t maxLines = expanded ? 60 : 10;
    const float width = ImGui::GetContentRegionAvail().x;
    const std::vector<std::string> lines = wrapLines(notes, width, maxLines);
    const bool clamped = !expanded &&
                         wrapLines(notes, width, maxLines + 1).size() >
                             lines.size();
    if (expanded) ImGui::BeginChild("##updatenotes", ImVec2(0, 360), false);
    for (const std::string& line : lines) ImGui::TextUnformatted(line.c_str());
    if (expanded) ImGui::EndChild();
    if (clamped && ImGui::SmallButton("Show more")) expanded = true;
    if (expanded && ImGui::SmallButton("Show less")) expanded = false;
}

// ── Bodies ──────────────────────────────────────────────────────────────────
// Bodies draw content + button row only; the single modal wrapper owns all
// framing. Pressed branches act AND close the popup; the wrapper's
// !shouldShow branch is the backstop for anything tick() dismisses.

// Cancel pressed on any in-flight body: arm the worker cancel and hide the
// modal now; tick() clears `cancelling` once the worker settles.
void requestCancel(DialogState& st) {
    st.service.cancelDownload();
    st.cancelling = true;
    st.open = false;
    ImGui::CloseCurrentPopup();
}

void drawCheckingBody(DialogState& st, int& focus, bool wasOpen,
                      const ImVec4& accent) {
    ImGui::Text("Checking for Updates");
    ImGui::Spacing();
    ImGui::TextWrapped("%s", st.service.statusText().c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (buttonRow({"Cancel"}, focus, wasOpen, accent) == 0 ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        requestCancel(st);
    }
}

void drawOfferBody(DialogState& st, int& focus, bool wasOpen,
                   const ImVec4& accent) {
    const ReleaseInfo info = st.service.release();
    // NoTitleBar: the title moves into the body so the removed header loses
    // no information.
    ImGui::Text("Update Available");
    ImGui::Spacing();
    ImGui::TextWrapped("Version %s is available. You are running %s.",
                       info.version.c_str(), APP_VERSION);
    if (!info.notes.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        drawNotes(info.notes, st.notesExpanded);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const int pressed = buttonRow(
        {"Download && Install", "Skip This Version", "Later"}, focus, wasOpen,
        accent);
    if (pressed == 0) {
        st.userInitiated = true;  // a download failure must show for this user
        st.service.startDownload();  // next frame renders the progress body
    } else if (pressed == 1) {
        st.skipRequested = true;  // persisted by tick()
        ImGui::CloseCurrentPopup();
    } else if (pressed == 2 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        // "Later" sticks for this exact version; the next check (manual or
        // automatic) clears it and re-offers.
        st.dismissedVersion = info.version;
        st.manual = false;
        st.open = false;
        ImGui::CloseCurrentPopup();
    }
}

void drawProgressBody(DialogState& st, int& focus, bool wasOpen,
                      const ImVec4& accent) {
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
    if (buttonRow({"Cancel"}, focus, wasOpen, accent) == 0 ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        requestCancel(st);
    }
}

void drawReadyBody(DialogState& st, int& focus, bool wasOpen,
                   const ImVec4& accent) {
    ImGui::Text("Update Ready");
    ImGui::Spacing();
    ImGui::TextWrapped("The update has been downloaded and verified.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const int pressed =
        buttonRow({"Restart Now", "Later"}, focus, wasOpen, accent);
    if (pressed == 0) {
        // Swap the staged binary in; the frame loop polls takeCloseRequest()
        // and exits, then main() relaunches through relaunch(). A failed
        // swap lands on Failed, which must show because the user clicked.
        st.userInitiated = true;
        if (st.service.applyStaged()) {
            st.closeRequested = true;
            ImGui::CloseCurrentPopup();
        }
    } else if (pressed == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        // Modal only and deferred for this session: the staged file stays
        // for it; the next launch's stale-file cleanup discards it.
        st.readyDeferred = true;
        st.manual = false;
        st.open = false;
        ImGui::CloseCurrentPopup();
    }
}

void drawFailedBody(DialogState& st, int& focus, bool wasOpen,
                    const ImVec4& accent) {
    const std::string url =
        "https://github.com/" + st.config.repoSlug + "/releases/latest";
    ImGui::Text("Update Failed");
    ImGui::Spacing();
    ImGui::TextWrapped("%s", st.service.errorText().c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const int pressed =
        buttonRow({"Open Releases Page", "OK"}, focus, wasOpen, accent);
    if (pressed == 0) {
        openUrl(url);  // modal stays up
    } else if (pressed == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        st.service.reset();
        st.open = false;
        st.manual = false;
        st.userInitiated = false;
        ImGui::CloseCurrentPopup();
    }
}

void drawUpToDateBody(DialogState& st, int& focus, bool wasOpen,
                      const ImVec4& accent) {
    ImGui::TextWrapped("Up to date \u2014 you are running %s.", APP_VERSION);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (buttonRow({"OK"}, focus, wasOpen, accent) == 0 ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        st.upToDateUntil = std::chrono::steady_clock::time_point{};
        st.manual = false;
        ImGui::CloseCurrentPopup();
    }
}

// Backdrop behind the modal. The frame loop skips the app UI while the
// updater modal is up, so a flat dim over the whole viewport is the clean
// background.
void drawBackdrop() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
        IM_COL32(0, 0, 0, 140));
}

// One modal body for the current phase. A phase change resets the button
// focus and the wasOpen latch: focus must not leak from one body's row into
// the next, and a held Enter must not activate the next phase's first
// button. Returns false for a stale open (reset/cancel raced the tick).
bool drawPhaseBody(DialogState& st, UpdatePhase phase, bool toastLive,
                   int& focus, bool& wasOpen, const ImVec4& accent) {
    switch (phase) {
        case UpdatePhase::Checking:
            drawCheckingBody(st, focus, wasOpen, accent);
            return true;
        case UpdatePhase::Available:
            drawOfferBody(st, focus, wasOpen, accent);
            return true;
        case UpdatePhase::Downloading:  // fall through
        case UpdatePhase::Verifying:
            drawProgressBody(st, focus, wasOpen, accent);
            return true;
        case UpdatePhase::ReadyToApply:
            drawReadyBody(st, focus, wasOpen, accent);
            return true;
        case UpdatePhase::Failed:
            drawFailedBody(st, focus, wasOpen, accent);
            return true;
        default:
            // UpToDate toast while live; anything else is a stale open —
            // self-close via the wrapper's backstop.
            if (toastLive) {
                drawUpToDateBody(st, focus, wasOpen, accent);
                return true;
            }
            return false;
    }
}

// Whether the modal should be up for this phase. "Later" is per-version
// (offer) and per-session (ready deferral); a fresh check clears both. A
// cancel in flight hides everything until the worker settles.
void applyOpenState(DialogState& st, UpdatePhase phase) {
    const bool offer = phase == UpdatePhase::Available &&
                       st.service.release().version != st.dismissedVersion;
    const bool progress =
        (phase == UpdatePhase::Downloading ||
         phase == UpdatePhase::Verifying) &&
        !st.cancelling;
    const bool failed = phase == UpdatePhase::Failed &&
                        (st.manual || st.userInitiated) && !st.cancelling;
    st.open = (phase == UpdatePhase::Checking && st.manual) || offer ||
              progress ||
              (phase == UpdatePhase::ReadyToApply && !st.readyDeferred) ||
              failed;
}

} // namespace

void startCheck(bool manualCheck) {
    DialogState& st = state();
    st.service.startCheck(UpdateCheckConfig{st.config.repoSlug, APP_VERSION});
    st.manual = manualCheck;
    st.open = false;
    st.notesExpanded = false;
    st.dismissedVersion.clear();
    st.readyDeferred = false;
    st.cancelling = false;
    st.userInitiated = false;
    st.lastPhase = static_cast<int>(st.service.phase());
}

void startup() {
    DialogState& st = state();
    st.config = loadConfig();
    st.configLoaded = true;
    st.exePath = currentExecutablePath();
    cleanupStaleUpdateFiles();
#if !APP_IS_RELEASE
    return;  // dev builds never phone home
#else
    if (st.autoStarted) return;
    if (!st.config.autoCheck) return;
    // Cooldown tolerant of a future/rolled-back clock: only a real elapsed
    // time inside the day suppresses the check.
    if (st.config.lastCheckEpoch != 0) {
        const long long since = nowSeconds() - st.config.lastCheckEpoch;
        if (since >= 0 && since < 86400) return;
    }
    st.autoStarted = true;
    startCheck(false);  // silent: tick() only raises a modal on a real offer
#endif
}

void requestManualCheck() {
    DialogState& st = state();
    if (st.service.busy()) {
        // A check/download is already in flight: just surface its modal.
        st.open = true;
        st.manual = true;
        st.readyDeferred = false;  // "Later" yields to an explicit re-check
        return;
    }
    st.service.reset();  // safe while !busy(): joins a finished worker only
    st.manual = true;
    startCheck(true);  // manual checks ignore cooldown and skip
}

void tick() {
    DialogState& st = state();
    UpdateService& svc = st.service;
    if (st.skipRequested) {
        // Runs while the release info is still readable (reset() clears it).
        st.config.skippedVersion = svc.release().version;
        saveConfig(st.config);
        svc.reset();  // cancel-armed: settles out of Checking without saving
        st.skipRequested = false;
        st.open = false;
        st.lastPhase = static_cast<int>(svc.phase());
        return;
    }
    const UpdatePhase prev = static_cast<UpdatePhase>(st.lastPhase);
    const UpdatePhase phase = svc.phase();
    st.lastPhase = static_cast<int>(phase);
    const bool checkEnded = prev == UpdatePhase::Checking && phase != prev;
    if (st.cancelling && (phase == UpdatePhase::Idle ||
                          phase == UpdatePhase::Failed)) {
        st.cancelling = false;  // settled; a cancel-caused Failed stays hidden
    }
    if (checkEnded && (phase == UpdatePhase::UpToDate ||
                       phase == UpdatePhase::Available)) {
        // Only a successful check starts a new cooldown; a failure retries
        // on the next startup.
        st.config.lastCheckEpoch = nowSeconds();
        saveConfig(st.config);
    }

    // Automatic offer: a version the user skipped before never offers again.
    // reset() lands on Idle, so the persist above cannot mis-fire for it.
    if (phase == UpdatePhase::Available && !st.manual &&
        !st.config.skippedVersion.empty() &&
        svc.release().version == st.config.skippedVersion) {
        svc.reset();
    }
    applyOpenState(st, phase);
    if (phase == UpdatePhase::RestartArmed) st.open = false;
    if (checkEnded && phase == UpdatePhase::UpToDate && st.manual) {
        st.upToDateUntil =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }
}

// Toast bookkeeping before the frame: live? expired (run the final wiping
// frame, §17)? Returns whether the up-to-date toast should show.
bool toastState(DialogState& st) {
    if (st.upToDateUntil == std::chrono::steady_clock::time_point{}) {
        return false;
    }
    if (std::chrono::steady_clock::now() < st.upToDateUntil) {
        return true;
    }
    // Expired: this is the final frame that wipes the toast; the still-open
    // popup is closed by the !shouldShow path in render().
    st.upToDateUntil = std::chrono::steady_clock::time_point{};
    st.manual = false;
    return false;
}

bool render() {
    DialogState& st = state();
    const bool toastLive = toastState(st);
    const bool shouldShow = st.open || toastLive;
    if (!shouldShow && !ImGui::IsPopupOpen(kPopup)) return false;
    if (shouldShow) drawBackdrop();

    static int focus = 0;
    static bool wasOpen = false;
    static int lastRenderPhase = -1;
    const int phaseInt = static_cast<int>(st.service.phase());
    if (phaseInt != lastRenderPhase) {
        focus = 0;
        wasOpen = false;
        lastRenderPhase = phaseInt;
    }
    const ImVec4 accent = accentColor();
    ImGui::OpenPopup(kPopup);  // idempotent while already open
    beginModalStyles(520.0f, accent);
    if (ImGui::BeginPopupModal(kPopup, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoTitleBar)) {
        bool drew = false;
        if (shouldShow) {
            drew = drawPhaseBody(st, static_cast<UpdatePhase>(phaseInt),
                                 toastLive, focus, wasOpen, accent);
        }
        if (!drew) {
            st.open = false;  // backstop: state dismissed it, close stale popup
            ImGui::CloseCurrentPopup();
        } else {
            wasOpen = true;
        }
        drawAccentFrame(accent);
        ImGui::EndPopup();
    } else {
        wasOpen = false;
    }
    endModalStyles();
    return true;
}

bool busy() {
    const DialogState& st = state();
    return st.service.busy() || st.open ||
           std::chrono::steady_clock::now() < st.upToDateUntil;
}

bool restartArmed() { return state().service.restartArmed(); }

bool takeCloseRequest() {
    DialogState& st = state();
    const bool close = st.closeRequested;
    st.closeRequested = false;
    return close;
}

bool relaunch(const std::vector<std::string>& args) {
    std::string error;
    return spawnRelaunch(state().exePath, args, error);
}

bool autoCheckEnabled() {
    DialogState& st = state();
    if (!st.configLoaded) {
        st.config = loadConfig();
        st.configLoaded = true;
    }
    return st.config.autoCheck;
}

void setAutoCheckEnabled(bool enabled) {
    DialogState& st = state();
    if (!st.configLoaded) {
        st.config = loadConfig();
        st.configLoaded = true;
    }
    st.config.autoCheck = enabled;
    saveConfig(st.config);
}

} // namespace updater
