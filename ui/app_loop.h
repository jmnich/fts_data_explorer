#pragma once

#include <string>

struct AppState;
struct AppConfig;
struct GLFWwindow;

// Encapsulated frame pipeline (Phase-1 M1.2b). The main loop reads like
// pseudocode:
//   pollEvents -> pollAsyncComputations -> scheduleRedraws ->
//   (idle gate) handleInput -> renderUI -> present
class AppLoop {
public:
    AppLoop(AppConfig& config, const std::string& configFilePath,
            GLFWwindow* window);
    // Runs one frame; returns false when the window should close.
    bool runFrame();

private:
    void pollEvents();             // GLFW poll + exit intercept
    void pollAsyncComputations();  // spectrum poll + 4 batch ticks
    void scheduleRedraws();        // FPS overlay + "Saved" toast keep-alive
    void handleInput();            // keyboard shortcuts + navigation + file load
    void renderUI();               // NewFrame + menu bar + welcome + dock + panels
    void present();                // Render/swap/deferred export

    AppConfig& config_;
    std::string configFilePath_;
    GLFWwindow* window_;
};
