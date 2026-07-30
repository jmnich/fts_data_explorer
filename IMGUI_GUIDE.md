# ImGui & ImPlot Guide for Scientific GUI Development

Concise reference for building ImGui-based scientific applications with ImPlot/ImPlot3D.

---

## 1. Project Conventions

- Classes: `PascalCase`. Methods: `PascalCase`. Members: `m_` prefix. Statics: `s_` prefix.
- File per feature: `feature.h`, `feature.cpp`. Heavy rendering stays in `main.cpp`'s draw loop.
- All state lives in application structs/classes — ImGui owns only internal widget state.

## 2. Immediate Mode Fundamentals

ImGui rebuilds the entire UI every frame. No persistent widget objects.

```cpp
void App::Draw() {
    // Returns true ONLY on the frame the button is clicked
    if (ImGui::Button("Save")) {
        DoAction();  // action runs once
    }
}
```

**Always pair Begin/End**, even when Begin returns false:
```cpp
if (ImGui::Begin("Window")) { /* visible content */ }
ImGui::End();  // always called
```

## 3. ID Management

Widgets are identified by their string label. Duplicate labels = bugs.

| Technique | Example |
|-----------|---------|
| `##` suffix (hidden suffix) | `ImGui::Button("Save##file")` vs `"Save##config"` |
| `PushID/PopID` for loops | `PushID(i); Button("Delete"); PopID();` |
| `###id` for stable windows | `ImGui::Begin("Results (%d)###results", &open, count)` |

## 4. Window & Panel Patterns

**Docked panels** — all panels use the same pattern:
```cpp
ImGui::Begin("My Panel", nullptr, ImGuiWindowFlags_NoCollapse);
// panel content
ImGui::End();
```

**Scrollable child regions** for file lists or plot areas:
```cpp
ImGui::BeginChild("##FileList", ImVec2(0, 0), ImGuiChildFlags_None,
                  ImGuiWindowFlags_AlwaysVerticalScrollbar);
for (auto& file : files) {
    if (ImGui::Button(file.c_str())) { SelectFile(i); }
}
ImGui::EndChild();
```

Pass `ImVec2(0, 0)` to fill remaining space, negative values to deduct from available.

## 5. ImPlot: Basic Plot Setup

Every plot follows this structure:

```cpp
// (1) Grid opacity (push before BeginPlot)
ImVec4 gridCol = ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid];
gridCol.w *= appState->gridAlpha;
ImPlot::PushStyleColor(ImPlotCol_AxisGrid, gridCol);

// (2) Plot with unique name ("##" suffix to avoid ID collisions)
if (ImPlot::BeginPlot("MyPlot##view", ImVec2(-1, -1),
        ImPlotFlags_NoTitle | ImPlotFlags_NoLegend)) {

    ImPlot::SetupAxes("X label", "Y label",
        ImPlotAxisFlags_NoTickMarks,
        ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks);
    ImPlot::PlotLine("Data", xData.data(), yData.data(), (int)nPoints);

    ImPlot::EndPlot();
}
ImPlot::PopStyleColor();
```

**Key rules:** Always fill `ImVec2(-1, -1)` for plot size. Unique plot names (no two plots share the same first `##`-free label).

## 6. ImPlot: Axis Configuration

```cpp
// Flags control axis behavior
ImPlotAxisFlags x_flags = ImPlotAxisFlags_NoTickMarks;
ImPlotAxisFlags y_flags = ImPlotAxisFlags_NoLabel | ImPlotAxisFlags_NoTickMarks;

// AutoFit — fit to data
if (mode == 0) y_flags |= ImPlotAxisFlags_AutoFit;

// RangeFit — fit to visible X range
if (mode == 1) y_flags |= ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit;

// Setup axes (inside BeginPlot)
ImPlot::SetupAxes(xLabel, yLabel, x_flags, y_flags);

// Log scale
if (yScale == 1) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);

// Forced Y limits
ImPlot::SetupAxisLimits(ImAxis_Y1, yMin, yMax, ImPlotCond_Always);

// Custom tick labels (use SetupAxisTicksLimited helper for "nice" ticks)
```

## 7. ImPlot: Subplots (Linked Axes)

For stacked plots sharing an X axis (e.g., interferogram reference + primary):

