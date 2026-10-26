#pragma once

#include <string>
#include <vector>

#include <SDL.h>

#include "SaveStore.h"
#include "TextRenderer.h"

// What the menu asks the running game to do.
class SavesHost
{
public:
    virtual ~SavesHost() = default;
    virtual bool SaveStateTo(int slot) = 0;
    virtual bool LoadStateFrom(int slot) = 0;
    virtual bool RestoreBackup(const std::string& path) = 0;
};

// "Saves" screen (F2): save-state slots with thumbnails, the automatic backups
// of the cartridge save, and a button that opens the folder in Explorer.
class SavesMenu
{
public:
    SavesMenu(SaveStore& store, SavesHost& host) : store(store), host(host) {}
    ~SavesMenu() { DropThumbs(); }

    bool IsOpen() const { return open; }
    void Open(int quickSlot);
    void Close();
    int QuickSlot() const { return quickSlot; }

    bool HandleEvent(const SDL_Event& e, SDL_Window* window, SDL_Renderer* renderer);
    void Render(SDL_Renderer* renderer, TextRenderer& text);

private:
    enum Tab { kStates, kBackups };
    enum Button { kNone = -1, kTabStates, kTabBackups, kSave, kLoad, kDelete, kRestore, kFolder, kClose,
                  kConfirmYes, kConfirmNo, kNumButtons };
    enum class Pending { None, Overwrite, Delete, Restore };

    void Refresh();
    void DropThumbs();
    void Activate(Button b);
    void Ask(Pending what, const std::string& question);
    void Confirm(bool yes);
    void Move(int dx, int dy);
    void SetStatus(const std::string& msg);
    int ItemAt(int x, int y) const;
    Button ButtonAt(int x, int y) const;
    int NumItems() const;
    void DrawButton(SDL_Renderer* r, TextRenderer& text, Button id, const std::string& label, int x, int y,
                    int w, int h, int textH, bool enabled);

    SaveStore& store;
    SavesHost& host;
    bool open = false;
    Tab tab = kStates;
    int selected = 1;  // slot (states tab, 0 = undo) or row (backups tab)
    int quickSlot = 1; // slot used by the quick save/load keys
    int scroll = 0;
    int visibleRows = 8;
    int hoverButton = kNone;

    Pending pending = Pending::None;
    std::string question;
    std::string status;
    Uint64 statusUntil = 0;

    SaveStore::Slot slots[SaveStore::kSlots + 1];
    std::vector<SaveStore::Backup> backups;
    SDL_Texture* thumbs[SaveStore::kSlots + 1] = {};
    bool thumbsStale = true;

    // Hit-test rectangles from the last Render (renderer pixels).
    std::vector<std::pair<SDL_Rect, int>> itemRects;
    SDL_Rect buttonRects[kNumButtons] = {};
    bool buttonEnabled[kNumButtons] = {};
};
