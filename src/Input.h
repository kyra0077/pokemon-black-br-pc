#pragma once

#include <string>
#include <vector>

#include <SDL.h>

// Everything the player can bind. The first twelve match the DS key bits used
// by NDS::SetKeyMask, in the same order.
enum class Action
{
    A, B, Select, Start, Right, Left, Up, Down, R, L, X, Y,
    Overlay, FastForward, Fullscreen, Minimap,
    SaveState, LoadState, NextSlot, SavesMenu,
    Count
};

constexpr int kNumActions = (int)Action::Count;
constexpr int kNumDSKeys = 12;
constexpr size_t kMaxBindingsPerAction = 6;

struct Binding
{
    enum Type { Key, Button, Axis } type;
    int code; // SDL_Scancode, SDL_GameControllerButton or SDL_GameControllerAxis
    int dir;  // axes only: +1 or -1

    bool operator==(const Binding& o) const
    {
        return type == o.type && code == o.code && (type != Axis || dir == o.dir);
    }
    bool IsPad() const { return type != Key; }
};

class InputConfig
{
public:
    std::vector<Binding> binds[kNumActions];
    bool showMinimap = true; // preview of the bottom screen while the overlay is closed

    void SetDefaults();
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;

    // Adds the binding to `action`, taking it away from whatever action had it.
    void Add(Action action, const Binding& b);
    void Clear(Action action) { binds[(int)action].clear(); }

    bool Held(Action action, const Uint8* keyboard, SDL_GameController* pad, bool padOnly = false) const;
};

// Player-facing names (Portuguese).
const char* ActionLabel(Action action);
std::string BindingName(const Binding& b);

constexpr int kAxisPressThreshold = 16000;
