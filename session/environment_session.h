#pragma once

#include "pthread_compat.h"   // GCC 16+: must precede <mutex> (_GNU_SOURCE undefined)
#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "session_base.h"
#include "spectral_pool.h"

#include <imgui.h>   // ImVec4 (ComparatorCurve::color)

struct AppState;
struct InterferogramMember;

// Phase-3 M3.2 — instantiable cross-workspace analysis tab (audit §3.3).
// LIVE object, never folded: multiple instances of a type coexist, each owns
// its state + futures; tab switch only changes which instance renders.
// Picks use STABLE keys (workspace path, or "cross.h5#sourceId") resolved to
// live sessions at each use; a closed referenced session degrades the owning
// rows (marked unavailable, removable) — never re-pointed.
enum class EnvType { Absorbance, Comparator };   // Pca removed (Phase-2 user decision)

// Comparator artifact types: what to overlay across the selected datasets.
// CorrectedInterferogram keeps value 4 so persisted configs from the merged
// "Interferogram" artifact (which preferred corrected) map faithfully.
enum class ComparatorArtifact {
    AverageSpectrum,       // session average spectrum (cachedAverageX/Y)
    RawSpectrum,           // individual spectra members (via the spectral pool)
    Snr,                   // SNR-per-wavelength (cachedSnrX/Y)
    T100,                  // 100% T transmittance curves (cachedTransX/Y per member)
    CorrectedInterferogram,// corrected interferograms (primary detector vs sample index)
    RawInterferogram,      // uncorrected interferograms (primary detector vs sample index)
};

const char* experimentTypeName(EnvType t);              // "Absorbance" / "Comparator"
const char* artifactLabel(ComparatorArtifact a); // "Average spectrum" / "SNR" / …
// True if `name` is one of the experiment type's docked panels (stable window
// names). Shared with app_loop.cpp's pre-DockSpace forced tab selection.
bool isExperimentPanelName(const char* name);

// One overlay curve: label + x/y already in the instance's display unit
// (or sample index for interferograms).
struct ComparatorCurve {
    std::string label;       // full label (legend / CSV headers)
    std::string shortLabel;  // compact label (cursor info box, no dataset name)
    std::vector<double> x, y;
    ImVec4 color;            // explicit line color; w==0 → use the colormap
};

// One selectable member of an artifact type. `xUnit` is the member's STORED
// x-unit (0/1/2), or -1 for interferograms (sample-index / OPD X, no conversion).
struct ArtifactMember {
    std::string id;
    int xUnit = 0;
    std::vector<double> x, y;
    std::string config;   // persisted member @config (params + inputs)
    bool stale = false;
};

// One stale-source diagnostic row (tooltip-only): friendly source label +
// why the member no longer matches the compute-time snapshot.
struct StaleDetail {
    std::string label;
    std::string reason;
};

struct ArtifactInfo {
    bool available = false;              // ≥1 member
    bool stale = false;                  // any member stale
    std::vector<ArtifactMember> members;
};

// One absorbance record: a reference artifact and a sample artifact, each an
// Average/Raw-spectrum member of some dataset, plus the computed result (the
// overlapping X grid in the display unit + clamped ratio + display curve).
// `status` is empty when the curve computed OK, else a short reason.
struct AbsorbanceCurve {
    std::string refKey;    int refArtifact = 0;    std::string refMember;
    std::string sampleKey; int sampleArtifact = 0; std::string sampleMember;
    std::vector<double> gridX, ratioY, curveY;
    std::string status;
    std::string name;          // "" = automatic "Curve N" (N = index+1)
    char nameBuf[128] = {};    // rename editor buffer (write-through to name)
};

// Correction parameters used to derive the corrected-IFG OPD axis from a raw
// interferogram (mirrors the interferogram view + spectrum pipeline).
struct IfgDeriveParams {
    double laserUm = 1.550;
    int method = 0;                      // 0 Hilbert, 1 peak-finding
    float prominence = 0.02f;
};

