// AppLoop: frame pipeline (extracted from main.cpp, Phase-1 M1.2b).
#include "app_loop.h"
#include "panels/panels.h"
#include "app_state.h"
#include "config.h"
#include "welcome.h"
#include "conversion_screen.h"
#include "about.h"
#include "menu_bar.h"
#include "theme.h"
#include "popup_utils.h"
#include "workspace_reader.h"
#include "window.h"
#include "spectral_toolbox.h"
#include "apodization.h"
#include "app_dirs.h"
#include "file_browser.h"
#include "session/cross_store.h"
#include <imgui.h>
#include "imgui_internal.h" // DockBuilder API
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

static bool naturalSortCompare(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        // Skip non-digit characters
        if (!isdigit(a[i]) || !isdigit(b[j])) {
            if (a[i] != b[j]) {
                return a[i] < b[j];
            }
            i++; j++;
        } else {
            // Compare numeric sequences by length then lexicographically.
            // Throw-free: std::stoi would throw std::out_of_range for digit runs
            // longer than INT_MAX, and a throwing sort comparator is UB.
            size_t numStartA = i;
            size_t numStartB = j;
            while (i < a.size() && isdigit(a[i])) i++;
            while (j < b.size() && isdigit(b[j])) j++;
            size_t lenA = i - numStartA;
            size_t lenB = j - numStartB;
            if (lenA != lenB) {
                return lenA < lenB;
            }
            int cmp = a.compare(numStartA, lenA, b, numStartB, lenB);
            if (cmp != 0) {
                return cmp < 0;
            }
        }
    }
    return a.size() < b.size();
}
ImVec4 modalAccent() {
    return GetAccentBase(StringToAccentColor(appState.currentAccentColor));
}
static void renderUnsavedPromptModal() {
    static int focus = 0;
    static bool wasOpen = false;
    // One modal at a time: while the exit modal is up, its OpenPopup would
    // close this one (and vice versa) — both would render stacked, with the
    // later one hiding the other (bugfix 5).
    if (!appState.showUnsavedPrompt || appState.showExitDirtyModal) {
        wasOpen = false;
        return;
    }
    ImGui::OpenPopup("Unsaved Changes##confirm");
    beginModal(520.0f, modalAccent());
    if (ImGui::BeginPopupModal("Unsaved Changes##confirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        // NoTitleBar: the title moves into the body so removing the header
        // loses no information. The workspace change list only renders when a
        // workspace is active — the prompt can also be raised for unsaved
        // EXPERIMENTS alone (replace-project flow, Phase 4).
        ImGui::Text("Unsaved Changes");
        ImGui::Spacing();
        ImGui::TextWrapped("Save changes to \"%s\" before continuing?",
                           appState.active ? appState.active->currentDatasetName.c_str()
                                           : "(no workspace)");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Change list: per-file "Spectrum: " entries aggregate into one
        // CAT_SPECTRA line with the distinct-file count; everything else is
        // shown verbatim. Names match the stale-drop modal (shared constants).
        // Bullet + TextWrapped: BulletText never wraps, so long entries (e.g.
        // the view-settings line at scaled UI sizes) clip at the pinned width.
        if (appState.active) {
        const std::string spectrumPrefix = "Spectrum: ";
        int spectrumCount = 0;
        for (const auto& entry : appState.active->workspace.changeLog)
            if (entry.rfind(spectrumPrefix, 0) == 0) ++spectrumCount;
        bool shown = false;
        if (spectrumCount > 0) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s (%d file%s)", CAT_SPECTRA, spectrumCount,
                               spectrumCount == 1 ? "" : "s");
            shown = true;
        }
        for (const auto& entry : appState.active->workspace.changeLog) {
            if (entry.rfind(spectrumPrefix, 0) == 0) continue;
            ImGui::Bullet();
            ImGui::TextWrapped("%s", entry.c_str());
            shown = true;
        }
        if (!shown) {
            ImGui::Bullet();
            ImGui::TextWrapped("Workspace contains unsaved changes.");
        }
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Tab-close resolution (M2.2): the modal saves/closes the ACTIVE tab
        // (pendingTabCloseIdx); the change list above comes from the same flat
        // fields, so text and data always agree.
        const bool closingTab = appState.pendingTabCloseIdx >= 0;
        // Phase 4: unsaved experiments ride the replace-project prompt.
        if (!closingTab) {
            int dirtyEnvCount = 0;
            for (const auto& env : appState.environments)
                if (env->dirty) ++dirtyEnvCount;
            if (dirtyEnvCount > 0) {
                ImGui::Bullet();
                ImGui::TextWrapped("%d unsaved experiment%s", dirtyEnvCount,
                                   dirtyEnvCount == 1 ? "" : "s");
            }
        }
        int pressed = modalButtonRow({"Save", "Don't Save", "Cancel"},
                                     focus, wasOpen, modalAccent());
        if (pressed == 0) {
            try {
                if (closingTab) appState.pendingWorkspaceAction = PendingWorkspaceAction::CloseWorkspace;
                requestSaveWorkspace(appState, "");
                if (!appState.showStaleDropPrompt) {
                    if (closingTab) {
                        const int idx = appState.pendingTabCloseIdx;
                        appState.pendingTabCloseIdx = -1;
                        appState.pendingWorkspaceAction = PendingWorkspaceAction::None;
                        appState.pendingRemoveIdx = idx;   // frame top removes
                        // Bugfix 2026-08-13: without this the modal re-opens
                        // every frame after the tab is removed (showUnsavedPrompt
                        // stays latched; the reopened modal's dispatch is a no-op
                        // with no pending action — only ESC could dismiss it).
                        appState.showUnsavedPrompt = false;
                        appState.needsRedraw = true;
                    } else {
                        // Phase 4: the replace-project prompt also saves
                        // unsaved experiments (they live in the .cross.h5).
                        if (appState.pendingWorkspaceAction ==
                            PendingWorkspaceAction::OpenMultiWorkspace) {
                            std::string err;
                            if (!crossSaveExperiments(
                                    appState, appState.sessionTab.multiWorkspacePath, err)) {
                                appState.adapterErrorMsg =
                                    std::string("Experiment save failed:\n") + err;
                                appState.showAdapterErrorPopup = true;
                            }
                        }
                        dispatchPendingAction(appState);
                    }
                    ImGui::CloseCurrentPopup();
                } else {
                    // The stale-drop confirmation takes over: close this popup
                    // now (the pending action stays stashed, dispatched after
                    // the drop). Without the close it lingers in the popup
                    // stack un-rendered and freezes the whole UI.
                    ImGui::CloseCurrentPopup();
                }
            } catch (const std::exception& e) {
                appState.pendingTabCloseIdx = -1;
                appState.pendingWorkspaceAction = PendingWorkspaceAction::None;
                appState.adapterErrorMsg = std::string("Save failed:\n") + e.what();
                appState.showAdapterErrorPopup = true;
            }
        } else if (pressed == 1) {
            if (closingTab) {
                const int idx = appState.pendingTabCloseIdx;
                appState.pendingTabCloseIdx = -1;
                appState.pendingWorkspaceAction = PendingWorkspaceAction::None;
                appState.pendingRemoveIdx = idx;   // frame top removes
                // Bugfix 2026-08-13: clear the latch so the modal does not
                // re-open after the removal (see the Save branch comment).
                appState.showUnsavedPrompt = false;
                appState.needsRedraw = true;
            } else {
                dispatchPendingAction(appState);   // discard RAM workspace
            }
            ImGui::CloseCurrentPopup();
        } else if (pressed == 2 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            appState.pendingWorkspaceAction = PendingWorkspaceAction::None;
            appState.pendingWorkspacePath.clear();
            appState.pendingTabCloseIdx = -1;
            appState.showUnsavedPrompt = false;
            appState.exitDeferredClose = false;   // Cancel drops a deferred close
            ImGui::CloseCurrentPopup();
        }
        drawModalAccentFrame(modalAccent());
        ImGui::EndPopup();
        wasOpen = true;
    } else {
        wasOpen = false;
    }
    endModal();
}
static void renderStaleDropPromptModal() {
    static int focus = 0;
    static bool wasOpen = false;
    // One modal at a time — see renderUnsavedPromptModal.
    if (!appState.showStaleDropPrompt || appState.showExitDirtyModal) {
        wasOpen = false;
        return;
    }
    ImGui::OpenPopup("Stale Data Will Be Dropped##confirm");
    // Fixed width ~1.6x the old autosized width: the wrapped header text
    // never clips and the category list gets breathing room.
    beginModal(560.0f, modalAccent());
    if (ImGui::BeginPopupModal("Stale Data Will Be Dropped##confirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        // NoTitleBar: the title moves into the body so removing the header
        // loses no information.
        ImGui::Text("Stale Data Will Be Dropped");
        ImGui::Spacing();
        ImGui::TextWrapped("Some results no longer match the current inputs or settings "
                           "and will not be saved:");
        ImGui::Spacing();
        // Bullet + TextWrapped (BulletText never wraps): keeps the list look
        // and wraps long lines at the pinned width instead of clipping.
        for (const auto& cat : appState.active->workspace.staleCategories()) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", cat.c_str());
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        int pressed = modalButtonRow({"Cancel", "Drop Stale Data"},
                                     focus, wasOpen, modalAccent());
        if (pressed == 0 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            appState.pendingSaveAsPath.clear();
            appState.showStaleDropPrompt = false;
            // Keep pendingWorkspaceAction: if the save was chained from the
            // Unsaved Changes modal, "Don't Save" must still dispatch it. Only
            // the Unsaved modal's own Cancel aborts the pending action.
            ImGui::CloseCurrentPopup();
        } else if (pressed == 1) {
            try {
                doSaveWorkspace(appState, appState.pendingSaveAsPath);
                appState.pendingSaveAsPath.clear();
                appState.showStaleDropPrompt = false;
                if (appState.pendingWorkspaceAction != PendingWorkspaceAction::None)
                    dispatchPendingAction(appState);
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& e) {
                appState.adapterErrorMsg = std::string("Save failed:\n") + e.what();
                appState.showAdapterErrorPopup = true;
            }
        }
        drawModalAccentFrame(modalAccent());
        ImGui::EndPopup();
        wasOpen = true;
    } else {
        wasOpen = false;
    }
    endModal();
}
// Multi-dirty modal (M2.2): lists every dirty tab; Save All saves each
// sequentially at frame top (one swap per frame), Discard All clears the
// latches, Cancel defers. Shared by the Exit flow (terminal action: close
// the window) and Ctrl+H go-home (terminal action: pendingGoHome).
static void renderExitDirtyModal() {
    static int focus = 0;
    static bool wasOpen = false;
    if (!appState.showExitDirtyModal) {
        wasOpen = false;
        return;
    }
    ImGui::OpenPopup("Exit with Unsaved Changes##confirm");
    beginModal(560.0f, modalAccent());
    if (ImGui::BeginPopupModal("Exit with Unsaved Changes##confirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::Text("%s", appState.exitTargetIsGoHome ? "Go Home with Unsaved Changes"
                                                      : "Unsaved Changes");
        ImGui::Spacing();
        ImGui::TextWrapped("The following tabs have unsaved changes:");
        ImGui::Spacing();
        for (const auto& label : appState.exitDirtyLabels) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", label.c_str());
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        int pressed = modalButtonRow({"Save All", "Discard All", "Cancel"},
                                     focus, wasOpen, modalAccent());
        if (pressed == 0) {
            appState.showExitDirtyModal = false;
            appState.exitSaveAllRunning = true;
            appState.exitSaveAllCursor = 0;
            appState.needsRedraw = true;
            ImGui::CloseCurrentPopup();
        } else if (pressed == 1) {
            for (int idx : appState.exitDirtyTabs) {
                if (idx == appState.activeSessionIdx &&
                    appState.activeTabKind == ActiveTabKind::Workspace) {
                    appState.active->workspace.dirty = false;
                    appState.active->workspace.changeLog.clear();
                } else if (idx >= 0 && idx < static_cast<int>(appState.sessions.size())) {
                    appState.sessions[idx]->workspace.dirty = false;
                    appState.sessions[idx]->workspace.changeLog.clear();
                }
            }
            // Phase 4: drop unsaved experiments with the same "Discard All".
            for (int idx : appState.exitDirtyEnvs) {
                if (idx >= 0 && idx < static_cast<int>(appState.environments.size()))
                    appState.environments[idx]->dirty = false;
            }
            appState.exitDirtyTabs.clear();
            appState.exitDirtyEnvs.clear();
            appState.exitDirtyLabels.clear();
            appState.showExitDirtyModal = false;
            if (appState.exitTargetIsGoHome) {
                appState.pendingGoHome = true;   // frame top closes all tabs
            } else {
                glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
            }
            appState.needsRedraw = true;
            ImGui::CloseCurrentPopup();
        } else if (pressed == 2 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            appState.exitDirtyTabs.clear();
            appState.exitDirtyEnvs.clear();
            appState.exitDirtyLabels.clear();
            appState.showExitDirtyModal = false;
            appState.exitDeferredClose = false;   // Cancel drops a deferred close
            appState.exitTargetIsGoHome = false;
            ImGui::CloseCurrentPopup();
        }
        drawModalAccentFrame(modalAccent());
        ImGui::EndPopup();
        wasOpen = true;
    } else {
        wasOpen = false;
    }
    endModal();
}

// Phase-4 experiment delete confirmation: dirty or persisted experiments
// confirm before removal (transient empty instances remove directly). On
// Delete: remove the experiment group from the .cross.h5 (if persisted) and
// drop the instance.
static void renderEnvDeleteConfirmModal() {
    static int focus = 0;
    static bool wasOpen = false;
    if (!appState.showEnvDeleteConfirm) {
        wasOpen = false;
        return;
    }
    const int idx = appState.pendingEnvDeleteIdx;
    const bool valid = idx >= 0 && idx < static_cast<int>(appState.environments.size());
    ImGui::OpenPopup("Delete Experiment##confirm");
    beginModal(480.0f, modalAccent());
    if (ImGui::BeginPopupModal("Delete Experiment##confirm", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::Text("Delete Experiment");
        ImGui::Spacing();
        if (valid) {
            auto* env = appState.environments[idx].get();
            ImGui::TextWrapped("Delete \"%s\"?", env->instanceName.c_str());
            if (env->dirty && !env->id.empty())
                ImGui::TextWrapped("Unsaved changes will be lost and the saved experiment "
                                   "removed from the project.");
            else if (env->dirty)
                ImGui::TextWrapped("This experiment has unsaved changes.");
            else
                ImGui::TextWrapped("The saved experiment will be removed from the project.");
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        int pressed = modalButtonRow({"Delete", "Cancel"}, focus, wasOpen, modalAccent());
        if (pressed == 0) {
            if (valid) {
                auto* env = appState.environments[idx].get();
                if (!env->id.empty() && appState.sessionTab.multiWorkspaceOpen) {
                    std::string err;
                    if (!crossExperimentRemove(appState.sessionTab.multiWorkspacePath,
                                               env->id, err)) {
                        appState.adapterErrorMsg = "Delete failed:\n" + err;
                        appState.showAdapterErrorPopup = true;
                    }
                }
                removeEnvironment(appState, idx);
            }
            appState.showEnvDeleteConfirm = false;
            appState.pendingEnvDeleteIdx = -1;
            appState.needsRedraw = true;
            ImGui::CloseCurrentPopup();
        } else if (pressed == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            appState.showEnvDeleteConfirm = false;
            appState.pendingEnvDeleteIdx = -1;
            ImGui::CloseCurrentPopup();
        }
        drawModalAccentFrame(modalAccent());
        ImGui::EndPopup();
        wasOpen = true;
    } else {
        wasOpen = false;
    }
    endModal();
}

// Exit "Save All" state machine: one dirty tab per frame. Runs at frame top
// (after the queued swap) so every save sees the tab's data in the flat
// fields. Pauses while an unsaved/stale confirmation modal is up; a stale
// "Cancel" is treated as save-skip (the user declined the drop; Save All is
// best-effort). Completion dispatches to the exitTargetIsGoHome terminal:
// window close for Exit, pendingGoHome for Ctrl+H.
static void advanceExitSaveAll() {
    if (!appState.exitSaveAllRunning) return;
    if (appState.showUnsavedPrompt || appState.showStaleDropPrompt) return;
    if (appState.pendingSwapIdx >= 0) return;   // swap queued, not yet executed
    if (appState.exitSaveAllCursor == appState.exitDirtyTabs.size()) {
        // Workspace saves done (or none were dirty) — Phase 4: dirty
        // experiments are live objects, no swap needed; save them all in one
        // pass (best-effort, mirrors the workspace saves).
        appState.exitSaveAllCursor++;   // run once
        const std::string& crossPath = appState.sessionTab.multiWorkspacePath;
        if (!crossPath.empty()) {
            for (int idx : appState.exitDirtyEnvs) {
                if (idx < 0 || idx >= static_cast<int>(appState.environments.size()))
                    continue;
                auto& env = appState.environments[idx];
                if (!env->dirty) continue;
                std::string err;
                if (!crossSaveExperiment(appState, *env, crossPath, err)) {
                    appState.adapterErrorMsg = std::string("Experiment save failed:\n") + err;
                    appState.showAdapterErrorPopup = true;
                    appState.exitSaveAllRunning = false;
                    appState.exitDirtyTabs.clear();
                    appState.exitDirtyEnvs.clear();
                    appState.exitDirtyLabels.clear();
                    appState.exitTargetIsGoHome = false;
                    return;
                }
                env->dirty = false;
            }
        }
        for (int idx : appState.exitDirtyEnvs) {
            if (idx >= 0 && idx < static_cast<int>(appState.environments.size()))
                appState.environments[idx]->dirty = false;
        }
        appState.exitSaveAllRunning = false;
        appState.exitDirtyTabs.clear();
        appState.exitDirtyEnvs.clear();
        appState.exitDirtyLabels.clear();
        if (appState.exitTargetIsGoHome) {
            appState.exitTargetIsGoHome = false;
            appState.pendingGoHome = true;
        } else {
            glfwSetWindowShouldClose(glfwGetCurrentContext(), GLFW_TRUE);
        }
        return;
    }
    if (appState.activeSessionIdx != appState.exitDirtyTabs[appState.exitSaveAllCursor]) {
        swapInSession(appState, appState.exitDirtyTabs[appState.exitSaveAllCursor]);
        return;
    }
    try {
        doSaveWorkspace(appState, "");
    } catch (const std::exception& e) {
        appState.adapterErrorMsg = std::string("Save failed:\n") + e.what();
        appState.showAdapterErrorPopup = true;
        appState.exitSaveAllRunning = false;
        appState.exitDirtyTabs.clear();
        appState.exitDirtyEnvs.clear();
        appState.exitDirtyLabels.clear();
        appState.exitTargetIsGoHome = false;
        return;
    }
    appState.exitSaveAllCursor++;
    if (appState.exitSaveAllCursor >= appState.exitDirtyTabs.size()) {
        // Workspace saves done — the experiment phase runs next frame.
        appState.needsRedraw = true;
    } else {
        swapInSession(appState, appState.exitDirtyTabs[appState.exitSaveAllCursor]);
    }
}

// White outline around a hovered tab item (bugfix 1). Called right after
// BeginTabItem (inside its shown gate) so LastItemData holds the tab rect.
static void drawTabHoverOutline() {
    if (!ImGui::IsItemHovered()) return;
    const ImVec2 minRect = ImGui::GetItemRectMin();
    const ImVec2 maxRect = ImGui::GetItemRectMax();
    const ImVec2 min(minRect.x - 1.0f, minRect.y - 1.0f);
    const ImVec2 max(maxRect.x + 1.0f, maxRect.y + 1.0f);
    ImGui::GetWindowDrawList()->AddRect(min, max,
        IM_COL32(255, 255, 255, 190), ImGui::GetStyle().TabRounding,
        ImDrawFlags_None, 1.5f);
}

// Tab strip (M2.2) — OUTSIDE the DockSpace window, between the menu bar and
// the DockSpace Begin. ONE tab bar: the Session tab is a real tab item as the
// FIRST entry (NoReorder: neither draggable nor crossable — order-pinned),
// unclosable by construction (nullptr p_open: no close button, and no
// middle-click/context close is handled for it). Workspace tabs follow.
// Returns the strip height in pixels (0 when no tabs exist yet, i.e. behind
// the launch welcome).
static float renderTabStrip() {
    if (!appState.sessionTabPresent) return 0.0f;
    // The strip must NOT render windowless: widgets after EndMainMenuBar
    // would land in ImGui's implicit "Debug##Default" fallback window (a
    // decorated 400x400 box at viewport+(60,60)). It gets its own borderless
    // window pinned under the menu bar, full width, with an explicit height:
    // tab content (FrameHeight) + the tab bar's bottom border + 1px.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float tabH = ImGui::GetFrameHeight();
    // Visual separation from the menu ribbon above: a gap band (shown in a
    // slightly lighter bg) + a 1px border line under it (bugfix 2).
    const float gap = 4.0f;
    const float stripH = gap + tabH + style.TabBarBorderSize + 1.0f;

    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + ImGui::GetFrameHeight()),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, stripH), ImGuiCond_Always);
    // Allow a height below ImGui's default 32px WindowMinSize: the strip's
    // content is only the ~21px tab bar; without this the window auto-grows
    // to 32px and the DockSpace (offset by stripH) sits 10px below the tab
    // bar's border — the dead strip of black that made the selector look
    // "crowded/junky".
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, gap));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.08f, 0.08f, 1.0f));
    // TabHovered overridden to the full accent so a hovered tab lights up
    // instead of dimming the active tab to near-black (bugfix 1: the theme's
    // TabHovered is ~as dark as the idle Tab color). Pushed BEFORE Begin so
    // it is balanced by the PopStyleColor(2) after End.
    ImGui::PushStyleColor(ImGuiCol_TabHovered,
                          ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
    ImGui::Begin("##TabStrip", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoFocusOnAppearing);
    // The tab bar reserves a FontSize-high (13px) "contents" area below the
    // tabs, which inflates the window's SizeContents to ~32px and makes the
    // window auto-grow past the requested stripH (the dead black band under
    // the tabs). The strip has no tab content — clamp the window to the tab
    // bar's real extent (tabs + border) so the DockSpace sits directly below
    // the tab bar instead of 10px lower.
    if (ImGuiWindow* sw = ImGui::GetCurrentWindow()) {
        if (sw->Size.y > stripH) {
            sw->Size.y = stripH;
            sw->SizeFull.y = stripH;
            if (sw->DC.CursorMaxPos.y > stripH) sw->DC.CursorMaxPos.y = stripH;
        }
    }
    // 1px border line at the top of the tab area (under the gap band) — the
    // visual separation between the menu ribbon and the tab selector (bugfix
    // 2). Drawn before the tab bar so the tabs overdraw it where they sit.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(0.0f, gap - 1.0f),
                    ImVec2(ImGui::GetWindowSize().x, gap - 1.0f),
                    ImGui::GetColorU32(ImGuiCol_Border));
    }

    // Reorderable: the visual strip order may differ from sessions[] (session
    // order is fixed at creation); selection/click/close are bound to the
    // stable tab IDs, so no index remap is needed. NoReorder keeps the
    // Session tab first (ImGui blocks dragging it and crossing over it).
    if (ImGui::BeginTabBar("##WorkspaceTabs",
            ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_Reorderable)) {
        // ── Session tab: identical look to workspace tabs (a real tab item),
        // unclosable (nullptr p_open) and order-pinned (NoReorder).
        const bool sessionActive = (appState.activeTabKind == ActiveTabKind::Session);
        const bool sessionShown = ImGui::BeginTabItem("Session##session", nullptr,
            ImGuiTabItemFlags_NoReorder |
            (sessionActive ? ImGuiTabItemFlags_SetSelected : 0));
        if (ImGui::IsItemClicked() && !sessionActive) focusSessionTab(appState);
        if (sessionShown) {
            drawTabHoverOutline();
            ImGui::EndTabItem();
        }

        // Environment tabs (Phase 3): after the workspace tabs, in the same
        // scrollable bar. LIVE instances (never folded); activation is direct
        // (no park/resume) — click sets activeTabKind + activeEnvIdx.
        // With one focused, no workspace tab is active — clear the bar's stale
        // selection so no workspace tab stays highlighted.
        if (appState.activeTabKind == ActiveTabKind::Environment) {
            ImGuiTabBar* bar = ImGui::GetCurrentTabBar();
            if (bar && bar->SelectedTabId != 0) {
                bar->SelectedTabId = 0;
                bar->NextSelectedTabId = 0;
            }
        }
        for (int i = 0; i < static_cast<int>(appState.sessions.size()); ++i) {
            WorkspaceSession* sess = appState.sessions[i].get();
            const bool isActive = (appState.activeTabKind == ActiveTabKind::Workspace &&
                                   appState.activeSessionIdx == i);
            const bool dirty = isActive ? appState.workspaceDirty() : sess->isDirty();
            const std::string label = sess->label() + (dirty ? " *" : "") +
                                      "##ws" + std::to_string(i);
            bool open = true;
            const bool shown = ImGui::BeginTabItem(label.c_str(), &open,
                isActive ? ImGuiTabItemFlags_SetSelected : 0);
            // Click detection runs OUTSIDE the shown gate: BeginTabItem
            // returns "contents visible", not "clicked" — a non-selected tab
            // (e.g. every workspace tab while the Session tab is focused)
            // returns false, yet ImGui's button hit-test still registered the
            // click (LastItemData), so IsItemClicked() is valid here.
            if (ImGui::IsItemClicked() && !isActive) swapInSession(appState, i);
            // EndTabItem must live INSIDE the BeginTabItem if-block: on the
            // tab's appearing frame (and when ItemAdd clips it) BeginTabItem
            // returns false WITHOUT pushing an ID — an unconditional
            // EndTabItem then pops the previous tab's ID, unbalancing the
            // stack and aborting in EndTabBar (crash on first tab render).
            if (shown) {
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
                    open = false;
                if (ImGui::BeginPopupContextItem("##tabClose")) {
                    if (ImGui::MenuItem("Close")) open = false;
                    ImGui::EndPopup();
                }
                drawTabHoverOutline();
                ImGui::EndTabItem();
            }
            if (!open) {
                closeTab(appState, i);
                break;   // sessions vector changed — stop iterating
            }
        }
        // Environment instances (Phase 3): live tabs, same close affordances
        // (hover [x] / middle-click / context). IDs ##env<i> resolve to the
        // environments vector — a reorder remaps only the strip order.
        for (int i = 0; i < static_cast<int>(appState.environments.size()); ++i) {
            auto* env = appState.environments[i].get();
            const bool isActive = (appState.activeTabKind == ActiveTabKind::Environment &&
                                   appState.activeEnvIdx == i);
            const std::string label =
                env->tabLabel() + "##env" + std::to_string(i);
            bool open = true;
            const bool shown = ImGui::BeginTabItem(label.c_str(), &open,
                isActive ? ImGuiTabItemFlags_SetSelected : 0);
            if (ImGui::IsItemClicked() && !isActive) activateEnvironment(appState, i);
            if (shown) {
                if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
                    open = false;
                if (ImGui::BeginPopupContextItem("##envClose")) {
                    if (ImGui::MenuItem("Close")) open = false;
                    ImGui::EndPopup();
                }
                drawTabHoverOutline();
                ImGui::EndTabItem();
            }
            if (!open) {
                env->closeRequest();
                break;   // environments vector changed — stop iterating
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    // Return the ACTUAL window height (the size clamp above caps the tab
    // bar's reserved contents space, so this equals the tab bar's real
    // extent). The DockSpace Y offset uses this value — a mismatch makes the
    // DockSpace overlap the strip's bottom edge.
    if (ImGuiWindow* strip = ImGui::FindWindowByName("##TabStrip"))
        return strip->Size.y;
    return stripH;
}

// Handle keyboard navigation for file selection
// @param csvFiles List of available CSV files
// @param currentSortedFileIndex Current file index reference
// @param filesChanged Reference to files changed flag
// @param keyboardNavigation Reference to keyboard navigation flag
void handleKeyboardNavigation(const std::vector<std::string>& csvFiles, 
                             size_t& currentSortedFileIndex, 
                             bool& filesChanged, 
                             bool& keyboardNavigation, 
                             bool shiftSelectMode, 
                             std::vector<std::string>& selectedFiles, 
                             std::vector<std::string>& selectedFilenames, 
                             std::vector<InterferogramData>& loadedData, 
                             std::vector<InterferogramData>& rawDataCache, 
                             bool& dataLoaded, 
                             const std::vector<std::string>& sortedFiles, 
                             bool enableDownsampling, 
                             size_t maxPointsBeforeDownsampling, 
                             size_t maxSelectableFiles) {
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) && !csvFiles.empty()) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            // Navigate up in file list (with wrapping)
            if (currentSortedFileIndex > 0) {
                currentSortedFileIndex--;
            } else {
                currentSortedFileIndex = csvFiles.size() - 1; // Wrap to bottom
            }
            filesChanged = true;
            keyboardNavigation = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            // Navigate down in file list (with wrapping)
            if (currentSortedFileIndex < csvFiles.size() - 1) {
                currentSortedFileIndex++;
            } else {
                currentSortedFileIndex = 0; // Wrap to top
            }
            filesChanged = true;
            keyboardNavigation = true;
        }
        
        // Handle Shift+Arrow for adding next file to selection
        if (shiftSelectMode) {
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                // Add the current file to selection with FIFO behavior if limit reached
                std::string fullPath = sortedFiles[currentSortedFileIndex];
                
                // Check if file is already selected
                auto it = std::find(selectedFiles.begin(), selectedFiles.end(), fullPath);
                if (it == selectedFiles.end()) {
                    // File not already selected, add it
                    try {
                        if (!appState.dataSourceReady()) {
                            filesChanged = false;
                            return;
                        }
                        InterferogramData data = loadInterferogram(appState, fullPath);
                        InterferogramData rawData = data; // Store raw data before any processing
                        
                        // Apply downsampling
                        if (enableDownsampling && data.referenceDetector.size() > maxPointsBeforeDownsampling) {
                            size_t localDownsampleFactor = data.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                            std::vector<double> downsampledRef, downsampledPrim;
                            for (size_t j = 0; j < data.referenceDetector.size(); j += localDownsampleFactor) {
                                downsampledRef.push_back(data.referenceDetector[j]);
                                downsampledPrim.push_back(data.primaryDetector[j]);
                            }
                            data.referenceDetector = downsampledRef;
                            data.primaryDetector = downsampledPrim;
                        }
                        
                        // Enforce 5-file limit with FIFO behavior
                        if (selectedFiles.size() >= maxSelectableFiles) {
                            // Remove oldest file (FIFO)
                            selectedFiles.erase(selectedFiles.begin());
                            selectedFilenames.erase(selectedFilenames.begin());
                            loadedData.erase(loadedData.begin());
                            rawDataCache.erase(rawDataCache.begin()); // Also remove from raw data cache
                        }
                        
                        loadedData.push_back(data);
                        rawDataCache.push_back(rawData); // Store raw data for spectrum computation
                        selectedFiles.push_back(fullPath);
                        
                        // Extract filename for legend
                        std::string filename = fullPath;
                        size_t last_slash = filename.find_last_of("/\\");
                        if (last_slash != std::string::npos) {
                            filename = filename.substr(last_slash + 1);
                        }
                        selectedFilenames.push_back(filename);
                        
                        dataLoaded = true;
                    } catch (const std::exception& e) {
                        std::cerr << "Error loading file: " << e.what() << std::endl;
                    }
                }
                
                // Don't change filesChanged since we're adding to selection, not replacing
                filesChanged = false;
            }
        }
    }
}

