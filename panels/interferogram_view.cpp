// Interferogram View + Interferogram config panel (Phase-1 M1.2c).
#include "panels.h"
#include "app_state.h"
#include "ui/window.h"
#include "spectral_toolbox.h"
#include "apodization.h"
#include "workspace_reader.h"
#include "hdf/workspace.h"
#include <imgui.h>
#include "implot.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <vector>

void renderInterferogramPanel() {
        bool isMainWindowFocused = false;
        ImGui::Begin("Interferogram View");
        isMainWindowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
        
        // Handle ESC key to reset zoom (only when main window is focused)
        if (isMainWindowFocused && appState.dataLoaded && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            // Reset X-axis zoom when ESC is pressed (only for main window)
            appState.zoomRange = {0, 0};
            appState.shouldAutoscale = true; // Always force redraw with full range when ESC is pressed
        }
        
        if (appState.dataLoaded && !appState.loadedData.empty()) {
            if (!appState.datasetInfo.hasInterferograms) {
                ImGui::Text("Interferograms not available for this data type.");
            } else {
            // Y-axis limits are now handled by the auto-fit toggle
            // When autoFitYAxis is true, ImPlot will auto-calculate Y-axis limits
            // When autoFitYAxis is false, we use the manually calculated limits
            
            // Determine zoom range
            size_t ref_start =  0;
            size_t ref_end =  appState.datasetInfo.hasReferenceChannel
                              ? appState.loadedData[0].referenceDetector.size()
                              : appState.loadedData[0].dataSize();
            size_t prim_start =  0;
            size_t prim_end =  appState.loadedData[0].primaryDetector.size();
            // Compute peak positions for X-axis alignment (from raw data for OPD accuracy)
            std::vector<size_t> peakPositions;
            if (appState.maxAtZero) {
                for (size_t i = 0; i < appState.loadedData.size(); i++) {
                    const auto& prim = (i < appState.rawDataCache.size() && !appState.rawDataCache[i].primaryDetector.empty())
                        ? appState.rawDataCache[i].primaryDetector
                        : appState.loadedData[i].primaryDetector;
                    auto peakIt = std::max_element(prim.begin(), prim.end());
                    peakPositions.push_back(static_cast<size_t>(std::distance(prim.begin(), peakIt)));
                }
            }
            // Map raw peak index to downsampled space for sample-mode X-axis shifts
            auto getDsPeak = [&](size_t i) -> size_t {
                if (!appState.enableDownsampling || i >= appState.rawDataCache.size()) return peakPositions[i];
                const auto& rawPrim = appState.rawDataCache[i].primaryDetector;
                if (rawPrim.empty()) return peakPositions[i];
                size_t rs = rawPrim.size();
                size_t ls = appState.loadedData[i].primaryDetector.size();
                if (rs <= ls || ls == 0) return peakPositions[i];
                return static_cast<size_t>(static_cast<double>(peakPositions[i]) * ls / rs + 0.5);
            };
            
            if (appState.loadedData.size() > 1) {
                ImGui::BeginGroup(); // Start horizontal group
                for (size_t i = 0; i < appState.loadedData.size(); i++) {
                    ImVec4 color;
                    // Assign same colors as used in plots
                    if (i == 0) {
                        color = ImVec4(0.6f, 0.5f, 0.1f, 1.0f); // Dark yellow - FIRST
                    } else if (i == 1) {
                        color = ImVec4(0.75f, 0.05f, 0.05f, 1.0f); // #C00E0E - Red
                    } else if (i == 2) {
                        color = ImVec4(0.15f, 0.45f, 0.28f, 1.0f); // #257448 - Green
                    } else if (i == 3) {
                        color = ImVec4(0.07f, 0.29f, 0.59f, 1.0f); // #114A97 - Blue
                    } else if (i == 4) {
                        color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); // Grey
                    }

                    std::string displayName = shortenFilename(appState.selectedFilenames[i]);

                    // Wrap to next line if this item won't fit on the current line
                    if (i > 0) {
                        float itemWidth = 12.0f + 2.0f * ImGui::GetStyle().ItemSpacing.x +
                                          ImGui::CalcTextSize(displayName.c_str()).x;
                        if (i < appState.loadedData.size() - 1)
                            itemWidth += ImGui::CalcTextSize("  ").x + ImGui::GetStyle().ItemSpacing.x;
                        // SameLine() would place the item after the previous item's end
                        float itemStartX = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x;
                        float rightEdge = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
                        if (itemStartX + itemWidth <= rightEdge)
                            ImGui::SameLine();
                    }

                    // Draw colored square patch
                    ImDrawList* draw_list = ImGui::GetWindowDrawList();
                    ImVec2 cursor_pos = ImGui::GetCursorScreenPos();
                    ImVec2 square_size(12, 12); // Size of the color square
                    
                    // Draw square patch
                    draw_list->AddRectFilled(cursor_pos, ImVec2(cursor_pos.x + square_size.x, cursor_pos.y + square_size.y), ImGui::ColorConvertFloat4ToU32(color));
                    draw_list->AddRect(cursor_pos, ImVec2(cursor_pos.x + square_size.x, cursor_pos.y + square_size.y), ImGui::ColorConvertFloat4ToU32(ImVec4(0.2f, 0.2f, 0.2f, 1.0f))); // Border
                    
                    // Move cursor forward and add text
                    ImGui::Dummy(square_size);
                    ImGui::SameLine();
                    std::string legendLabel = appState.selectedFilenames[i];
#if FTS_BUILD_HDF5
                    // "Show timestamps": append hh:mm:ss for original members.
                    if (appState.hasWorkspace() && appState.showTimestamps) {
                        std::string ts = memberTimestampHMS(appState.workspace, appState.selectedFiles[i]);
                        if (!ts.empty()) legendLabel += " [" + ts + "]";
                    }
#endif
                    ImGui::Text("%s", legendLabel.c_str());
                    if (i < appState.loadedData.size() - 1) {
                        ImGui::SameLine();
                        ImGui::Text("  "); // Add some spacing between items
                    }
                }
                ImGui::EndGroup(); // End horizontal group
                ImGui::Separator();
            }
            
            // Create ImPlot subplots - two vertically stacked plots with custom height ratio
            // Reference plot: 1 unit height, Primary plot: 2 units height (2x taller)
            const bool hasRef = appState.datasetInfo.hasReferenceChannel;
            float row_ratios[2] = {1.0f, 2.0f};
            float row_ratios1[1] = {1.0f};
            int numRows = hasRef ? 2 : 1;
            
            // Pre-allocate plot specs to avoid repeated construction in rendering loop
            std::vector<ImPlotSpec> plotSpecs;
            if (appState.dataLoaded) {
                plotSpecs.resize(appState.loadedData.size());
                for (size_t i = 0; i < appState.loadedData.size(); i++) {
                    plotSpecs[i].LineWeight = 2.0f;
                    
                    // Assign specific colors based on the requested scheme (yellow first)
                    if (i == 0) {
                        plotSpecs[i].LineColor = ImVec4(0.6f, 0.5f, 0.1f, 1.0f); // Dark yellow - FIRST
                    } else if (i == 1) {
                        plotSpecs[i].LineColor = ImVec4(0.75f, 0.05f, 0.05f, 1.0f); // #C00E0E - Red
                    } else if (i == 2) {
                        plotSpecs[i].LineColor = ImVec4(0.15f, 0.45f, 0.28f, 1.0f); // #257448 - Green
                    } else if (i == 3) {
                        plotSpecs[i].LineColor = ImVec4(0.07f, 0.29f, 0.59f, 1.0f); // #114A97 - Blue
                    } else if (i == 4) {
                        plotSpecs[i].LineColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); // Grey
                    }
                }
            }

            // Handle box selection zoom manually using ImGui mouse input
            // This completely bypasses ImPlot's input system
            ImVec2 mousePos = ImGui::GetMousePos();
            bool isOverPlot = ImGui::IsWindowHovered();
            appState.isMouseOverPlot = isOverPlot;

            // Handle X-range selection with Shift key - state management only (only when main window is focused)
            bool shiftPressed = ImGui::GetIO().KeyShift;
            if (isMainWindowFocused && isOverPlot && shiftPressed && !appState.isSelectingXRange) {
                // Start selection when Shift is pressed over plot
                appState.isSelectingXRange = true;
                // Reset selection positions
                appState.selectionStartX = 0.0;
                appState.selectionEndX = 0.0;
                std::cout << "DEBUG: Started X-range selection" << std::endl;
            } else if (!shiftPressed && appState.isSelectingXRange) {
                // End selection when Shift is released
                appState.isSelectingXRange = false;
                
                // Only finalize if we have valid selection
                if(appState.selectionStartX != appState.selectionEndX) {
                    appState.applyXRangeSelection = true;
                    
                    if(appState.selectionStartX > appState.selectionEndX)
                    {
                        // make sure start is always smaller
                        double dum = appState.selectionStartX;
                        appState.selectionStartX = appState.selectionEndX;
                        appState.selectionEndX = dum;
                    }
                    
                    std::cout << "DEBUG: Finalizing X-range selection: Start=" << appState.selectionStartX << ", End=" << appState.selectionEndX << std::endl;
                } else {
                    std::cout << "DEBUG: X-range selection cancelled (no valid range)" << std::endl;
                }
            }

            
            // Ensure X-axis cache is populated for OPD mode or peak-finding
            // (runs before subplots regardless of hasRef)
            if ((appState.xAxisBase == 1 || appState.xCorrectionMethod == 1) && appState.dataLoaded) {
                if (appState.datasetInfo.axisIsCorrected) {
                    for (size_t i = 0; i < appState.loadedData.size(); i++) {
                        const std::string& fileId = appState.selectedFilenames[i];
                        if (appState.hilbertXCache.find(fileId) == appState.hilbertXCache.end()) {
                            const auto& opd = appState.loadedData[i].opdAxis;
                            if (!opd.empty()) {
                                std::vector<double> hilbX(opd.size());
                                for (size_t j = 0; j < opd.size(); j++)
                                    hilbX[j] = opd[j] * 1e6;
                                appState.hilbertXCache[fileId] = std::move(hilbX);
                            }
                        }
                    }
                } else {
                    if (appState.hilbertCacheLaserWavelength != appState.spectrum.refLaserTextbox) {
                        appState.hilbertXCache.clear();
                        appState.peakPositionsCache.clear();
                        appState.hilbertCacheLaserWavelength = appState.spectrum.refLaserTextbox;
                    }
                    for (size_t i = 0; i < appState.loadedData.size(); i++) {
                        const std::string& fileId = appState.selectedFilenames[i];
                        if (appState.hilbertXCache.find(fileId) == appState.hilbertXCache.end()) {
                            std::vector<double> hilbX;
                            // Always use full-resolution raw data for computation
                            const auto& refDet = (i < appState.rawDataCache.size())
                                ? appState.rawDataCache[i].referenceDetector
                                : appState.loadedData[i].referenceDetector;
                            if (appState.xCorrectionMethod == 1) {
                                std::vector<size_t> peakIdxs;
                                SpectralToolbox::xAxisFromPeaks(
                                    refDet, appState.hilbertCacheLaserWavelength,
                                    appState.peakProminenceThreshold,
                                    hilbX, &peakIdxs);
                                if (!hilbX.empty()) {
                                    appState.peakPositionsCache[fileId] = std::move(peakIdxs);
                                }
                            } else {
                                SpectralToolbox::xAxisFromHilbert(refDet, appState.hilbertCacheLaserWavelength, hilbX);
                            }
                            if (!hilbX.empty()) {
                                // Axis from Hilbert/peaks is mirror displacement;
                                // double to round-trip OPD (matches processSpectrum).
                                for (double& v : hilbX) v *= 2.0;
                                appState.hilbertXCache[fileId] = std::move(hilbX);
                            }
                        }
                    }
                }
            }

            if (ImPlot::BeginSubplots("Detector Plots", numRows, 1, ImVec2(-1, -1), ImPlotSubplotFlags_NoTitle | ImPlotSubplotFlags_LinkAllX | ImPlotSubplotFlags_NoLegend, hasRef ? row_ratios : row_ratios1)) {

                if (hasRef) {
                // Reference detector plot (top)
                ImPlotFlags ref_flags = ImPlotFlags_NoTitle | ImPlotFlags_NoLegend;
                if (appState.dataLoaded && appState.loadedData[0].referenceDetector.size() > 50000) {
                    ref_flags |= ImPlotFlags_NoInputs; // Only disable inputs for large datasets
                }
                // Never show crosshairs
                {
                    ImVec4 refGridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
                    refGridCol.w *= appState.gridAlpha;
                    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, refGridCol);
                }
                if (ImPlot::BeginPlot("Reference", ImVec2(-1, -1), ref_flags)) {
                    // Set up axes with auto-fit flag for Y-axis when enabled
                    ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;
                    if (appState.autoFitYAxis) {
                        y_flags |= ImPlotAxisFlags_AutoFit;
                    }
                    const char* refXLabel = (appState.xAxisBase == 1) ? "OPD [\xC2\xB5m]" : "Sample num";
                    ImPlot::SetupAxes(refXLabel, "Voltage [V]", ImPlotAxisFlags_NoTickMarks, y_flags);
                    // Conditionally optimize grid rendering for large datasets
                    if (appState.dataLoaded && appState.loadedData[0].referenceDetector.size() > 50000) {
                        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(0.6f, 0.6f, 0.6f, 0.75f * appState.gridAlpha));
                        // Optimize by reducing grid line rendering overhead for large datasets
                    }

                    if (appState.shouldAutoscale || appState.forceXAutofit) {
                        // Set initial view to show all data when new data is loaded or when downsampling is toggled
                        if (appState.xAxisBase == 1 && appState.dataLoaded) {
                            double xMin = std::numeric_limits<double>::max();
                            double xMax = std::numeric_limits<double>::lowest();
                            for (size_t i = 0; i < appState.loadedData.size(); i++) {
                                const auto& hx = appState.hilbertXCache[appState.selectedFilenames[i]];
                                if (!hx.empty()) {
                                    double off = (appState.maxAtZero && i < peakPositions.size()) ? hx[peakPositions[i]] : 0.0;
                                    xMin = std::min(xMin, hx.front() - off);
                                    xMax = std::max(xMax, hx.back() - off);
                                }
                            }
                            if (xMin < xMax) {
                                if (!appState.autoFitYAxis) {
                                    ImPlot::SetupAxesLimits(xMin, xMax, appState.ref_y_min, appState.ref_y_max, ImPlotCond_Always);
                                } else {
                                    ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImPlotCond_Always);
                                }
                            }
                        } else {
                            if (appState.maxAtZero && !peakPositions.empty()) {
                                double xMin = std::numeric_limits<double>::max();
                                double xMax = std::numeric_limits<double>::lowest();
                                for (size_t i = 0; i < appState.loadedData.size(); i++) {
                                    double N = static_cast<double>(appState.loadedData[i].referenceDetector.size());
                                    double off = static_cast<double>(getDsPeak(i));
                                    xMin = std::min(xMin, -off);
                                    xMax = std::max(xMax, N - 1.0 - off);
                                }
                                if (xMin < xMax) {
                                    if (!appState.autoFitYAxis) {
                                        ImPlot::SetupAxesLimits(xMin, xMax, appState.ref_y_min, appState.ref_y_max, ImPlotCond_Always);
                                    } else {
                                        ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImPlotCond_Always);
                                    }
                                }
                            } else {
                                if (!appState.autoFitYAxis) {
                                    ImPlot::SetupAxesLimits(0, appState.loadedData[0].referenceDetector.size(), appState.ref_y_min, appState.ref_y_max, ImPlotCond_Always);
                                } else {
                                    ImPlot::SetupAxisLimits(ImAxis_X1, 0, appState.loadedData[0].referenceDetector.size(), ImPlotCond_Always);
                                }
                            }
                        }
                        // Reset the force flag after use
                        if (appState.forceXAutofit) {
                            appState.forceXAutofit = false;
                        }
                    }
                    // Apply X-range selection if finalized and flag is set
                    if (appState.applyXRangeSelection && appState.selectionStartX != appState.selectionEndX) {
                        ImPlot::SetupAxisLimits(ImAxis_X1, appState.selectionStartX, appState.selectionEndX, ImPlotCond_Always);
                        appState.applyXRangeSelection = false; // Reset flag after applying
                    }
                    {
                        double xMin = appState.last_x_min;
                        double xMax = appState.last_x_max;
                        if (xMin >= xMax && appState.dataLoaded) {
                            if (appState.xAxisBase == 1) {
                                const auto& hx = appState.hilbertXCache[appState.selectedFilenames[0]];
                                if (!hx.empty()) {
                                    double off = (appState.maxAtZero && !peakPositions.empty()) ? hx[peakPositions[0]] : 0.0;
                                    xMin = hx.front() - off;
                                    xMax = hx.back() - off;
                                } else {
                                    xMin = 0.0;
                                    xMax = static_cast<double>(appState.loadedData[0].referenceDetector.size());
                                }
                            } else if (appState.maxAtZero && !peakPositions.empty()) {
                                double N = static_cast<double>(appState.loadedData[0].referenceDetector.size());
                                double off = static_cast<double>(getDsPeak(0));
                                xMin = -off;
                                xMax = N - 1.0 - off;
                            } else {
                                xMin = 0.0;
                                xMax = static_cast<double>(appState.loadedData[0].referenceDetector.size());
                            }
                        }
                        float yMin = appState.last_ref_y_min;
                        float yMax = appState.last_ref_y_max;
                        if (yMin >= yMax) {
                            yMin = appState.ref_y_min;
                            yMax = appState.ref_y_max;
                        }
                        SetupAxisTicksLimited(ImAxis_X1, xMin, xMax);
                        SetupAxisTicksLimited(ImAxis_Y1, yMin, yMax);
                    }
                    // Plot all selected datasets with pre-allocated specs
                    if (appState.dataLoaded) {  // Only plot if data is loaded
                        size_t data_count = ref_end - ref_start;
                        if (data_count > 0 && ref_start < appState.loadedData[0].referenceDetector.size()) {
                            for (size_t i = 0; i < appState.loadedData.size(); i++) {
                                const auto& refData = appState.loadedData[i].referenceDetector;
                                if (ref_start < refData.size()) {
                                    size_t actual_count = std::min(data_count, refData.size() - ref_start);
                                    if (appState.xAxisBase == 1) {
                                        const auto& hilbX = appState.hilbertXCache[appState.selectedFilenames[i]];
                                        if (!hilbX.empty()) {
                                            // Map downsampled index to full-res OPD cache proportionally
                                            double ratio = static_cast<double>(hilbX.size()) / refData.size();
                                            auto mapX = [&](size_t j) -> double {
                                                size_t idx = static_cast<size_t>((ref_start + j) * ratio);
                                                if (idx >= hilbX.size()) idx = hilbX.size() - 1;
                                                return hilbX[idx];
                                            };
                                            if (appState.maxAtZero && !peakPositions.empty()) {
                                                std::vector<double> shiftedX(actual_count);
                                                double peakHilbX = hilbX[peakPositions[i]];
                                                for (size_t j = 0; j < actual_count; j++)
                                                    shiftedX[j] = mapX(j) - peakHilbX;
                                                ImPlot::PlotLine("", shiftedX.data(), &refData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                            } else {
                                                std::vector<double> mappedX(actual_count);
                                                for (size_t j = 0; j < actual_count; j++)
                                                    mappedX[j] = mapX(j);
                                                ImPlot::PlotLine("", mappedX.data(), &refData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                            }
                                        }
                                    } else if (appState.maxAtZero && !peakPositions.empty()) {
                                        std::vector<double> shiftedX(actual_count);
                                        int peak = static_cast<int>(getDsPeak(i));
                                        for (size_t j = 0; j < actual_count; j++)
                                            shiftedX[j] = static_cast<double>(static_cast<int>(ref_start + j) - peak);
                                        ImPlot::PlotLine("", shiftedX.data(), &refData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                    } else {
                                        ImPlot::PlotLine("", 
                                                       &refData[ref_start], 
                                                       actual_count, 1.0, 0.0, plotSpecs[i]);
                                    }
                                }
                            }
                        } else {
                            std::cout << "DEBUG: Invalid data range for plotting: start=" << ref_start << ", end=" << ref_end << ", size=" << appState.loadedData[0].referenceDetector.size() << std::endl;
                        }
                    }

                    // Peak markers overlay (peak-finding mode, sample-mode axis only)
                    if (appState.showPeakIndicators && appState.xCorrectionMethod == 1 && appState.xAxisBase == 0 && appState.dataLoaded) {
                        for (size_t i = 0; i < appState.loadedData.size(); i++) {
                            const std::string& fileId = appState.selectedFilenames[i];
                            auto pit = appState.peakPositionsCache.find(fileId);
                            if (pit == appState.peakPositionsCache.end() || pit->second.empty()) continue;
                            const auto& ref = (i < appState.rawDataCache.size() && !appState.rawDataCache[i].referenceDetector.empty())
                                ? appState.rawDataCache[i].referenceDetector
                                : appState.loadedData[i].referenceDetector;
                            double dsScale = 1.0;
                            if (appState.enableDownsampling && i < appState.rawDataCache.size() && !appState.rawDataCache[i].referenceDetector.empty()) {
                                size_t rawSize = appState.rawDataCache[i].referenceDetector.size();
                                size_t loadedSize = appState.loadedData[i].referenceDetector.size();
                                if (rawSize > loadedSize && loadedSize > 0)
                                    dsScale = static_cast<double>(loadedSize) / rawSize;
                            }
                            int xOffset = (appState.maxAtZero && i < peakPositions.size()) ? static_cast<int>(getDsPeak(i)) : 0;
                            std::vector<double> mx(pit->second.size());
                            std::vector<double> my(pit->second.size());
                            for (size_t j = 0; j < pit->second.size(); j++) {
                                size_t idx = pit->second[j];
                                mx[j] = static_cast<double>(idx) * dsScale - xOffset;
                                my[j] = ref[idx];
                            }
                            ImPlotSpec pkSpec;
                            pkSpec.Marker = ImPlotMarker_Circle;
                            pkSpec.MarkerSize = 4.0f;
                            pkSpec.MarkerFillColor = ImVec4(1,0,0,1);
                            ImPlot::PlotScatter("##PeakMarkers", mx.data(), my.data(), (int)mx.size(), pkSpec);
                        }
                    }
                    
                    // Handle X-range selection within plot context
                    if (appState.isSelectingXRange) {
                        // Get current mouse position in plot coordinates
                        ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
                        
                        // Initialize start position if not set
                        if (appState.selectionStartX == 0.0 && appState.selectionEndX == 0.0) {
                            appState.selectionStartX = mousePos.x;
                        }
                        appState.selectionEndX = mousePos.x;
                        
                        // Get current plot limits to draw vertical lines
                        double y_min = ImPlot::GetPlotLimits().Y.Min;
                        double y_max = ImPlot::GetPlotLimits().Y.Max;
                        
                        // Ensure proper ordering (left to right)
                        double selection_left = std::min(appState.selectionStartX, appState.selectionEndX);
                        double selection_right = std::max(appState.selectionStartX, appState.selectionEndX);
                        
                        // Create arrays for shaded region - need X array and two Y arrays (bottom and top)
                        double shade_x[2] = {selection_left, selection_right};
                        double shade_y1[2] = {y_min, y_min};  // Bottom edge
                        double shade_y2[2] = {y_max, y_max};  // Top edge
                        
                        // Create spec for dark purple translucent fill
                        ImPlotSpec fillSpec;
                        fillSpec.FillColor = ImVec4(0.5f, 0.0f, 0.5f, 0.3f); // Dark purple with 30% opacity
                        
                        // Draw translucent dark purple fill between selection lines
                        ImPlot::PlotShaded("##SelectionFill", shade_x, shade_y1, shade_y2, 2, fillSpec);
                        
                        // Create arrays for vertical line points
                        double start_x[2] = {appState.selectionStartX, appState.selectionStartX};
                        double start_y[2] = {y_min, y_max};
                        double end_x[2] = {appState.selectionEndX, appState.selectionEndX};
                        double end_y[2] = {y_min, y_max};
                        
                        // Draw vertical line at start position
                        ImPlot::PlotLine("##SelectionStart", start_x, start_y, 2);
                        
                        // Draw vertical line at end position
                                                
                        ImPlot::PlotLine("##SelectionEnd", end_x, end_y, 2);
                    }
                    
                    appState.last_ref_y_min = static_cast<float>(ImPlot::GetPlotLimits().Y.Min);
                    appState.last_ref_y_max = static_cast<float>(ImPlot::GetPlotLimits().Y.Max);
                    ImPlot::EndPlot();
                    if (appState.dataLoaded && appState.loadedData[0].referenceDetector.size() > 50000) {
                        ImPlot::PopStyleColor(); // Pop grid color only if we pushed it
                    }
                }
                ImPlot::PopStyleColor(); // Restore original grid color
                } // end of hasRef block

                
                // Primary detector plot (bottom)
                ImPlotFlags prim_flags = ImPlotFlags_NoTitle;
                if (appState.dataLoaded && appState.loadedData[0].primaryDetector.size() > 50000) {
                    prim_flags |= ImPlotFlags_NoInputs; // Only disable inputs for large datasets
                }

                // Never show crosshairs
                {
                    ImVec4 primGridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
                    primGridCol.w *= appState.gridAlpha;
                    ImPlot::PushStyleColor(ImPlotCol_AxisGrid, primGridCol);
                }
                if (ImPlot::BeginPlot("Primary", ImVec2(-1, -1), prim_flags)) {
                    // Set up axes with auto-fit flag for Y-axis when enabled
                    ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;
                    if (appState.autoFitYAxis) {
                        y_flags |= ImPlotAxisFlags_AutoFit;
                    }

                    const char* primXLabel = (appState.xAxisBase == 1) ? "OPD [\xC2\xB5m]" : "Sample num";
                    ImPlot::SetupAxes(primXLabel, "Voltage [V]", ImPlotAxisFlags_NoTickMarks, y_flags);

                    // Conditionally optimize grid rendering for large datasets
                    if (appState.dataLoaded && appState.loadedData[0].primaryDetector.size() > 50000) {
                        ImPlot::PushStyleColor(ImPlotCol_AxisGrid, ImVec4(0.6f, 0.6f, 0.6f, 0.75f * appState.gridAlpha));
                        // Optimize by reducing grid line rendering overhead for large datasets
                    }

                    // Only apply arrow key navigation when main window is focused
                    if (isMainWindowFocused) {
                        if(appState.leftArrowHandleFlag) {
                            float translationAmount = (appState.last_x_max - appState.last_x_min) / 10;
                            ImPlot::SetupAxisLimits(ImAxis_X1, appState.last_x_min - translationAmount, appState.last_x_max - translationAmount, ImPlotCond_Always);
                            appState.leftArrowHandleFlag = false;
                        } else if(appState.rightArrowHandleFlag) {
                            float translationAmount = (appState.last_x_max - appState.last_x_min) / 10;
                            ImPlot::SetupAxisLimits(ImAxis_X1, appState.last_x_min + translationAmount, appState.last_x_max + translationAmount, ImPlotCond_Always);
                            appState.rightArrowHandleFlag = false;
                        }
                    }

                    if (appState.shouldAutoscale || appState.forceXAutofit) {
                        // Set initial view to show all data when new data is loaded or when downsampling is toggled
                        if (appState.xAxisBase == 1 && appState.dataLoaded) {
                            double xMin = std::numeric_limits<double>::max();
                            double xMax = std::numeric_limits<double>::lowest();
                            for (size_t i = 0; i < appState.loadedData.size(); i++) {
                                const auto& hx = appState.hilbertXCache[appState.selectedFilenames[i]];
                                if (!hx.empty()) {
                                    double off = (appState.maxAtZero && i < peakPositions.size()) ? hx[peakPositions[i]] : 0.0;
                                    xMin = std::min(xMin, hx.front() - off);
                                    xMax = std::max(xMax, hx.back() - off);
                                }
                            }
                            if (xMin < xMax) {
                                if (!appState.autoFitYAxis) {
                                    ImPlot::SetupAxesLimits(xMin, xMax, appState.prim_y_min, appState.prim_y_max, ImPlotCond_Always);
                                } else {
                                    ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImPlotCond_Always);
                                }
                            }
                        } else {
                            if (appState.maxAtZero && !peakPositions.empty()) {
                                double xMin = std::numeric_limits<double>::max();
                                double xMax = std::numeric_limits<double>::lowest();
                                for (size_t i = 0; i < appState.loadedData.size(); i++) {
                                    double N = static_cast<double>(appState.loadedData[i].primaryDetector.size());
                                    double off = static_cast<double>(getDsPeak(i));
                                    xMin = std::min(xMin, -off);
                                    xMax = std::max(xMax, N - 1.0 - off);
                                }
                                if (xMin < xMax) {
                                    if (!appState.autoFitYAxis) {
                                        ImPlot::SetupAxesLimits(xMin, xMax, appState.prim_y_min, appState.prim_y_max, ImPlotCond_Always);
                                    } else {
                                        ImPlot::SetupAxisLimits(ImAxis_X1, xMin, xMax, ImPlotCond_Always);
                                    }
                                }
                            } else {
                                if (!appState.autoFitYAxis) {
                                    ImPlot::SetupAxesLimits(0, appState.loadedData[0].primaryDetector.size(), appState.prim_y_min, appState.prim_y_max, ImPlotCond_Always);
                                } else {
                                    ImPlot::SetupAxisLimits(ImAxis_X1, 0, appState.loadedData[0].primaryDetector.size(), ImPlotCond_Always);
                                }
                            }
                        }
                    }
                    // Apply X-range selection if finalized and flag is set
                    if (appState.applyXRangeSelection && appState.selectionStartX != appState.selectionEndX) {
                        ImPlot::SetupAxisLimits(ImAxis_X1, appState.selectionStartX, appState.selectionEndX, ImPlotCond_Always);
                        appState.applyXRangeSelection = false; // Reset flag after applying
                    }
                    {
                        double xMin = appState.last_x_min;
                        double xMax = appState.last_x_max;
                        if (xMin >= xMax && appState.dataLoaded) {
                            if (appState.xAxisBase == 1) {
                                const auto& hx = appState.hilbertXCache[appState.selectedFilenames[0]];
                                if (!hx.empty()) {
                                    double off = (appState.maxAtZero && !peakPositions.empty()) ? hx[peakPositions[0]] : 0.0;
                                    xMin = hx.front() - off;
                                    xMax = hx.back() - off;
                                } else {
                                    xMin = 0.0;
                                    xMax = static_cast<double>(appState.loadedData[0].primaryDetector.size());
                                }
                            } else if (appState.maxAtZero && !peakPositions.empty()) {
                                double N = static_cast<double>(appState.loadedData[0].primaryDetector.size());
                                double off = static_cast<double>(getDsPeak(0));
                                xMin = -off;
                                xMax = N - 1.0 - off;
                            } else {
                                xMin = 0.0;
                                xMax = static_cast<double>(appState.loadedData[0].primaryDetector.size());
                            }
                        }
                        float yMin = appState.last_prim_y_min;
                        float yMax = appState.last_prim_y_max;
                        if (yMin >= yMax) {
                            yMin = appState.prim_y_min;
                            yMax = appState.prim_y_max;
                        }
                        SetupAxisTicksLimited(ImAxis_X1, xMin, xMax);
                        SetupAxisTicksLimited(ImAxis_Y1, yMin, yMax);
                    }
                    // Reuse the same plot specs as reference plot (already pre-allocated)
                    // Plot all selected datasets with same colors as reference
                    if (appState.dataLoaded) {  // Only plot if data is loaded
                        size_t data_count = ref_end - ref_start;
                        if (data_count > 0 && ref_start < appState.loadedData[0].primaryDetector.size()) {
                            for (size_t i = 0; i < appState.loadedData.size(); i++) {
                                const auto& primData = appState.loadedData[i].primaryDetector;
                                if (ref_start < primData.size()) {
                                    size_t actual_count = std::min(data_count, primData.size() - ref_start);
                                    if (appState.xAxisBase == 1) {
                                        const auto& hilbX = appState.hilbertXCache[appState.selectedFilenames[i]];
                                        if (!hilbX.empty()) {
                                            // Map downsampled index to full-res OPD cache proportionally
                                            double ratio = static_cast<double>(hilbX.size()) / primData.size();
                                            auto mapX = [&](size_t j) -> double {
                                                size_t idx = static_cast<size_t>((ref_start + j) * ratio);
                                                if (idx >= hilbX.size()) idx = hilbX.size() - 1;
                                                return hilbX[idx];
                                            };
                                            if (appState.maxAtZero && !peakPositions.empty()) {
                                                std::vector<double> shiftedX(actual_count);
                                                double peakHilbX = hilbX[peakPositions[i]];
                                                for (size_t j = 0; j < actual_count; j++)
                                                    shiftedX[j] = mapX(j) - peakHilbX;
                                                ImPlot::PlotLine("", shiftedX.data(), &primData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                            } else {
                                                std::vector<double> mappedX(actual_count);
                                                for (size_t j = 0; j < actual_count; j++)
                                                    mappedX[j] = mapX(j);
                                                ImPlot::PlotLine("", mappedX.data(), &primData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                            }
                                        }
                                    } else if (appState.maxAtZero && !peakPositions.empty()) {
                                        std::vector<double> shiftedX(actual_count);
                                        int peak = static_cast<int>(getDsPeak(i));
                                        for (size_t j = 0; j < actual_count; j++)
                                            shiftedX[j] = static_cast<double>(static_cast<int>(ref_start + j) - peak);
                                        ImPlot::PlotLine("", shiftedX.data(), &primData[ref_start], static_cast<int>(actual_count), plotSpecs[i]);
                                    } else {
                                        ImPlot::PlotLine("", 
                                                       &primData[ref_start], 
                                                         actual_count, 1.0, 0.0, plotSpecs[i]);
                                    }
                                }
                            }
                        }
                        
                        // Draw apodization window overlay (spectrum view is always available)
                        if (appState.dataLoaded) {
                            const auto& primDataOverlay = appState.loadedData[0].primaryDetector;
                            if (!primDataOverlay.empty()) {
                                auto w = static_cast<ApodizationWindow>(appState.spectrum.apodizationSelector);
                                auto window = Apodization::createWindow(
                                    w, primDataOverlay.size(),
                                    std::max_element(primDataOverlay.begin(), primDataOverlay.end()) - primDataOverlay.begin(),
                                    appState.spectrum.apodizationParams);
                                double scale = *std::max_element(primDataOverlay.begin(), primDataOverlay.end());
                                if (scale > 0.0) {
                                    for (auto& v : window) v *= scale;
                                }
                                ImPlotSpec windowSpec;
                                windowSpec.LineColor = ImVec4(0.0f, 1.0f, 1.0f, 0.5f);
                                windowSpec.LineWeight = 2.0f;
                                if (appState.xAxisBase == 1 && !appState.selectedFilenames.empty()) {
                                    const auto& hilbX = appState.hilbertXCache[appState.selectedFilenames[0]];
                                    if (!hilbX.empty()) {
                                        double ratio = static_cast<double>(hilbX.size()) / primDataOverlay.size();
                                        std::vector<double> overlayX(window.size());
                                        if (appState.maxAtZero && !peakPositions.empty()) {
                                            double peakHilbX = hilbX[peakPositions[0]];
                                            for (size_t j = 0; j < window.size(); j++) {
                                                size_t idx = static_cast<size_t>(j * ratio);
                                                if (idx >= hilbX.size()) idx = hilbX.size() - 1;
                                                overlayX[j] = hilbX[idx] - peakHilbX;
                                            }
                                        } else {
                                            for (size_t j = 0; j < window.size(); j++) {
                                                size_t idx = static_cast<size_t>(j * ratio);
                                                if (idx >= hilbX.size()) idx = hilbX.size() - 1;
                                                overlayX[j] = hilbX[idx];
                                            }
                                        }
                                        ImPlot::PlotLine("##ApodWindow", overlayX.data(), window.data(), static_cast<int>(window.size()), windowSpec);
                                    }
                                } else if (appState.maxAtZero && !peakPositions.empty()) {
                                    std::vector<double> shiftedX(window.size());
                                    int peak = static_cast<int>(getDsPeak(0));
                                    for (size_t j = 0; j < window.size(); j++)
                                        shiftedX[j] = static_cast<double>(static_cast<int>(j) - peak);
                                    ImPlot::PlotLine("##ApodWindow", shiftedX.data(), window.data(), static_cast<int>(window.size()), windowSpec);
                                } else {
                                    ImPlot::PlotLine("##ApodWindow", window.data(), static_cast<int>(window.size()),
                                                     1.0, 0.0, windowSpec);
                                }
                            }
                        }
                    }

                    // Handle X-range selection within plot context
                    if (appState.isSelectingXRange) {
                        // Get current mouse position in plot coordinates
                        ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
                        
                        // Initialize start position if not set
                        if (appState.selectionStartX == 0.0 && appState.selectionEndX == 0.0) {
                            appState.selectionStartX = mousePos.x;
                        }
                        appState.selectionEndX = mousePos.x;
                        
                        // Get current plot limits to draw vertical lines
                        double y_min = ImPlot::GetPlotLimits().Y.Min;
                        double y_max = ImPlot::GetPlotLimits().Y.Max;
                        
                        // Ensure proper ordering (left to right)
                        double selection_left = std::min(appState.selectionStartX, appState.selectionEndX);
                        double selection_right = std::max(appState.selectionStartX, appState.selectionEndX);
                        
                        // Create arrays for shaded region - need X array and two Y arrays (bottom and top)
                        double shade_x[2] = {selection_left, selection_right};
                        double shade_y1[2] = {y_min, y_min};  // Bottom edge
                        double shade_y2[2] = {y_max, y_max};  // Top edge
                        
                        // Create spec for dark purple translucent fill
                        ImPlotSpec fillSpec;
                        fillSpec.FillColor = ImVec4(0.5f, 0.0f, 0.5f, 0.3f); // Dark purple with 30% opacity
                        
                        // Draw translucent dark purple fill between selection lines
                        ImPlot::PlotShaded("##SelectionFill", shade_x, shade_y1, shade_y2, 2, fillSpec);
                        
                        // Create arrays for vertical line points
                        double start_x[2] = {appState.selectionStartX, appState.selectionStartX};
                        double start_y[2] = {y_min, y_max};
                        double end_x[2] = {appState.selectionEndX, appState.selectionEndX};
                        double end_y[2] = {y_min, y_max};
                        
                        // Draw vertical line at start position
                        ImPlot::PlotLine("##SelectionStart", start_x, start_y, 2);
                        
                        // Draw vertical line at end position
                        ImPlot::PlotLine("##SelectionEnd", end_x, end_y, 2);
                    }
                    
                    // Add "LARGE DATA" indicator for large datasets (>50k points)
                    if (appState.dataLoaded && appState.loadedData[0].primaryDetector.size() > 50000) {
                        // Use ImPlot's annotation system for reliable positioning
                        // Position at top-right of plot with small offset
                        ImPlot::Annotation(ImPlot::GetPlotLimits().X.Max, ImPlot::GetPlotLimits().Y.Max, 
                                         ImVec4(1.0f, 1.0f, 1.0f, 1.0f), ImVec2(-10, 10), true, "LARGE DATA");
                    }
                    
                    appState.last_x_max = ImPlot::GetPlotLimits().X.Max;
                    appState.last_x_min = ImPlot::GetPlotLimits().X.Min;
                    appState.last_prim_y_min = static_cast<float>(ImPlot::GetPlotLimits().Y.Min);
                    appState.last_prim_y_max = static_cast<float>(ImPlot::GetPlotLimits().Y.Max);

                    ImPlot::EndPlot();
                    if (appState.dataLoaded && appState.loadedData[0].primaryDetector.size() > 50000) {
                        ImPlot::PopStyleColor(); // Pop grid color only if we pushed it
                    }
                }
                ImPlot::PopStyleColor(); // Restore original grid color
                
                // Reset autoscale flag after use
                if (appState.shouldAutoscale) {
                    appState.shouldAutoscale = false;
                }
                
                ImPlot::EndSubplots();
            }
            
            
            } // end of hasInterferograms else block
        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();
}

