// Files panel + workspace member-deletion engine (Phase-1 M1.2c).
#include "panels.h"
#include "app_state.h"
#include "popup_utils.h"
#include "workspace_reader.h"
#include "hdf/workspace.h"
#include "spectral_toolbox.h"
#include <imgui.h>
#include <algorithm>
#include <filesystem>
#include <iostream>

static bool workspaceMemberIsOriginal(const Workspace& ws, const std::string& path) {
    auto slash = path.find('/', 1);
    if (path.empty() || path[0] != '/' || slash == std::string::npos) return false;
    std::string group = path.substr(1, slash - 1);
    std::string id = path.substr(slash + 1);
    auto isOrig = [&](const auto& members) {
        for (const auto& m : members)
            if (m.id == id) return m.kind == MemberKind::Original;
        return false;
    };
    if (group == "igm_uncorrected_x") return isOrig(ws.uncorrectedIfg.members);
    if (group == "igm_corrected_x")   return isOrig(ws.correctedIfg.members);
    if (group == "spectra")           return isOrig(ws.spectra.members);
    return false;
}

static void workspaceEraseMember(Workspace& ws, const std::string& path) {
    auto slash = path.find('/', 1);
    if (path.empty() || path[0] != '/' || slash == std::string::npos) return;
    std::string group = path.substr(1, slash - 1);
    std::string id = path.substr(slash + 1);
    auto eraseById = [&](auto& members) {
        members.erase(std::remove_if(members.begin(), members.end(),
            [&](const auto& m) { return m.id == id; }), members.end());
    };
    if (group == "igm_uncorrected_x") eraseById(ws.uncorrectedIfg.members);
    else if (group == "igm_corrected_x") eraseById(ws.correctedIfg.members);
    else if (group == "spectra") eraseById(ws.spectra.members);
    else if (group == "average_spectra") eraseById(ws.averageSpectra.members);
    else if (group == "snr_spectra") eraseById(ws.snrSpectra.members);
    else if (group == "allan_werle") eraseById(ws.allanWerle.members);
    else if (group == "t100") eraseById(ws.t100.members);
}

static void removeFileFromEngine(AppState& s, const std::string& id) {
    s.csvFiles.erase(std::remove(s.csvFiles.begin(), s.csvFiles.end(), id), s.csvFiles.end());
    auto sortedIt = std::find(s.sortedFiles.begin(), s.sortedFiles.end(), id);
    size_t sortedIdx = (sortedIt != s.sortedFiles.end())
        ? static_cast<size_t>(std::distance(s.sortedFiles.begin(), sortedIt)) : s.sortedFiles.size();
    if (sortedIt != s.sortedFiles.end()) s.sortedFiles.erase(sortedIt);
    if (sortedIdx < s.filesSelectedForAveraging.size())
        s.filesSelectedForAveraging.erase(s.filesSelectedForAveraging.begin() + sortedIdx);
    for (size_t i = 0; i < s.selectedFiles.size();) {
        if (s.selectedFiles[i] == id) {
            s.selectedFiles.erase(s.selectedFiles.begin() + i);
            if (i < s.selectedFilenames.size()) s.selectedFilenames.erase(s.selectedFilenames.begin() + i);
            if (i < s.loadedData.size()) s.loadedData.erase(s.loadedData.begin() + i);
            if (i < s.rawDataCache.size()) s.rawDataCache.erase(s.rawDataCache.begin() + i);
        } else {
            ++i;
        }
    }
    // Keep the navigation index and the data-loaded flag consistent with the
    // shrunk lists (mirrors the legacy performFileDeletion clamp): a stale
    // index would OOB-index sortedFiles, and dataLoaded=true with an empty
    // loadedData would OOB-index loadedData[0] in the frame loop.
    if (s.currentSortedFileIndex >= s.sortedFiles.size())
        s.currentSortedFileIndex = s.sortedFiles.empty() ? 0 : s.sortedFiles.size() - 1;
    s.dataLoaded = !s.loadedData.empty();
    s.spectrum.cachedSpectra.erase(id);
    s.spectrum.cachedFrequencies.erase(id);
    s.spectrum.lastPrimaryDetectors.erase(id);
    s.spectrum.lastSpectrumParams.erase(id);
    s.hilbertXCache.erase(id);
    s.peakPositionsCache.erase(id);
    s.t100.cachedTransX.erase(id);
    s.t100.cachedTransY.erase(id);
}