/**
 * Handle UI scaling changes
 * @param io ImGuiIO reference for DPI scaling
 * @param uiScale UI scale factor reference
 * @param currentUiSize Current UI size setting
 * @param uiSizeChanged Reference to UI size changed flag
 */
// Environment windows are docked dynamically (SetNextWindowDockID FirstUseEver
// in EnvironmentSession::render) — no default-layout entry needed; per-tab-type
// layout persistence arrives in Phase 4 (P16).
static void rebuildDefaultLayout(ImGuiID dockspace_id, float topOffset) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderSetNodeSize(dockspace_id,
        ImVec2(vp->Size.x, vp->Size.y - topOffset));

    ImGuiID dock_left, dock_right;
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Left, 0.16f, &dock_left, &dock_right);

    ImGuiID dock_left_top, dock_left_bottom;
    ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Up, 0.40f, &dock_left_top, &dock_left_bottom);

    ImGuiID dock_left_bottom_top, dock_left_bottom_bottom;
    ImGui::DockBuilderSplitNode(dock_left_bottom, ImGuiDir_Up, 0.50f, &dock_left_bottom_top, &dock_left_bottom_bottom);

    ImGuiID dock_center, dock_right_panel;
    ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Left, 0.48f, &dock_center, &dock_right_panel);

    ImGuiID dock_right_top, dock_right_bottom;
    ImGui::DockBuilderSplitNode(dock_right_panel, ImGuiDir_Up, 0.50f, &dock_right_top, &dock_right_bottom);

    ImGui::DockBuilderDockWindow("Files",              dock_left_top);
    // Session-tab panels dock directly in the main dock space (no
    // intermediate "Session" host window): Datasets shares the left-top node
    // with Files, Active/Available Environments the right column — the
    // requested "Datasets left, other two stacked right". Each node shows the
    // session panel only while the Session tab is active (the workspace
    // panels are gated out); forceDockSelection keeps it the selected tab.
    ImGui::DockBuilderDockWindow("Datasets",              dock_left_top);
    ImGui::DockBuilderDockWindow("Metadata",           dock_left_bottom_top);
    ImGui::DockBuilderDockWindow("Export",             dock_left_bottom_top);
    ImGui::DockBuilderDockWindow("SNR",                dock_left_bottom_top);
    ImGui::DockBuilderDockWindow("100% T",             dock_left_bottom_top);
    ImGui::DockBuilderDockWindow("Allan",              dock_left_bottom_top);
    ImGui::DockBuilderDockWindow("Spectrum",           dock_left_bottom_bottom);
    ImGui::DockBuilderDockWindow("Interferogram",      dock_left_bottom_bottom);
    ImGui::DockBuilderDockWindow("Average",            dock_left_bottom_bottom);
    ImGui::DockBuilderDockWindow("Interferogram View", dock_center);
    ImGui::DockBuilderDockWindow("100% T View",        dock_center);
    ImGui::DockBuilderDockWindow("Allan View",         dock_center);
    ImGui::DockBuilderDockWindow("SNR View",           dock_right_top);
    ImGui::DockBuilderDockWindow("Average View",       dock_right_top);
    ImGui::DockBuilderDockWindow("Active Environments", dock_right_top);
    ImGui::DockBuilderDockWindow("Spectrum View",      dock_right_bottom);
    ImGui::DockBuilderDockWindow("Available Environments", dock_right_bottom);

    ImGui::DockBuilderFinish(dockspace_id);
}
static void renderSpectrumViewPanel() {
        ImGui::Begin("Spectrum View");
        if (appState.active->dataLoaded && !appState.active->loadedData.empty()) {
            // Pre-load precomputed spectra into spectrum cache (always refresh)
            if (appState.active->datasetInfo.hasPrecomputedSpectra) {
                auto targetUnit = static_cast<SpectralToolbox::SpectrumXUnit>(appState.active->spectrum.xUnitSelector);
                for (size_t i = 0; i < appState.active->loadedData.size(); i++) {
                    const std::string& fid = appState.active->selectedFilenames[i];
                    if (appState.active->spectrum.cachedFrequencies.find(fid) == appState.active->spectrum.cachedFrequencies.end()) {
                        // File stores wavenumber in cm-1; convert to target unit
                        std::vector<double> freqs = appState.active->rawDataCache[i].referenceDetector;
                        for (double& f : freqs)
                            f = SpectralToolbox::convertXValue(f,
                                SpectralToolbox::SpectrumXUnit::CmInv, targetUnit);
                        appState.active->spectrum.cachedFrequencies[fid] = std::move(freqs);
                        appState.active->spectrum.cachedSpectra[fid] = appState.active->rawDataCache[i].primaryDetector;
                        appState.active->spectrum.lastPrimaryDetectors[fid] = appState.active->rawDataCache[i].primaryDetector;
                        double activeParam = 0.0;
                        if (appState.active->spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Gauss))
                            activeParam = static_cast<double>(appState.active->spectrum.apodizationParams.gaussSigma);
                        else if (appState.active->spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Rectangular))
                            activeParam = static_cast<double>(appState.active->spectrum.apodizationParams.rectWidth);
                        else if (appState.active->spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::NortonBeer))
                            activeParam = static_cast<double>(appState.active->spectrum.apodizationParams.nortonBeerFwhm);
                        else if (appState.active->spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::DolphChebyshev))
                            activeParam = static_cast<double>(appState.active->spectrum.apodizationParams.dolphChebyshevAt);
                        else if (appState.active->spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Hamming))
                            activeParam = static_cast<double>(appState.active->spectrum.apodizationParams.hammingAlpha);
                        else if (appState.active->spectrum.apodizationSelector == static_cast<int>(ApodizationWindow::Kaiser))
                            activeParam = static_cast<double>(appState.active->spectrum.apodizationParams.kaiserBeta);
                        appState.active->spectrum.lastSpectrumParams[fid] = {
                            static_cast<double>(appState.active->spectrum.Kpadding),
                            static_cast<double>(appState.active->spectrum.refLaserTextbox),
                            static_cast<double>(appState.active->spectrum.apodizationSelector),
                            activeParam,
                            appState.active->spectrum.apodizationParams.rectAsymMode ? 1.0 : 0.0,
                            static_cast<double>(appState.active->xCorrectionMethod),
                            static_cast<double>(appState.active->peakProminenceThreshold),
                            0.0
                        };
                    }
                }
            }
            std::vector<std::pair<std::string, std::vector<double>>> primaryDetectors;
            for (size_t i = 0; i < appState.active->loadedData.size(); i++) {
                primaryDetectors.emplace_back(appState.active->selectedFilenames[i], appState.active->loadedData[i].primaryDetector);
            }
            appState.active->spectrum.renderSpectrumContents(primaryDetectors, appState.active->rawDataCache);
        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();
}

