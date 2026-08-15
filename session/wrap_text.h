#pragma once

#include <string>
#include <vector>

// Pure word-wrap core: splits `text` into lines of <= maxWidth px using the
// per-char width callback, at most `maxLines` lines. '\n' is a hard break and
// never part of a line. When clamped, the last line is trimmed and suffixed
// with "…" (ellipsisWidth must include its own width). No ImGui dependency —
// the GUI feeds CalcTextSize in via the callback (see session_tab.cpp's
// wrapToLines). Tested by playground/tests/wrap_text/test_wrap.cpp.
inline std::vector<std::string> wrapToLinesCore(const std::string& text, float maxWidth,
                                                int maxLines, float (*charWidth)(char),
                                                float ellipsisWidth) {
    std::vector<std::string> lines;
    if (text.empty() || maxWidth <= 0.0f) return lines;
    size_t i = 0;
    while (i < text.size() && static_cast<int>(lines.size()) < maxLines) {
        size_t lastBreak = std::string::npos;
        float width = 0.0f;
        size_t j = i;
        while (j < text.size()) {
            if (text[j] == '\n') break;   // hard break: never part of a line
            const float w = charWidth(text[j]);
            if (width + w > maxWidth) break;
            width += w;
            if (text[j] == ' ') lastBreak = j;
            ++j;
        }
        std::string line;
        if (j >= text.size()) {
            line = text.substr(i);
            i = text.size();
        } else {
            const bool atSpace =
                (lastBreak != std::string::npos && lastBreak >= i);
            const size_t breakAt = atSpace ? lastBreak : j;
            line = text.substr(i, breakAt - i);
            // Space break: the space is in the line, skip it. Hard break:
            // the overflow char is not in the line — resume AT it (newlines
            // are skipped; they never occupy a line of their own).
            i = atSpace ? breakAt + 1
                        : (j > i ? (text[j] == '\n' ? j + 1 : j) : i + 1);
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    if (i < text.size() && !lines.empty()) {   // clamped: ellipsize last line
        std::string& last = lines.back();
        float wsum = 0.0f;
        for (char c : last) wsum += charWidth(c);
        while (last.size() > 1 && wsum + ellipsisWidth > maxWidth) {
            wsum -= charWidth(last.back());
            last.pop_back();
        }
        last += "…";
    }
    return lines;
}

// ImGui wrapper around wrapToLinesCore: per-char widths via CalcTextSize.
// Defined in session/session_tab.cpp (keeps this header ImGui-free so the
// standalone playground test can include it without linking ImGui).
std::vector<std::string> wrapToLines(const std::string& text, float maxWidth,
                                     int maxLines);
