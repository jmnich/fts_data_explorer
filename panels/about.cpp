#include "about.h"
#include "imgui.h"
#include <cstdio>
#include "version.h"
#include "app_state.h"
#include "theme.h"
#include "popup_utils.h"

static bool s_showAbout = false;

void openAboutPopup() {
    s_showAbout = true;
}

// The whole body lives in one read-only InputTextMultiline so its text can be
// selected with the mouse and copied with Ctrl+C / Ctrl+A (ImGui::Text is not
// selectable). Formatted once on first use because APP_VERSION is embedded.
// Layout assumes the default embedded font (ProggyClean: monospace, ASCII
// glyph range 0x20-0xFF) — the aligned label columns and library table rely
// on it; in a proportional font they degrade to slightly ragged left edges.
static char aboutBuf[4096];
static char* getAboutText() {
    static bool initialized = false;
    if (!initialized) {
        snprintf(aboutBuf, sizeof(aboutBuf),
            "Lightweight, fast scientific program for rapid exploration\n"
            "of raw data produced by Fourier spectrometers.\n"
            "\n"
            "\n"
            "  Version:      %s\n"
            "\n"
            "  Author:       Jakub Mnich\n"
            "  Affiliation:  Wroclaw University of Science and Technology, Poland\n"
            "  Email:        jakub.mnich@pwr.edu.pl\n"
            "  GitHub:       https://github.com/jmnich/fts_data_explorer\n"
            "  License:      GNU GPLv3\n"
            "                https://www.gnu.org/licenses/gpl-3.0.en.html\n"
            "\n"
            "\n"
            "\n"
            "  THIRD-PARTY LIBRARIES\n"
            "  ---------------------\n"
            "\n"
            "  %-19s%-19s%s\n"
            "  %-19s%-19s%s\n"
            "  %-19s%-19s%s\n"
            "  %-19s%-19s%s\n"
            "  %-19s%-19s%s\n"
            "  %-19s%-19s%s\n"
            "  %-19s%-19s%s\n"
            "  %-19s%-19s%s\n"
            "  %-19s%-19s%s\n"
            "\n"
            "\n"
            "\n"
            "  UNDERLYING LITERATURE\n"
            "  ---------------------\n"
            "\n"
            "  Norton-Beer apodization windows:\n"
            "      K. F. F. Ntokas, J. Ungermann, and M. Kaufmann, Norton-Beer apodization\n"
            "      and its Fourier transform, Journal of the Optical Society of America A,\n"
            "      vol. 40, p. 2026, Nov. 2023.\n"
            "\n"
            "  Apodization window formulas:\n"
            "      SciPy 1.0: Fundamental Algorithms for Scientific Computing in Python,\n"
            "      Nature Methods, vol. 17, pp. 261-272, 2020.\n"
            "\n"
            "  FFTW3 library (spectrum computation):\n"
            "      M. Frigo and S. G. Johnson, \"The design and implementation of FFTW3,\"\n"
            "      Proceedings of the IEEE, vol. 93, no. 2, pp. 216-231, 2005. Special\n"
            "      issue on Program Generation, Optimization, and Platform Adaptation.\n"
            "\n"
            "  Building spectrometers and X-axis correction:\n"
            "      J. Mnich, J. Kunsch, M. Budden, T. Gebert, M. Schossig, J. Sotor, and\n"
            "      L. A. Sterczewski, Ultra-broadband room-temperature Fourier transform\n"
            "      spectrometer with watt-level power consumption, Optics Express,\n"
            "      vol. 32, p. 45801, Dec. 2024.\n"
            "\n"
            "  General FTS handbook:\n"
            "      P. R. Griffiths and J. A. De Haseth, Fourier transform infrared\n"
            "      spectrometry. No. v. 171 in Chemical analysis, Hoboken, N.J:\n"
            "      Wiley-Interscience, 2nd ed., 2007.\n"
            "\n"
            "  HDF5 data format (workspace storage):\n"
            "      M. Folk, Q. Koziol, E. Pourmal, J. Johnson, D. Robinson, and H. Tang,\n"
            "      \"An overview of the HDF5 technology suite and its applications,\" in\n"
            "      Proceedings of the EDBT/ICDT 2011 Workshop on Array Databases,\n"
            "      ACM, 2011.\n"
            "\n"
            "  HITRAN database (gas absorption markers):\n"
            "      I. E. Gordon, L. S. Rothman, R. J. Hargreaves, F. M. Gomez,\n"
            "      T. Bertin, C. Hill, et al., \"The HITRAN2024 molecular spectroscopic\n"
            "      database\", J. Quant. Spectrosc. Radiat. Transfer 353, 109807 (2026).\n"
            "      DOI: 10.1016/j.jqsrt.2026.109807\n"
            "\n"
            "\n"
            "\n"
            "  FUNDING ACKNOWLEDGEMENT\n"
            "  -----------------------\n"
            "\n"
            "  This application is a universal tool developed for the TeraERC\n"
            "  project (European Research Council grant no. 101117433, ERC\n"
            "  Starting Grant). Views and opinions expressed are, however, those\n"
            "  of the authors only and do not necessarily reflect those of the\n"
            "  European Union or the European Research Council Executive Agency.\n"
            "  Neither the European Union nor the granting authority can be held\n"
            "  responsible for them.\n"
            "\n"
            "\n"
            "\n"
            "  AI DISCLOSURE\n"
            "  -------------\n"
            "\n"
            "  This application was developed with the use of coding agents\n"
            "  utilizing LLMs from various suppliers.\n",
            APP_VERSION,
            "Dear ImGui", "MIT", "github.com/ocornut/imgui",
            "ImPlot", "MIT", "github.com/epezent/implot",
            "ImPlot3D", "MIT", "github.com/brenocq/implot3d",
            "GLFW", "zlib/libpng", "github.com/glfw/glfw",
            "FFTW3", "GPL", "fftw.org",
            "tinyfiledialogs", "zlib/libpng", "sourceforge.net/projects/tinyfiledialogs",
            "stb_image", "MIT/public domain", "github.com/nothings/stb",
            "Nlohmann JSON", "MIT", "github.com/nlohmann/json",
            "HDF5", "BSD-3-Clause", "github.com/HDFGroup/hdf5");
        initialized = true;
    }
    return aboutBuf;
}

