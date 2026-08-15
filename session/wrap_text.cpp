// ImGui wrapper around wrapToLinesCore (declared in wrap_text.h): per-char
// widths via CalcTextSize. Lives in its own TU so both fts_data_explorer and
// fts_session_roundtrip can use it (the roundtrip harness doesn't compile
// session_tab.cpp). The header stays ImGui-free so the standalone playground
// test can include it without linking ImGui.
#include "wrap_text.h"

#include <imgui.h>

std::vector<std::string> wrapToLines(const std::string& text, float maxWidth,
                                     int maxLines) {
    return wrapToLinesCore(text, maxWidth, maxLines,
                           [](char c) {
                               return ImGui::CalcTextSize(&c, &c + 1).x;
                           },
                           ImGui::CalcTextSize("…").x);
}
