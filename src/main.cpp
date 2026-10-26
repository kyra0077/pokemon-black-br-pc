// Native Windows frontend: boots the embedded ROM straight into a window,
// shows the top screen full size and keeps the bottom screen as a
// retractable touch overlay.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_syswm.h>

#include "NDS.h"
#include "NDSCart.h"
#include "Args.h"
#include "GPU.h"
#include "GPU3D_Soft.h"
#include "SPU.h"
#include "SPI_Firmware.h"

#include "App.h"
#include "ControlsMenu.h"
#include "Input.h"
#include "SavesMenu.h"
#include "SaveStore.h"
#include "Savestate.h"
#include "TextRenderer.h"
#include "rom_info.h"
#ifdef HYBRID_RECOMP
#include "hybrid.h"
#endif

#ifndef GAME_DISPLAY_NAME
#define GAME_DISPLAY_NAME "Pokemon - Black (BR)"
#endif

using namespace melonDS;

namespace
{

constexpr int kScreenW = 256;
constexpr int kScreenH = 192;
constexpr double kFrameRate = 33513982.0 / 560190.0; // native DS refresh, ~59.83 Hz
constexpr int kAudioRate = 48000;
constexpr int kAudioChunk = 512;
constexpr int kAudioHighWater = 1024;   // frames buffered before the emulator waits
constexpr float kOverlayHeight = 0.82f; // fraction of the game viewport
constexpr float kOverlaySlideSec = 0.14f;
constexpr int kStickDeadzone = 8000;
constexpr float kCursorSpeed = 260.0f;  // DS pixels per second at full deflection
constexpr Uint64 kSaveFlushDelayMs = 1500;
constexpr Uint64 kHintDurationMs = 7000;
constexpr Uint64 kHybridReportMs = 10000;

FILE* gLogFile = nullptr;

std::wstring Widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

void Fatal(const char* msg)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, GAME_DISPLAY_NAME, msg, nullptr);
}

float EaseOut(float t) { return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); }

// Scales the 256x192 source with nearest neighbour to the closest integer
// multiple, then lets the final draw filter linearly: sharp pixels without
// the uneven columns of plain nearest scaling.
struct SharpScreen
{
    SDL_Texture* source = nullptr;
    SDL_Texture* scaled = nullptr;
    int factor = 0;

    bool Init(SDL_Renderer* r)
    {
        source = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, kScreenW, kScreenH);
        if (!source) return false;
        SDL_SetTextureScaleMode(source, SDL_ScaleModeNearest);
        return true;
    }

    void Upload(const u32* pixels)
    {
        if (pixels) SDL_UpdateTexture(source, nullptr, pixels, kScreenW * 4);
    }

    void Draw(SDL_Renderer* r, const SDL_FRect& dst, Uint8 alpha = 255)
    {
        SDL_Texture* out = Prepare(r, dst);
        SDL_SetTextureBlendMode(out, alpha < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
        SDL_SetTextureAlphaMod(out, alpha);
        SDL_RenderCopyF(r, out, nullptr, &dst);
    }

    // Returns the texture to draw at dst (the integer-scaled copy when available).
    SDL_Texture* Prepare(SDL_Renderer* r, const SDL_FRect& dst)
    {
        int want = std::clamp((int)std::ceil(std::max(dst.w / kScreenW, dst.h / kScreenH)), 1, 8);
        if (want != factor || !scaled)
        {
            if (scaled) SDL_DestroyTexture(scaled);
            scaled = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_TARGET,
                                       kScreenW * want, kScreenH * want);
            factor = want;
            if (scaled) SDL_SetTextureScaleMode(scaled, SDL_ScaleModeLinear);
        }
        if (!scaled) return source;
        SDL_Texture* prev = SDL_GetRenderTarget(r);
        SDL_SetRenderTarget(r, scaled);
        SDL_RenderCopy(r, source, nullptr, nullptr);
        SDL_SetRenderTarget(r, prev);
        return scaled;
    }

    void Destroy()
    {
        if (scaled) SDL_DestroyTexture(scaled);
        if (source) SDL_DestroyTexture(source);
        scaled = source = nullptr;
    }
};

} // namespace

struct App::Impl : public SavesHost
{
    App* owner = nullptr;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SharpScreen top, bottom;
    SharpScreen mini; // separate copy: its scale differs from the overlay's

    // Minitela: live preview of the bottom screen while the overlay is closed.
    SDL_FRect miniRect{};
    float miniAnim = 0.0f;
    bool miniHover = false;
    bool minimapHeldPrev = false;
    Uint64 miniPulseUntil = 0;
    std::vector<u32> miniSamples;
    Uint64 lastMouseMove = 0;
    std::string iniPath;
    SDL_GameController* pad = nullptr;

    SDL_AudioDeviceID audioDev = 0;
    SDL_mutex* audioLock = nullptr;
    SDL_cond* audioCond = nullptr;
    std::atomic<bool> audioReady{false};
    s16 lastSample[2] = {0, 0};

    std::unique_ptr<NDS> nds;
    bool running = true;

    // Overlay (bottom screen) state.
    bool overlayOpen = false;
    float overlayAnim = 0.0f; // 0 hidden .. 1 fully shown
    SDL_FRect gameRect{}, overlayRect{};
    bool mouseTouch = false;
    bool padTouch = false;
    float cursorX = kScreenW / 2.0f, cursorY = kScreenH / 2.0f;
    bool showPadCursor = false;
    // Remappable controls (controles.ini) and the F1 screen that edits them.
    InputConfig input;
    std::unique_ptr<ControlsMenu> menu;
    std::unique_ptr<TextRenderer> text;
    bool overlayHeldPrev = false, fullscreenHeldPrev = false;
    bool suppressHeldInput = false;
    Uint64 hintUntil = 0;
    int touchX = 0, touchY = 0;