void renderAboutPopup() {
    if (s_showAbout) {
        ImGui::OpenPopup("About FTS Data Explorer");
        s_showAbout = false;
    }

    ImVec4 accent = GetAccentBase(StringToAccentColor(appState.currentAccentColor));
    beginModal(1200.0f, accent);
    if (ImGui::BeginPopupModal("About FTS Data Explorer", NULL,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar)) {

    ImGui::SetWindowSize(ImVec2(1200, 800));

    // NoTitleBar: the title moves into the body so removing the header loses
    // no information.
    ImGui::Text("About FTS Data Explorer");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Selectable content ─────────────────────────────────────────────────
    // Read-only multiline input instead of a Text + child: gives native
    // mouse-drag selection, double-click word select, Ctrl+A and Ctrl+C copy.
    // Styled to look like plain text: transparent frame (the multiline input
    // paints its child window with the current FrameBg), no border, no inner
    // padding so the text stays flush with the title above. The widget is a
    // scrolling child of its own, so the text scrolls even when it is not
    // focused. Note: while the input is active it consumes Escape (first
    // Escape deactivates the selection, second closes the popup).
    //
    // Width: a size.x of 0 would fall back to DC.ItemWidth, which popups
    // default to 65% of the window width (ItemWidthDefault, imgui.cpp Begin)
    // — pass the exact available width so the field fills the modal.
    const float contentWidth = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetStyleColorVec4(ImGuiCol_PopupBg));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::InputTextMultiline("##about", getAboutText(), sizeof(aboutBuf),
                              ImVec2(contentWidth, -(ImGui::GetFrameHeightWithSpacing() + 10)),
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    // ── Close button ───────────────────────────────────────────────────────
    ImGui::Separator();
    static int closeFocus = 0;
    static bool wasOpen = false;
    if (modalButtonRow({"Close"}, closeFocus, wasOpen, accent) == 0 ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        // Idle-render gate: without this the popup stays on screen until the
        // next event wakes the loop (mouse move) — the closing frame still
        // presents the popup (session_tab.cpp:74 pattern).
        appState.needsRedraw = true;
    }
    wasOpen = true;

    drawModalAccentFrame(accent);
    ImGui::EndPopup();
    }
    endModal();
}