void performWorkspaceMemberDeletion(AppState& s, const std::string& absPath) {
    const std::string id = absPath.substr(absPath.find_last_of('/') + 1);
    const bool isOriginal = workspaceMemberIsOriginal(s.workspace, absPath);

    // 1. Remove the member from its group.
    workspaceEraseMember(s.workspace, absPath);
    if (isOriginal)
        s.workspace.deletedOriginalPaths.push_back(absPath);

    // 2. Cascade: downstream derivatives (and their dependents) go stale.
    markDependentsStale(s.workspace, absPath);

    // 3. Clear engine state referencing the removed member.
    removeFileFromEngine(s, id);

    // 4. Original removed from the active group → re-derive datasetInfo +
    //    csvFiles (active-group priority may shift) and clear panel caches.
    if (isOriginal) {
        s.datasetInfo = workspaceDatasetInfo(s.workspace);
        s.csvFiles = workspaceFileList(s.workspace);
        clearPanelCaches(s);
        s.filesChanged = true;
    }

    s.workspace.dirty = true;
    logWorkspaceChange(s.workspace, "Deleted: " + absPath);
    s.needsRedraw = true;
}

static void stripWorkspaceDerivatives(AppState& s) {
    auto clearDerivs = [](auto& members) {
        members.erase(std::remove_if(members.begin(), members.end(),
            [](const auto& m) { return m.kind == MemberKind::Derivative; }),
            members.end());
    };
    clearDerivs(s.workspace.uncorrectedIfg.members);
    clearDerivs(s.workspace.correctedIfg.members);
    clearDerivs(s.workspace.spectra.members);
    s.workspace.averageSpectra.members.clear();
    s.workspace.snrSpectra.members.clear();
    s.workspace.allanWerle.members.clear();
    s.workspace.t100.members.clear();
    clearPanelDerivedResults(s);
    s.workspace.dirty = true;
    logWorkspaceChange(s.workspace, "Removed all derivative results");
    s.needsRedraw = true;
}

void performFileDeletion(AppState& appState, size_t index) {
#if FTS_BUILD_HDF5
    // Defensive guard: in workspace mode sortedFiles holds member IDs, not
    // disk paths — the filesystem remove below would either fail or delete an
    // unrelated file named like the member from the CWD. Route to the
    // workspace-aware deletion (cascade + engine cleanup). Callers already
    // route around this, but the function must be safe on its own.
    if (appState.hasWorkspace()) {
        if (index < appState.sortedFiles.size()) {
            std::string path = memberPathOf(appState.workspace, appState.sortedFiles[index]);
            if (!path.empty())
                performWorkspaceMemberDeletion(appState, path);
        }
        return;
    }
#endif
    const auto& file = appState.sortedFiles[index];

    std::error_code ec;
    bool removed = std::filesystem::remove(file, ec);
    if (!removed || ec) {
        std::cerr << "Failed to delete file: " << file << " (" << ec.message() << ")" << std::endl;
        return;
    }

    std::cout << "Deleted file: " << file << std::endl;

    // Remove from csvFiles
    auto csvIt = std::find(appState.csvFiles.begin(), appState.csvFiles.end(), file);
    if (csvIt != appState.csvFiles.end())
        appState.csvFiles.erase(csvIt);

    // Remove from sortedFiles at index
    appState.sortedFiles.erase(appState.sortedFiles.begin() + index);

    // Remove from filesSelectedForAveraging
    if (index < appState.filesSelectedForAveraging.size())
        appState.filesSelectedForAveraging.erase(appState.filesSelectedForAveraging.begin() + index);

    // If the file was in selectedFiles, remove it there too
    auto selIt = std::find(appState.selectedFiles.begin(), appState.selectedFiles.end(), file);
    if (selIt != appState.selectedFiles.end()) {
        size_t selIdx = std::distance(appState.selectedFiles.begin(), selIt);
        appState.selectedFiles.erase(appState.selectedFiles.begin() + selIdx);
        appState.selectedFilenames.erase(appState.selectedFilenames.begin() + selIdx);
        appState.loadedData.erase(appState.loadedData.begin() + selIdx);
        appState.rawDataCache.erase(appState.rawDataCache.begin() + selIdx);
    }

    // Adjust currentSortedFileIndex: jump to previous file when deleting current
    if (index < appState.currentSortedFileIndex) {
        appState.currentSortedFileIndex--;
    } else if (index == appState.currentSortedFileIndex) {
        if (appState.currentSortedFileIndex > 0)
            appState.currentSortedFileIndex--;
        appState.filesChanged = true; // trigger reload from new position
    }
    if (appState.currentSortedFileIndex >= appState.sortedFiles.size())
        appState.currentSortedFileIndex = appState.sortedFiles.empty() ? 0 : appState.sortedFiles.size() - 1;

    if (appState.loadedData.empty())
        appState.dataLoaded = false;

    appState.needsRedraw = true;
}