    // Save data.
    std::string savePath;
    std::vector<u8> saveData;
    bool saveDirty = false;
    Uint64 saveLastWrite = 0;

    // Save states, .sav backups and the F2 menu that manages them.
    std::unique_ptr<SaveStore> store;
    std::unique_ptr<SavesMenu> savesMenu;
    int quickSlot = 1;
    bool saveStateHeldPrev = false, loadStateHeldPrev = false, nextSlotHeldPrev = false, savesMenuHeldPrev = false;
    std::string toast;
    Uint64 toastUntil = 0;

    // --hybrid: whitelisted ARM9 functions run as statically recompiled code.
    bool hybrid = false, hybridAll = false;
    Uint64 hybridNextTitle = 0, hybridNextReport = 0;
    std::string hybridStatus;

    static void AudioCallback(void* user, Uint8* stream, int len)
    {
        auto* self = (Impl*)user;
        int frames = len / 4;
        s16* out = (s16*)stream;
        int got = 0;
        if (self->audioReady.load())
        {
            SDL_LockMutex(self->audioLock);
            got = self->nds->SPU.ReadOutput(out, frames);
            SDL_CondSignal(self->audioCond);
            SDL_UnlockMutex(self->audioLock);
        }
        if (got > 0)
        {
            self->lastSample[0] = out[got * 2 - 2];
            self->lastSample[1] = out[got * 2 - 1];
        }
        // Underrun: hold the last sample instead of clicking to silence.
        for (int i = std::max(got, 0); i < frames; i++)
        {
            out[i * 2] = self->lastSample[0];
            out[i * 2 + 1] = self->lastSample[1];
        }
    }

