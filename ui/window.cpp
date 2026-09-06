// GLFW/ImGui bootstrap (extracted from main.cpp, Phase-1 M1.2a).
#include "window.h"
#include "app_state.h"
#include "config.h"
#include "app_dirs.h"
#include "icon.h"
#include "stb_image.h"
#include "theme.h"
#include "version.h"
#include "welcome.h"
#include <imgui.h>
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "implot3d.h"
#include <GLFW/glfw3.h>
#include <iostream>
#include <cstring>
#include <cmath>
#include <vector>
#include <filesystem>

void SetupAxisTicksLimited(ImAxis axis, double min, double max, int maxTicks) {
    double range = max - min;
    if (range <= 0.0) return;

    double roughStep = range / (maxTicks - 1);
    double exponent = std::floor(std::log10(roughStep));
    double fraction = roughStep / std::pow(10.0, exponent);

    double niceFraction;
    if (fraction <= 1.0) niceFraction = 1.0;
    else if (fraction <= 2.0) niceFraction = 2.0;
    else if (fraction <= 5.0) niceFraction = 5.0;
    else niceFraction = 10.0;

    double step = niceFraction * std::pow(10.0, exponent);
    double firstTick = std::ceil(min / step) * step;

    std::vector<double> ticks;
    ticks.reserve(maxTicks);
    for (double tick = firstTick; tick <= max + step * 0.5; tick += step) {
        // Snap to an exact step multiple (see panels/spectral_plot.cpp):
        // accumulated FP error otherwise yields labels like "-2.78e-17"
        // near zero. `t + 0.0` normalizes a snapped -0 back to +0.
        double t = std::round(tick / step) * step;
        ticks.push_back(t + 0.0);
    }

    if (!ticks.empty()) {
        ImPlot::SetupAxisTicks(axis, ticks.data(), ticks.size(), nullptr);
    }
}

static int s_iconW = 0, s_iconH = 0;
static unsigned char* s_iconTemplate = nullptr; // cached white-on-transparent RGBA

static void freeIconTemplate() {
    if (s_iconTemplate) {
        stbi_image_free(s_iconTemplate);
        s_iconTemplate = nullptr;
        s_iconW = s_iconH = 0;
    }
}

void applyWindowIcon(GLFWwindow* window, const ImVec4& accent) {
    if (!s_iconTemplate) {
        int ch;
        s_iconTemplate = stbi_load_from_memory(
            assets_icon_png, assets_icon_png_len, &s_iconW, &s_iconH, &ch, 4);
        if (!s_iconTemplate) {
            std::cerr << "Failed to decode icon PNG" << std::endl;
            return;
        }
    }

    int totalPixels = s_iconW * s_iconH;
    auto* pixels = (unsigned char*)malloc((size_t)totalPixels * 4);
    if (!pixels) return;

    // Dark background (matches app theme)
    memset(pixels, 26, (size_t)totalPixels * 4);

    // Tint waveform pixels with accent color
    // Boost saturation for icon visibility (1.5x, clamped)
    float sr = std::min(accent.x * 2.0f, 1.0f);
    float sg = std::min(accent.y * 2.0f, 1.0f);
    float sb = std::min(accent.z * 2.0f, 1.0f);
    int r = (int)(sr * 255.0f);
    int g = (int)(sg * 255.0f);
    int b = (int)(sb * 255.0f);
    for (int i = 0; i < totalPixels; i++) {
        int idx = i * 4;
        if (s_iconTemplate[idx + 3] > 0) {
            pixels[idx + 0] = (unsigned char)r;
            pixels[idx + 1] = (unsigned char)g;
            pixels[idx + 2] = (unsigned char)b;
            pixels[idx + 3] = s_iconTemplate[idx + 3];
        }
    }

    GLFWimage icon = { s_iconW, s_iconH, pixels };
    glfwSetWindowIcon(window, 1, &icon);
    free(pixels);
}

