#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

enum class MemberKind { Original, Derivative };

// Every spec member carries kind/columns/units, and originals may carry a timestamp.
// origin/config are per-member for spectra/derivative groups (spec shows @origin/@config
// on each spectra/<id>/ sub-group). For interferogram groups they are empty — the
// group-level origin/config on MemberGroup applies instead (§7.2 of the spec).
struct MemberBase {
    std::string id;                          // slug, unique within its type group
    MemberKind  kind = MemberKind::Derivative;
    std::string timestamp;                   // ISO-8601 UTC or empty (originals only)
    std::vector<std::string> columns;        // 2-col datasets carry columns+units attrs
    std::vector<std::string> units;
    std::string origin;                      // per-member @origin JSON (spectra/, average_spectra/, etc.)
    std::string config;                      // per-member @config JSON (spectra/, average_spectra/, etc.)
};

// igm_uncorrected_x/ and igm_corrected_x/ — flat [N,2] datasets.
// uncorrected: col0 = Reference detector, col1 = Primary detector.
// corrected:   col0 = Primary detector,   col1 = OPD axis (um).
struct InterferogramMember : MemberBase {
    bool corrected = false;
    std::vector<double> col0;
    std::vector<double> col1;
};

// spectra/, average_spectra/, snr_spectra/, and t100 reference/stddev curves.
struct TwoColumnMember : MemberBase {
    std::vector<double> x;
    std::vector<double> y;
};

// allan_werle/ — T x W surface plus its two axes.
struct AllanMember : MemberBase {
    std::vector<double> taus;        // [T], units "s"
    std::vector<double> wavelengths; // [W], units "um"
    std::vector<double> surface;     // [T*W] row-major
};

// t100/ — reference, stddev, and one transmittance curve per input file.
struct T100Member : MemberBase {
    struct Curve { std::string fileId; std::vector<double> x, y; };
    TwoColumnMember reference;
    TwoColumnMember stddev;
    std::vector<Curve> curves;
};

// One type group: members + group-level @schema/@origin/@config attributes.
// For interferogram groups (igm_uncorrected_x, igm_corrected_x), the group-level
// origin/config serve all members in the group (members' own origin/config are empty).
// For spectra/derivative groups, each member carries its own origin/config;
// the group-level origin/config here serve as defaults (typically empty).
template <typename T>
struct MemberGroup {
    std::string schema;     // spec §4: "interferogram", "spectrum/v1", etc.
    std::string origin;     // JSON string, written once at creation (pool-level for IFGs)
    std::string config;     // JSON string, group-level settings
    std::vector<T> members;
};

// The in-memory model — the single object the app engine will talk to.
struct Workspace {
    std::string format;     // "unified-spectral-data-container"
    std::string created;    // ISO-8601 UTC

    // Root metadata (opaque in Phase 0; typed accessors arrive in Phase 3).
    nlohmann::json measurementConfig;   // measurement_config.json
    std::string    measurementComment;  // measurement_comment.txt
    std::string    tags;                // comma-separated

    nlohmann::json workspaceJson;       // full §8 view-state blob, preserved verbatim

    MemberGroup<InterferogramMember> uncorrectedIfg;  // igm_uncorrected_x/
    MemberGroup<InterferogramMember> correctedIfg;    // igm_corrected_x/
    MemberGroup<TwoColumnMember>     spectra;         // spectra/
    MemberGroup<TwoColumnMember>     averageSpectra;  // average_spectra/
    MemberGroup<TwoColumnMember>     snrSpectra;      // snr_spectra/
    MemberGroup<AllanMember>         allanWerle;      // allan_werle/
    MemberGroup<T100Member>          t100;            // t100/

    bool dirty = false;

    // Availability flags for the engine (mirrors current DatasetInfo).
    bool hasInterferograms() const;
    bool hasReferenceChannel() const;
    bool axisIsCorrected() const;
    bool hasPrecomputedSpectra() const;

    // inputs bookkeeping (spec rule 10).
    bool inputsAreValid() const;
    std::vector<std::string> danglingInputs() const;
};

// Slug + collision suffix, unique within a group: "sample_0001", "avg_of_3".
std::string makeUniqueId(const std::string& base, const std::vector<std::string>& existingIds);

// Absolute-path lookup used by inputs integrity: "/igm_uncorrected_x/record_0".
std::optional<std::string> findMemberPath(const Workspace& ws, const std::string& id);

// Group-level provenance (spec §5) — application identity only, no host info.
nlohmann::json makeOriginJson(const std::string& appName, const std::string& version);