    void OpenAudio()
    {
        SDL_AudioSpec want{}, have{};
        want.freq = kAudioRate;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = kAudioChunk;
        want.callback = AudioCallback;
        want.userdata = this;
        audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!audioDev)
            Platform::Log(Platform::LogLevel::Warn, "audio: %s\n", SDL_GetError());
        audioLock = SDL_CreateMutex();
        audioCond = SDL_CreateCond();
    }

    void WaitForAudio()
    {
        SDL_LockMutex(audioLock);
        while (nds->SPU.GetOutputSize() >= kAudioHighWater)
        {
            if (SDL_CondWaitTimeout(audioCond, audioLock, 100) == SDL_MUTEX_TIMEDOUT) break;
        }
        SDL_UnlockMutex(audioLock);
    }

    void LoadSave()
    {
        savePath = store->SavPath();
        FILE* f = _wfopen(Widen(savePath).c_str(), L"rb");
        if (!f) return;
        _fseeki64(f, 0, SEEK_END);
        s64 len = _ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);
        if (len > 0 && len <= 16 * 1024 * 1024)
        {
            saveData.resize((size_t)len);
            if (fread(saveData.data(), 1, saveData.size(), f) != saveData.size()) saveData.clear();
        }
        fclose(f);
    }

    void FlushSave()
    {
        if (!saveDirty) return;
        std::wstring final = Widen(savePath);
        std::wstring tmp = final + L".tmp";
        FILE* f = _wfopen(tmp.c_str(), L"wb");
        if (!f) return;
        bool ok = fwrite(saveData.data(), 1, saveData.size(), f) == saveData.size();
        ok &= fflush(f) == 0;
        fclose(f);
        if (!ok) return;
        store->BackupSav(); // the version about to be replaced goes to saves\backups
        if (MoveFileExW(tmp.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            saveDirty = false;
    }

    void Toast(const std::string& msg, Uint64 ms = 2500)
    {
        toast = msg;
        toastUntil = SDL_GetTicks64() + ms;
    }

    const u32* TopScreen() const { return nds->GPU.Framebuffer[nds->GPU.FrontBuffer][0].get(); }

    // SavesHost: also used by the quick save/load keys.
    bool SaveStateTo(int slot) override
    {
        Savestate state;
        if (state.Error || !nds->DoSavestate(&state) || state.Error)
        {
            Toast("Não foi possível salvar o estado.");
            return false;
        }
        if (!store->WriteState(slot, state.Buffer(), state.Length(), TopScreen()))
        {
            Toast("Não foi possível gravar o arquivo do estado.");
            return false;
        }
        Toast("Estado salvo no espaço " + std::to_string(slot) + ".");
        return true;
    }

    bool LoadStateFrom(int slot) override
    {
        std::vector<u8> data;
        if (!store->ReadState(slot, data))
        {
            Toast("O espaço " + std::to_string(slot) + " está vazio ou o arquivo não é deste jogo.");
            return false;
        }

        // Keep the current moment: in memory to recover from a bad file, and on
        // disk as the "undo" slot in case the player loaded the wrong state.
        Savestate current;
        if (current.Error || !nds->DoSavestate(&current) || current.Error)
        {
            Toast("Não foi possível guardar o momento atual; nada foi carregado.");
            return false;
        }
        if (slot != SaveStore::kUndoSlot)
            store->WriteState(SaveStore::kUndoSlot, current.Buffer(), current.Length(), TopScreen());

        Savestate state(data.data(), (u32)data.size(), false);
        if (state.Error || !nds->DoSavestate(&state) || state.Error)
        {
            current.Rewind(false);
            nds->DoSavestate(&current);
            Toast("Esse estado não pôde ser carregado; o jogo continua de onde estava.");
            return false;
        }
        mouseTouch = padTouch = false;
        nds->ReleaseScreen();
        Toast(slot == SaveStore::kUndoSlot ? std::string("Voltou para antes do último carregamento.")
                                           : "Estado do espaço " + std::to_string(slot) + " carregado.");
        return true;
    }

    bool RestoreBackup(const std::string& path) override
    {
        NDSCart::CartCommon* cart = nds->GetNDSCart();
        std::vector<u8> data;
        FILE* f = _wfopen(Widen(path).c_str(), L"rb");
        if (!f || !cart) { if (f) fclose(f); return false; }
        _fseeki64(f, 0, SEEK_END);
        s64 len = _ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);
        if (len > 0) data.resize((size_t)len);
        bool ok = len > 0 && fread(data.data(), 1, data.size(), f) == data.size();
        fclose(f);
        if (!ok || data.size() != cart->GetSaveMemoryLength()) return false;

        FlushSave();                                   // write any pending in-game save first
        cart->SetSaveMemory(data.data(), (u32)data.size()); // marks it dirty through OnSaveWrite
        FlushSave();                                   // current .sav -> backups, restored copy -> .sav
        RestartConsole();
        Toast("Save restaurado; o jogo foi reiniciado.", 4000);
        return true;
    }

    // Boots the cart that is already inserted (startup and after restoring a save).
    void RestartConsole()
    {
        nds->Reset();
        // Built-in FreeBIOS has no boot menu, so always boot the cart directly.
        nds->SetupDirectBoot(GAME_DISPLAY_NAME ".nds");

        time_t now = time(nullptr);
        tm local{};
        localtime_s(&local, &now);
        nds->RTC.SetDateTime(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                             local.tm_hour, local.tm_min, local.tm_sec);

        nds->Start();
        mouseTouch = padTouch = false;
    }

    bool BootConsole()
    {
        HRSRC res = FindResourceW(nullptr, L"ROM_DATA", MAKEINTRESOURCEW(10) /* RT_RCDATA */);
        HGLOBAL handle = res ? LoadResource(nullptr, res) : nullptr;
        const u8* rom = handle ? (const u8*)LockResource(handle) : nullptr;
        u32 romLen = res ? SizeofResource(nullptr, res) : 0;
        if (!rom || romLen != ROM_EMBEDDED_SIZE || memcmp(rom + 12, ROM_GAME_CODE, 4) != 0)
        {
            Fatal("Os dados do jogo embutidos no executavel estao ausentes ou corrompidos.");
            return false;
        }

        Firmware firmware(0);
        auto& user = firmware.GetEffectiveUserData();
        user.Settings &= ~Firmware::Language::Reserved;
        user.Settings |= Firmware::Language::English;
        firmware.UpdateChecksums();

        NDSArgs args;
        args.Firmware = std::move(firmware);
        args.OutputSampleRate = kAudioRate;
        args.Interpolation = AudioInterpolation::Cubic;
        if (hybrid) args.JIT = std::nullopt; // recompiled calls hook into the interpreter
        nds = std::make_unique<NDS>(std::move(args), owner);

        static_cast<SoftRenderer&>(nds->GPU.GetRenderer3D()).SetThreaded(true, nds->GPU);

        LoadSave();
        NDSCart::NDSCartArgs cartArgs;
        if (!saveData.empty())
        {
            cartArgs.SRAM = std::make_unique<u8[]>(saveData.size());
            memcpy(cartArgs.SRAM.get(), saveData.data(), saveData.size());
            cartArgs.SRAMLength = (u32)saveData.size();
        }

        auto cart = NDSCart::ParseROM(rom, romLen, owner, std::move(cartArgs));
        if (!cart)
        {
            Fatal("Falha ao carregar o cartucho embutido.");
            return false;
        }
        nds->SetNDSCart(std::move(cart));
        RestartConsole();
#ifdef HYBRID_RECOMP
        if (hybrid) Hybrid::Enable(*nds, hybridAll);
#endif
        return true;
    }

#ifdef HYBRID_RECOMP
    std::string HybridStatus() const
    {
        const Hybrid::Stats& s = Hybrid::GetStats();
        char buf[160];
        snprintf(buf, sizeof(buf), "hibrido: %llu chamadas recompiladas (%.1f%%), %u funcoes",
                 (unsigned long long)s.committed, s.calls ? 100.0 * s.committed / s.calls : 0.0, s.used);
        return buf;
    }

    void WriteHybridReport()
    {
        std::wstring path = Widen(App::ExeDirectory() + GAME_DISPLAY_NAME " - hybrid.log");
        if (FILE* f = _wfopen(path.c_str(), L"w"))
        {
            fputs(Hybrid::Report(60).c_str(), f);
            fclose(f);
        }
    }

    // Counters in the window title (once a second) and the log file (every 10 s).
    void UpdateHybridStatus()
    {
        Uint64 now = SDL_GetTicks64();
        if (now >= hybridNextTitle)
        {
            hybridNextTitle = now + 1000;
            hybridStatus = HybridStatus();
            SDL_SetWindowTitle(window, (std::string(GAME_DISPLAY_NAME " - ") + hybridStatus).c_str());
            text->Clear(); // the renderer caches a texture per distinct string
        }
        if (now >= hybridNextReport)
        {
            hybridNextReport = now + kHybridReportMs;
            WriteHybridReport();
        }
    }
#endif

    void SetWindowIcon()
    {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info)) return;
        HINSTANCE inst = GetModuleHandleW(nullptr);
        HWND hwnd = info.info.win.window;
        auto big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
        auto small = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        if (big) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
        if (small) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
    }

    void ToggleFullscreen()
    {
        bool fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
        SDL_SetWindowFullscreen(window, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    }

    void SetOverlay(bool open)
    {
        overlayOpen = open;
        mouseTouch = padTouch = false;
        if (open)
        {
            cursorX = kScreenW / 2.0f;
            cursorY = kScreenH / 2.0f;
        }
        miniHover = false;
        UpdateCursor();
    }

    bool OverlayInteractive() const { return overlayOpen && overlayAnim > 0.98f; }

    void UpdateLayout()
    {
        int w, h;
        SDL_GetRendererOutputSize(renderer, &w, &h);
        float scale = std::min(w / (float)kScreenW, h / (float)kScreenH);
        gameRect.w = kScreenW * scale;
        gameRect.h = kScreenH * scale;
        gameRect.x = (w - gameRect.w) * 0.5f;
        gameRect.y = (h - gameRect.h) * 0.5f;

        overlayRect.h = std::round(gameRect.h * kOverlayHeight);
        overlayRect.w = std::round(overlayRect.h * kScreenW / kScreenH);
        overlayRect.x = std::round(gameRect.x + (gameRect.w - overlayRect.w) * 0.5f);
        float shownY = gameRect.y + (gameRect.h - overlayRect.h) * 0.5f;
        float hiddenY = (float)h + 8.0f;
        overlayRect.y = std::round(hiddenY + (shownY - hiddenY) * EaseOut(overlayAnim));

        // The minitela sits in the side bar when the window is wider than 4:3
        // (typical 16:9 fullscreen), so it doesn't cover the game; otherwise in
        // the game's top-right corner, clear of the dialogue box and HP bars.
        float margin = std::max(8.0f, std::round(h * 0.015f));
        float bar = gameRect.x;
        if (bar >= 150.0f)
        {
            miniRect.w = std::min(bar - margin * 2, std::round(h * 0.32f * kScreenW / kScreenH));
            miniRect.h = std::round(miniRect.w * kScreenH / kScreenW);
            miniRect.x = std::round(gameRect.x + gameRect.w + (bar - miniRect.w) * 0.5f);
            miniRect.y = std::round(gameRect.y + margin * 3);
        }
        else
        {
            miniRect.h = std::round(gameRect.h * 0.18f);
            miniRect.w = std::round(miniRect.h * kScreenW / kScreenH);
            miniRect.x = std::round(gameRect.x + gameRect.w - miniRect.w - margin);
            miniRect.y = std::round(gameRect.y + margin * 2.5f);
        }
    }

    // Window coordinates (mouse events) -> renderer pixels (layout).
    void ToRenderer(int wx, int wy, float& px, float& py)
    {
        int ww, wh, rw, rh;
        SDL_GetWindowSize(window, &ww, &wh);
        SDL_GetRendererOutputSize(renderer, &rw, &rh);
        px = wx * (rw / (float)std::max(ww, 1));
        py = wy * (rh / (float)std::max(wh, 1));
    }

    bool OverMinimap(int wx, int wy)
    {
        if (miniAnim < 0.5f) return false;
        float px, py;
        ToRenderer(wx, wy, px, py);
        return px >= miniRect.x && py >= miniRect.y && px < miniRect.x + miniRect.w && py < miniRect.y + miniRect.h;
    }

    // Window pixel -> DS touch coordinate; false when outside the overlay.
    bool MapToTouch(int wx, int wy, bool clamp, int& tx, int& ty)
    {
        float px, py;
        ToRenderer(wx, wy, px, py);
        float fx = (px - overlayRect.x) * kScreenW / overlayRect.w;
        float fy = (py - overlayRect.y) * kScreenH / overlayRect.h;
        bool inside = fx >= 0 && fy >= 0 && fx < kScreenW && fy < kScreenH;
        if (!inside && !clamp) return false;
        tx = std::clamp((int)fx, 0, kScreenW - 1);
        ty = std::clamp((int)fy, 0, kScreenH - 1);
        return true;
    }

    void OpenPad(int index)
    {
        if (pad || !SDL_IsGameController(index)) return;
        pad = SDL_GameControllerOpen(index);
    }

    void OpenMenu()
    {
        menu->Open();
        mouseTouch = padTouch = false;
        hintUntil = 0;
        UpdateCursor();
        if (audioDev) SDL_PauseAudioDevice(audioDev, 1);
    }

    void OpenSavesMenu()
    {
        FlushSave(); // so the backups list includes the latest in-game save
        savesMenu->Open(quickSlot);
        mouseTouch = padTouch = false;
        hintUntil = 0;
        UpdateCursor();
        if (audioDev) SDL_PauseAudioDevice(audioDev, 1);
    }

    bool AnyMenuOpen() const { return menu->IsOpen() || savesMenu->IsOpen(); }

    void OnMenuClosed()
    {
        suppressHeldInput = true; // e.g. the Enter that closed the menu is also DS Start
        quickSlot = savesMenu->QuickSlot();
        UpdateCursor();
        if (audioDev) SDL_PauseAudioDevice(audioDev, 0);
    }

    void HandleEvent(const SDL_Event& e)
    {
        if (savesMenu->HandleEvent(e, window, renderer))
        {
            if (!savesMenu->IsOpen()) OnMenuClosed();
            return;
        }
        if (menu->HandleEvent(e, window, renderer))
        {
            if (!menu->IsOpen()) OnMenuClosed();
            return;
        }

        switch (e.type)
        {
        case SDL_QUIT:
            running = false;
            break;

        case SDL_KEYDOWN:
            if (e.key.repeat) break;
            // F1, Esc and Alt+Enter are fixed so the player can't lock themselves out.
            if (e.key.keysym.scancode == SDL_SCANCODE_F1)
                OpenMenu();
            else if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
                SetOverlay(!overlayOpen);
            else if (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (e.key.keysym.mod & KMOD_ALT))
                ToggleFullscreen();
            showPadCursor = false;
            break;

        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button != SDL_BUTTON_LEFT || e.button.which == SDL_TOUCH_MOUSEID) break;
            showPadCursor = false;
            lastMouseMove = SDL_GetTicks64();
            if (OverlayInteractive())
            {
                if (MapToTouch(e.button.x, e.button.y, false, touchX, touchY))
                    mouseTouch = true;
                else
                    SetOverlay(false); // click outside the panel retracts it
            }
            else if (!overlayOpen && OverMinimap(e.button.x, e.button.y))
            {
                SetOverlay(true);
            }
            break;

        case SDL_MOUSEMOTION:
            lastMouseMove = SDL_GetTicks64();
            miniHover = !overlayOpen && OverMinimap(e.motion.x, e.motion.y);
            if (mouseTouch) MapToTouch(e.motion.x, e.motion.y, true, touchX, touchY);
            if (overlayOpen && !padTouch) showPadCursor = false;
            break;

        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) mouseTouch = false;
            break;

        case SDL_CONTROLLERDEVICEADDED:
            OpenPad(e.cdevice.which);
            break;

        case SDL_CONTROLLERDEVICEREMOVED:
            if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == e.cdevice.which)
            {
                SDL_GameControllerClose(pad);
                pad = nullptr;
                padTouch = false;
                for (int i = 0; i < SDL_NumJoysticks(); i++) OpenPad(i);
            }
            break;

        case SDL_CONTROLLERBUTTONDOWN:
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE)
                OpenMenu();
            else if (overlayOpen)
                showPadCursor = true;
            break;

        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) mouseTouch = false;
            break;
        }
    }

    static float StickValue(Sint16 v)
    {
        if (std::abs(v) < kStickDeadzone) return 0.0f;
        float f = (std::abs(v) - kStickDeadzone) / (32767.0f - kStickDeadzone);
        return std::copysign(std::min(f, 1.0f), (float)v);
    }

    // Edge-triggered hotkeys (overlay, fullscreen), polled so keys, buttons
    // and trigger axes all behave the same.
    void PollHotkeys()
    {
        const Uint8* kb = SDL_GetKeyboardState(nullptr);
        bool overlayHeld = input.Held(Action::Overlay, kb, pad);
        bool fullscreenHeld = input.Held(Action::Fullscreen, kb, pad);
        bool minimapHeld = input.Held(Action::Minimap, kb, pad);
        bool saveHeld = input.Held(Action::SaveState, kb, pad);
        bool loadHeld = input.Held(Action::LoadState, kb, pad);
        bool slotHeld = input.Held(Action::NextSlot, kb, pad);
        bool savesHeld = input.Held(Action::SavesMenu, kb, pad);
        if (!AnyMenuOpen())
        {
            if (overlayHeld && !overlayHeldPrev) SetOverlay(!overlayOpen);
            if (fullscreenHeld && !fullscreenHeldPrev) ToggleFullscreen();
            if (minimapHeld && !minimapHeldPrev)
            {
                input.showMinimap = !input.showMinimap;
                input.Save(iniPath);
            }
            if (saveHeld && !saveStateHeldPrev) SaveStateTo(quickSlot);
            if (loadHeld && !loadStateHeldPrev) LoadStateFrom(quickSlot);
            if (slotHeld && !nextSlotHeldPrev)
            {
                quickSlot = quickSlot % SaveStore::kSlots + 1;
                SaveStore::Slot s = store->GetSlot(quickSlot);
                Toast("Espaço rápido: " + std::to_string(quickSlot) +
                      (s.exists ? " (" + SaveStore::FormatTime(s.when) + ")" : " (vazio)"));
            }
            if (savesHeld && !savesMenuHeldPrev) OpenSavesMenu();
        }
        overlayHeldPrev = overlayHeld;
        fullscreenHeldPrev = fullscreenHeld;
        minimapHeldPrev = minimapHeld;
        saveStateHeldPrev = saveHeld;
        loadStateHeldPrev = loadHeld;
        nextSlotHeldPrev = slotHeld;
        savesMenuHeldPrev = savesHeld;
    }

    // The cursor shows while it's useful (overlay, menu) or right after the mouse moved.
    void UpdateCursor()
    {
        bool want = overlayOpen || AnyMenuOpen() || SDL_GetTicks64() - lastMouseMove < 2000;
        if ((SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE) != want)
            SDL_ShowCursor(want ? SDL_ENABLE : SDL_DISABLE);
        if (!want) miniHover = false;
    }

    // Flags a big change on the bottom screen (a new menu, a prompt...) so the
    // minitela can draw attention to it. Compares a sparse grid of pixels.
    void WatchBottomScreen(const u32* fb)
    {
        if (!fb) return;
        constexpr int kStep = 4;
        size_t n = (kScreenW / kStep) * (kScreenH / kStep);
        bool first = miniSamples.size() != n;
        if (first) miniSamples.assign(n, 0);
        size_t changed = 0, i = 0;
        for (int y = 0; y < kScreenH; y += kStep)
            for (int x = 0; x < kScreenW; x += kStep, i++)
            {
                u32 v = fb[y * kScreenW + x] & 0xFFFFFF;
                if (v != miniSamples[i]) changed++;
                miniSamples[i] = v;
            }
        if (!first && !overlayOpen && changed * 100 > n * 35)
            miniPulseUntil = SDL_GetTicks64() + 2500;
    }

    void DrawMinimap()
    {
        float a = EaseOut(miniAnim);
        Uint8 alpha = (Uint8)(a * (miniHover ? 255 : 225));
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        float border = std::max(2.0f, std::round(miniRect.h / 55.0f));
        SDL_FRect shadow{miniRect.x - border + 3, miniRect.y - border + 4, miniRect.w + border * 2, miniRect.h + border * 2};
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)(110 * a));
        SDL_RenderFillRectF(renderer, &shadow);

        // Border: grey, white on hover, pulsing blue after a big change.
        float r = 190, g = 192, b = 200;
        Uint64 now = SDL_GetTicks64();
        if (now < miniPulseUntil)
        {
            float t = 0.5f + 0.5f * std::sin(now * 0.012f);
            r += (110 - r) * t; g += (205 - g) * t; b += (255 - b) * t;
        }
        if (miniHover) r = g = b = 255;
        SDL_FRect frame{miniRect.x - border, miniRect.y - border, miniRect.w + border * 2, miniRect.h + border * 2};
        SDL_SetRenderDrawColor(renderer, (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)(alpha));
        SDL_RenderFillRectF(renderer, &frame);
        mini.Draw(renderer, miniRect, alpha);

        // Label with the first key that opens the panel.
        const auto& binds = input.binds[(int)Action::Overlay];
        std::string key;
        for (const Binding& bnd : binds)
            if (!bnd.IsPad()) { key = BindingName(bnd); break; }
        if (key.empty() && !binds.empty()) key = BindingName(binds[0]);
        std::string label = key.empty() ? "Tela de toque" : key + ": tela de toque";
        int th = std::max(12, (int)(miniRect.h / 8.5f));
        int tw = text->Measure(label, th);
        SDL_Rect pill{(int)(miniRect.x + miniRect.w - tw - th * 0.8f), (int)(miniRect.y - border - th * 1.5f),
                      tw + (int)(th * 0.8f), (int)(th * 1.35f)};
        pill.x = std::max(pill.x, (int)miniRect.x - (int)border);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)(170 * a));
        SDL_RenderFillRect(renderer, &pill);
        text->Draw(label, pill.x + (int)(th * 0.4f), pill.y + th / 10, th, SDL_Color{235, 235, 240, alpha});
    }

    u32 PollInput(float dt)
    {
        const Uint8* kb = SDL_GetKeyboardState(nullptr);
        bool down[kNumDSKeys] = {};
        bool any = false;
        for (int i = 0; i < kNumDSKeys; i++)
            any |= down[i] = input.Held((Action)i, kb, pad);
        if (suppressHeldInput)
        {
            if (any) return 0xFFF;
            suppressHeldInput = false;
        }

        enum { kA = (int)Action::A, kLeft = (int)Action::Left, kRight = (int)Action::Right,
               kUp = (int)Action::Up, kDown = (int)Action::Down };

        padTouch = false;
        if (pad)
        {
            // Whatever controller input is bound to DS "A" taps the touch screen while it's open.
            bool padA = input.Held(Action::A, kb, pad, true);

            float lx = StickValue(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX));
            float ly = StickValue(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY));
            float rx = StickValue(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX));
            float ry = StickValue(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY));

            if (OverlayInteractive())
            {
                // Either stick drives the stylus cursor; the "A" button touches.
                float mx = std::abs(lx) > std::abs(rx) ? lx : rx;
                float my = std::abs(ly) > std::abs(ry) ? ly : ry;
                if (mx != 0.0f || my != 0.0f) showPadCursor = true;
                cursorX = std::clamp(cursorX + mx * kCursorSpeed * dt, 0.0f, kScreenW - 1.0f);
                cursorY = std::clamp(cursorY + my * kCursorSpeed * dt, 0.0f, kScreenH - 1.0f);
                if (padA)
                {
                    down[kA] = input.Held(Action::A, kb, nullptr); // keyboard A still presses A
                    padTouch = true;
                    showPadCursor = true;
                    touchX = (int)cursorX;
                    touchY = (int)cursorY;
                }
            }
            else
            {
                // Left stick doubles as the D-pad while playing.
                if (lx < -0.5f) down[kLeft] = true;
                if (lx > 0.5f) down[kRight] = true;
                if (ly < -0.5f) down[kUp] = true;
                if (ly > 0.5f) down[kDown] = true;
            }
        }

        // The DS can't press opposite directions at once.
        if (down[kLeft] && down[kRight]) down[kLeft] = down[kRight] = false;
        if (down[kUp] && down[kDown]) down[kUp] = down[kDown] = false;

        u32 mask = 0xFFF;
        for (int i = 0; i < kNumDSKeys; i++)
            if (down[i]) mask &= ~(1u << i);
        return mask;
    }

    void Render()
    {
        int fb = nds->GPU.FrontBuffer;
        const u32* bottomFb = nds->GPU.Framebuffer[fb][1].get();
        top.Upload(nds->GPU.Framebuffer[fb][0].get());
        if (overlayAnim > 0.0f) bottom.Upload(bottomFb);
        if (miniAnim > 0.0f) mini.Upload(bottomFb);
        WatchBottomScreen(bottomFb);

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        top.Draw(renderer, gameRect);
        if (miniAnim > 0.0f) DrawMinimap();

        if (overlayAnim > 0.0f)
        {
            float a = EaseOut(overlayAnim);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)(150 * a));
            SDL_RenderFillRectF(renderer, &gameRect);

            float border = std::max(2.0f, std::round(overlayRect.h / 160.0f));
            SDL_FRect frame{overlayRect.x - border, overlayRect.y - border,
                            overlayRect.w + border * 2, overlayRect.h + border * 2};
            SDL_SetRenderDrawColor(renderer, 230, 230, 235, 255);
            SDL_RenderFillRectF(renderer, &frame);
            bottom.Draw(renderer, overlayRect);

            if (showPadCursor && OverlayInteractive())
            {
                float sx = overlayRect.x + (cursorX + 0.5f) * overlayRect.w / kScreenW;
                float sy = overlayRect.y + (cursorY + 0.5f) * overlayRect.h / kScreenH;
                float arm = overlayRect.h / 24.0f, th = std::max(2.0f, overlayRect.h / 200.0f);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
                SDL_FRect hs{sx - arm - 1, sy - th / 2 - 1, arm * 2 + 2, th + 2}, vs{sx - th / 2 - 1, sy - arm - 1, th + 2, arm * 2 + 2};
                SDL_RenderFillRectF(renderer, &hs);
                SDL_RenderFillRectF(renderer, &vs);
                Uint8 tint = padTouch ? 80 : 255; // red while pressing
                SDL_SetRenderDrawColor(renderer, 255, tint, tint, 255);
                SDL_FRect h2{sx - arm, sy - th / 2, arm * 2, th}, v2{sx - th / 2, sy - arm, th, arm * 2};
                SDL_RenderFillRectF(renderer, &h2);
                SDL_RenderFillRectF(renderer, &v2);
            }
        }

