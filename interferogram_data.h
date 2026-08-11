#pragma once

#include <vector>
#include <string>
#include <cerrno>
#include <cstdlib>
#include <charconv>
#include <type_traits>
#include <utility>

// Data structure to hold interferogram data
struct InterferogramData {
    std::vector<double> referenceDetector;
    std::vector<double> primaryDetector;
    std::vector<double> opdAxis;    // pre-corrected OPD axis in meters (empty if not available)
    std::string metadata;

    size_t dataSize() const {
        if (!primaryDetector.empty()) return primaryDetector.size();
        if (!referenceDetector.empty()) return referenceDetector.size();
        if (!opdAxis.empty()) return opdAxis.size();
        return 0;
    }
};

// Data type flags describing what the current workspace holds.
enum class DataType {
    UncorrectedDualIFG,   // dual detectors, needs Hilbert OPD correction
    CorrectedSingleIFG,   // OPD already in meters, single detector
    PrecomputedSpectra    // spectra only, no interferograms
};

struct DatasetInfo {
    DataType dataType;

    bool hasInterferograms      = false;
    bool hasReferenceChannel    = false;
    bool axisIsCorrected        = false;
    bool hasPrecomputedSpectra  = false;
    bool hasMetadataFile        = false;
};

namespace fts_parse_detail {
// std::from_chars for floating-point types requires libstdc++ >= GCC 11 or
// libc++ >= LLVM 14; older standard libraries only support integers. The
// primary template falls back to strtod (exact std::stod semantics) and the
// partial specialization is selected wherever floating-point from_chars
// exists. Detected via SFINAE so no toolchain version macros are needed.
template <typename T, typename = void>
struct ParseDoubleImpl {
    static bool parse(const char* begin, const char* end, double& out) {
        (void)end;
        char* pEnd = nullptr;
        errno = 0;
        out = std::strtod(begin, &pEnd);
        return pEnd != begin && errno != ERANGE;
    }
};
template <typename T>
struct ParseDoubleImpl<T, std::void_t<decltype(std::from_chars(
                              std::declval<const char*>(),
                              std::declval<const char*>(),
                              std::declval<T&>()))>> {
    static bool parse(const char* begin, const char* end, double& out) {
        auto res = std::from_chars(begin, end, out);
        return res.ec == std::errc() && res.ptr > begin;
    }
};
inline bool parseDouble(const char* begin, const char* end, double& out) {
    return ParseDoubleImpl<double>::parse(begin, end, out);
}
} // namespace fts_parse_detail

// Lock-free, locale-independent double parsing. std::stod/std::strtod are
// globally serialized in the Windows CRT (strtod takes a global locale lock),
// so parallel workers parsing many files concurrently get progressively
// slower with more threads. std::from_chars has no such lock and is faster.
// Mirrors std::stod semantics: skips leading whitespace, parses a prefix,
// returns false if no number could be parsed.
inline bool parseDoubleFromChars(const std::string& s, double& out) {
    const char* begin = s.data();
    const char* end = begin + s.size();
    while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
    // std::stod accepts a leading '+'; std::from_chars does not.
    if (begin < end && *begin == '+') ++begin;
    if (begin == end) return false;
    // libstdc++'s from_chars does not parse hexadecimal floats ("0x1p3");
    // it would silently parse just "0". Fall back to strtod for hex strings
    // (rare in practice; keeps exact std::stod parity on all platforms).
    if (begin + 1 < end && begin[0] == '0' && (begin[1] == 'x' || begin[1] == 'X')) {
        char* pEnd = nullptr;
        errno = 0;
        out = std::strtod(begin, &pEnd);
        return pEnd != begin && errno != ERANGE;
    }
    return fts_parse_detail::parseDouble(begin, end, out);
}
