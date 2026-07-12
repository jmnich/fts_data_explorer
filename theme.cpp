#include "theme.h"
#include <algorithm>

ImVec4 GetAccentBase(AccentColor color) {
    switch (color) {
        case AccentColor::DefaultBlue: return ImVec4(0.20f, 0.40f, 0.70f, 1.00f);
        case AccentColor::Green:       return ImVec4(0.20f, 0.55f, 0.30f, 1.00f);
        case AccentColor::Purple:      return ImVec4(0.50f, 0.25f, 0.60f, 1.00f);
        case AccentColor::Red:         return ImVec4(0.65f, 0.30f, 0.30f, 1.00f);
        case AccentColor::Brown:       return ImVec4(0.55f, 0.35f, 0.20f, 1.00f);
        case AccentColor::Cyan:        return ImVec4(0.20f, 0.50f, 0.55f, 1.00f);
        default:                       return ImVec4(0.20f, 0.40f, 0.70f, 1.00f);
    }
}

ImVec4 GetAccentHovered(AccentColor color) {
    ImVec4 base = GetAccentBase(color);
    return ImVec4(
        std::min(base.x + 0.15f, 1.0f),
        std::min(base.y + 0.15f, 1.0f),
        std::min(base.z + 0.15f, 1.0f),
        1.0f
    );
}

ImVec4 GetAccentActive(AccentColor color) {
    ImVec4 base = GetAccentBase(color);
    return ImVec4(
        std::max(base.x - 0.10f, 0.0f),
        std::max(base.y - 0.10f, 0.0f),
        std::max(base.z - 0.10f, 0.0f),
        1.0f
    );
}

ImVec4 GetAccentMuted(AccentColor color) {
    ImVec4 base = GetAccentBase(color);
    return ImVec4(base.x * 0.5f, base.y * 0.5f, base.z * 0.5f, 0.6f);
}

ImVec4 GetAccentVeryMuted(AccentColor color) {
    ImVec4 base = GetAccentBase(color);
    return ImVec4(base.x * 0.45f, base.y * 0.45f, base.z * 0.45f, 0.55f);
}

ImVec4 GetAccentSubtle(AccentColor color) {
    ImVec4 base = GetAccentBase(color);
    return ImVec4(base.x * 0.25f, base.y * 0.25f, base.z * 0.25f, 0.3f);
}

