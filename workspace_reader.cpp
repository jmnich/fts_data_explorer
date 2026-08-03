#if FTS_BUILD_HDF5

#include "workspace_reader.h"

#include <algorithm>
#include <stdexcept>

namespace {

const InterferogramMember* findInGroup(const std::vector<InterferogramMember>& members,
                                       const std::string& id) {
    for (const auto& m : members)
        if (m.id == id) return &m;
    return nullptr;
}

const TwoColumnMember* findInGroup(const std::vector<TwoColumnMember>& members,
                                   const std::string& id, bool originalsOnly) {
    for (const auto& m : members) {
        if (m.id == id && (!originalsOnly || m.kind == MemberKind::Original))
            return &m;
    }
    return nullptr;
}

std::string memberIds(const std::vector<InterferogramMember>& members) {
    std::vector<std::string> ids;
    for (const auto& m : members) ids.push_back(m.id);
    std::string out;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) out += ", ";
        out += ids[i];
    }
    return out;
}

std::string memberIds(const std::vector<TwoColumnMember>& members,
                      bool originalsOnly) {
    std::vector<std::string> ids;
    for (const auto& m : members)
        if (!originalsOnly || m.kind == MemberKind::Original) ids.push_back(m.id);
    std::string out;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) out += ", ";
        out += ids[i];
    }
    return out;
}

std::string readMetadata(const MemberBase& m, const std::string& group) {
    std::string s = "ID: " + m.id + "\nKind: "
        + (m.kind == MemberKind::Original ? "Original" : "Derivative");
    if (!m.timestamp.empty()) s += "\nTimestamp: " + m.timestamp;
    if (!m.columns.empty()) {
        s += "\nColumns: ";
        for (size_t i = 0; i < m.columns.size(); ++i) {
            if (i) s += ", ";
            s += m.columns[i];
        }
    }
    if (!m.units.empty()) {
        s += "\nUnits: ";
        for (size_t i = 0; i < m.units.size(); ++i) {
            if (i) s += ", ";
            s += m.units[i];
        }
    }
    s += "\nGroup: " + group;
    return s;
}

} // namespace

DatasetInfo workspaceDatasetInfo(const Workspace& ws) {
    DatasetInfo info;
    info.adapterName = kHdfWorkspaceAdapter;
    info.hasInterferograms = ws.hasInterferograms();
    info.hasReferenceChannel = ws.hasReferenceChannel();
    info.axisIsCorrected = ws.axisIsCorrected();
    info.hasPrecomputedSpectra = ws.hasPrecomputedSpectra();
    info.hasMetadataFile = !ws.measurementComment.empty() || !ws.measurementConfig.empty();

    if (ws.hasReferenceChannel())
        info.dataType = DataType::UncorrectedDualIFG;
    else if (ws.axisIsCorrected())
        info.dataType = DataType::CorrectedSingleIFG;
    else
        info.dataType = DataType::PrecomputedSpectra;
    return info;
}

std::vector<std::string> workspaceFileList(const Workspace& ws) {
    std::vector<std::string> ids;
    if (!ws.correctedIfg.members.empty()) {
        for (const auto& m : ws.correctedIfg.members) ids.push_back(m.id);
    } else if (!ws.uncorrectedIfg.members.empty()) {
        for (const auto& m : ws.uncorrectedIfg.members) ids.push_back(m.id);
    } else {
        for (const auto& m : ws.spectra.members)
            if (m.kind == MemberKind::Original) ids.push_back(m.id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

InterferogramData workspaceRead(const Workspace& ws, const std::string& id) {
    // Active group priority: corrected > uncorrected > spectra originals.
    if (const auto* m = findInGroup(ws.correctedIfg.members, id)) {
        InterferogramData data;
        data.primaryDetector = m->col0;
        // ponytail: spec stores OPD in um, engine expects meters. Convert here,
        // not in the Workspace model.
        data.opdAxis.resize(m->col1.size());
        for (size_t i = 0; i < m->col1.size(); ++i)
            data.opdAxis[i] = m->col1[i] * 1e-6;
        data.metadata = readMetadata(*m, "igm_corrected_x");
        return data;
    }
    if (const auto* m = findInGroup(ws.uncorrectedIfg.members, id)) {
        InterferogramData data;
        data.referenceDetector = m->col0;
        data.primaryDetector = m->col1;
        data.metadata = readMetadata(*m, "igm_uncorrected_x");
        return data;
    }
    if (const auto* m = findInGroup(ws.spectra.members, id, /*originalsOnly=*/true)) {
        InterferogramData data;
        data.referenceDetector = m->x;
        data.primaryDetector = m->y;
        data.metadata = readMetadata(*m, "spectra");
        return data;
    }
    throw std::runtime_error("Unknown member: " + id);
}

#endif // FTS_BUILD_HDF5