#ifdef HYBRID_RECOMP
        if (hybrid && !hybridStatus.empty())
        {
            // Same counters as the title bar, visible in fullscreen too.
            int w, h;
            SDL_GetRendererOutputSize(renderer, &w, &h);
            int th = std::max(12, h / 60);
            SDL_Rect box{th / 2, th / 2, text->Measure(hybridStatus, th) + th, th + th / 2};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
            SDL_RenderFillRect(renderer, &box);
            text->Draw(hybridStatus, box.x + th / 2, box.y + th / 4 - th / 8, th, SDL_Color{150, 230, 160, 255});
        }
#endif

        if (menu->IsOpen())
        {
            menu->Render(renderer, *text);
        }
        else if (savesMenu->IsOpen())
        {
            savesMenu->Render(renderer, *text);
        }
        else
        {
            int w, h;
            SDL_GetRendererOutputSize(renderer, &w, &h);
            int th = std::max(14, h / 40);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            if (SDL_GetTicks64() < hintUntil)
            {
                const char* hint = "F1: controles  ·  F2: saves e estados";
                int tw = text->Measure(hint, th);
                SDL_Rect box{th / 2, h - th * 2 - th / 2, tw + th, th * 2};
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
                SDL_RenderFillRect(renderer, &box);
                text->Draw(hint, box.x + th / 2, box.y + th / 2 - th / 8, th, SDL_Color{235, 235, 240, 255});
            }
            if (SDL_GetTicks64() < toastUntil)
            {
                int tw = text->Measure(toast, th);
                SDL_Rect box{(w - tw - th * 2) / 2, h - th * 4, tw + th * 2, th * 2};
                SDL_SetRenderDrawColor(renderer, 20, 30, 45, 220);
                SDL_RenderFillRect(renderer, &box);
                text->Draw(toast, w / 2, box.y + th / 2 - th / 8, th, SDL_Color{150, 215, 255, 255}, 1);
            }
        }
        SDL_RenderPresent(renderer);
    }

    int Run(int argc, char** argv)
    {
        bool windowed = false;
        for (int i = 1; i < argc; i++)
        {
            if (!strcmp(argv[i], "--windowed") || !strcmp(argv[i], "-w")) windowed = true;
#ifdef HYBRID_RECOMP
            else if (!strcmp(argv[i], "--hybrid")) hybrid = true;
            else if (!strcmp(argv[i], "--hybrid-all")) hybrid = hybridAll = true;
#endif
        }

        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
        {
            Fatal(SDL_GetError());
            return 1;
        }

        Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
        if (!windowed) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        window = SDL_CreateWindow(GAME_DISPLAY_NAME, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  kScreenW * 4, kScreenH * 4, flags);
        if (!window) { Fatal(SDL_GetError()); return 1; }
        SDL_SetWindowMinimumSize(window, kScreenW, kScreenH);
        SetWindowIcon();

        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
        if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);
        if (!renderer || !top.Init(renderer) || !bottom.Init(renderer) || !mini.Init(renderer))
        {
            Fatal(SDL_GetError());
            return 1;
        }
        SDL_ShowCursor(SDL_DISABLE);

        iniPath = App::ExeDirectory() + "controles.ini";
        if (!input.Load(iniPath))
        {
            input.SetDefaults();
            input.Save(iniPath); // leave an editable file next to the .exe
        }
        menu = std::make_unique<ControlsMenu>(input, iniPath);
        text = std::make_unique<TextRenderer>(renderer);
        store = std::make_unique<SaveStore>(App::ExeDirectory(), GAME_DISPLAY_NAME, ROM_EMBEDDED_CRC32);
        savesMenu = std::make_unique<SavesMenu>(*store, *this);
        hintUntil = SDL_GetTicks64() + kHintDurationMs;

        // Paint one black frame so the window doesn't flash white while booting.
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);

        for (int i = 0; i < SDL_NumJoysticks(); i++) OpenPad(i);

        OpenAudio();
        if (!BootConsole()) return 1;
        audioReady = true;
        if (audioDev) SDL_PauseAudioDevice(audioDev, 0);

        const double perfFreq = (double)SDL_GetPerformanceFrequency();
        const double frameTicks = perfFreq / kFrameRate;
        double nextFrame = (double)SDL_GetPerformanceCounter();
        Uint64 lastTick = SDL_GetPerformanceCounter();

        while (running)
        {
            SDL_Event e;
            while (SDL_PollEvent(&e)) HandleEvent(e);
            if (!running) break;

            Uint64 nowTick = SDL_GetPerformanceCounter();
            float dt = std::min((float)((nowTick - lastTick) / perfFreq), 0.1f);
            lastTick = nowTick;

            float step = dt / kOverlaySlideSec;
            overlayAnim = std::clamp(overlayAnim + (overlayOpen ? step : -step), 0.0f, 1.0f);
            bool miniShown = input.showMinimap && !overlayOpen;
            miniAnim = std::clamp(miniAnim + (miniShown ? step : -step), 0.0f, 1.0f);
            UpdateLayout();
            PollHotkeys();
            UpdateCursor();

            if (AnyMenuOpen())
            {
                // Paused: keep redrawing the last frame under the menu.
                Render();
                SDL_Delay(16);
                continue;
            }

            nds->SetKeyMask(PollInput(dt));
            if ((mouseTouch || padTouch) && OverlayInteractive())
                nds->TouchScreen((u16)touchX, (u16)touchY);
            else
                nds->ReleaseScreen();

            bool fastForward = input.Held(Action::FastForward, SDL_GetKeyboardState(nullptr), pad);
            nds->RunFrame();
            if (!running) break;
            Render();
#ifdef HYBRID_RECOMP
            if (hybrid) UpdateHybridStatus();
#endif

            if (saveDirty && SDL_GetTicks64() - saveLastWrite > kSaveFlushDelayMs)
                FlushSave();

            if (fastForward)
            {
                nextFrame = (double)SDL_GetPerformanceCounter();
            }
            else if (audioDev)
            {
                WaitForAudio();
            }
            else
            {
                nextFrame += frameTicks;
                double now = (double)SDL_GetPerformanceCounter();
                if (nextFrame < now - frameTicks * 4) nextFrame = now;
                while ((double)SDL_GetPerformanceCounter() < nextFrame)
                {
                    double left = (nextFrame - (double)SDL_GetPerformanceCounter()) / perfFreq;
                    if (left > 0.002) SDL_Delay((Uint32)((left - 0.001) * 1000));
                }
            }
        }

        audioReady = false;
        if (audioDev) SDL_CloseAudioDevice(audioDev);
        FlushSave();