class EnvironmentSession : public SessionBase {
public:
    EnvType type = EnvType::Absorbance;
    std::string id;                      // stable experiment id in the .cross.h5
                                         // ("" = transient, never saved)
    // HDF5 storage footprint of the persisted experiments/<id> group,
    // recomputed on every project load and save (in-memory only).
    uint64_t sizeBytes = 0;
    std::string instanceName;            // "Absorbance 1", ... (unique window title)
    // RENAME-STABLE identity (bugfix 2026-08-14): generated once at
    // construction, never changes. Tab-strip keys ("exp:<stripKey>") and the
    // ImPlot plot id ("##envPlot<stripKey>") use it, so renaming neither
    // shuffles the tab nor resets the plot's X range. Persisted experiments
    // are matched to the manifest order via their saved id.
    std::string stripKey;
    std::string comment;                 // free-text note (in-memory, Phase 3)
    char commentBuf[4096] = {};          // editor buffer, write-through to comment
    char nameBuf[256] = {};              // editor buffer for the inline rename
    // Phase 4: unsaved-changes flag (set by every mutating UI action, cleared
    // by [Save]) + result-staleness flag (member snapshots at compute time vs
    // the sources' current member state — data-grounded, window-aware params).
    bool dirty = false;
    bool stale = false;
    // Tooltip-only diagnostics for the current stale flag (rebuilt by
    // updateStaleness): one row per mismatched source.
    std::vector<StaleDetail> staleDetails;
    // Tab-strip visibility (bugfix 2026-08-14): closing the tab HIDES it from
    // the strip — the instance stays live in experiments[] and re-opens via
    // the Active Experiments panel row (activateExperiment clears it).
    bool tabHidden = false;
    // Member snapshots per referenced source at compute time (M4.1); the
    // staleness check compares these to the sources' current member state.
    // Key: sourceKey + "\x1f" + artifact + "\x1f" + memberId (two curves may
    // reference the same source with different members).
    std::map<std::string, MemberSnapshot> storedFingerprints;
    int xUnitSelector = 0;               // 0 cm-1, 1 um, 2 THz
    int prevXUnitSelector = 0;
    int yMode = 0;                       // Absorbance only: 0 T%, 1 A
    // Plot ranging (spectrum-view scheme, both types): 0 all, 1 tight, 2 force.
    int yAxisMode = 0;
    int prevYAxisMode = -1;
    double forcedYMin = 0.0, forcedYMax = 1.0;
    // Y scale (spectrum-view scheme): 0 lin, 1 log10, 2 dB. log/dB are gated
    // off for T100 and Interferogram artifacts (non-positive values).
    int yScaleSelector = 0;
    int prevYScaleSelector = -1;
    // Tracking cursor (spectrum-view scheme): marks ALL displayed curves.
    bool showTrackingCursor = false;
    // HITRAN gas-marker toggles (spectral artifacts only; index i <->
    // kHitranGases[i]). Persisted in the experiment config.json.
    std::array<bool, 8> hitranGasEnabled{};
    // Display-only stride downsampling to maxPointsBeforeDownsampling (the
    // interferogram-view scheme). OFF by default: full-resolution display;
    // the cursor reads full-res data and CSV export never downsamples.
    bool downsampleDisplay = false;

    // Comparator selection.
    int artifactSelector = 0;            // ComparatorArtifact index
    std::vector<std::string> comparatorKeys;   // stable keys; empty = all open datasets
    // True once the user has toggled a checkbox: an empty comparatorKeys then
    // means "nothing selected", not "all open datasets".
    bool comparatorKeysExplicit = false;
    // Per-dataset member pick (multi-member artifacts: raw spectra / T100 / IFG).
    // key = dataset stable key, value = member id ("" / absent = first member).
    std::map<std::string, std::string> memberPicks;

    // Absorbance curves: one record per reference/sample pair. STABLE keys —
    // resolved per use; degraded, never re-pointed.
    std::vector<AbsorbanceCurve> curves;
    // True once a compute ran and produced a ratio for ≥1 curve.
    bool computed = false;
    // Set by any selector/curve mutation; consumed at the top of render() to
    // re-run the synchronous compute.
    bool resultsDirty_ = false;

