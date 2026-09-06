// Metadata panel (extracted from main.cpp, Phase-1 M1.2c).
#include "panels.h"
#include "app_state.h"
#include "workspace_reader.h"
#include <imgui.h>
#include <fstream>
#include <string>

static std::string configValueText(const nlohmann::json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "null";
    return v.dump();
}

static void renderConfigRows(const nlohmann::json& obj) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const std::string& key = it.key();
        const nlohmann::json& v = it.value();
        ImGui::PushID(key.c_str());
        if (v.is_object()) {
            if (ImGui::TreeNode(key.c_str())) {
                renderConfigRows(v);
                ImGui::TreePop();
            }
        } else if (v.is_array()) {
            if (ImGui::TreeNode(key.c_str())) {
                int idx = 0;
                for (const auto& el : v) {
                    ImGui::PushID(idx++);
                    if (el.is_object()) renderConfigRows(el);
                    else ImGui::Text("%s", configValueText(el).c_str());
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        } else {
            ImGui::Text("%s", key.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled(": %s", configValueText(v).c_str());
        }
        ImGui::PopID();
    }
}

static void renderMeasurementConfig(const nlohmann::json& cfg) {
    static const char* kOrder[] = {"instrument", "detector", "acquisitionCard",
                                   "acquisition", "laser", "environment", "legacy"};
    for (const char* key : kOrder) {
        auto it = cfg.find(key);
        if (it == cfg.end()) continue;
        ImGui::PushID(key);
        if (ImGui::TreeNode(key)) {
            renderConfigRows(*it);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    for (auto it = cfg.begin(); it != cfg.end(); ++it) {
        const std::string& key = it.key();
        bool known = false;
        for (const char* k : kOrder)
            if (key == k) { known = true; break; }
        if (known) continue;
        ImGui::PushID(key.c_str());
        if (ImGui::TreeNode(key.c_str())) {
            renderConfigRows(it.value());
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}
void renderMetadataPanel() {

        ImGui::Begin("Metadata");
        ImGui::PushTextWrapPos(); // Enable text wrapping
        if (appState.active->dataLoaded) {
            ImGui::Text("File: %s", appState.active->csvFiles.empty() ? "None" : appState.active->csvFiles[0].c_str());
            ImGui::Text("Samples: %zu", appState.active->loadedData.empty() ? 0 : appState.active->loadedData[0].dataSize());
            const char* dataTypeName = appState.active->datasetInfo.hasPrecomputedSpectra
                ? "Precomputed spectra"
                : (appState.active->datasetInfo.axisIsCorrected ? "Corrected single IFG"
                                                        : "Uncorrected dual IFG");
            ImGui::Text("Data type: %s", dataTypeName);
            
            // Display comments if comments.txt exists (WUST format)
#if FTS_BUILD_HDF5
            if (appState.hasWorkspace()) {
                ImGui::Separator();

                // Editable comment (multi-line) + tags (single line). Each
                // change mirrors into the workspace and marks it dirty.
                ImGui::Text("Comment:");
                // Stretch the comment box to roughly half the panel's usable
                // height so the metadata area makes better use of docked
                // space (tags + config below stay reachable; the panel scrolls
                // if the config tree is expanded).
                float commentHeight = ImGui::GetContentRegionAvail().y * 0.5f;
                float minCommentH = 3.0f * ImGui::GetTextLineHeightWithSpacing();
                if (commentHeight < minCommentH) commentHeight = minCommentH;
                if (ImGui::InputTextMultiline("##metadataComment",
                        appState.active->metadataCommentBuffer,
                        sizeof(appState.active->metadataCommentBuffer),
                        ImVec2(-FLT_MIN, commentHeight))) {
                    appState.active->workspace.measurementComment = appState.active->metadataCommentBuffer;
                    appState.active->workspace.dirty = true;
                    logWorkspaceChange(appState.active->workspace, "Edited comment");
                    appState.needsRedraw = true;
                }
                ImGui::Text("Tags:");
                if (ImGui::InputText("##metadataTags", appState.active->metadataTagsBuffer,
                                     sizeof(appState.active->metadataTagsBuffer))) {
                    appState.active->workspace.tags = appState.active->metadataTagsBuffer;
                    appState.active->workspace.dirty = true;
                    logWorkspaceChange(appState.active->workspace, "Edited tags");
                    appState.needsRedraw = true;
                }

                if (!appState.active->workspace.created.empty())
                    ImGui::TextWrapped("@created: %s", appState.active->workspace.created.c_str());
                if (!appState.active->workspace.format.empty())
                    ImGui::TextWrapped("@format: %s", appState.active->workspace.format.c_str());
                if (!appState.active->workspace.measurementConfig.empty()) {
                    if (ImGui::TreeNode("Measurement Config")) {
                        renderMeasurementConfig(appState.active->workspace.measurementConfig);
                        ImGui::TreePop();
                    }
                }
            } else
#endif
            if (appState.active->datasetInfo.hasMetadataFile) {
                ImGui::Separator();
                ImGui::Text("Comments:");
                
                std::string commentsPath = appState.active->currentDirectory;
                size_t last_slash = commentsPath.find_last_of("/\\");
                if (last_slash != std::string::npos) {
                    commentsPath = commentsPath.substr(0, last_slash); // Go up to parent directory
                }
                commentsPath += "/comments.txt";
                
                std::ifstream commentsFile(commentsPath);
                if (commentsFile.is_open()) {
                    std::string line;
                    while (std::getline(commentsFile, line)) {
                        ImGui::TextWrapped("%s", line.c_str());
                    }
                    commentsFile.close();
                } else {
                    ImGui::Text("<Comments Empty>");
                }
            } else {
                ImGui::Separator();
                if (!appState.active->loadedData.empty() && !appState.active->loadedData[0].metadata.empty()) {
                    ImGui::TextWrapped("%s", appState.active->loadedData[0].metadata.c_str());
                } else {
                    ImGui::Text("-no data-");
                }
            }
        } else {
            ImGui::Text("No metadata available.");
        }
        ImGui::PopTextWrapPos(); // Disable text wrapping
        ImGui::End();

}
