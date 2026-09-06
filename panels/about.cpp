#include "about.h"
#include "imgui.h"
#include "version.h"
#include "app_state.h"
#include "theme.h"
#include "popup_utils.h"

static bool s_showAbout = false;

void openAboutPopup() {
    s_showAbout = true;
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

    // ── Scrollable content ─────────────────────────────────────────────────
    ImGui::BeginChild("##AboutContent", ImVec2(0, -(ImGui::GetFrameHeightWithSpacing() + 10)),
                      ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    // ── Project info (easily editable) ─────────────────────────────────────
    ImGui::Text("Lightweight, fast scientific program for rapid exploration");
    ImGui::Text("of raw data produced by Fourier spectrometers.");
    ImGui::Text(" ");
    ImGui::Text("Version: %s", APP_VERSION);
    ImGui::Text(" ");
    ImGui::Text("Author: Jakub Mnich");
    ImGui::Text("Affiliation: Wroclaw University of Science and Technology, Poland");
    ImGui::Text("Email: jakub.mnich@pwr.edu.pl");
    ImGui::Text("GitHub: https://github.com/jmnich/fts_data_explorer");
    ImGui::Text(" ");
    ImGui::Text("Licensed under: GNU GPLv3");
    ImGui::Text("https://www.gnu.org/licenses/gpl-3.0.en.html");
    ImGui::Text(" ");
    ImGui::Separator();
    ImGui::Text(" ");
    // ── Third-party libraries (easily editable) ────────────────────────────
    ImGui::Text("Third-Party Libraries:");
    ImGui::BulletText("Dear ImGui (MIT) - github.com/ocornut/imgui");
    ImGui::BulletText("ImPlot (MIT) - github.com/epezent/implot");
    ImGui::BulletText("ImPlot3D (MIT) - github.com/brenocq/implot3d");
    ImGui::BulletText("GLFW (zlib/libpng) - github.com/glfw/glfw");
    ImGui::BulletText("FFTW3 (GPL) - fftw.org");
    ImGui::BulletText("tinyfiledialogs (zlib/libpng) - sourceforge.net/projects/tinyfiledialogs");
    ImGui::BulletText("stb_image (MIT/public domain) - github.com/nothings/stb");
    ImGui::BulletText("Nlohmann JSON (MIT) - github.com/nlohmann/json");
    ImGui::BulletText("HDF5 (BSD-3-Clause) - github.com/HDFGroup/hdf5");
    ImGui::Text(" ");
    ImGui::Separator();
    ImGui::Text(" ");
    // ── Papers ────────────────────────────
    ImGui::Text("Underlying literature");
    ImGui::BulletText("Norton-Beer apodization windows:");
    ImGui::Text("    K. F. F. Ntokas, J. Ungermann, and M. Kaufmann, Norton-Beer apodization");
    ImGui::Text("    and its Fourier transform, Journal of the Optical Society of America A, vol. 40,");
    ImGui::Text("    p. 2026, Nov. 2023.");

    ImGui::BulletText("Apodization window formulas:");
    ImGui::Text("    SciPy 1.0: Fundamental Algorithms for Scientific Computing in Python");
    ImGui::Text("    Nature Methods, vol. 17, pp. 261–272, 2020.");

    ImGui::BulletText("Building spectrometers and X-axis correction:");
    ImGui::Text("    J. Mnich, J. Kunsch, M. Budden, T. Gebert, M. Schossig, J. Sotor, and");
    ImGui::Text("    L. A. Sterczewski, Ultra-broadband room-temperature Fourier transform");
    ImGui::Text("    spectrometer with watt-level power consumption, Optics Express, vol. 32,");
    ImGui::Text("    p. 45801, Dec. 2024");

    ImGui::BulletText("General FTS handbook:");
    ImGui::Text("    P. R. Griffiths and J. A. De Haseth, Fourier transform infrared spectrometry.");
    ImGui::Text("    No. v. 171 in Chemical analysis, Hoboken, N.J: Wiley-Interscience, 2nd ed ed., 2007");
    ImGui::Text(" ");

    ImGui::BulletText("HDF5 data format (workspace storage):");
    ImGui::Text("    The HDF Group, Hierarchical Data Format, version 5 [Software]. The HDF Group.");
    ImGui::Text("    https://github.com/HDFGroup/hdf5, DOI: 10.5281/zenodo.17808614");
    ImGui::Text(" ");

    ImGui::BulletText("HITRAN database (gas absorption markers):");
    ImGui::Text("    I. E. Gordon, L. S. Rothman, R. J. Hargreaves, F. M. Gomez, T. Bertin, C. Hill,");
    ImGui::Text("    et al., \"The HITRAN2024 molecular spectroscopic database\",");
    ImGui::Text("    J. Quant. Spectrosc. Radiat. Transfer 353, 109807 (2026).");
    ImGui::Text("    DOI: 10.1016/j.jqsrt.2026.109807");
    ImGui::Text(" ");

    // ── Disclosure ────────────────────────────
    ImGui::Separator();
    ImGui::Text("AI disclosure:");
    ImGui::Text("This application was developped with the use of coding agents");
    ImGui::Text("utilizing LLMs from various suppliers.");

    ImGui::EndChild();

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