    // T100-pattern plot interaction (t100.h subset).
    bool isSelectingXRange = false;
    double selectionStartX = 0.0;
    double selectionEndX = 0.0;
    bool shouldAutoscale = true;
    double manualXMin = 0.0, manualXMax = 0.0;
    double manualYMin = 0.0, manualYMax = 0.0;
    bool leftArrowPressedLastFrame = false, rightArrowPressedLastFrame = false;
    bool leftArrowHandleFlag = false, rightArrowHandleFlag = false;
    double pendingNextXMin = 0.0, pendingNextXMax = -1.0;
    // Last-rendered plot X limits (captured each frame in renderPlot) — the
    // "current plot area" source for the export X-range mode.
    double viewXMin = 0.0, viewXMax = 0.0;
    // Export X-range mode: 0 all, 1 current plot area, 2 manual.
    int exportXRangeMode = 0;
    double exportXMin = 0.0, exportXMax = 0.0;

    // Synchronous artifact-based compute (Average/Raw spectra, no FFT pool):
    // per curve, resample the sample onto the reference's overlapping X region
    // and divide (clamped). Idempotent; cheap enough to run on selector change.
    void computeAbsorbance(AppState& s);

    EnvironmentSession(EnvType t, const std::string& name);
    EnvironmentSession(const EnvironmentSession&) = delete;
    EnvironmentSession& operator=(const EnvironmentSession&) = delete;

    void tickAsync() override {}          // synchronous compute; no per-frame poll
    void render() override;              // config window + view window (docked)
    // Tab-selector close: DEACTIVATES the tab only (focus the Session tab) —
    // the instance stays live and listed in the Active Experiments panel.
    // Deletion happens exclusively via requestDelete() from that panel.
    void closeRequest() override;
    // Delete the instance: dirty or persisted experiments confirm via the
    // delete modal; transient clean instances remove directly.
    void requestDelete();
    bool isDirty() const override { return dirty; }
    const std::string& title() const override { return titleCache_; }
    void onActivate() override {}        // AppLoop sets needsRedraw
    void onDeactivate() override {}      // Phase 4: layout save

    // Rename the experiment (label + window title); marks dirty.
    void rename(const std::string& name);
    // Re-derive the staleness flag from storedFingerprints vs the sources'
    // current params (open tab → parked session; not open → persisted
    // workspace.json). Call at load and after [Compute].
    void updateStaleness(AppState& s);
    // Tab label with dirty star + staleness badge ("Name * ⚠").
    std::string tabLabel() const;

    // yMode toggle: rewrite every curve's curveY from its ratioY — instant.
    void applyYMode();
    // xUnit change: convert every curve's gridX in place (ratios are
    // unit-independent).
    void convertXInPlace();
    // Display label for a curve's source key (open-tab label, cross-source
    // name, or the raw key).
    std::string sourceLabel(const std::string& key) const;
    // Legend/CSV label for one curve: the user-chosen name, or "Curve N"
    // (N = index+1) when unnamed.
    std::string curveLabel(const AbsorbanceCurve& c, size_t index) const;