static void renderAverageViewPanel() {
        ImGui::Begin("Average View");
        appState.active->averageSpectrum.renderAverageContents(appState.active->spectrum.showTrackingCursor);
        ImGui::End();
}

static void renderSnrViewPanel() {
        ImGui::Begin("SNR View");
        appState.active->snrSpectrum.renderSnrContents(appState.active->spectrum.showTrackingCursor);
        ImGui::End();
}

static void renderAllanViewPanel() {
        ImGui::Begin("Allan View");
        appState.active->allanVariance.renderAllanContents(appState.active->spectrum.showTrackingCursor);
        ImGui::End();
}

static void renderT100ViewPanel() {
        ImGui::Begin("100% T View");
        if (appState.active->dataLoaded && !appState.active->selectedFilenames.empty()) {
            appState.active->t100.renderT100Contents(appState.active->spectrum.showTrackingCursor);
        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();
}

// ── AppLoop ─────────────────────────────────────────────────────────────────

AppLoop::AppLoop(AppConfig& config, const std::string& configFilePath,
                 GLFWwindow* window)
    : config_(config), configFilePath_(configFilePath), window_(window) {}

// One frame. Reads like pseudocode: poll -> async poll -> redraw scheduling ->
// (idle gate) -> input -> UI -> present. Returns false when the window closes.
bool AppLoop::runFrame() {
    // Frame-top queued-swap execution (M2.2/3): tab clicks queue (park, idx)
    // mid-frame; the actual park/resume runs HERE — before any poll walks a
    // future vector — so no panel ever renders or polls against a
    // half-swapped state.
    executePendingSwap(appState);
    // A stashed open of a NEW workspace tab: the blank session was swapped in
    // above; load the workspace into the (now blank) flat fields.
    executePendingOpen(appState);
    // Close of a PARKED dirty tab: the swap above brought it in — show its
    // unsaved modal now (flat fields hold the right workspace).
    if (appState.pendingCloseAfterSwap >= 0) {
        appState.pendingTabCloseIdx = appState.pendingCloseAfterSwap;
        appState.pendingCloseAfterSwap = -1;
        appState.showUnsavedPrompt = true;
        appState.needsRedraw = true;
    }
    // Queued removal of the ACTIVE tab (clean close, or the unsaved modal's
    // Save/Discard resolution): park-free removal at frame top.
    if (appState.pendingRemoveIdx >= 0) {
        const int idx = appState.pendingRemoveIdx;
        appState.pendingRemoveIdx = -1;
        removeTab(appState, idx);
    }
    // Exit "Save All": sequential per-tab saves, one swap per frame.
    advanceExitSaveAll();
    // Ctrl+H go-home finalizer: every tab is clean here (clean at request
    // time, or saved/discarded via the shared modal). Must run AFTER
    // advanceExitSaveAll — its completion sets pendingGoHome.
    if (appState.pendingGoHome) {
#if FTS_BUILD_HDF5
        finalizeGoHome(appState);
#endif
    }

    pollEvents();
    if (glfwWindowShouldClose(window_)) return false;
    pollAsyncComputations();
    tickSessions();
    scheduleRedraws();

    // Skip rendering when UI is static — saves CPU/GPU
    if (!appState.needsRedraw) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return true;
    }
    if (!appState.showWelcomeScreen || appState.welcomeScreenInitialized) {
        appState.needsRedraw = false;
    }

    handleInput();
    renderUI();
    present();
    return true;
}

