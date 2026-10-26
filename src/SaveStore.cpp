#include "SaveStore.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

namespace
{

constexpr char kStateMagic[4] = {'P', 'B', 'R', 'S'};
constexpr uint32_t kStateVersion = 1;
constexpr int kThumbW = 128, kThumbH = 96;

std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

time_t ToTimeT(const FILETIME& ft)
{
    ULARGE_INTEGER v;
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return (time_t)((v.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

bool FileInfo(const std::string& path, time_t& when, uint64_t& size)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(Widen(path).c_str(), GetFileExInfoStandard, &d)) return false;
    when = ToTimeT(d.ftLastWriteTime);
    size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    return true;
}

bool ReadAll(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = _wfopen(Widen(path).c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    long long len = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out.resize(len > 0 ? (size_t)len : 0);
    bool ok = out.empty() || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// Writes to a temp file and swaps it in, so a crash never leaves half a file.
bool WriteAtomic(const std::string& path, const void* a, size_t alen, const void* b = nullptr, size_t blen = 0)
{
    std::wstring final = Widen(path), tmp = final + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(a, 1, alen, f) == alen;
    if (b && blen) ok &= fwrite(b, 1, blen, f) == blen;
    ok &= fflush(f) == 0;
    fclose(f);
    if (!ok || !MoveFileExW(tmp.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// 24-bit BMP thumbnail, the top screen halved with a 2x2 box filter.
bool WriteThumb(const std::string& path, const uint32_t* top)
{
    const int rowBytes = kThumbW * 3; // 384, already a multiple of 4
    std::vector<uint8_t> file(54 + rowBytes * kThumbH);
    uint8_t* h = file.data();
    auto put32 = [&](int off, uint32_t v) { memcpy(h + off, &v, 4); };
    h[0] = 'B'; h[1] = 'M';
    put32(2, (uint32_t)file.size());
    put32(10, 54);
    put32(14, 40);
    put32(18, kThumbW);
    put32(22, kThumbH); // positive: rows stored bottom-up
    h[26] = 1; h[28] = 24;
    put32(34, rowBytes * kThumbH);

    for (int y = 0; y < kThumbH; y++)
    {
        uint8_t* row = h + 54 + (kThumbH - 1 - y) * rowBytes;
        for (int x = 0; x < kThumbW; x++)
        {
            uint32_t r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++)
                {
                    uint32_t p = top[(y * 2 + dy) * 256 + x * 2 + dx];
                    r += (p >> 16) & 0xFF; g += (p >> 8) & 0xFF; b += p & 0xFF;
                }
            row[x * 3 + 0] = (uint8_t)(b / 4);
            row[x * 3 + 1] = (uint8_t)(g / 4);
            row[x * 3 + 2] = (uint8_t)(r / 4);
        }
    }
    return WriteAtomic(path, file.data(), file.size());
}

struct StateHeader
{
    char magic[4];
    uint32_t version;
    uint32_t romCrc;
    uint32_t length;
};

} // namespace

SaveStore::SaveStore(const std::string& exeDir, const std::string& game, uint32_t crc)
    : savPath(exeDir + game + ".sav"),
      rootDir(exeDir + "saves"),
      backupDir(exeDir + "saves\\backups"),
      stateDir(exeDir + "saves\\estados"),
      gameName(game),
      romCrc(crc)
{
    CreateDirectoryW(Widen(rootDir).c_str(), nullptr);
    CreateDirectoryW(Widen(backupDir).c_str(), nullptr);
    CreateDirectoryW(Widen(stateDir).c_str(), nullptr);
}

std::string SaveStore::FormatTime(time_t t)
{
    tm local{};
    localtime_s(&local, &t);
    char buf[32];
    strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &local);
    return buf;
}

std::vector<SaveStore::Backup> SaveStore::ListBackups() const
{
    std::vector<Backup> list;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(Widen(backupDir + "\\*.sav").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return list;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Backup b;
        b.path = backupDir + "\\" + Narrow(fd.cFileName);
        b.when = ToTimeT(fd.ftLastWriteTime);
        b.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        list.push_back(b);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(list.begin(), list.end(), [](const Backup& a, const Backup& b) { return a.when > b.when; });
    return list;
}

void SaveStore::BackupSav()
{
    time_t when;
    uint64_t size;
    if (!FileInfo(savPath, when, size) || size == 0) return;

    std::vector<Backup> list = ListBackups();
    if (!list.empty() && list[0].size == size)
    {
        std::vector<uint8_t> cur, last;
        if (ReadAll(savPath, cur) && ReadAll(list[0].path, last) && cur == last) return;
    }

    // Named after when that version of the save was written (CopyFile keeps the date).
    tm local{};
    localtime_s(&local, &when);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &local);
    std::string base = backupDir + "\\" + gameName + " " + stamp;
    std::string dest = base + ".sav";
    for (int i = 2; GetFileAttributesW(Widen(dest).c_str()) != INVALID_FILE_ATTRIBUTES; i++)
        dest = base + " (" + std::to_string(i) + ").sav";
    if (!CopyFileW(Widen(savPath).c_str(), Widen(dest).c_str(), TRUE)) return;

    list = ListBackups();
    for (size_t i = kMaxBackups; i < list.size(); i++)
        DeleteFileW(Widen(list[i].path).c_str());
}

SaveStore::Slot SaveStore::GetSlot(int slot) const
{
    Slot s;
    std::string name = slot == kUndoSlot ? "desfazer" : "estado" + std::to_string(slot);
    s.statePath = stateDir + "\\" + name + ".mst";
    s.thumbPath = stateDir + "\\" + name + ".bmp";
    uint64_t size;
    s.exists = FileInfo(s.statePath, s.when, size) && size > sizeof(StateHeader);
    return s;
}

bool SaveStore::WriteState(int slot, const void* data, size_t len, const uint32_t* top)
{
    Slot s = GetSlot(slot);
    StateHeader hdr;
    memcpy(hdr.magic, kStateMagic, 4);
    hdr.version = kStateVersion;
    hdr.romCrc = romCrc;
    hdr.length = (uint32_t)len;
    if (!WriteAtomic(s.statePath, &hdr, sizeof(hdr), data, len)) return false;
    if (top) WriteThumb(s.thumbPath, top);
    return true;
}

bool SaveStore::ReadState(int slot, std::vector<uint8_t>& out) const
{
    Slot s = GetSlot(slot);
    std::vector<uint8_t> file;
    if (!s.exists || !ReadAll(s.statePath, file) || file.size() < sizeof(StateHeader)) return false;
    StateHeader hdr;
    memcpy(&hdr, file.data(), sizeof(hdr));
    if (memcmp(hdr.magic, kStateMagic, 4) != 0 || hdr.version != kStateVersion || hdr.romCrc != romCrc ||
        hdr.length != file.size() - sizeof(hdr))
        return false;
    out.assign(file.begin() + sizeof(hdr), file.end());
    return true;
}

bool SaveStore::DeleteState(int slot)
{
    Slot s = GetSlot(slot);
    DeleteFileW(Widen(s.thumbPath).c_str());
    return DeleteFileW(Widen(s.statePath).c_str()) != 0;
}

void SaveStore::OpenFolder() const
{
    ShellExecuteW(nullptr, L"open", Widen(rootDir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
