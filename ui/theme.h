#pragma once

#include <string>
#include "imgui.h"
#include "implot.h"

enum class AccentColor : int {
    DefaultBlue = 0,
    Green = 1,
    Purple = 2,
    Red = 3,
    Brown = 4,
    Cyan = 5,
    Count = 6
};

inline const char* AccentColorToString(AccentColor color) {
    switch (color) {
        case AccentColor::DefaultBlue: return "Default (Blue)";
        case AccentColor::Green:       return "Green";
        case AccentColor::Purple:      return "Purple";
        case AccentColor::Red:         return "Red";
        case AccentColor::Brown:       return "Brown";
        case AccentColor::Cyan:        return "Cyan";
        default:                       return "Default (Blue)";
    }
}

inline AccentColor StringToAccentColor(const std::string& str) {
    if (str == "green") return AccentColor::Green;
    if (str == "purple") return AccentColor::Purple;
    if (str == "red") return AccentColor::Red;
    if (str == "brown") return AccentColor::Brown;
    if (str == "cyan") return AccentColor::Cyan;
    return AccentColor::DefaultBlue;
}

inline const char* AccentColorToConfigString(AccentColor color) {
    switch (color) {
        case AccentColor::DefaultBlue: return "default";
        case AccentColor::Green:       return "green";
        case AccentColor::Purple:      return "purple";
        case AccentColor::Red:         return "red";
        case AccentColor::Brown:       return "brown";
        case AccentColor::Cyan:        return "cyan";
        default:                       return "default";
    }
}

// Helper functions for external use (e.g., welcome screen)
ImVec4 GetAccentBase(AccentColor color);
ImVec4 GetAccentHovered(AccentColor color);
ImVec4 GetAccentActive(AccentColor color);
ImVec4 GetAccentMuted(AccentColor color);
ImVec4 GetAccentVeryMuted(AccentColor color);
ImVec4 GetAccentDark(AccentColor color);   // deep accent fill (full alpha) — keeps white text readable
ImVec4 GetAccentSubtle(AccentColor color);

void ApplyTheme(ImGuiStyle& style, ImPlotStyle& plotStyle, AccentColor accent);
AccentColor GetAccentColorFromString(const std::string& str);
std::string GetAccentConfigString(AccentColor color);