```cpp
if (ImPlot::BeginSubplots("Detector Plots", numRows, 1, ImVec2(-1, -1),
        ImPlotSubplotFlags_NoTitle | ImPlotSubplotFlags_LinkAllX |
        ImPlotSubplotFlags_NoLegend, rowHeightRatios)) {

    ImPlot::BeginPlot("Reference");
    ImPlot::SetupAxes(...);
    ImPlot::PlotLine("##ref", ...);
    ImPlot::EndPlot();

    ImPlot::BeginPlot("Primary");
    ImPlot::SetupAxes(...);
    ImPlot::PlotLine("##prim", ...);
    ImPlot::EndPlot();

    ImPlot::EndSubplots();
}
```

`LinkAllX` keeps X axes synchronized during pan/zoom.

## 8. ImPlot: Shift+Drag Range Selection

Selection detection happens inside `BeginPlot`, but the new range is applied **next frame** via `SetNextAxisLimits` to avoid ImPlot's locked-state assertion:

```cpp
// Outside BeginPlot (before it, after checking flags):
if (pendingNextXMin < pendingNextXMax) {
    ImPlot::SetNextAxisLimits(ImAxis_X1, pendingNextXMin, pendingNextXMax,
                              ImPlotCond_Always);
    pendingNextXMin = pendingNextXMax = 0;  // consumed
}

// Inside BeginPlot - detection:
if (isFocused && ImPlot::IsPlotHovered() && io.KeyShift && !isSelecting) {
    isSelecting = true;
    selectionStart = ImPlot::GetPlotMousePos().x;
} else if (!io.KeyShift && isSelecting) {
    isSelecting = false;
    double x2 = ImPlot::GetPlotMousePos().x;
    if (fabs(selectionStart - x2) > 0) {
        pendingNextXMin = min(selectionStart, x2);
        pendingNextXMax = max(selectionStart, x2);
    }
}

// Inside BeginPlot - visualization:
if (isSelecting) {
    double mouseX = ImPlot::GetPlotMousePos().x;
    double fillX[3] = {selectionStart, mouseX, mouseX};
    double fillY[3] = {plotLim.Y.Min, plotLim.Y.Min, plotLim.Y.Max};
    ImPlot::PlotShaded("##SelFill", fillX, fillY, 3);
}
```

## 9. ImPlot: Keyboard Interaction

**ESC → autoscale:** Set a `shouldAutoscale` flag, then inside `BeginPlot`:
```cpp
if (ImGui::IsKeyPressed(ImGuiKey_Escape) && isFocused) {
    shouldAutoscale = true;
}
// ... inside BeginPlot:
if (shouldAutoscale) {
    shouldAutoscale = false;
    ImPlot::SetupAxisLimits(ImAxis_X1, globalXMin, globalXMax, ImPlotCond_Always);
    ImPlot::SetupAxisLimits(ImAxis_Y1, globalYMin, globalYMax, ImPlotCond_Always);
}
```

**Arrow key pan (10% of visible range)** — use flags consumed before `BeginPlot`:
```cpp
// Edge-triggered flags:
if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !leftLastFrame && isFocused) {
    leftLastFrame = true;
    leftPending = true;
} else if (ImGui::IsKeyReleased(ImGuiKey_LeftArrow)) { leftLastFrame = false; }

// Apply before BeginPlot:
if (leftPending && !shouldAutoscale) {
    double range = manualXMax - manualXMin;
    ImPlot::SetNextAxisLimits(ImAxis_X1, manualXMin - range*0.1,
                              manualXMax - range*0.1, ImPlotCond_Always);
    leftPending = false;
}
```

## 10. ImPlot: Tracking Cursor

```cpp
if (showCursor && ImPlot::IsPlotHovered()) {
    ImPlotPoint mouse = ImPlot::GetPlotMousePos();
    double yVal = /* interpolate from data */;
    ImPlot::PlotLine("##Cursor", &mouse.x, &yVal, 1);   // vertical line
    ImPlot::PlotScatter("##CursorPt", &mouse.x, &yVal, 1); // marker
    ImPlot::Annotation(mouse.x, yVal, IM_COL32(255,255,0,255),
                       ImVec2(8, -8), false, "X=%.2f Y=%.4e", mouse.x, yVal);
}
```

## 11. ImPlot3D: Surface Plots

For 3D variance surfaces (Allan-Werle):

```cpp
if (ImPlot3D::BeginPlot("Allan3DSurface", ImVec2(-1, -1),
        ImPlot3DFlags_NoTitle)) {
    ImPlot3D::SetupAxis(ImAxis3D_X, "log tau");
    ImPlot3D::SetupAxis(ImAxis3D_Y, "log sigma");
    ImPlot3D::SetupAxis(ImAxis3D_Z, "log var");

    ImPlot3D::PlotSurface("##Surface", xData.data(), yData.data(),
                          zData.data(), nX, nY,
                          ImPlot3DSurfaceFlags_Mesh);

    ImPlot3D::EndPlot();
}
```