void renderFilesPanel() {

        ImGui::Begin("Files");
        // Open delete confirmation popup if pending (called within frame context)
        if (appState.showDeleteConfirmPopup) {
            ImGui::OpenPopup("Delete File##confirm");
        }
        ImGui::PushTextWrapPos(); // Enable text wrapping
        ImGui::Text("Current Dataset: %s", appState.currentDatasetName.c_str());
        ImGui::Separator();
        ImGui::Text("Select:");
        ImGui::SameLine();
        if (ImGui::Button("All##FilesAll")) {
            for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
                appState.filesSelectedForAveraging[i] = true;
            appState.needsRedraw = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("None##FilesNone")) {
            for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
                appState.filesSelectedForAveraging[i] = false;
            appState.needsRedraw = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("10")) {
            for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
                appState.filesSelectedForAveraging[i] = (i < 10);
            appState.needsRedraw = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("50%")) {
            size_t half = appState.filesSelectedForAveraging.size() / 2;
            for (size_t i = 0; i < appState.filesSelectedForAveraging.size(); i++)
                appState.filesSelectedForAveraging[i] = (i < half);
            appState.needsRedraw = true;
            }

            ImGui::Separator();
#if FTS_BUILD_HDF5
        // In workspace mode the entries are member IDs, not disk paths.
        if (appState.hasWorkspace()) {
            if (ImGui::Button("Strip derivatives", ImVec2(-FLT_MIN, 0))) {
                stripWorkspaceDerivatives(appState);
            }
            ImGui::Separator();
        }
#endif
        ImGui::BeginChild("##FileList", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_AlwaysVerticalScrollbar);

        // Use the pre-sorted files list
        size_t currentSortedIndex = appState.currentSortedFileIndex;

        // Only calculate scroll position when using keyboard navigation
        if (appState.keyboardNavigation) {
            if (currentSortedIndex > 0 && ImGui::GetScrollY() + ImGui::GetWindowHeight() < (currentSortedIndex + 1) * ImGui::GetTextLineHeightWithSpacing()) {
                ImGui::SetScrollY((currentSortedIndex + 1) * ImGui::GetTextLineHeightWithSpacing() - ImGui::GetWindowHeight());
            } else if (currentSortedIndex == 0) {
                ImGui::SetScrollY(0);
            }
        }
        
        for (size_t i = 0; i < appState.sortedFiles.size(); ) {
            const auto& file = appState.sortedFiles[i];
            // Extract just the filename without path
            std::string filename = file;
            size_t last_slash = filename.find_last_of("/\\");
            if (last_slash != std::string::npos) {
                filename = filename.substr(last_slash + 1);
            }
            std::string fullFilename = filename;
            filename = shortenFilename(filename);
            
            // Delete button (left) — unique label per row avoids needing PushID.
            // Workspace mode: member delete (always confirms, decision 1).
            // Legacy mode: file delete from disk (confirm, or skip-flag).
            float btnH = ImGui::GetFrameHeight();
#if FTS_BUILD_HDF5
            if (appState.hasWorkspace()) {
                std::string delBtnId = "×##del" + std::to_string(i);
                if (ImGui::Button(delBtnId.c_str(), ImVec2(btnH, btnH))) {
                    appState.pendingWorkspaceDeletionPath = memberPathOf(appState.workspace, file);
                    if (!appState.pendingWorkspaceDeletionPath.empty()) {
                        appState.showWorkspaceDeleteConfirmPopup = true;
                        appState.needsRedraw = true;
                    }
                }
                ImGui::SameLine();
            } else
#endif
            {
                std::string delBtnId = "×##del" + std::to_string(i);
                if (ImGui::Button(delBtnId.c_str(), ImVec2(btnH, btnH))) {
                    if (appState.skipDeleteConfirm) {
                        performFileDeletion(appState, i);
                        continue;
                    } else {
                        appState.deleteConfirmIndex = i;
                        appState.showDeleteConfirmPopup = true;
                    }
                }
                ImGui::SameLine();
            }
            
            ImGui::PushID(static_cast<int>(i));
            
            // Enhanced highlighting for the currently selected file
            int stylesPushed = 1; // Default: push 1 style
            bool isFileSelected = (std::find(appState.selectedFiles.begin(), appState.selectedFiles.end(), file) != appState.selectedFiles.end());
            
            if (isFileSelected) {
                // Find the index of this file in the selectedFiles vector to determine its color
                auto it = std::find(appState.selectedFiles.begin(), appState.selectedFiles.end(), file);
                size_t fileIndex = std::distance(appState.selectedFiles.begin(), it);
                
                // Get the color matching the plot curve color
                ImVec4 buttonColor;
                ImVec4 hoverColor;
                
                if (fileIndex == 0) {
                    buttonColor = ImVec4(0.6f, 0.5f, 0.1f, 0.8f); // Dark yellow - matches plot
                    hoverColor = ImVec4(0.7f, 0.6f, 0.2f, 0.9f); // Lighter yellow on hover
                } else if (fileIndex == 1) {
                    buttonColor = ImVec4(0.75f, 0.05f, 0.05f, 0.8f); // #C00E0E - Red
                    hoverColor = ImVec4(0.85f, 0.15f, 0.15f, 0.9f); // Lighter red on hover
                } else if (fileIndex == 2) {
                    buttonColor = ImVec4(0.15f, 0.45f, 0.28f, 0.8f); // #257448 - Green
                    hoverColor = ImVec4(0.25f, 0.55f, 0.38f, 0.9f); // Lighter green on hover
                } else if (fileIndex == 3) {
                    buttonColor = ImVec4(0.07f, 0.29f, 0.59f, 0.8f); // #114A97 - Blue
                    hoverColor = ImVec4(0.17f, 0.39f, 0.69f, 0.9f); // Lighter blue on hover
                } else if (fileIndex == 4) {
                    buttonColor = ImVec4(0.5f, 0.5f, 0.5f, 0.8f); // Grey
                    hoverColor = ImVec4(0.6f, 0.6f, 0.6f, 0.9f); // Lighter grey on hover
                }
                
                ImGui::PushStyleColor(ImGuiCol_Button, buttonColor);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hoverColor);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f)); // White text
                stylesPushed = 3; // Selected: push 3 styles
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 0.5f)); // Default button color
            }
            
            // Compute widths: delete button already placed, then filename, then checkbox
            float chkWidth = ImGui::GetFrameHeight();
            float btnWidth = ImGui::GetContentRegionAvail().x - chkWidth - ImGui::GetStyle().ItemSpacing.x;

            // "Show timestamps" ribbon: append the member's hh:mm:ss to the row
            // label (original records only — memberTimestampHMS returns "" for
            // derivatives / empty timestamps). Rows are under PushID(i), so the
            // label text change never shifts widget IDs (IMGUI_GUIDE §3).
            std::string rowLabel = filename;