void ApplyTheme(ImGuiStyle& style, ImPlotStyle& plotStyle, AccentColor accent) {
    ImVec4 accentBase = GetAccentBase(accent);
    ImVec4 accentHovered = GetAccentHovered(accent);
    ImVec4 accentActive = GetAccentActive(accent);
    ImVec4 accentMuted = GetAccentMuted(accent);
    ImVec4 accentVeryMuted = GetAccentVeryMuted(accent);
    ImVec4 accentSubtle = GetAccentSubtle(accent);

    // Dark background colors (preserve existing black theme)
    ImVec4 blackBg = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    ImVec4 darkBg = ImVec4(0.08f, 0.08f, 0.08f, 1.0f);
    ImVec4 darkerBg = ImVec4(0.12f, 0.12f, 0.12f, 1.0f);

    // Buttons
    style.Colors[ImGuiCol_Button] = accentMuted;
    style.Colors[ImGuiCol_ButtonHovered] = accentHovered;
    style.Colors[ImGuiCol_ButtonActive] = accentActive;

    // Headers (tree nodes, selectables, menu items)
    style.Colors[ImGuiCol_Header] = accentMuted;
    style.Colors[ImGuiCol_HeaderHovered] = accentHovered;
    style.Colors[ImGuiCol_HeaderActive] = accentActive;

    // Checkmarks (checkbox tick, radio button circle)
    style.Colors[ImGuiCol_CheckMark] = accentBase;

    // Checkbox background when selected
    style.Colors[ImGuiCol_CheckboxSelectedBg] = accentMuted;

    // Sliders
    style.Colors[ImGuiCol_SliderGrab] = accentBase;
    style.Colors[ImGuiCol_SliderGrabActive] = accentHovered;

    // Frame backgrounds (input fields, combo boxes, plots, sliders, text input)
    style.Colors[ImGuiCol_FrameBg] = darkerBg;
    style.Colors[ImGuiCol_FrameBgHovered] = accentVeryMuted;
    style.Colors[ImGuiCol_FrameBgActive] = accentMuted;

    // Tabs (focused tab bar)
    style.Colors[ImGuiCol_Tab] = accentVeryMuted;
    style.Colors[ImGuiCol_TabHovered] = accentMuted;
    style.Colors[ImGuiCol_TabActive] = accentBase;
    style.Colors[ImGuiCol_TabSelected] = accentBase;
    style.Colors[ImGuiCol_TabSelectedOverline] = accentBase;

    // Tabs (unfocused/dimmed tab bar)
    style.Colors[ImGuiCol_TabDimmed] = ImVec4(accentVeryMuted.x * 0.6f, accentVeryMuted.y * 0.6f, accentVeryMuted.z * 0.6f, 1.0f);
    style.Colors[ImGuiCol_TabDimmedSelected] = accentVeryMuted;
    style.Colors[ImGuiCol_TabDimmedSelectedOverline] = accentBase;

    // Resize grips
    style.Colors[ImGuiCol_ResizeGrip] = accentVeryMuted;
    style.Colors[ImGuiCol_ResizeGripHovered] = accentMuted;
    style.Colors[ImGuiCol_ResizeGripActive] = accentBase;

    // Separators
    style.Colors[ImGuiCol_Separator] = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
    style.Colors[ImGuiCol_SeparatorHovered] = accentMuted;
    style.Colors[ImGuiCol_SeparatorActive] = accentBase;

    // Scrollbar
    style.Colors[ImGuiCol_ScrollbarBg] = blackBg;
    style.Colors[ImGuiCol_ScrollbarGrab] = accentMuted;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = accentHovered;
    style.Colors[ImGuiCol_ScrollbarGrabActive] = accentActive;

    // Title bar (docking)
    style.Colors[ImGuiCol_TitleBg] = blackBg;
    style.Colors[ImGuiCol_TitleBgActive] = accentVeryMuted;
    style.Colors[ImGuiCol_TitleBgCollapsed] = blackBg;

    // Menubar
    style.Colors[ImGuiCol_MenuBarBg] = darkBg;

    // Docking
    style.Colors[ImGuiCol_DockingPreview] = accentMuted;
    style.Colors[ImGuiCol_DockingEmptyBg] = blackBg;

    // Navigation
    style.Colors[ImGuiCol_NavHighlight] = accentBase;
    style.Colors[ImGuiCol_NavWindowingHighlight] = accentBase;
    style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);
    style.Colors[ImGuiCol_NavCursor] = accentBase;

    // Modal window dimming
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.8f);

    // Window backgrounds
    style.Colors[ImGuiCol_WindowBg] = blackBg;
    style.Colors[ImGuiCol_ChildBg] = blackBg;
    style.Colors[ImGuiCol_PopupBg] = darkBg;

    // Border
    style.Colors[ImGuiCol_Border] = ImVec4(0.2f, 0.2f, 0.2f, 1.0f);
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    // Text
    style.Colors[ImGuiCol_Text] = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    style.Colors[ImGuiCol_TextLink] = accentBase;
    style.Colors[ImGuiCol_TextSelectedBg] = accentMuted;

    // Table
    style.Colors[ImGuiCol_TableHeaderBg] = accentVeryMuted;
    style.Colors[ImGuiCol_TableBorderStrong] = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt] = accentSubtle;

    // Tree
    style.Colors[ImGuiCol_TreeLines] = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);

    // Drag & Drop
    style.Colors[ImGuiCol_DragDropTarget] = accentBase;
    style.Colors[ImGuiCol_DragDropTargetBg] = accentVeryMuted;

    // Unsaved marker
    style.Colors[ImGuiCol_UnsavedMarker] = accentBase;

    // Input text cursor
    style.Colors[ImGuiCol_InputTextCursor] = accentBase;

    // Plot colors (preserve yellow for data lines)
    style.Colors[ImGuiCol_PlotLines] = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogram] = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);

    // ============ ImPlot Colors ============

    // Plot frame background
    plotStyle.Colors[ImPlotCol_FrameBg] = blackBg;

    // Plot area background
    plotStyle.Colors[ImPlotCol_PlotBg] = blackBg;

    // Plot border
    plotStyle.Colors[ImPlotCol_PlotBorder] = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);

    // Legend
    plotStyle.Colors[ImPlotCol_LegendBg] = darkBg;
    plotStyle.Colors[ImPlotCol_LegendBorder] = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);
    plotStyle.Colors[ImPlotCol_LegendText] = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);

    // Title text
    plotStyle.Colors[ImPlotCol_TitleText] = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);

    // Inlay text (text inside plots)
    plotStyle.Colors[ImPlotCol_InlayText] = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);

    // Axis text (labels and ticks)
    plotStyle.Colors[ImPlotCol_AxisText] = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);

    // Axis grid
    plotStyle.Colors[ImPlotCol_AxisGrid] = ImVec4(0.2f, 0.2f, 0.2f, 1.0f);

    // Axis ticks
    plotStyle.Colors[ImPlotCol_AxisTick] = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);

    // Axis background (hover region)
    plotStyle.Colors[ImPlotCol_AxisBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    plotStyle.Colors[ImPlotCol_AxisBgHovered] = accentVeryMuted;
    plotStyle.Colors[ImPlotCol_AxisBgActive] = accentMuted;

    // Selection (box selection)
    plotStyle.Colors[ImPlotCol_Selection] = ImVec4(accentBase.x, accentBase.y, accentBase.z, 0.5f);

    // Crosshairs
    plotStyle.Colors[ImPlotCol_Crosshairs] = accentBase;
}

AccentColor GetAccentColorFromString(const std::string& str) {
    return StringToAccentColor(str);
}

std::string GetAccentConfigString(AccentColor color) {
    return AccentColorToConfigString(color);
}