Place the 3D plot above your 2D ImPlot slice for a layered view.

## 12. Toggle Button Groups

For X-unit selectors (cm⁻¹/µm/THz) or Y-scale (lin/log):

```cpp
const ImVec4 colActive = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
const ImVec4 colInactive(0.22f, 0.22f, 0.22f, 0.7f);

// Button 1
ImGui::PushStyleColor(ImGuiCol_Button,        sel == 0 ? colActive : colInactive);
ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel == 0 ? colActive : colInactive);
ImGui::PushStyleColor(ImGuiCol_ButtonActive,  colActive);
if (ImGui::Button("cm-1##XUnit")) { sel = 0; }
ImGui::PopStyleColor(3);
ImGui::SameLine();

// Button 2 (same pattern with sel == 1)
// Button 3 (same pattern with sel == 2)
```

Always pop exactly as many colors as pushed.

## 13. Async Computation + Plot Update

**Batch-computation pattern** (Average, SNR, Allan, T100 std dev):

```cpp
// In a tickCalculation() method called each frame before ImGui::NewFrame():
if (!batchActive) {
    batchActive = true;
    for (auto& file : selectedFiles) {
        pendingFutures_.push_back(
            pool->enqueue([path = file.path]() {
                return processSpectrum(path);
            })
        );
    }
}

// Poll results each frame:
for (auto& fut : pendingFutures_) {
    if (fut.valid() && fut.wait_for(0s) == std::future_status::ready) {
        auto result = fut.get();  // move to main thread
        accumulate(result);
        completedCount_++;
    }
}

// Finalize when done:
if (completedCount_ >= totalSubmitted_) {
    finalizeComputation();
    batchActive = false;
}
```

**Sync fallback for first load** (spectrum panel): if no cached data exists, compute synchronously to avoid a blank frame:
```cpp
if (!hasCache) {
    processSpectrum(path);   // synchronous
} else {
    // submit async refresh
    pendingFuture_ = pool->enqueue(...);
}
```

**Always capture by value in lambdas** — never capture `this` or reference panel members.

## 14. Popups: Simple Modal

```cpp
// OpenPopup must be called AFTER ImGui::NewFrame(), inside window scope:
if (shouldShowPopup) {
    ImGui::OpenPopup("Error");
}

if (ImGui::BeginPopupModal("Error", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Something went wrong.");
    if (ImGui::Button("OK")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
```

## 15. Popups: Keyboard-Navigated Modal

For dialogs with multiple buttons navigable by arrow keys:

```cpp
static int focusIdx = 0;
static bool prevPopupOpen = false;

if (ImGui::BeginPopupModal("Confirm##delete", NULL,
        ImGuiWindowFlags_AlwaysAutoResize)) {
    bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter);

    // Arrow navigation
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && focusIdx > 0) focusIdx--;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && focusIdx < 2) focusIdx++;

    // Button 0 (Cancel)
    if (focusIdx == 0)
        ImGui::PushStyleColor(ImGuiCol_Button,
            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
        (enter && prevPopupOpen && focusIdx == 0)) {
        ImGui::CloseCurrentPopup();
    }
    if (focusIdx != 0) ImGui::PopStyleColor();
    ImGui::SameLine();

    // Button 1 (Yes) — same push/pop pattern with focusIdx == 1
    // Button 2 (Yes, don't ask again) — with focusIdx == 2

    ImGui::EndPopup();
    prevPopupOpen = true;
}
```

**`prevPopupOpen` gate:** Prevents the Enter key that *opened* the popup from triggering a button inside it. `p_open = nullptr` prevents ImGui's auto-close-on-Escape; Escape is handled manually in the Cancel condition.

## 16. Tables

```cpp
if (ImGui::BeginTable("##DataTable", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingStretchSame)) {
    // Headers
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Value A");
    ImGui::TableSetupColumn("Value B");
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

    // Data rows
    for (auto& row : rows) {
        ImGui::TableNextRow();
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowColor);
        ImGui::TableSetColumnIndex(0); ImGui::Text("%s", row.name);
        ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", row.a);
        ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f", row.b);
    }
    ImGui::EndTable();
}
```

## 17. Idle Rendering Optimization

Skip frames when nothing changes to reduce CPU/GPU usage:

```cpp
std::atomic<bool> needsRedraw{true};

// GLFW callback sets the flag:
void keyCallback(...) { needsRedraw = true; }

// Main loop:
while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    if (!needsRedraw) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;  // skip poll, NewFrame, Render, SwapBuffers
    }

    needsRedraw = false;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    // ... all UI rendering ...

    ImGui::Render();
    glfwSwapBuffers(window);
}

// Force 1 fps when showFPS is on:
static double lastFPSCheck = 0;
if (showFPS && glfwGetTime() - lastFPSCheck > 1.0) {
    needsRedraw = true;
    lastFPSCheck = glfwGetTime();
}
```

## 18. Dockspace Setup

One-time layout on first launch (`dockspaceNeedsInit` flag):

```cpp
ImGui::Begin("DockSpace", nullptr,
    ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground |
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoBringToFrontOnFocus);

ImGuiID dockspace_id = ImGui::GetID("MainDockSpace_v2");
ImGui::DockSpace(dockspace_id);

static bool first = true;
if (first) {
    first = false;
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGuiID dock_center = dockspace_id;
    ImGuiID dock_left = ImGui::DockBuilderSplitNode(dock_center,
        ImGuiDir_Left, 0.25f, nullptr, &dock_center);
    ImGuiID dock_left_bottom = ImGui::DockBuilderSplitNode(dock_left,
        ImGuiDir_Down, 0.5f, nullptr, &dock_left);

    ImGui::DockBuilderDockWindow("Files", dock_left);
    ImGui::DockBuilderDockWindow("Metadata", dock_left_bottom);
    ImGui::DockBuilderDockWindow("Interferogram View", dock_center);
    // ... dock remaining panels ...
    ImGui::DockBuilderFinish(dockspace_id);
}
ImGui::End();
```

## 19. Common Pitfalls

| Pitfall | Fix |
|---------|-----|
| `OpenPopup` before `NewFrame` | Always call between `NewFrame()` and `Render()` |
| Forgetting `End()`/`EndPlot()` | Every `Begin`/`BeginPlot` needs its matching `End` |
| ID collision in loops | Use `PushID(i)` or `##unique` per row |
| Duplicate plot names | Each plot needs a globally unique label |
| Computations inside `BeginPlot` | Submit/poll async results **before** `BeginPlot` |
| Popup inside `PushID` | `OpenPopup` inherits ID stack — match scope with `BeginPopupModal` |
| Enter key activating popup buttons | Use `prevPopupOpen` gate (see §15) |
| `this`/ref captured in thread lambdas | Capture by value (strings, paths, PODs) |
| FFTW planner not thread-safe | Serialize `fftw_plan_dft_1d` with a mutex |
| Style stack imbalance | Every `Push*` must have a matching `Pop*` |
| Window title changes break layout | Use `###id` suffix for stable internal ID |

## 20. Scroll Wheel Zoom Rate Limiting

ImGui's `ConfigInputTrickleEventQueue` blocks wheel events after any `MousePos` in the same frame, causing stale wheel events to accumulate and drain slowly — producing delayed "ghost" zoom after the user stops scrolling.

**Rate limiter** (`main.cpp`, after `NewFrame()`, before any `BeginPlot()`):

```cpp
// Persistent locals
float scrollCarryOverY = 0.0f, scrollCarryOverX = 0.0f;

{
    ImGuiIO& io = ImGui::GetIO();

    if (!appState.scrollEventsThisPoll) {
        // No GLFW scroll this frame — stale queue events. Zero immediately.
        scrollCarryOverY = scrollCarryOverX = 0.0f;
        io.MouseWheel = io.MouseWheelH = 0.0f;
    } else {
        float totalY = scrollCarryOverY + io.MouseWheel;
        float totalX = scrollCarryOverX + io.MouseWheelH;
        io.MouseWheel  = std::clamp(totalY, -1.0f, 1.0f);
        io.MouseWheelH = std::clamp(totalX, -1.0f, 1.0f);
        scrollCarryOverY = std::clamp(totalY - io.MouseWheel, -60.0f, 60.0f);
        scrollCarryOverX = std::clamp(totalX - io.MouseWheelH, -60.0f, 60.0f);
    }
    appState.scrollEventsThisPoll = false;
}

if (scrollCarryOverY != 0.0f || scrollCarryOverX != 0.0f)
    appState.needsRedraw = true;
```

`scrollEventsThisPoll` is `std::atomic<bool>` in `app_state.h`, set by the GLFW scroll callback alongside `needsRedraw`. It distinguishes active scrolling from stale ImGui-queued events.

**Key points:** zero `io.MouseWheel` on stop (can't access internal queue, so zero what ImPlot reads); no forced redraw to drain stale queue (it's small, drains naturally); carry-over cap at ±60 (~1 s at 60 fps); direction reversal resets carry-over immediately.
