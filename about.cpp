#include "about.h"
#include "imgui.h"

static bool s_showAbout = false;

void openAboutPopup() {
    s_showAbout = true;
}

void renderAboutPopup() {
    if (s_showAbout) {
        ImGui::OpenPopup("About FTS Data Explorer");
        s_showAbout = false;
    }

    if (!ImGui::BeginPopupModal("About FTS Data Explorer", NULL,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        return;
    }

    ImGui::SetWindowSize(ImVec2(1200, 800));

    // ── Scrollable content ─────────────────────────────────────────────────
    ImGui::BeginChild("##AboutContent", ImVec2(0, -(ImGui::GetFrameHeightWithSpacing() + 10)),
                      ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    // ── Project info (easily editable) ─────────────────────────────────────
    ImGui::Text("Author: Jakub Mnich");
    ImGui::Text("Email: jakub.mnich@pwr.edu.pl");
    ImGui::Text("GitHub: https://github.com/jmnich/fts_data_explorer");
    ImGui::Text("Description: Scientific program for rapid exploration of");
    ImGui::Text("             raw data produced by Fourier spectrometers.");
    ImGui::Separator();

    // ── Third-party libraries (easily editable) ────────────────────────────
    ImGui::Text("Third-Party Libraries:");
    ImGui::BulletText("Dear ImGui (MIT) - github.com/ocornut/imgui");
    ImGui::BulletText("ImPlot (MIT) - github.com/epezent/implot");
    ImGui::BulletText("ImPlot3D (MIT) - github.com/brenocq/implot3d");
    ImGui::BulletText("GLFW (zlib/libpng) - github.com/glfw/glfw");
    ImGui::BulletText("FFTW3 (GPL) - fftw.org");
    ImGui::BulletText("tinyfiledialogs (zlib/libpng) - sourceforge.net/projects/tinyfiledialogs");
    ImGui::BulletText("stb_image (MIT/public domain) - github.com/nothings/stb");

    ImGui::EndChild();

    // ── Close button ───────────────────────────────────────────────────────
    ImGui::Separator();
    float closeBtnWidth = 120.0f;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - closeBtnWidth) * 0.5f);
    if (ImGui::Button("Close", ImVec2(closeBtnWidth, 0))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}