void AppLoop::pollEvents() {
        glfwPollEvents();

#if FTS_BUILD_HDF5
        // Exit intercept (M2.2): defer closing while ANY tab is dirty so the
        // multi-dirty modal can run (Save All / Discard All / Cancel). The
        // active workspace's dirty flag lives in the flat fields; parked
        // sessions answer via their latch.
        if (glfwWindowShouldClose(window_) && !appState.showExitDirtyModal &&
            !appState.showUnsavedPrompt && !appState.showStaleDropPrompt &&
            appState.exitSaveAllRunning == false &&
            appState.pendingWorkspaceAction == PendingWorkspaceAction::None) {
            std::vector<int> dirtyTabs;
            std::vector<std::string> dirtyLabels;
            collectDirtyTabs(appState, dirtyTabs, dirtyLabels);
            if (!dirtyTabs.empty() || !appState.exitDirtyEnvs.empty()) {
                glfwSetWindowShouldClose(window_, GLFW_FALSE);   // defer
                appState.showExitDirtyModal = true;
                appState.exitDirtyTabs = std::move(dirtyTabs);
                appState.exitDirtyLabels = std::move(dirtyLabels);
                appState.needsRedraw = true;
            }
        }
        // A close request while a dirty-flow modal/state is pending must NOT
        // close: the pending prompt would be skipped and the dirty tab
        // silently dropped (e.g. the tab-close "Unsaved Changes" modal is up,
        // or the exit modal itself, or Save All is mid-run). Defer the close
        // and re-apply it once the pending flow resolves. needsRedraw is set
        // so the pending prompt re-renders — the X press never goes by
        // silently (bugfix 5).
        if (glfwWindowShouldClose(window_)) {
            if (appState.showUnsavedPrompt || appState.showExitDirtyModal ||
                appState.showStaleDropPrompt || appState.exitSaveAllRunning ||
                appState.pendingWorkspaceAction != PendingWorkspaceAction::None) {
                glfwSetWindowShouldClose(window_, GLFW_FALSE);
                appState.exitDeferredClose = true;
                appState.needsRedraw = true;
            }
        }
        if (appState.exitDeferredClose && !appState.showUnsavedPrompt &&
            !appState.showExitDirtyModal && !appState.showStaleDropPrompt &&
            !appState.exitSaveAllRunning &&
            appState.pendingWorkspaceAction == PendingWorkspaceAction::None) {
            appState.exitDeferredClose = false;
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
        }
#endif
}