#if FTS_BUILD_HDF5
            if (appState.hasWorkspace() && appState.showTimestamps) {
                std::string ts = memberTimestampHMS(appState.workspace, file);
                if (!ts.empty()) rowLabel += " [" + ts + "]";
            }
#endif
            if (ImGui::Button(rowLabel.c_str(), ImVec2(btnWidth, 0))) {
                // Handle multi-select with Ctrl key
                if (appState.multiSelectMode) {
                    // Toggle selection for this file
                    std::string fullPath = appState.sortedFiles[i];
                    auto it = std::find(appState.selectedFiles.begin(), appState.selectedFiles.end(), fullPath);
                    if (it != appState.selectedFiles.end()) {
                        // File already selected, remove it
                        size_t index = std::distance(appState.selectedFiles.begin(), it);
                        appState.selectedFiles.erase(appState.selectedFiles.begin() + index);
                        appState.selectedFilenames.erase(appState.selectedFilenames.begin() + index);
                        appState.loadedData.erase(appState.loadedData.begin() + index);
                        appState.rawDataCache.erase(appState.rawDataCache.begin() + index); // Also remove from raw data cache
                    } else {
                        // Check if we would exceed the limit
                        if (appState.selectedFiles.size() < appState.MAX_SELECTABLE_FILES) {
                            try {
                                InterferogramData data = loadInterferogram(appState, fullPath);
        
                                
                                // Store raw data in cache before downsampling
                                InterferogramData rawData = data;
                                
                                // Apply downsampling to multi-selected files too
                                if (appState.enableDownsampling && data.referenceDetector.size() > appState.maxPointsBeforeDownsampling) {
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
                                
                                appState.loadedData.push_back(data);
                                appState.rawDataCache.push_back(rawData); // Store raw data for spectrum computation
                                appState.selectedFiles.push_back(fullPath);
                                std::string idName = fullPath;
                                size_t idSlash = idName.find_last_of("/\\");
                                if (idSlash != std::string::npos) idName = idName.substr(idSlash + 1);
                                appState.selectedFilenames.push_back(idName);
                            } catch (const std::exception& e) {
                                std::cerr << "Error loading file: " << e.what() << std::endl;
                            }
                        } else {
                            ImGui::OpenPopup("Selection Limit");
                            appState.needsRedraw = true;
                        }
                    }
                } else if (appState.shiftSelectMode) {
                    // Handle Shift+Click for range selection
                    size_t startIndex = std::min(appState.lastSelectedIndex, i);
                    size_t endIndex = std::max(appState.lastSelectedIndex, i);
                    
                    // Clear current selection
                    appState.selectedFiles.clear();
                    appState.selectedFilenames.clear();
                    appState.loadedData.clear();
                    appState.rawDataCache.clear(); // Clear raw data cache too
                    
                    // Add all files in the range, respecting the 5-file limit
                    size_t filesToAdd = 0;
                    for (size_t j = startIndex; j <= endIndex; j++) {
                        if (filesToAdd >= appState.MAX_SELECTABLE_FILES) break;
                        
                        try {
                            std::string fullPath = appState.sortedFiles[j];
                            InterferogramData data = loadInterferogram(appState, fullPath);
                            
                            // Store raw data in cache before downsampling
                            InterferogramData rawData = data;
                            
                            // Apply downsampling
                            if (appState.enableDownsampling && data.referenceDetector.size() > appState.maxPointsBeforeDownsampling) {
                                size_t localDownsampleFactor = data.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
                                std::vector<double> downsampledRef, downsampledPrim;
                                for (size_t k = 0; k < data.referenceDetector.size(); k += localDownsampleFactor) {
                                    downsampledRef.push_back(data.referenceDetector[k]);
                                    downsampledPrim.push_back(data.primaryDetector[k]);
                                }
                                data.referenceDetector = downsampledRef;
                                data.primaryDetector = downsampledPrim;
                            }
                            
                            appState.loadedData.push_back(data);
                            appState.rawDataCache.push_back(rawData); // Store raw data for spectrum computation
                            appState.selectedFiles.push_back(fullPath);
                            
                            // Extract filename for legend
                            std::string filename = appState.sortedFiles[j];
                            size_t last_slash = filename.find_last_of("/\\");
                            if (last_slash != std::string::npos) {
                                filename = filename.substr(last_slash + 1);
                            }
                            appState.selectedFilenames.push_back(filename);
                            filesToAdd++;
                        } catch (const std::exception& e) {
                            std::cerr << "Error loading file: " << e.what() << std::endl;
                        }
                    }
                    
                    // Update last selected index
                    appState.lastSelectedIndex = i;
                    appState.dataLoaded = !appState.selectedFiles.empty();
                } else {
                    // Single selection - replace current selection
                    appState.currentSortedFileIndex = i;
                    appState.filesChanged = true;
                    // Update last selected index for future Shift+Click
                    appState.lastSelectedIndex = i;
                }
            }
            
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", fullFilename.c_str());
            
            // Pop the correct number of styles
            ImGui::PopStyleColor(stylesPushed);
            
            // Averaging checkbox (right-aligned)
            ImGui::SameLine();
            if (i < appState.filesSelectedForAveraging.size()) {
                bool chk = appState.filesSelectedForAveraging[i];
                bool wasUnchecked = !chk;
                if (wasUnchecked) {
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));
                    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.5f, 0.5f, 0.5f, 0.6f));
                }
                ImGui::PushID(static_cast<int>(i + 100000));
                if (ImGui::Checkbox("##AvgSel", &chk)) {
                    appState.filesSelectedForAveraging[i] = chk;
                    appState.needsRedraw = true;
                }
                ImGui::PopID();
                if (wasUnchecked) {
                    ImGui::PopStyleColor(2);
                }
            }

            // Auto-scroll to keep selected item visible only when selection changes via keyboard
            if (i == appState.currentSortedFileIndex && appState.keyboardNavigation) {
                ImGui::SetScrollHereY(0.5f); // Scroll to center the selected item
            }

            ImGui::PopID();
            i++;
        }
        ImGui::EndChild();

