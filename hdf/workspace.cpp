#include "workspace.h"

#include <algorithm>

#include "hdf5_util.h"

// ---- availability flags (mirror DatasetInfo) ----

bool Workspace::hasInterferograms() const {
    return !uncorrectedIfg.members.empty() || !correctedIfg.members.empty();
}

bool Workspace::hasReferenceChannel() const {
    return !uncorrectedIfg.members.empty();
}

bool Workspace::axisIsCorrected() const {
    return !correctedIfg.members.empty();
}

bool Workspace::hasPrecomputedSpectra() const {
    return std::any_of(spectra.members.begin(), spectra.members.end(),
                       [](const TwoColumnMember& m) { return m.kind == MemberKind::Original; });
}

// ---- inputs bookkeeping (spec rule 10) ----

static void appendGroupPaths(const std::string& group, const std::vector<std::string>& ids,
                             std::vector<std::string>& out) {
    for (const auto& id : ids) out.push_back("/" + group + "/" + id);
}

static std::vector<std::string> allMemberPaths(const Workspace& ws) {
    std::vector<std::string> out;
    auto ifgIds = [](const auto& members) {
        std::vector<std::string> ids;
        for (const auto& m : members) ids.push_back(m.id);
        return ids;
    };
    appendGroupPaths("igm_uncorrected_x", ifgIds(ws.uncorrectedIfg.members), out);
    appendGroupPaths("igm_corrected_x",   ifgIds(ws.correctedIfg.members),   out);
    appendGroupPaths("spectra",           ifgIds(ws.spectra.members),        out);
    appendGroupPaths("average_spectra",   ifgIds(ws.averageSpectra.members), out);
    appendGroupPaths("snr_spectra",       ifgIds(ws.snrSpectra.members),     out);
    appendGroupPaths("allan_werle",       ifgIds(ws.allanWerle.members),     out);
    appendGroupPaths("t100",              ifgIds(ws.t100.members),           out);
    return out;
}

std::optional<std::string> findMemberPath(const Workspace& ws, const std::string& id) {
    auto first = [&](const auto& group, const std::string& groupName)
        -> std::optional<std::string> {
        for (const auto& m : group.members)
            if (m.id == id) return "/" + groupName + "/" + id;
        return std::nullopt;
    };
    if (auto p = first(ws.uncorrectedIfg, "igm_uncorrected_x")) return p;
    if (auto p = first(ws.correctedIfg,   "igm_corrected_x"))   return p;
    if (auto p = first(ws.spectra,        "spectra"))           return p;
    if (auto p = first(ws.averageSpectra, "average_spectra"))   return p;
    if (auto p = first(ws.snrSpectra,     "snr_spectra"))       return p;
    if (auto p = first(ws.allanWerle,     "allan_werle"))       return p;
    if (auto p = first(ws.t100,           "t100"))              return p;
    return std::nullopt;
}

bool Workspace::inputsAreValid() const {
    return danglingInputs().empty();
}

static void collectDangling(const std::string& configJson,
                            const std::vector<std::string>& allPaths,
                            std::vector<std::string>& out) {
    if (configJson.empty()) return;
    nlohmann::json cfg = nlohmann::json::parse(configJson, nullptr, false);
    if (cfg.is_discarded() || !cfg.is_object()) return;
    auto it = cfg.find("inputs");
    if (it == cfg.end() || !it->is_array()) return;
    for (const auto& p : *it) {
        if (!p.is_string()) continue;
        std::string path = p.get<std::string>();
        if (std::find(allPaths.begin(), allPaths.end(), path) == allPaths.end())
            out.push_back(path);
    }
}

std::vector<std::string> Workspace::danglingInputs() const {
    std::vector<std::string> all = allMemberPaths(*this);
    std::vector<std::string> out;
    for (const auto& m : uncorrectedIfg.members)
        if (m.kind == MemberKind::Derivative) collectDangling(m.config, all, out);
    for (const auto& m : correctedIfg.members)
        if (m.kind == MemberKind::Derivative) collectDangling(m.config, all, out);
    for (const auto& m : spectra.members)
        if (m.kind == MemberKind::Derivative) collectDangling(m.config, all, out);
    for (const auto& m : averageSpectra.members) collectDangling(m.config, all, out);
    for (const auto& m : snrSpectra.members)     collectDangling(m.config, all, out);
    for (const auto& m : allanWerle.members)     collectDangling(m.config, all, out);
    for (const auto& m : t100.members)           collectDangling(m.config, all, out);
    return out;
}

// ---- helpers ----

std::string makeUniqueId(const std::string& base, const std::vector<std::string>& existingIds) {
    if (std::find(existingIds.begin(), existingIds.end(), base) == existingIds.end())
        return base;
    int suffix = 2;
    for (;;) {
        std::string candidate = base + "_" + std::to_string(suffix);
        if (std::find(existingIds.begin(), existingIds.end(), candidate) == existingIds.end())
            return candidate;
        ++suffix;
    }
}

nlohmann::json makeOriginJson(const std::string& appName, const std::string& version) {
    return {
        {"timestamp", h5UtcNowIso()},
        {"application", appName},
        {"version", version},
    };
}