void AppLoop::pollAsyncComputations() {
        // Workspace-tab polls run only while a workspace tab is active — the
        // flat fields then hold THAT tab's data (M2.3). Session/environment
        // tabs poll via SessionBase::tickAsync() instead.
        // The active pointer is null while no workspace tab exists (launch
        // welcome / go-home) — the kind defaults to Workspace, so check both.
        if (appState.activeTabKind != ActiveTabKind::Workspace || !appState.active) return;
        // Poll pending async spectrum computations
        if (!appState.active->spectrum.pendingSpectra_.empty()) {
            appState.needsRedraw = true;
            appState.active->spectrum.pollPendingSpectra();
        }

        // Tick average spectrum calculation (parallel batch-submit + poll)
        if (appState.active->averageSpectrum.calcInProgress) {
            appState.needsRedraw = true;
            appState.active->averageSpectrum.tickCalculation();
        }

        // Tick SNR spectrum calculation (parallel batch-submit + poll)
        if (appState.active->snrSpectrum.calcInProgress) {
            appState.needsRedraw = true;
            appState.active->snrSpectrum.tickCalculation();
        }

        // Tick Allan variance calculation (parallel batch-submit + poll)
        if (appState.active->allanVariance.calcInProgress) {
            appState.needsRedraw = true;
            appState.active->allanVariance.tickCalculation();
        }

        // Tick T100 standard deviation calculation (parallel batch-submit + poll)
        if (appState.active->t100.calcStdInProgress) {
            appState.needsRedraw = true;
            if (appState.active->t100.tickStdCalculation()) {
                appState.needsRedraw = true;
            }
        }

}

// Per-tab async polls (M2.3): workspace tabs are no-ops here — their
// polling runs on the flat fields in pollAsyncComputations while active;
// Session/environment tabs (M2.5/Phase 3) poll their own futures. The
// ACTIVE environment instance polls only (audit §5.5): inactive instances
// drain on re-activation.
void AppLoop::tickSessions() {
    sessionTab_.tickAsync();
    for (auto& sess : appState.sessions) sess->tickAsync();
    if (appState.activeTabKind == ActiveTabKind::Environment &&
        appState.activeEnvIdx >= 0 &&
        appState.activeEnvIdx < static_cast<int>(appState.environments.size())) {
        appState.environments[appState.activeEnvIdx]->tickAsync();
    }
}

void AppLoop::scheduleRedraws() {
        static double lastForceRedrawTime = 0.0;
        if (!appState.needsRedraw && appState.showFPS) {
            double now = glfwGetTime();
            if (now - lastForceRedrawTime >= 1.0) {
                appState.needsRedraw = true;
                lastForceRedrawTime = now;
            }
        }

        // "Saved" toast: keep frames rendering while the toast is live, plus
        // one final frame right after expiry so the overlay is actually
        // cleared from the screen — the idle skip would otherwise freeze the
        // last pre-expiry frame with the toast still visible.
        if (appState.saveToastUntil > 0.0) {
            if (glfwGetTime() >= appState.saveToastUntil) {
                appState.saveToastUntil = 0.0;
                appState.needsRedraw = true;
            } else if (!appState.needsRedraw) {
                appState.needsRedraw = true;
            }
        }
}

