#pragma once

#include <string>

#include <SDL.h>

#include "Input.h"
#include "TextRenderer.h"

// In-game "Controles" screen (F1): lists every action with its bindings and
// lets the player add or clear keys, controller buttons and triggers.
class ControlsMenu
{
public:
    ControlsMenu(InputConfig& config, std::string iniPath) : cfg(config), path(std::move(iniPath)) {}

    bool IsOpen() const { return open; }
    void Open();
    void Close(); // saves controles.ini

    // Returns true when the event belongs to the menu.
    bool HandleEvent(const SDL_Event& e, SDL_Window* window, SDL_Renderer* renderer);
    void Render(SDL_Renderer* renderer, TextRenderer& text);

private:
    enum { kRowRestore = kNumActions, kRowBack, kNumRows };

    Action RowAction(int row) const;
    void Activate(int row);
    void ClearRow(int row);
    void Capture(const Binding& b);
    void SetStatus(const std::string& msg);
    int RowAt(int x, int y) const;
    void ToRenderer(SDL_Window* window, SDL_Renderer* renderer, int& x, int& y) const;

    InputConfig& cfg;
    std::string path;
    bool open = false;
    int selected = 0;
    int hover = -1;
    bool capturing = false;
    std::string status;
    Uint64 statusUntil = 0;

    // Layout from the last Render, used for mouse hit tests (renderer pixels).
    SDL_Rect rowRects[kNumRows] = {};
};
