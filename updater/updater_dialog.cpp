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
#include "version.h"

#if defined(_WIN32)
#include <shellapi.h>
#else
#include <sys/types.h>
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

// Greedy word wrap into display lines: accumulate words while they fit the
// width; a word that alone exceeds the width is kept on its own line.
// Newlines in the text are honoured. When maxLines clamps the output, the
// last line ends in "…".
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
            const std::string candidate =
                line.empty() ? word : line + " " + word;
            if (ImGui::CalcTextSize(candidate.c_str()).x <= width ||
                line.empty()) {
                line = candidate;
            } else {
                i = start;  // word starts the next line
                break;
            }
            while (i < n && (text[i] == ' ' || text[i] == '\t')) ++i;
        }
        lines.push_back(line);
    }
    const bool clamped = i < n;
    if (clamped && !lines.empty()) lines.back() += "\u2026";
    return lines;
}

// Open a URL in the user's browser without app helpers. POSIX: fork +
// xdg-open in the child. Windows: ShellExecuteA.
bool openUrl(const std::string& url) {
#if defined(_WIN32)
    return ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr,
                         SW_SHOWNORMAL) > (HINSTANCE)32;
#else
    const pid_t pid = fork();
    if (pid == 0) {
        execlp("xdg-open", "xdg-open", url.c_str(), (char*)nullptr);
        _exit(127);
    }
    return pid > 0;
#endif
}

// Release notes: wrapped first lines, with a Show more/less toggle only when
// the text was actually clamped (the +1-line probe detects the clamp). The
// height change is absorbed by the modal's AlwaysAutoResize.
void drawNotes(const std::string& notes, bool& expanded) {
    const size_t maxLines = expanded ? 60 : 10;
    const float width = ImGui::GetContentRegionAvail().x;
    const std::vector<std::string> lines = wrapLines(notes, width, maxLines);
    const bool clamped = !expanded &&
                         wrapLines(notes, width, maxLines + 1).size() >
                             lines.size();
    for (const std::string& line : lines) ImGui::TextUnformatted(line.c_str());
    if (clamped && ImGui::SmallButton("Show more")) expanded = true;
    if (expanded && ImGui::SmallButton("Show less")) expanded = false;
}

// ── Bodies ──────────────────────────────────────────────────────────────────
// Bodies draw content + button row only; the single modal wrapper owns all
// framing. Pressed branches act AND close the popup; the wrapper's
// !shouldShow branch is the backstop for anything tick() dismisses.

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
        st.service.startDownload();  // next frame renders the progress body
    } else if (pressed == 1) {
        st.skipRequested = true;  // persisted by tick()
        ImGui::CloseCurrentPopup();
    } else if (pressed == 2 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        st.offerDismissed = true;  // silent until the next manual check
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
    if (buttonRow({"Cancel"}, focus, wasOpen, accent) == 0) {
        st.service.cancelDownload();  // phase falls back to Idle
        ImGui::CloseCurrentPopup();   // tick() clears open; close it now
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
        // and exits, then main() relaunches through relaunch().
        if (st.service.applyStaged()) {
            st.closeRequested = true;
            ImGui::CloseCurrentPopup();
        }
    } else if (pressed == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        // Modal only: the staged file stays for this session; the next
        // launch's stale-file cleanup discards it.
        st.open = false;
        st.manual = false;
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

} // namespace

void startCheck(bool manualCheck) {
    DialogState& st = state();
    st.service.startCheck(UpdateCheckConfig{st.config.repoSlug, APP_VERSION});
    st.manual = manualCheck;
    st.open = false;
    st.offerDismissed = false;
    st.notesExpanded = false;
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
    const long long now = nowSeconds();
    if (st.config.lastCheckEpoch != 0 && now - st.config.lastCheckEpoch < 86400)
        return;
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
        svc.reset();
        st.skipRequested = false;
        st.open = false;
        st.lastPhase = static_cast<int>(svc.phase());
        return;
    }
    const UpdatePhase prev = static_cast<UpdatePhase>(st.lastPhase);
    const UpdatePhase phase = svc.phase();
    st.lastPhase = static_cast<int>(phase);
    const bool checkEnded = prev == UpdatePhase::Checking && phase != prev;
    if (checkEnded) {
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

    // Offer is silent once dismissed; a manual re-check always re-offers.
    const bool offer = phase == UpdatePhase::Available &&
                       (st.manual || !st.offerDismissed);
    st.open = offer || phase == UpdatePhase::Downloading ||
              phase == UpdatePhase::Verifying ||
              phase == UpdatePhase::ReadyToApply ||
              (phase == UpdatePhase::Failed && st.manual);
    if (phase == UpdatePhase::RestartArmed) st.open = false;
    if (checkEnded && phase == UpdatePhase::UpToDate && st.manual) {
        st.upToDateUntil =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
    }
}

bool render() {
    DialogState& st = state();
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    const bool armed =
        st.upToDateUntil != std::chrono::steady_clock::time_point{};
    const bool toastLive = armed && now < st.upToDateUntil;
    if (armed && !toastLive) {
        // Final frame after expiry so the toast is actually wiped (§17);
        // the still-open toast popup is closed by the !shouldShow path below.
        st.upToDateUntil = std::chrono::steady_clock::time_point{};
        st.manual = false;
    }
    const bool shouldShow = st.open || toastLive;
    if (!shouldShow && !ImGui::IsPopupOpen(kPopup)) return false;

    static int focus = 0;
    static bool wasOpen = false;
    const ImVec4 accent = accentColor();
    ImGui::OpenPopup(kPopup);  // idempotent while already open
    beginModalStyles(520.0f, accent);
    if (ImGui::BeginPopupModal(kPopup, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoTitleBar)) {
        if (!shouldShow) {
            st.open = false;  // backstop: state dismissed it, close stale popup
            ImGui::CloseCurrentPopup();
        } else {
            switch (st.service.phase()) {
                case UpdatePhase::Available:
                    drawOfferBody(st, focus, wasOpen, accent);
                    break;
                case UpdatePhase::Downloading:  // fall through
                case UpdatePhase::Verifying:
                    drawProgressBody(st, focus, wasOpen, accent);
                    break;
                case UpdatePhase::ReadyToApply:
                    drawReadyBody(st, focus, wasOpen, accent);
                    break;
                case UpdatePhase::Failed:
                    drawFailedBody(st, focus, wasOpen, accent);
                    break;
                default:
                    // UpToDate toast while live; anything else is a stale
                    // open (reset/cancel raced the tick) — self-close.
                    if (toastLive)
                        drawUpToDateBody(st, focus, wasOpen, accent);
                    else
                        ImGui::CloseCurrentPopup();
                    break;
            }
        }
        wasOpen = true;
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