void AppLoop::handleInput() {
    ImGuiIO& io = ImGui::GetIO();

        // Per-workspace input (M2.3): shortcuts, navigation and file loading
        // operate on the ACTIVE workspace tab's fields. With a non-workspace
        // tab focused (or none at all — launch welcome / go-home), the
        // per-workspace edge flags are cleared so no stale key edge fires
        // after the next swap.
        const bool wsActive = (appState.activeTabKind == ActiveTabKind::Workspace &&
                               appState.activeSessionIdx >= 0 && appState.active);
        if (!wsActive) {
            appState.yKeyPressedLastFrame = false;
            appState.aKeyPressedLastFrame = false;
            appState.dKeyPressedLastFrame = false;
            appState.qKeyPressedLastFrame = false;
            appState.sKeyPressedLastFrame = false;
        } else {
        appState.active->multiSelectMode = ImGui::GetIO().KeyCtrl;
        appState.active->shiftSelectMode = ImGui::GetIO().KeyShift;
        }

        // Handle keyboard shortcuts - only trigger once per key press
        if (wsActive && !ImGui::GetIO().WantCaptureKeyboard) {
            bool yKeyPressed = glfwGetKey(window_, GLFW_KEY_Y) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            bool aKeyPressed = glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            bool dKeyPressed = glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            bool qKeyPressed = glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            bool sKeyPressed = glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS && ImGui::GetIO().KeyCtrl;
            
            // 'Ctrl+Y' - Toggle auto-fit Y-axis (only on initial press)
            if (yKeyPressed && !appState.yKeyPressedLastFrame) {
                appState.active->autoFitYAxis = !appState.active->autoFitYAxis;
                if (appState.active->autoFitYAxis && appState.active->dataLoaded) {
                    if (!appState.active->loadedData[0].referenceDetector.empty()) {
                        auto ref_min_max = std::minmax_element(appState.active->loadedData[0].referenceDetector.begin(), appState.active->loadedData[0].referenceDetector.end());
                        appState.active->ref_y_min = *ref_min_max.first;
                        appState.active->ref_y_max = *ref_min_max.second;
                    }
                    auto prim_min_max = std::minmax_element(appState.active->loadedData[0].primaryDetector.begin(), appState.active->loadedData[0].primaryDetector.end());
                    appState.active->prim_y_min = *prim_min_max.first;
                    appState.active->prim_y_max = *prim_min_max.second;
                }
            }
            
            // 'Ctrl+A' - Toggle max at zero (only on initial press)
            if (aKeyPressed && !appState.aKeyPressedLastFrame) {
                appState.active->maxAtZero = !appState.active->maxAtZero;
                appState.active->shouldAutoscale = true;
            }
            
            // 'Ctrl+D' - Toggle downsampling (only on initial press)
            if (dKeyPressed && !appState.dKeyPressedLastFrame) {
                appState.active->enableDownsampling = !appState.active->enableDownsampling;
                appState.active->hilbertXCache.clear();
                if (appState.active->dataLoaded) {
                    // Reload all selected files with new downsampling setting while preserving selection
                    std::vector<InterferogramData> reloadedData;
                    for (const auto& filePath : appState.active->selectedFiles) {
                        try {
                            InterferogramData data = loadInterferogram(appState, filePath);
                            
                            // Apply downsampling if enabled and dataset is large
                            if (appState.active->enableDownsampling && data.referenceDetector.size() > appState.maxPointsBeforeDownsampling) {
                                size_t localDownsampleFactor = data.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                                
                                // Downsample both reference and primary detectors
                                std::vector<double> downsampledRef, downsampledPrim;
                                for (size_t j = 0; j < data.referenceDetector.size(); j += localDownsampleFactor) {
                                    downsampledRef.push_back(data.referenceDetector[j]);
                                    downsampledPrim.push_back(data.primaryDetector[j]);
                                }
                                data.referenceDetector = downsampledRef;
                                data.primaryDetector = downsampledPrim;
                            }
                            
                            reloadedData.push_back(data);
                        } catch (const std::exception& e) {
                            std::cerr << "Error reloading file: " << e.what() << std::endl;
                        }
                    }
                    
                    if (!reloadedData.empty()) {
                        appState.active->loadedData = reloadedData;
                        // Also update raw data cache - need to reload raw data
                        // IMPORTANT: We need to reload the ORIGINAL raw data, not the processed data
                        appState.active->rawDataCache.clear();
                        size_t reloadedIdx = 0;
                        for (const auto& file : appState.active->selectedFiles) {
                            try {
                                InterferogramData rawData = loadInterferogram(appState, file);
                                appState.active->rawDataCache.push_back(rawData);
                            } catch (const std::exception& e) {
                                std::cerr << "Error reloading raw data for spectrum: " << e.what() << std::endl;
                                if (reloadedIdx < reloadedData.size())
                                    appState.active->rawDataCache.push_back(reloadedData[reloadedIdx]);
                            }
                            reloadedIdx++;
                        }
                        // Force X-axis to show all data when downsampling is toggled (same behavior as menu)
                        appState.active->zoomRange = {0, 0};
                        appState.active->shouldAutoscale = true;
                        appState.active->forceXAutofit = true; // Set global flag to force X-axis autofit
                        std::cout << "Reloaded " << appState.active->loadedData.size() << " datasets with " 
                                  << (appState.active->enableDownsampling ? "enabled" : "disabled") << " downsampling" << std::endl;
                    }
                }
            }
            
            // 'Ctrl+Q' - Toggle tracking cursor (only on initial press)
            if (qKeyPressed && !appState.qKeyPressedLastFrame) {
                appState.active->spectrum.showTrackingCursor = !appState.active->spectrum.showTrackingCursor;
                appState.needsRedraw = true;
            }

#if FTS_BUILD_HDF5
            // 'Ctrl+S' - Save workspace; 'Ctrl+Shift+S' - Save As (workspace mode only)
            if (sKeyPressed && !appState.sKeyPressedLastFrame && appState.hasWorkspace()) {
                try {
                    if (ImGui::GetIO().KeyShift)
                        saveWorkspaceAs(appState, window_);
                    else
                        requestSaveWorkspace(appState, "");
                } catch (const std::exception& e) {
                    appState.adapterErrorMsg = std::string("Save failed:\n") + e.what();
                    appState.showAdapterErrorPopup = true;
                }
                appState.needsRedraw = true;
            }
#endif

            // Update key state tracking for next frame
            appState.yKeyPressedLastFrame = yKeyPressed;
            appState.aKeyPressedLastFrame = aKeyPressed;
            appState.dKeyPressedLastFrame = dKeyPressed;
            appState.qKeyPressedLastFrame = qKeyPressed;
            appState.sKeyPressedLastFrame = sKeyPressed;
        } else {
            // Reset key states when keyboard is captured (e.g., typing in text field)
            appState.yKeyPressedLastFrame = false;
            appState.aKeyPressedLastFrame = false;
            appState.qKeyPressedLastFrame = false;
            appState.sKeyPressedLastFrame = false;
        }

        // Reapply UI scaling if size changed
        handleUIScaling(io, appState.uiScale, appState.currentUiSize, appState.uiSizeChanged);

        // Apply accent color theme if changed
        if (appState.accentColorChanged) {
            ImGuiStyle& style = ImGui::GetStyle();
            ImPlotStyle& plotStyle = ImPlot::GetStyle();
            ApplyTheme(style, plotStyle, StringToAccentColor(appState.currentAccentColor));
            applyWindowIcon(window_, GetAccentBase(StringToAccentColor(appState.currentAccentColor)));
            appState.accentColorChanged = false;
            appState.needsRedraw = true;
        }
        
        // Calculate FPS
        float currentTime = ImGui::GetTime();
        appState.frameCount++;
        if (currentTime - appState.lastTime >= 1.0f) {
            appState.fps = static_cast<float>(appState.frameCount) / (currentTime - appState.lastTime);
            appState.frameCount = 0;
            appState.lastTime = currentTime;
        }
        
        // Track window state changes
        handleWindowEvents(window_, config_);
        
        // Update sorted files list for keyboard navigation (active workspace
        // tab only — no tab exists at launch / go-home).
        if (wsActive) {
        appState.active->sortedFiles = appState.active->csvFiles;
        std::sort(appState.active->sortedFiles.begin(), appState.active->sortedFiles.end(), [](const std::string& a, const std::string& b) {
            std::string nameA = a;
            std::string nameB = b;
            size_t last_slash_a = nameA.find_last_of("/\\");
            size_t last_slash_b = nameB.find_last_of("/\\");
            if (last_slash_a != std::string::npos) nameA = nameA.substr(last_slash_a + 1);
            if (last_slash_b != std::string::npos) nameB = nameB.substr(last_slash_b + 1);
            return naturalSortCompare(nameA, nameB);
        });
        }
        
        // Ensure averaging checkboxes match the sorted files size
        if (wsActive) {
        if (appState.active->filesSelectedForAveraging.size() != appState.active->sortedFiles.size()) {
            appState.active->filesSelectedForAveraging.clear();
            appState.active->filesSelectedForAveraging.resize(appState.active->sortedFiles.size(), true);
        }

// Handle keyboard navigation for file selection
        handleKeyboardNavigation(appState.active->csvFiles, appState.active->currentSortedFileIndex, appState.active->filesChanged, appState.active->keyboardNavigation, 
                                appState.active->shiftSelectMode, appState.active->selectedFiles, appState.active->selectedFilenames, appState.active->loadedData, appState.active->rawDataCache, appState.active->dataLoaded, 
                                appState.active->sortedFiles, appState.active->enableDownsampling, appState.maxPointsBeforeDownsampling, appState.MAX_SELECTABLE_FILES);
        

        

        
        // Handle Delete key to remove currently navigated file
        // (OpenPopup is deferred to the Files panel, after NewFrame)
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) &&
            ImGui::IsKeyPressed(ImGuiKey_Delete) &&
            !appState.active->sortedFiles.empty()) {
#if FTS_BUILD_HDF5
            if (appState.hasWorkspace()) {
                appState.active->pendingWorkspaceDeletionPath =
                    memberPathOf(appState.active->workspace, appState.active->sortedFiles[appState.active->currentSortedFileIndex]);
                if (!appState.active->pendingWorkspaceDeletionPath.empty()) {
                    appState.active->showWorkspaceDeleteConfirmPopup = true;
                    appState.needsRedraw = true;
                }
            } else
#endif
            if (appState.active->skipDeleteConfirm) {
                performFileDeletion(appState, appState.active->currentSortedFileIndex);
            } else {
                appState.active->deleteConfirmIndex = appState.active->currentSortedFileIndex;
                appState.active->showDeleteConfirmPopup = true;
                appState.needsRedraw = true;
            }
        }

        // Space toggles the selection checkbox for all highlighted files
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) &&
            !ImGui::GetIO().WantCaptureKeyboard &&
            ImGui::IsKeyPressed(ImGuiKey_Space) &&
            appState.active->dataLoaded) {
            for (const auto& selFile : appState.active->selectedFiles) {
                auto it = std::find(appState.active->sortedFiles.begin(), appState.active->sortedFiles.end(), selFile);
                if (it != appState.active->sortedFiles.end()) {
                    size_t idx = std::distance(appState.active->sortedFiles.begin(), it);
                    if (idx < appState.active->filesSelectedForAveraging.size())
                        appState.active->filesSelectedForAveraging[idx] = !appState.active->filesSelectedForAveraging[idx];
                }
            }
            appState.needsRedraw = true;
        }

        // 'Left/Right Arrow' - Pan left by 10% of current visible range
        if (glfwGetKey(window_, GLFW_KEY_LEFT) == GLFW_PRESS && !appState.active->leftArrowPressedLastFrame) {

            appState.active->leftArrowPressedLastFrame = true;
            appState.active->leftArrowHandleFlag = true;
        }
        else if (glfwGetKey(window_, GLFW_KEY_LEFT) == GLFW_RELEASE) {
            appState.active->leftArrowPressedLastFrame = false;
            appState.active->leftArrowHandleFlag = false;
        }

        if (glfwGetKey(window_, GLFW_KEY_RIGHT) == GLFW_PRESS && !appState.active->rightArrowPressedLastFrame) {

            appState.active->rightArrowPressedLastFrame = true;
            appState.active->rightArrowHandleFlag = true;
        }
        else if (glfwGetKey(window_, GLFW_KEY_RIGHT) == GLFW_RELEASE) {
            appState.active->rightArrowPressedLastFrame = false;
            appState.active->rightArrowHandleFlag = false;
        }

        // Load file if navigation changed
        if (appState.active->filesChanged && !appState.active->csvFiles.empty() && appState.dataSourceReady()) {
            try {
                // Load the currently selected file
                InterferogramData data = loadInterferogram(appState, appState.active->sortedFiles[appState.active->currentSortedFileIndex]);
                

                
                // Store raw data in cache before any processing for spectrum computation
                appState.active->rawDataCache.clear();
                appState.active->rawDataCache.push_back(data);
                
                // Create a copy for processing (downsampling, etc.)
                InterferogramData processedData = data;
                
                // Apply downsampling if enabled and dataset is large
                if (appState.active->enableDownsampling && processedData.referenceDetector.size() > appState.maxPointsBeforeDownsampling) {
                    size_t localDownsampleFactor = processedData.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                    
                    // Downsample both reference and primary detectors
                    std::vector<double> downsampledRef, downsampledPrim;
                    for (size_t i = 0; i < processedData.referenceDetector.size(); i += localDownsampleFactor) {
                        downsampledRef.push_back(processedData.referenceDetector[i]);
                        downsampledPrim.push_back(processedData.primaryDetector[i]);
                    }
                    processedData.referenceDetector = downsampledRef;
                    processedData.primaryDetector = downsampledPrim;
                    std::cout << "Downsampled dataset from " << (downsampledRef.size() * localDownsampleFactor) 
                              << " to " << downsampledRef.size() << " points (factor: " << localDownsampleFactor << ")" << std::endl;
                }
                
                // For single selection (no Ctrl), replace current selection
                appState.active->loadedData.clear();
                // DON'T clear raw data cache - we need it for spectrum calculation!
                // appState.active->rawDataCache.clear(); // Clear raw data cache too
                appState.active->selectedFiles.clear();
                appState.active->selectedFilenames.clear();
                
                // Always load the processed data
                appState.active->loadedData.push_back(processedData);
                // Raw data is already in cache from line 718, no need to add again
                // appState.active->rawDataCache.push_back(data); // Store raw data for spectrum computation

                appState.active->selectedFiles.push_back(appState.active->sortedFiles[appState.active->currentSortedFileIndex]);
                
                // Extract filename for legend
                std::string filename = appState.active->sortedFiles[appState.active->currentSortedFileIndex];
                size_t last_slash = filename.find_last_of("/\\");
                if (last_slash != std::string::npos) {
                    filename = filename.substr(last_slash + 1);
                }
                appState.active->selectedFilenames.push_back(filename);
                
                // Update current dataset name (extract from current directory path)
                std::string dirPath = appState.active->currentDirectory;
                size_t dir_last_slash = dirPath.find_last_of("/\\");
                if (dir_last_slash != std::string::npos) {
                    appState.active->currentDatasetName = dirPath.substr(dir_last_slash + 1);
                    // If this is "raw_data", get the parent directory name
                    if (appState.active->currentDatasetName == "raw_data" && dir_last_slash > 0) {
                        size_t parent_slash = dirPath.substr(0, dir_last_slash).find_last_of("/\\");
                        if (parent_slash != std::string::npos) {
                            appState.active->currentDatasetName = dirPath.substr(parent_slash + 1, dir_last_slash - parent_slash - 1);
                        }
                    }
                }
                
                appState.active->dataLoaded = true;
                appState.needsRedraw = true;
                
                // Handle autoscale behavior based on AGENTS.md requirements:
                // "when the application loads a file for display for the first time after launch or work directory switch, axes zoom to fit all data."
                if (appState.active->isFirstDataLoad) {
                    appState.active->zoomRange = {0, 0};
                    appState.active->shouldAutoscale = true; // Trigger autoscale
                    
                    // Recalculate Y-axis limits from the actual data for autoscale
                    if (!data.referenceDetector.empty()) {
                        auto ref_min_max = std::minmax_element(data.referenceDetector.begin(), data.referenceDetector.end());
                        appState.active->ref_y_min = *ref_min_max.first;
                        appState.active->ref_y_max = *ref_min_max.second;
                    }
                    auto prim_min_max = std::minmax_element(data.primaryDetector.begin(), data.primaryDetector.end());
                    appState.active->prim_y_min = *prim_min_max.first;
                    appState.active->prim_y_max = *prim_min_max.second;
                    
                    // Reset first load flag after handling
                    appState.active->isFirstDataLoad = false;
                }
                appState.active->filesChanged = false;
                
                // Add parent directory to recent datasets from the loaded file path
                if (!appState.active->selectedFiles.empty()) {
                    std::string datasetPath = appState.active->selectedFiles[0];
                    size_t last_slash = datasetPath.find_last_of("/\\");
                    if (last_slash != std::string::npos) {
                        std::string parentDir = datasetPath.substr(0, last_slash);
                        size_t raw_data_pos = parentDir.find_last_of("/\\");
                        if (raw_data_pos != std::string::npos &&
                            parentDir.substr(raw_data_pos + 1) == "raw_data") {
                            parentDir = parentDir.substr(0, raw_data_pos);
                        }
                        addToRecentDatasets(config_, configFilePath_, parentDir);
                    }
                }
                
            } catch (const std::exception& e) {
                std::cerr << "Error loading file: " << e.what() << std::endl;
                appState.active->dataLoaded = false;
                appState.active->filesChanged = false;
            }
        }
        }   // end wsActive gate

        // Ctrl+H: go back to home — close every workspace tab and re-show the
        // launch welcome screen (dirty tabs route through the Save All /
        // Discard All modal first). GLOBAL shortcut: lives OUTSIDE the
        // wsActive gate so it works from the Session tab too (bugfix
        // 2026-08-13).
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) && ImGui::IsKeyPressed(ImGuiKey_H) && ImGui::GetIO().KeyCtrl) {
#if FTS_BUILD_HDF5
            requestGoHome(appState);
#else
            resetActiveWorkspaceTab(appState);
#endif
        }

}

