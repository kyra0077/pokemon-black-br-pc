#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

// Files the player can manage from the "Saves" menu, all next to the .exe:
//   <game>.sav                        cartridge save (what the game itself writes)
//   saves\backups\<game> <date>.sav   previous versions of the .sav, newest kept
//   saves\estados\estadoN.mst/.bmp    save states with a thumbnail (0 = undo slot)
class SaveStore
{
public:
    static constexpr int kSlots = 6;      // regular slots 1..kSlots
    static constexpr int kUndoSlot = 0;   // state taken automatically before each load
    static constexpr size_t kMaxBackups = 30;

    SaveStore(const std::string& exeDir, const std::string& gameName, uint32_t romCrc);

    const std::string& SavPath() const { return savPath; }
    const std::string& RootDir() const { return rootDir; }

    // Copies the current .sav into the backups folder (skipped when it matches
    // the newest backup), then trims old backups.
    void BackupSav();

    struct Backup
    {
        std::string path;
        time_t when;
        uint64_t size;
    };
    std::vector<Backup> ListBackups() const; // newest first

    struct Slot
    {
        bool exists = false;
        time_t when = 0;
        std::string statePath, thumbPath;
    };
    Slot GetSlot(int slot) const;

    // `top` is the 256x192 top-screen framebuffer, saved as the thumbnail.
    bool WriteState(int slot, const void* data, size_t len, const uint32_t* top);
    bool ReadState(int slot, std::vector<uint8_t>& out) const; // false if missing or for another ROM
    bool DeleteState(int slot);

    void OpenFolder() const;

    static std::string FormatTime(time_t t);

private:
    std::string savPath, rootDir, backupDir, stateDir, gameName;
    uint32_t romCrc;
};