#ifdef HYBRID_RECOMP
        if (hybrid) WriteHybridReport();
#endif
        if (menu->IsOpen()) menu->Close(); // keeps edits made right before quitting
        if (nds) nds->Stop();
        nds.reset();
        savesMenu.reset(); // owns thumbnail textures; must go before the renderer
        text.reset();
        top.Destroy();
        bottom.Destroy();
        mini.Destroy();
        if (pad) SDL_GameControllerClose(pad);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }
};

std::string App::ExeDirectory()
{
    static std::string dir = [] {
        std::wstring buf(MAX_PATH, L'\0');
        DWORD n;
        while ((n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size())) >= buf.size())
            buf.resize(buf.size() * 2);
        buf.resize(n);
        buf.erase(buf.find_last_of(L"\\/") + 1);
        int len = WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string out(len > 0 ? len - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), -1, out.data(), len, nullptr, nullptr);
        return out;
    }();
    return dir;
}

void App::LogV(int level, const char* fmt, va_list args)
{
    if (!fmt) return;
    // Only errors are kept on disk (melonDS levels: 0 debug, 1 info, 2 warn, 3 error).
    // Warnings are routine, e.g. the trimmed ROM being padded back to 256 MB.
    if (level < 3) return;
    char line[1024];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n <= 0 || line[0] == '\n') return;
    if (!gLogFile) gLogFile = _wfopen(Widen(ExeDirectory() + GAME_DISPLAY_NAME ".log").c_str(), L"w");
    if (gLogFile)
    {
        fputs(line, gLogFile);
        fflush(gLogFile);
    }
}

void App::OnEmuStop(Platform::StopReason reason)
{
    if (!impl) return;
    if (reason != Platform::StopReason::External)
        impl->running = false;
}

void App::OnSaveWrite(const uint8_t* data, uint32_t len)
{
    if (!impl) return;
    impl->saveData.assign(data, data + len);
    impl->saveDirty = true;
    impl->saveLastWrite = SDL_GetTicks64();
}

int App::Run(int argc, char** argv)
{
    Impl state;
    state.owner = this;
    impl = &state;
    int ret = state.Run(argc, argv);
    impl = nullptr;
    return ret;
}

int main(int argc, char** argv)
{
    SDL_SetMainReady();
    App app;
    return app.Run(argc, argv);
}