#if FTS_BUILD_HDF5
        // Derived products section (workspace mode only): derivative members,
        // each deletable immediately (no confirm — recomputable, spec rule 7).
        if (appState.hasWorkspace()) {
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Derived products")) {
                bool any = false;
                auto listGroup = [&](const char* groupName, const auto& members) {
                    for (const auto& m : members) {
                        if (m.kind != MemberKind::Derivative) continue;
                        any = true;
                        std::string path = std::string("/") + groupName + "/" + m.id;
                        if (ImGui::Button(("×##delderv" + path).c_str(),
                                          ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                            performWorkspaceMemberDeletion(appState, path);
                            appState.needsRedraw = true;
                        }
                        ImGui::SameLine();
                        ImGui::Text("%s/%s", groupName, m.id.c_str());
                        if (m.stale) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("(stale)");
                        }
                    }
                };
                listGroup("spectra", appState.workspace.spectra.members);
                listGroup("average_spectra", appState.workspace.averageSpectra.members);
                listGroup("snr_spectra", appState.workspace.snrSpectra.members);
                listGroup("allan_werle", appState.workspace.allanWerle.members);
                listGroup("t100", appState.workspace.t100.members);
                if (!any) ImGui::TextDisabled("(none)");
            }
        }

        // Workspace member delete confirmation (decision 1: always confirm).
        {
            static int delFocus = 0;
            static bool prevWPopupOpen = false;
            if (!appState.showWorkspaceDeleteConfirmPopup) {
                prevWPopupOpen = false;
            }
            if (appState.showWorkspaceDeleteConfirmPopup) {
                ImGui::OpenPopup("Delete Member##confirm");
            }
            beginModal(480.0f, modalAccent());
            if (ImGui::BeginPopupModal("Delete Member##confirm", NULL,
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                const std::string& delPath = appState.pendingWorkspaceDeletionPath;
                size_t dependentCount = appState.workspace.dependentsOf(delPath).size();
                ImGui::Text("Delete member?");   // body restates the title (NoTitleBar)
                ImGui::Spacing();
                ImGui::TextWrapped("%s", delPath.c_str());
                ImGui::Spacing();
                if (dependentCount > 0)
                    ImGui::TextWrapped("Some derived results may be marked stale.");
                ImGui::Separator();
                ImGui::Spacing();

                int pressed = modalButtonRow({"Cancel", "Delete"},
                                             delFocus, prevWPopupOpen, modalAccent());
                if (pressed == 0 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    appState.showWorkspaceDeleteConfirmPopup = false;
                    appState.pendingWorkspaceDeletionPath.clear();
                    ImGui::CloseCurrentPopup();
                } else if (pressed == 1) {
                    if (!appState.pendingWorkspaceDeletionPath.empty())
                        performWorkspaceMemberDeletion(appState, appState.pendingWorkspaceDeletionPath);
                    appState.showWorkspaceDeleteConfirmPopup = false;
                    appState.pendingWorkspaceDeletionPath.clear();
                    ImGui::CloseCurrentPopup();
                }
                drawModalAccentFrame(modalAccent());
                ImGui::EndPopup();
                prevWPopupOpen = true;
            }
            endModal();
        }
#endif

        // Show selection limit popup if needed
        {
            static int selFocus = 0;
            static bool prevSelPopupOpen = false;
            if (!ImGui::IsPopupOpen("Selection Limit"))
                prevSelPopupOpen = false;
            beginModal(440.0f, modalAccent());
            if (ImGui::BeginPopupModal("Selection Limit", NULL,
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                ImGui::TextWrapped("Maximum of %zu files can be selected at once.",
                                   appState.MAX_SELECTABLE_FILES);
                ImGui::TextWrapped("Please deselect some files first.");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (modalButtonRow({"OK"}, selFocus, prevSelPopupOpen, modalAccent()) == 0 ||
                    ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    ImGui::CloseCurrentPopup();
                }
                drawModalAccentFrame(modalAccent());
                ImGui::EndPopup();
                prevSelPopupOpen = true;
            }
            endModal();
        }

        // Delete confirmation popup
        {
            static int focusIdx = 0;
            static bool prevPopupOpen = false;
            if (!appState.showDeleteConfirmPopup)
                prevPopupOpen = false;

            beginModal(480.0f, modalAccent());
            if (ImGui::BeginPopupModal("Delete File##confirm", NULL,
                                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                size_t idx = appState.deleteConfirmIndex;
                std::string fname = idx < appState.sortedFiles.size()
                    ? appState.sortedFiles[idx].substr(appState.sortedFiles[idx].find_last_of("/\\") + 1)
                    : "";

                ImGui::Text("Are you sure you want to delete?");   // body restates the title (NoTitleBar)
                ImGui::Spacing();
                ImGui::TextWrapped("%s", fname.c_str());
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                int pressed = modalButtonRow(
                    {"Cancel", "Yes", "Yes, don't ask again"},
                    focusIdx, prevPopupOpen, modalAccent());
                if (pressed == 0 || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    appState.showDeleteConfirmPopup = false;
                    ImGui::CloseCurrentPopup();
                } else if (pressed == 1) {
                    if (idx < appState.sortedFiles.size())
                        performFileDeletion(appState, idx);
                    appState.showDeleteConfirmPopup = false;
                    ImGui::CloseCurrentPopup();
                } else if (pressed == 2) {
                    appState.skipDeleteConfirm = true;
                    if (idx < appState.sortedFiles.size())
                        performFileDeletion(appState, idx);
                    appState.showDeleteConfirmPopup = false;
                    ImGui::CloseCurrentPopup();
                }

                drawModalAccentFrame(modalAccent());
                ImGui::EndPopup();
                prevPopupOpen = true;
            }
            endModal();
        }
        
        ImGui::PopTextWrapPos(); // Disable text wrapping
        ImGui::End();

}

// Single read path for all engine loads: the workspace.
InterferogramData loadInterferogram(AppState& s, const std::string& id) {
    return workspaceRead(s.workspace, id);
}
