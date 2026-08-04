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

bool memberPathExists(const Workspace& ws, const std::string& path) {
    auto slash = path.find('/', 1);
    if (path.empty() || path[0] != '/' || slash == std::string::npos) return false;
    std::string group = path.substr(1, slash - 1);
    std::string id = path.substr(slash + 1);
    auto has = [&](const auto& members) {
        for (const auto& m : members)
            if (m.id == id) return true;
        return false;
    };
    if (group == "igm_uncorrected_x") return has(ws.uncorrectedIfg.members);
    if (group == "igm_corrected_x")   return has(ws.correctedIfg.members);
    if (group == "spectra")           return has(ws.spectra.members);
    if (group == "average_spectra")   return has(ws.averageSpectra.members);
    if (group == "snr_spectra")       return has(ws.snrSpectra.members);
    if (group == "allan_werle")       return has(ws.allanWerle.members);
    if (group == "t100")              return has(ws.t100.members);
    return false;
}

bool memberPathIsStale(const Workspace& ws, const std::string& path) {
    auto slash = path.find('/', 1);
    if (path.empty() || path[0] != '/' || slash == std::string::npos) return false;
    std::string group = path.substr(1, slash - 1);
    std::string id = path.substr(slash + 1);
    auto staleOf = [&](const auto& members) {
        for (const auto& m : members)
            if (m.id == id) return m.stale;
        return false;
    };
    if (group == "igm_uncorrected_x") return staleOf(ws.uncorrectedIfg.members);
    if (group == "igm_corrected_x")   return staleOf(ws.correctedIfg.members);
    if (group == "spectra")           return staleOf(ws.spectra.members);
    if (group == "average_spectra")   return staleOf(ws.averageSpectra.members);
    if (group == "snr_spectra")       return staleOf(ws.snrSpectra.members);
    if (group == "allan_werle")       return staleOf(ws.allanWerle.members);
    if (group == "t100")              return staleOf(ws.t100.members);
    return false;
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

// t100 members carry their reference source in config.reference.path (decision 3).
// A non-empty path that names no member is dangling just like a missing input.
static void collectDanglingRefPath(const std::string& configJson,
                                   const std::vector<std::string>& allPaths,
                                   std::vector<std::string>& out) {
    if (configJson.empty()) return;
    nlohmann::json cfg = nlohmann::json::parse(configJson, nullptr, false);
    if (cfg.is_discarded() || !cfg.is_object()) return;
    auto ref = cfg.find("reference");
    if (ref == cfg.end() || !ref->is_object()) return;
    auto pathIt = ref->find("path");
    if (pathIt == ref->end() || !pathIt->is_string()) return;
    std::string path = pathIt->get<std::string>();
    if (!path.empty() && std::find(allPaths.begin(), allPaths.end(), path) == allPaths.end())
        out.push_back(path);
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
    for (const auto& m : t100.members) {
        collectDangling(m.config, all, out);
        collectDanglingRefPath(m.config, all, out);
    }
    return out;
}

// ---- stale bookkeeping (Phase 2) ----

// Does this member's config reference `path` in inputs or t100 reference.path?
static bool configReferences(const std::string& configJson, const std::string& path) {
    if (configJson.empty()) return false;
    nlohmann::json cfg = nlohmann::json::parse(configJson, nullptr, false);
    if (cfg.is_discarded() || !cfg.is_object()) return false;
    if (auto it = cfg.find("inputs"); it != cfg.end() && it->is_array()) {
        for (const auto& p : *it)
            if (p.is_string() && p.get<std::string>() == path) return true;
    }
    if (auto ref = cfg.find("reference"); ref != cfg.end() && ref->is_object()) {
        if (auto rp = ref->find("path"); rp != ref->end() && rp->is_string() &&
            rp->get<std::string>() == path) return true;
    }
    return false;
}

std::vector<std::string> Workspace::dependentsOf(const std::string& path) const {
    std::vector<std::string> out;
    auto check = [&](const auto& group, const std::string& groupName) {
        for (const auto& m : group.members) {
            if (m.kind != MemberKind::Derivative) continue;
            if (configReferences(m.config, path))
                out.push_back("/" + groupName + "/" + m.id);
        }
    };
    check(uncorrectedIfg, "igm_uncorrected_x");
    check(correctedIfg,   "igm_corrected_x");
    check(spectra,        "spectra");
    check(averageSpectra, "average_spectra");
    check(snrSpectra,     "snr_spectra");
    check(allanWerle,     "allan_werle");
    check(t100,           "t100");
    return out;
}

template <typename T>
static T* findMemberByPath(MemberGroup<T>& group, const std::string& groupName,
                           const std::string& path) {
    const std::string prefix = "/" + groupName + "/";
    if (path.rfind(prefix, 0) != 0) return nullptr;
    std::string id = path.substr(prefix.size());
    for (auto& m : group.members)
        if (m.id == id) return &m;
    return nullptr;
}

std::vector<std::string> markDependentsStale(Workspace& ws, const std::string& path) {
    std::vector<std::string> affected;
    std::vector<std::string> queue = ws.dependentsOf(path);
    while (!queue.empty()) {
        std::string p = queue.back();
        queue.pop_back();
        if (std::find(affected.begin(), affected.end(), p) != affected.end()) continue;

        MemberBase* m = nullptr;
        if (auto* mm = findMemberByPath(ws.uncorrectedIfg, "igm_uncorrected_x", p)) m = mm;
        else if (auto* mm = findMemberByPath(ws.correctedIfg, "igm_corrected_x", p)) m = mm;
        else if (auto* mm = findMemberByPath(ws.spectra, "spectra", p)) m = mm;
        else if (auto* mm = findMemberByPath(ws.averageSpectra, "average_spectra", p)) m = mm;
        else if (auto* mm = findMemberByPath(ws.snrSpectra, "snr_spectra", p)) m = mm;
        else if (auto* mm = findMemberByPath(ws.allanWerle, "allan_werle", p)) m = mm;
        else if (auto* mm = findMemberByPath(ws.t100, "t100", p)) m = mm;

        if (!m || m->stale) continue;
        m->stale = true;
        affected.push_back(p);
        for (auto& dep : ws.dependentsOf(p)) queue.push_back(dep);
    }
    return affected;
}

template <typename T>
static void pruneGroup(MemberGroup<T>& group) {
    group.members.erase(std::remove_if(group.members.begin(), group.members.end(),
        [](const T& m) { return m.kind == MemberKind::Derivative && m.stale; }),
        group.members.end());
}

Workspace Workspace::pruneStale() const {
    Workspace copy = *this;
    pruneGroup(copy.uncorrectedIfg);
    pruneGroup(copy.correctedIfg);
    pruneGroup(copy.spectra);
    pruneGroup(copy.averageSpectra);
    pruneGroup(copy.snrSpectra);
    pruneGroup(copy.allanWerle);
    pruneGroup(copy.t100);
    return copy;
}

std::vector<std::string> Workspace::staleCategories() const {
    auto hasStaleDeriv = [](const auto& members) {
        for (const auto& m : members)
            if (m.kind == MemberKind::Derivative && m.stale) return true;
        return false;
    };
    std::vector<std::string> out;
    if (hasStaleDeriv(spectra.members))        out.push_back("Spectra");
    if (hasStaleDeriv(averageSpectra.members)) out.push_back("Average spectrum");
    if (hasStaleDeriv(snrSpectra.members))     out.push_back("SNR spectrum");
    if (hasStaleDeriv(allanWerle.members))     out.push_back("Allan-Werle");
    if (hasStaleDeriv(t100.members))           out.push_back("100% T");
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