    // Extract overlay curves for the selected artifact from the selected
    // datasets (public for the roundtrip test; pure data extraction).
    std::vector<ComparatorCurve> gatherCurves(AppState& s);

private:
    std::string titleCache_;
    // Corrected-IFG derivation (sourceKey#memberId -> OPD axis um, RAM only):
    // rebuilt per entry when the source's correction params change.
    std::map<std::string, std::vector<double>> derivedOpdCache_;
    std::map<std::string, IfgDeriveParams> derivedOpdParams_;
    // Members available in a workspace for one artifact type. For
    // CorrectedInterferogram the persisted group wins; datasets without it
    // fall back to deriving corrected IFGs from the raw group (cached).
    ArtifactInfo artifactInfo(const Workspace& ws, ComparatorArtifact a,
                              const std::string& sourceKey);
    // Correction params for a comparator source: live session when open,
    // else the persisted workspace params, else defaults.
    IfgDeriveParams ifgDeriveParamsFor(const std::string& sourceKey,
                                       const Workspace& ws);
    // OPD axis (um) for an uncorrected IFG member: mirror displacement ×2
    // from the reference detector (view/pipeline algorithm), cached.
    std::vector<double> derivedOpdAxis(const std::string& sourceKey,
                                       const InterferogramMember& m,
                                       const IfgDeriveParams& p);
    // Extract one artifact member (x/y/xUnit/config) for a source key; false
    // when the source is absent or the artifact has no members (mirrors
    // gatherCurves).
    bool extractArtifact(AppState& s, const std::string& key, ComparatorArtifact a,
                         const std::string& memberId, ArtifactMember& out);
    // Compute-time member snapshot for a source (same resolution as
    // extractArtifact); false when the member cannot be resolved.
    bool memberSnapshotForKey(AppState& s, const std::string& key, ComparatorArtifact a,
                              const std::string& memberId, MemberSnapshot& out);
    // Rebuild storedFingerprints (composite keys) from every curve's current
    // ref/sample members. Clears the map first. Called by computeAbsorbance
    // and by updateStaleness as the legacy-format migration (re-baseline).
    void captureSnapshots(AppState& s);
    // Stale-warning overlay on the plot (message box + Recompute button +
    // hover tooltip with staleDetails). Rendered after EndPlot in renderPlot
    // and in the empty-data branch.
    void renderStaleWarning(ImDrawList* dl, const ImVec2& rectMin,
                            const ImVec2& rectMax);
    void renderConfigWindow();           // instanceName (pickers/selectors/comment)
    void renderViewWindow();             // instanceName + " View" (plot)
    void renderAbsorbanceConfig();
    // Trim + commit a curve's nameBuf into its name ("" = back to the auto
    // "Curve N" label); marks dirty. Called on Enter and on focus loss.
    void applyCurveName(AbsorbanceCurve& c);
    void renderComparatorConfig();
    void renderDatasetSelector();        // comparator included-datasets checkbox list
    void renderXUnitButtons();
    void renderYModeButtons();           // Absorbance: T% / A toggle
    void renderYAxisControls();
    void renderYScaleButtons();          // lin/log/dB, gated off for T100/IFG
    void renderCursorToggle();
    // Docked "Plot Ranging" panel (comparator): X unit / Y scale / Y axis /
    // cursor — the spectrum-view navigation block, split into its own window.
    void renderRangingWindow();
    // Docked "Export" panel (comparator): X-range mode + manual min/max.
    void renderExportWindow();
    void renderCommentEditor();
    // Comment box height in px: auto-sizes to the wrapped content, never
    // below 10 lines (recomputed every frame, so typing and dock resizes
    // reflow the box automatically).
    float commentBoxHeight() const;
    // Common overlay plot with spectrum-view navigation (locked/tight/all Y,
    // shift+drag range, ESC fit-all, arrows pan, wheel zoom).
    void renderPlot(const std::vector<ComparatorCurve>& curves,
                    const std::string& xLabel, const std::string& yLabel,
                    bool hasGuideline, double guideline, bool showLegend);
    void exportCsv();
};

// Registry ops (main thread). createExperiment auto-names from the monotonic
// counters and activates the new instance. removeExperiment erases + fixes
// activeExperimentIdx (== removed → focus the Session tab).
EnvironmentSession* createExperiment(AppState& s, EnvType t);
void activateExperiment(AppState& s, int idx);
void removeExperiment(AppState& s, int idx);
// Close ALL instances (project switch / go-home); resets the env tab state.
void clearExperiments(AppState& s);
// Project-open helper (main.cpp/session_tab/welcome open paths): load the
// manifest, clear the current experiments, then restore the persisted
// experiments from the file. Returns false on failure (err set).
bool crossOpenProject(AppState& s, const std::string& path, std::string& err);