bool initializeApplication(AppConfig& config, GLFWwindow*& window) {
    std::cout << "FTS Data Explorer " << APP_VERSION << " - Starting application..." << std::endl;

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return false;
    }

    // Configure OpenGL context for better performance
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    // Enable hardware acceleration and prefer dedicated GPU
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);

    // Create window with saved settings
    {
        std::string title = std::string("FTS Data Explorer ") + APP_VERSION;
        window = glfwCreateWindow(config.windowWidth, config.windowHeight, title.c_str(), nullptr, nullptr);
    }
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return false;
    }

    // Set window position if saved (not centered)
    if (config.windowPosX != -1 && config.windowPosY != -1) {
        glfwSetWindowPos(window, config.windowPosX, config.windowPosY);
    }

    glfwMakeContextCurrent(window);

    glfwSwapInterval(1);

    glfwSetWindowUserPointer(window, &appState);

    // Install dirty-flag callbacks BEFORE ImGui GLFW init (ImGui wraps them)
    glfwSetCursorPosCallback(window, [](GLFWwindow* w, double, double) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int, int, int) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetScrollCallback(window, [](GLFWwindow* w, double xoffset, double yoffset) {
        auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(w));
        state->scrollAccumX += static_cast<float>(xoffset);
        state->scrollAccumY += static_cast<float>(yoffset);
        state->lastScrollEventTime = glfwGetTime();
        state->needsRedraw = true;
    });
    glfwSetKeyCallback(window, [](GLFWwindow* w, int, int, int, int) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetCharCallback(window, [](GLFWwindow* w, unsigned int) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetDropCallback(window, [](GLFWwindow* w, int, const char**) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow* w, int, int) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetWindowFocusCallback(window, [](GLFWwindow* w, int) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetWindowRefreshCallback(window, [](GLFWwindow* w) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetWindowPosCallback(window, [](GLFWwindow* w, int, int) {
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });
    glfwSetWindowCloseCallback(window, [](GLFWwindow* w) {
        glfwSetWindowShouldClose(w, GLFW_TRUE);
        // Wake the idle loop: the exit intercept in pollEvents needs a
        // rendered frame to show the save/cancel prompt (bugfix 5).
        static_cast<AppState*>(glfwGetWindowUserPointer(w))->needsRedraw = true;
    });

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Process the ENTIRE queued input batch every frame (bugfix: selector
    // toggles waited for mouse movement). With trickling on (default), a
    // click arriving while the loop idles is consumed one event per frame —
    // the release could sit in ImGui's queue after the idle gate re-engaged,
    // so the click only registered when the next GLFW event (typically a
    // mouse move) woke the loop. Wheel input is unaffected: the app's own
    // scroll accumulator + rate limiter (IMGUI_GUIDE §20) clamps to one
    // notch per frame regardless.
    io.ConfigInputTrickleEventQueue = false;

    ImGui::StyleColorsDark();

    // Initialize ImPlot context
    ImPlot::CreateContext();
    ImPlot3D::CreateContext();

    // Use a lighter grid color for improved visibility
    ImPlot::GetStyle().Colors[ImPlotCol_AxisGrid] = ImVec4(0.85f, 0.85f, 0.85f, 0.85f);
    ImPlot::GetStyle().MajorGridSize = ImVec2(2.5f, 2.5f);
    ImPlot::GetStyle().MinorGridSize = ImVec2(1.5f, 1.5f);
    ImPlot3D::GetStyle().Colors[ImPlot3DCol_AxisGrid] = ImVec4(0.85f, 0.85f, 0.85f, 0.85f);
    
    // Optimize for large datasets - disable anti-aliasing (will be conditionally applied)
    // ImGuiStyle& style = ImGui::GetStyle();
    // style.AntiAliasedLines = false;

    return true;
}

void handleWindowEvents(GLFWwindow* window, AppConfig& config) {
    // Track window state changes
    int newWidth, newHeight;
    glfwGetWindowSize(window, &newWidth, &newHeight);
    if (newWidth != config.windowWidth || newHeight != config.windowHeight) {
        config.windowWidth = newWidth;
        config.windowHeight = newHeight;
    }

    int newPosX, newPosY;
    glfwGetWindowPos(window, &newPosX, &newPosY);
    if (newPosX != config.windowPosX || newPosY != config.windowPosY) {
        config.windowPosX = newPosX;
        config.windowPosY = newPosY;
    }

    // Check if window is maximized
    config.windowMaximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED);
}

void handleUIScaling(ImGuiIO& io, float& uiScale, const std::string& currentUiSize, bool& uiSizeChanged) {
    if (uiSizeChanged) {
        float dpi_scale = io.DisplayFramebufferScale.x;
        
        // Update scale based on new UI size
        if (currentUiSize == "tiny") {
            uiScale = 0.75f;
        } else if (currentUiSize == "small") {
            uiScale = 0.9f;
        } else if (currentUiSize == "normal") {
            uiScale = 1.0f;
        } else if (currentUiSize == "large") {
            uiScale = 1.4f;
        } else if (currentUiSize == "huge") {
            uiScale = 1.8f;
        }
        
        // Reapply scaling (font only to avoid UI issues)
        io.FontGlobalScale = dpi_scale * uiScale;
        
        uiSizeChanged = false; // Reset flag
    }
}

/**
 * Clean up application resources
 * @param window GLFW window pointer
 */

void cleanupApplication(GLFWwindow* window) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot3D::DestroyContext();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    freeIconTemplate();
    
    glfwDestroyWindow(window);
    glfwTerminate();
}


// One-time post-window setup (extracted from main(), Phase-1 M1.2b).
void setupApplication(AppConfig& config, GLFWwindow* window) {
    // Get ImGui IO (context already created in initializeApplication)
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // ImGui ini + per-tab-type/per-workspace layout snapshots live next to
    // IniFilename (layout_persistence.cpp derives from it), so placing it in
    // appDataDir() keeps all of them out of the repo root / CWD.
    ensureAppDirs();
    static std::string iniPath = appDataDir() + "/imgui.ini";
    // Migration: if a repo-root imgui.ini exists (left by a pre-relocate
    // binary), move it to appDataDir. Overwrite an older appDataDir copy so
    // the most recent layout wins.
    if (std::filesystem::exists("imgui.ini")) {
        std::error_code ec;
        std::filesystem::rename("imgui.ini", iniPath, ec);
        if (ec) {
            // rename can fail cross-filesystem; fall back to copy+remove
            std::filesystem::copy_file("imgui.ini", iniPath,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) std::filesystem::remove("imgui.ini", ec);
        }
    }
    io.IniFilename = iniPath.c_str();

    ImGui::StyleColorsDark();

    // Apply accent theme
    ImGuiStyle& style = ImGui::GetStyle();
    ImPlotStyle& plotStyle = ImPlot::GetStyle();
    ApplyTheme(style, plotStyle, StringToAccentColor(appState.currentAccentColor));

    // Set window icon tinted with current accent color
    applyWindowIcon(window, GetAccentBase(StringToAccentColor(appState.currentAccentColor)));

    // Load welcome screen background texture
    initWelcomeBackground();

    // Customize colors to use black background
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_PopupBg] = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

    // Customize plot colors
    ImVec4 yellow_color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // Bright yellow
    ImVec4 purple_color = ImVec4(0.5f, 0.0f, 0.5f, 1.0f); // Purple for selection

    // Set plot colors
    style.Colors[ImGuiCol_PlotLines] = yellow_color;
    style.Colors[ImGuiCol_PlotLinesHovered] = yellow_color;
    style.Colors[ImGuiCol_PlotHistogram] = yellow_color;
    style.Colors[ImGuiCol_PlotHistogramHovered] = yellow_color;

    // Set ImPlot selection color to match our custom purple
    plotStyle.Colors[ImPlotCol_Selection] = purple_color;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 460");

    // Set up for high DPI displays AFTER backend initialization
    // Apply scaling based on UI size setting
    float dpi_scale = io.DisplayFramebufferScale.x;

    // Apply UI scaling based on selected size
    if (appState.currentUiSize == "tiny") {
        appState.uiScale = 0.75f;
    } else if (appState.currentUiSize == "small") {
        appState.uiScale = 0.9f;
    } else if (appState.currentUiSize == "normal") {
        appState.uiScale = 1.0f;
    } else if (appState.currentUiSize == "large") {
        appState.uiScale = 1.4f;
    } else if (appState.currentUiSize == "huge") {
        appState.uiScale = 1.8f;
    }

    // Apply the scaling (font only initially to avoid UI issues)
    io.FontGlobalScale = dpi_scale * appState.uiScale;

    // Session defaults from config (M4.5: no session exists at startup — the
    // defaults are applied to each NEW workspace tab at creation instead,
    // see applySessionDefaults in workspace_session.cpp).
    appState.showFPS = config.showFPS; // Load from config
    appState.showTimestamps = config.showTimestamps; // Load from config
    appState.gridAlpha = config.gridAlpha; // Load from config
    appState.currentAccentColor = config.accentColor; // Load accent color from config

    // Load docking layout flag from config (persisted so DockBuilder runs only once)
    appState.defaultLayoutApplied = config.defaultLayoutApplied;

    // If imgui.ini is missing (e.g. user deleted it to reset layout),
    // force DockBuilder to run regardless of config state.
    if (!std::filesystem::exists(io.IniFilename)) {
        appState.defaultLayoutApplied = false;
    }
}