void AppLoop::renderUI() {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Rate-limit mouse wheel to at most one notch per frame, with carry-over
        // for excess. Raw deltas are accumulated in the GLFW scroll callback
        // (before ImGui's input queue), so stale queued wheel events can never
        // reach ImPlot. The accumulator drains here at 1 notch/frame — but only
        // while scroll input is fresh; once no new wheel event has arrived for a
        // short grace period the excess is discarded so zoom stops promptly
        // instead of ghosting on after the wheel stops.
        {
            ImGuiIO& io = ImGui::GetIO();

            const double now = glfwGetTime();
            if (now - appState.lastScrollEventTime > 0.08) {
                appState.scrollAccumX = 0.0f;
                appState.scrollAccumY = 0.0f;
                io.MouseWheel  = 0.0f;
                io.MouseWheelH = 0.0f;
            } else {
                float totalY = appState.scrollAccumY;
                appState.scrollAccumY = 0.0f;
                float totalX = appState.scrollAccumX;
                appState.scrollAccumX = 0.0f;

                io.MouseWheel  = std::clamp(totalY, -1.0f, 1.0f);
                io.MouseWheelH = std::clamp(totalX, -1.0f, 1.0f);

                appState.scrollAccumY += std::clamp(totalY - io.MouseWheel, -60.0f, 60.0f);
                appState.scrollAccumX += std::clamp(totalX - io.MouseWheelH, -60.0f, 60.0f);
            }
        }

        // Keep rendering while the scroll accumulator drains
        if (appState.scrollAccumY != 0.0f || appState.scrollAccumX != 0.0f)
            appState.needsRedraw = true;

#if FTS_BUILD_HDF5
        // Phase 3 view-state dirty latch: diff the managed view-state JSON
        // against the baseline. The baseline is finalized at the end of the
        // first rendered frame (see the post-render block), so the first-load
        // autoscale of the zoom ranges can never false-dirty a fresh open.
        // Latching (no baseline update on diff) matches the coarse-dirty model.
        if (appState.hasWorkspace() && !appState.active->workspace.dirty &&
            !appState.active->viewStateBaselinePending) {
            if (viewStateJson(appState) != appState.active->viewStateBaseline) {
                appState.active->workspace.dirty = true;
                // One entry per dirty period: the latch only fires on the
                // clean->dirty transition.
                logWorkspaceChange(appState.active->workspace,
                                   "View settings (zooms, ranges, panel options)");
            }
        }
#endif

        // Conditionally disable anti-aliasing for large datasets (>50k points).
        // Guarded: the active pointer is null while no workspace tab exists
        // (launch welcome / go-home), and after a tab switch loadedData may be
        // empty while dataLoaded is still latched — never index [0] without
        // the empty check.
        if (appState.active && appState.active->dataLoaded && !appState.active->loadedData.empty() &&
            appState.active->loadedData[0].dataSize() > 50000) {
            ImGui::GetStyle().AntiAliasedLines = false;
        }
        
        // Show welcome screen if no data is loaded and we haven't initialized yet
        if (appState.showWelcomeScreen && !appState.welcomeScreenInitialized) {
            // While a workspace-discard/save flow is pending, keep the welcome
            // popup suppressed: its per-frame OpenPopup would force-close the
            // Unsaved Changes / stale-drop modal (OpenPopupEx closes open
            // popups with a different id), making the welcome modal
            // "disappear" and the flow look broken.
            bool showPopup = !appState.showAdapterErrorPopup
                          && !appState.conversionScreen.open
                          && !appState.showUnsavedPrompt
                          && !appState.showStaleDropPrompt;
            renderWelcomeScreen(appState, config_, configFilePath_, showPopup);
        }

        // Phase 5: dataset conversion screen (foreign formats -> .h5)
        if (appState.conversionScreen.open) {
            ImGui::OpenPopup("Convert Dataset##conversion");
            appState.needsRedraw = true;
        }
        renderConversionScreen(appState);

        // Render adapter error popup
        if (appState.showAdapterErrorPopup) {
            ImGui::OpenPopup("Adapter Error##adapterError");
            appState.needsRedraw = true;
        }
        beginModal(520.0f, modalAccent());
        if (ImGui::BeginPopupModal("Adapter Error##adapterError", &appState.showAdapterErrorPopup,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            // NoTitleBar: the title moves into the body so removing the
            // header loses no information.
            ImGui::Text("Adapter Error");
            ImGui::Spacing();
            ImGui::TextWrapped("%s", appState.adapterErrorMsg.c_str());
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            static int errFocus = 0;
            static bool errWasOpen = false;
            if (modalButtonRow({"OK"}, errFocus, errWasOpen, modalAccent()) == 0 ||
                ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                appState.showAdapterErrorPopup = false;
                ImGui::CloseCurrentPopup();
            }
            errWasOpen = true;
            drawModalAccentFrame(modalAccent());
            ImGui::EndPopup();
        }
        endModal();

#if FTS_BUILD_HDF5
        // Phase 2 modals: unsaved-changes + stale-drop confirmation.
        renderUnsavedPromptModal();
        renderStaleDropPromptModal();
        renderExitDirtyModal();
        renderEnvDeleteConfirmModal();
#endif
        
        // Only render main docking interface if welcome screen is not active
        if (appState.welcomeScreenInitialized) {
            // Set up docking
            ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking;
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->Pos);
            ImGui::SetNextWindowSize(viewport->Size);
            ImGui::SetNextWindowViewport(viewport->ID);
            
            // Create ribbon menu first, before docking
            renderMainMenuBar(config_, configFilePath_, window_);
            // Tab strip (M2.2): Session pinned left, scrollable workspace bar.
            // OUTSIDE the DockSpace window — the DockSpace Y offset below is
            // bumped by the strip height so the dock area does not overlap it.
            const float stripH = renderTabStrip();
            const float topOffset = ImGui::GetFrameHeight() + stripH;

            // Always create dockspace, but make background transparent when welcome screen is active
            // Push style variables for full viewport docking
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

            // Make dockspace background transparent when welcome screen is active to show pattern
            if (appState.showWelcomeScreen && !appState.welcomeScreenInitialized) {
                ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            }
            
            window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
            window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
            
            // Adjust window position to account for menu bar + tab strip
            ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + topOffset));
            ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y - topOffset));
            
            // Create main dockspace window
            ImGui::Begin("DockSpace", nullptr, window_flags);
            ImGui::PopStyleVar(2);
            
            if (appState.showWelcomeScreen && !appState.welcomeScreenInitialized) {
                ImGui::PopStyleColor(); // Restore window background color
            }
            
            // Create docking space
            ImGuiID dockspace_id = ImGui::GetID("MainDockSpace_v2");

                // Session tab active: force each session panel's dock node
                // selection BEFORE DockSpace() runs its tab-bar layout.
                // DockNodeUpdateTabBar computes node->VisibleWindow from
                // VisibleTabId (derived from SelectedTabId/NextSelectedTabId);
                // the panels' Begin then reads it via BeginDocked ->
                // DockTabIsVisible. Setting the selection inside
                // SessionTab::render() (after Begin) is one frame too late —
                // with idle rendering that next frame may never happen, so the
                // session content stays invisible.
                if (appState.activeTabKind == ActiveTabKind::Session) {
                    const char* sessionPanels[3] = {"Datasets",
                                                    "Active Environments",
                                                    "Available Environments"};
                    for (const char* name : sessionPanels) {
                        if (ImGuiWindow* pw = ImGui::FindWindowByName(name)) {
                            if (pw->DockNode) {
                                pw->DockNode->SelectedTabId = pw->TabId;
                                if (pw->DockNode->TabBar)
                                    pw->DockNode->TabBar->NextSelectedTabId = pw->TabId;
                            }
                        }
                    }
                }
                // Phase 3: same forced selection for the active environment
                // instance's window (its dock tab must be visible after a
                // strip click; the instance's render arms the next frame via
                // IsWindowAppearing + needsRedraw).
                if (appState.activeTabKind == ActiveTabKind::Environment &&
                    appState.activeEnvIdx >= 0 &&
                    appState.activeEnvIdx < static_cast<int>(appState.environments.size())) {
                    const std::string winName = appState.environments[appState.activeEnvIdx]->title();
                    const std::string viewName = winName + " View";
                    for (const std::string& n : {winName, viewName}) {
                        if (ImGuiWindow* pw = ImGui::FindWindowByName(n.c_str())) {
                            if (pw->DockNode) {
                                pw->DockNode->SelectedTabId = pw->TabId;
                                if (pw->DockNode->TabBar)
                                    pw->DockNode->TabBar->NextSelectedTabId = pw->TabId;
                            }
                        }
                    }
                }

                // Handle manual layout restore request (from Settings menu)
                if (appState.restoreLayoutRequested) {
                    appState.restoreLayoutRequested = false;
                    rebuildDefaultLayout(dockspace_id, topOffset);
                }

                // Apply default layout only on first launch (persisted via config)
                if (!appState.defaultLayoutApplied) {
                    appState.defaultLayoutApplied = true;
                    config_.defaultLayoutApplied = true;
                    config_.saveToFile(configFilePath_);
                    rebuildDefaultLayout(dockspace_id, topOffset);
                }
                // Session-panel layout version (bugfix 2026-08-13): the panels
                // moved from a nested dock space into the main dock; their old
                // imgui.ini DockIds point at the retired nested nodes, which
                // ImGui recreates as implicit floating windows at stale
                // positions. One rebuild re-docks them (DockBuilderDockWindow
                // overwrites the DockIds); the persisted version fires this
                // exactly once.
                if (config_.sessionPanelLayoutVersion < 3) {
                    config_.sessionPanelLayoutVersion = 3;
                    config_.saveToFile(configFilePath_);
                    rebuildDefaultLayout(dockspace_id, topOffset);
                }

ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), 0);

                // White-panel fix: dock nodes whose windows do not render this
                // frame (e.g. workspace panels parked while the Session tab is
                // focused) are painted by DockContextEndFrame with their stale
                // LastBgColor — IM_COL32_WHITE at node creation. Force every
                // leaf node's bg to the window bg; windows that render later in
                // the frame overwrite it (imgui.cpp:7633 sets LastBgColor +
                // IsBgDrawnThisFrame at each docked window's render).
                {
                    ImGuiDockContext* dc = &ImGui::GetCurrentContext()->DockContext;
                    const ImU32 winBg = ImGui::GetColorU32(ImGuiCol_WindowBg);
                    for (int n = 0; n < dc->Nodes.Data.Size; n++)
                        if (ImGuiDockNode* node = (ImGuiDockNode*)dc->Nodes.Data[n].val_p)
                            if (node->IsLeafNode() && node->HostWindow)
                                node->LastBgColor = winBg;
                }
                
                ImGui::End();
            
        // Only render panels when welcome screen is not active AND a workspace
        // tab is focused (M2.3): the flat fields hold the active workspace's
        // data; the Session tab renders its own windows instead.
        if ((!appState.showWelcomeScreen || appState.welcomeScreenInitialized) &&
            appState.activeTabKind == ActiveTabKind::Workspace && appState.active) {
        // Files panel (left)
        renderFilesPanel();
        
        // Interferogram panel (main)
        renderInterferogramPanel();

// Spectrum panel (bottom)
        appState.active->spectrum.renderPanel(appState);
        
        // Average config panel
        renderAveragePanel();

        // SNR config panel
        renderSnrPanel();

        // Allan config panel
        renderAllanPanel();

        // 100% T config panel (docked)
        renderT100Panel();

        // Interferogram Config panel (docked)
        renderInterferogramConfigPanel();

        // Export panel (docked)
        appState.active->exportPanel.renderPanel();

        // Metadata panel (right)
        renderMetadataPanel();
        
        // Spectrum View panel (docked)
        renderSpectrumViewPanel();

        // Average View panel (docked)
        renderAverageViewPanel();

        // SNR View panel (docked)
        renderSnrViewPanel();

        // Allan View panel (docked)
        renderAllanViewPanel();

        // 100% T View panel (docked)
        renderT100ViewPanel();

        // Close the panel condition (welcome screen)
        }

        // Session tab: three dockable panels rendered directly in the main
        // dock space (M2.5). Renders whenever the Session tab is focused —
        // MUST be outside the workspace gate above (a bug here once left the
        // dock area empty/black and the strip unresponsive).
        // NOTE: no SetNextWindowFocus here — a per-frame FocusWindow() steals
        // the active id of the panels' tabs on the frame after a press
        // (activeWinRoot != focus root), killing their drag/undock. The dock
        // nodes' tab selections are forced above (pre-DockSpace) instead.
        if (appState.activeTabKind == ActiveTabKind::Session) {
            sessionTab_.render();
        }
        // Environment instance (Phase 3): the ACTIVE instance's window body.
        // Live object — no park/resume; the dock-node selection is forced
        // above (pre-DockSpace) so the window's tab is visible.
        if (appState.activeTabKind == ActiveTabKind::Environment &&
            appState.activeEnvIdx >= 0 &&
            appState.activeEnvIdx < static_cast<int>(appState.environments.size())) {
            appState.environments[appState.activeEnvIdx]->render();
        }
        
        // Close the docking condition
        }


        ImGuiIO& io = ImGui::GetIO();
        if (appState.showFPS) {
            // Create a high-contrast FPS counter in top-right corner
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 120, 30), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
            ImGui::SetNextWindowBgAlpha(0.7f); // Semi-transparent background
            
            ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | 
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | 
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
            
            ImGui::Begin("FPS Counter", nullptr, flags);
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "FPS: %.1f", appState.fps); // White text for high contrast
            ImGui::End();
        }
        if (appState.active && appState.active->exportPanel.exportPending) {
            fprintf(stderr, "DEBUG: Export progress overlay rendering\n");
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            ImVec2 size = ImGui::GetIO().DisplaySize;
            dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 160));
            const char* msg = "Export in progress... Please wait.";
            ImVec2 ts = ImGui::CalcTextSize(msg);
            ImVec2 pos((size.x - ts.x) * 0.5f, (size.y - ts.y) * 0.5f);
            dl->AddRectFilled(
                ImVec2(pos.x - 20, pos.y - 12),
                ImVec2(pos.x + ts.x + 20, pos.y + ts.y + 12),
                IM_COL32(30, 30, 50, 230), 8.0f);
            // Accent border, matching the "Saved" toast family.
            dl->AddRect(
                ImVec2(pos.x - 20, pos.y - 12),
                ImVec2(pos.x + ts.x + 20, pos.y + ts.y + 12),
                ImGui::ColorConvertFloat4ToU32(modalAccent()),
                8.0f, ImDrawFlags_None, 2.0f);
            dl->AddText(pos, IM_COL32(255, 255, 255, 255), msg);
        }
        if (glfwGetTime() < appState.saveToastUntil) {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            ImVec2 size = ImGui::GetIO().DisplaySize;
            const char* msg = "Saved";
            ImVec2 ts = ImGui::CalcTextSize(msg);
            ImVec2 pos((size.x - ts.x) * 0.5f, (size.y - ts.y) * 0.5f);
            dl->AddRectFilled(
                ImVec2(pos.x - 22, pos.y - 14),
                ImVec2(pos.x + ts.x + 22, pos.y + ts.y + 14),
                IM_COL32(30, 30, 50, 230), 8.0f);
            dl->AddRect(
                ImVec2(pos.x - 22, pos.y - 14),
                ImVec2(pos.x + ts.x + 22, pos.y + ts.y + 14),
                ImGui::ColorConvertFloat4ToU32(modalAccent()),
                8.0f, ImDrawFlags_None, 2.0f);
            dl->AddText(pos, IM_COL32(255, 255, 255, 255), msg);
        }

        // Phase 3: finalize the view-state baseline at the end of the first
        // rendered frame, AFTER the panels have rendered and the first-load
        // autoscale has written the final zoom ranges. Latch compares from the
        // next frame.