void renderInterferogramConfigPanel() {

        ImGui::Begin("Interferogram");
        if (!appState.datasetInfo.hasInterferograms && appState.dataLoaded) {
            ImGui::Text("Interferograms not available for this data type.");
        } else if (appState.dataLoaded) {
            const ImVec4 cfgBtnColors[2] = {
                ImVec4(0.22f, 0.22f, 0.22f, 0.7f),
                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive)
            };

            // Row 1: X axis base (sample / OPD)
            ImGui::Text("X axis base");
            ImGui::SameLine();
            const bool xSample = (appState.xAxisBase == 0);
            const bool xOPD = (appState.xAxisBase == 1);
            const bool axisCorrected = appState.datasetInfo.axisIsCorrected;

            if (axisCorrected) ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[(xSample && !axisCorrected) ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  (xSample && !axisCorrected) ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("sample##XBaseSample")) {
                if (!xSample && !axisCorrected) {
                    appState.xAxisBase = 0;
                    appState.shouldAutoscale = true;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);
            if (axisCorrected) ImGui::EndDisabled();
            ImGui::SameLine(0.0f, 0.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[xOPD ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  xOPD ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("OPD##XBaseOPD")) {
                if (!xOPD) {
                    appState.xAxisBase = 1;
                    appState.shouldAutoscale = true;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);

            // Row 2: Max at zero (off / on)
            ImGui::Text("Max at zero");
            ImGui::SameLine();
            const bool alignOff = !appState.maxAtZero;
            const bool alignOn  =  appState.maxAtZero;

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[alignOff ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  alignOff ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("off##AlignOff")) {
                if (appState.maxAtZero) {
                    appState.maxAtZero = false;
                    appState.shouldAutoscale = true;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine(0.0f, 0.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[alignOn ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  alignOn ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("on##AlignOn")) {
                if (!appState.maxAtZero) {
                    appState.maxAtZero = true;
                    appState.shouldAutoscale = true;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);

            // Row 3: Auto-fit Y (off / on)
            ImGui::Text("Auto-fit Y");
            ImGui::SameLine();
            const bool afyOff = !appState.autoFitYAxis;
            const bool afyOn  =  appState.autoFitYAxis;

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[afyOff ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  afyOff ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("off##AfyOff")) {
                if (appState.autoFitYAxis) {
                    appState.autoFitYAxis = false;
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine(0.0f, 0.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[afyOn ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  afyOn ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("on##AfyOn")) {
                if (!appState.autoFitYAxis) {
                    appState.autoFitYAxis = true;
                    if (appState.dataLoaded) {
                        if (!appState.loadedData[0].referenceDetector.empty()) {
                            auto ref_min_max = std::minmax_element(appState.loadedData[0].referenceDetector.begin(), appState.loadedData[0].referenceDetector.end());
                            appState.ref_y_min = *ref_min_max.first;
                            appState.ref_y_max = *ref_min_max.second;
                        }
                        auto prim_min_max = std::minmax_element(appState.loadedData[0].primaryDetector.begin(), appState.loadedData[0].primaryDetector.end());
                        appState.prim_y_min = *prim_min_max.first;
                        appState.prim_y_max = *prim_min_max.second;
                    }
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);

            // Row 5: Downsample (off / on)
            ImGui::Text("Downsample");
            ImGui::SameLine();
            const bool dsOff = !appState.enableDownsampling;
            const bool dsOn  =  appState.enableDownsampling;

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[dsOff ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  dsOff ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("off##DsOff")) {
                if (appState.enableDownsampling) {
                    appState.enableDownsampling = false;
                    appState.hilbertXCache.clear();
                    appState.peakPositionsCache.clear();
                    if (appState.dataLoaded) {
                        // Reload all selected files with new downsampling setting
                        std::vector<InterferogramData> reloadedData;
                        for (const auto& filePath : appState.selectedFiles) {
                            try {
                                InterferogramData data = loadInterferogram(appState, filePath);
                                if (appState.enableDownsampling && data.referenceDetector.size() > appState.maxPointsBeforeDownsampling) {
                                    size_t localDownsampleFactor = data.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
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
                            appState.loadedData = reloadedData;
                            appState.rawDataCache.clear();
                            size_t reloadedIdx = 0;
                            for (const auto& file : appState.selectedFiles) {
                                try {
                                    InterferogramData rawData = loadInterferogram(appState, file);
                                    appState.rawDataCache.push_back(rawData);
                                } catch (const std::exception& e) {
                                    std::cerr << "Error reloading raw data: " << e.what() << std::endl;
                                    if (reloadedIdx < reloadedData.size())
                                        appState.rawDataCache.push_back(reloadedData[reloadedIdx]);
                                }
                                reloadedIdx++;
                            }
                            appState.zoomRange = {0, 0};
                            appState.shouldAutoscale = true;
                            appState.forceXAutofit = true;
                        }
                    }
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);
            ImGui::SameLine(0.0f, 0.0f);

            ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[dsOn ? 1 : 0]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  dsOn ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
            if (ImGui::Button("on##DsOn")) {
                if (!appState.enableDownsampling) {
                    appState.enableDownsampling = true;
                    appState.hilbertXCache.clear();
                    appState.peakPositionsCache.clear();
                    if (appState.dataLoaded) {
                        // Reload all selected files with new downsampling setting
                        std::vector<InterferogramData> reloadedData;
                        for (const auto& filePath : appState.selectedFiles) {
                            try {
                                InterferogramData data = loadInterferogram(appState, filePath);
                                if (appState.enableDownsampling && data.referenceDetector.size() > appState.maxPointsBeforeDownsampling) {
                                    size_t localDownsampleFactor = data.referenceDetector.size() / appState.maxPointsBeforeDownsampling + 1;
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
                            appState.loadedData = reloadedData;
                            appState.rawDataCache.clear();
                            size_t reloadedIdx = 0;
                            for (const auto& file : appState.selectedFiles) {
                                try {
                                    InterferogramData rawData = loadInterferogram(appState, file);
                                    appState.rawDataCache.push_back(rawData);
                                } catch (const std::exception& e) {
                                    std::cerr << "Error reloading raw data: " << e.what() << std::endl;
                                    if (reloadedIdx < reloadedData.size())
                                        appState.rawDataCache.push_back(reloadedData[reloadedIdx]);
                                }
                                reloadedIdx++;
                            }
                            appState.zoomRange = {0, 0};
                            appState.shouldAutoscale = true;
                            appState.forceXAutofit = true;
                        }
                    }
                    appState.needsRedraw = true;
                }
            }
            ImGui::PopStyleColor(3);

            // Row 6: X correction (Hilbert / Peaks) — only when feature is available
            const bool canPeakFind = !appState.datasetInfo.axisIsCorrected && appState.datasetInfo.hasReferenceChannel;
            if (canPeakFind) {
                ImGui::Text("X correction");
                ImGui::SameLine();
                const bool hilbSel = (appState.xCorrectionMethod == 0);
                const bool peakSel = (appState.xCorrectionMethod == 1);

                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[hilbSel ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  hilbSel ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                if (ImGui::Button("Hilbert##XCorrHilb")) {
                    if (appState.xCorrectionMethod != 0) {
                        appState.xCorrectionMethod = 0;
                        appState.hilbertXCache.clear();
                        appState.peakPositionsCache.clear();
                        appState.spectrum.cachedFrequencies.clear();
                        appState.spectrum.cachedSpectra.clear();
                        appState.spectrum.lastPrimaryDetectors.clear();
                        appState.spectrum.lastSpectrumParams.clear();
                        appState.spectrum.pendingSpectra_.clear();
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0.0f, 0.0f);

                ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[peakSel ? 1 : 0]);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  peakSel ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                if (ImGui::Button("Peaks##XCorrPeak")) {
                    if (appState.xCorrectionMethod != 1) {
                        appState.xCorrectionMethod = 1;
                        appState.hilbertXCache.clear();
                        appState.peakPositionsCache.clear();
                        appState.spectrum.cachedFrequencies.clear();
                        appState.spectrum.cachedSpectra.clear();
                        appState.spectrum.lastPrimaryDetectors.clear();
                        appState.spectrum.lastSpectrumParams.clear();
                        appState.spectrum.pendingSpectra_.clear();
                        appState.needsRedraw = true;
                    }
                }
                ImGui::PopStyleColor(3);

                // Row 7: Peak prominence slider — only visible when PeakFinding active
                if (appState.xCorrectionMethod == 1) {
                    ImGui::Text("Peak promin.");
                    ImGui::SameLine();
                    float prom = appState.peakProminenceThreshold;
                    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::SliderFloat("##PeakProm", &prom, 0.0f, 0.5f, "%.3f")) {
                        if (std::abs(prom - appState.peakProminenceThreshold) > 1e-6f) {
                            appState.peakProminenceThreshold = prom;
                            appState.hilbertXCache.clear();
                            appState.peakPositionsCache.clear();
                            appState.spectrum.cachedFrequencies.clear();
                            appState.spectrum.cachedSpectra.clear();
                            appState.spectrum.lastPrimaryDetectors.clear();
                            appState.spectrum.lastSpectrumParams.clear();
                            appState.spectrum.pendingSpectra_.clear();
                            appState.needsRedraw = true;
                        }
                    }

                    // Row 8: Peak indicators (off / on) — only in sample mode
                    if (appState.xAxisBase != 0) ImGui::BeginDisabled();
                    ImGui::Text("Peak markers");
                    ImGui::SameLine();
                    const bool pmOff = !appState.showPeakIndicators;
                    const bool pmOn  =  appState.showPeakIndicators;

                    ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[pmOff ? 1 : 0]);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  pmOff ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                    if (ImGui::Button("off##PmOff")) {
                        if (appState.showPeakIndicators) {
                            appState.showPeakIndicators = false;
                            appState.needsRedraw = true;
                        }
                    }
                    ImGui::PopStyleColor(3);
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::PushStyleColor(ImGuiCol_Button,        cfgBtnColors[pmOn ? 1 : 0]);
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  pmOn ? cfgBtnColors[1] : ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   cfgBtnColors[1]);
                    if (ImGui::Button("on##PmOn")) {
                        if (!appState.showPeakIndicators) {
                            appState.showPeakIndicators = true;
                            appState.needsRedraw = true;
                        }
                    }
                    ImGui::PopStyleColor(3);
                    if (appState.xAxisBase != 0) ImGui::EndDisabled();
                }
            }
        } else {
            ImGui::Text("No data loaded.");
        }
        ImGui::End();

}