#if FTS_BUILD_HDF5
        if (appState.hasWorkspace() && appState.active->viewStateBaselinePending) {
            appState.active->viewStateBaseline = viewStateJson(appState);
            appState.active->viewStateBaselinePending = false;
            // Pristine open: the first frame's auto-computes (spectrum mirror
            // in wsMirrorSpectrum) are re-baselined along with the view state
            // — opening a file is not "unsaved changes" by itself.
            if (appState.active->workspaceDirtyRebaselinePending) {
                appState.active->workspaceDirtyRebaselinePending = false;
                appState.active->workspace.dirty = false;
                appState.active->workspace.changeLog.clear();
            }
        }
#endif
}

void AppLoop::present() {
        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window_, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        
        // Use a black background color to match the welcome screen
        // This creates a consistent dark theme throughout the application
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        
        glfwSwapBuffers(window_);

        // Execute deferred export after the frame is visible on screen
        // (exportPanel lives in the active session — null while no tab exists).
        if (appState.active && appState.active->exportPanel.exportPending) {
            appState.active->exportPanel.executePendingExport();
            appState.needsRedraw = true;
        }
        
        // Force redraw every frame while welcome screen is active (pattern persistence)
        if (appState.showWelcomeScreen && !appState.welcomeScreenInitialized) {
            appState.needsRedraw = true;
        }
        
        // Reset keyboard navigation flag after rendering
        if (appState.active) appState.active->keyboardNavigation = false;
}
