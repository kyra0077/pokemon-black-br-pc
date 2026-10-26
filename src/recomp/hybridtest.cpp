// Headless check of the hybrid mode: boots the game, presses buttons on a
// fixed script (START through the intro and title, then A to advance), saves
// screenshots and prints how many calls ran as recompiled code.
//
// Usage: recomp_hybridtest <rom.nds> [--hybrid] [--all] [--frames N]
//        [--shots DIR] [--shot-every N] [--report-every N] [--start-frames N]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "NDS.h"
#include "NDSCart.h"
#include "Args.h"
#include "GPU.h"
#include "SPI_Firmware.h"

#include "App.h"
#include "hybrid.h"

using namespace melonDS;

std::string App::ExeDirectory() { return ""; }
void App::LogV(int level, const char* fmt, va_list args) { if (level >= 3) vfprintf(stderr, fmt, args); }
void App::OnEmuStop(Platform::StopReason) {}
void App::OnSaveWrite(const uint8_t*, uint32_t) {}

namespace
{

std::vector<u8> ReadFile(const char* path)
{
    std::vector<u8> data;
    FILE* f = fopen(path, "rb");
    if (!f) return data;
    fseek(f, 0, SEEK_END);
    data.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    fclose(f);
    return data;
}

u32 Crc32(const u8* p, size_t n, u32 crc = 0)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

void PutBE(std::vector<u8>& v, u32 x)
{
    for (int s = 24; s >= 0; s -= 8) v.push_back((u8)(x >> s));
}

void Chunk(std::vector<u8>& png, const char* type, const std::vector<u8>& data)
{
    PutBE(png, (u32)data.size());
    size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    PutBE(png, Crc32(&png[start], png.size() - start));
}

// Both screens stacked, as an uncompressed (stored-deflate) PNG.
void SavePng(const std::string& path, NDS& nds)
{
    const int w = 256, h = 384;
    int fb = nds.GPU.FrontBuffer;
    std::vector<u8> raw;
    for (int y = 0; y < h; y++)
    {
        const u32* line = nds.GPU.Framebuffer[fb][y / 192].get() + (y % 192) * w;
        raw.push_back(0); // filter: none
        for (int x = 0; x < w; x++)
        {
            u32 px = line[x];
            raw.push_back((u8)(px >> 16));
            raw.push_back((u8)(px >> 8));
            raw.push_back((u8)px);
        }
    }
    std::vector<u8> z = {0x78, 0x01};
    for (size_t off = 0; off < raw.size(); off += 65535)
    {
        size_t n = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + n == raw.size() ? 1 : 0);
        z.push_back((u8)n); z.push_back((u8)(n >> 8));
        z.push_back((u8)~n); z.push_back((u8)(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
    u32 a = 1, b = 0;
    for (u8 c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    PutBE(z, (b << 16) | a);

    std::vector<u8> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<u8> ihdr;
    PutBE(ihdr, w); PutBE(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    if (FILE* f = fopen(path.c_str(), "wb"))
    {
        fwrite(png.data(), 1, png.size(), f);
        fclose(f);
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <rom.nds> [--hybrid] [--all] [--frames N] [--shots DIR] [--shot-every N]"
                        " [--report-every N] [--start-frames N]\n", argv[0]);
        return 2;
    }
    bool hybrid = false, all = false;
    int frames = 3600, shotEvery = 300, reportEvery = 600, startFrames = 1500;
    std::string shots;
    for (int i = 2; i < argc; i++)
    {
        if (!strcmp(argv[i], "--hybrid")) hybrid = true;
        else if (!strcmp(argv[i], "--all")) hybrid = all = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shots") && i + 1 < argc) shots = argv[++i];
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) shotEvery = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--report-every") && i + 1 < argc) reportEvery = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--start-frames") && i + 1 < argc) startFrames = atoi(argv[++i]);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::vector<u8> rom = ReadFile(argv[1]);
    if (rom.empty()) { fprintf(stderr, "can't read %s\n", argv[1]); return 2; }

    Firmware firmware(0);
    auto& user = firmware.GetEffectiveUserData();
    user.Settings &= ~Firmware::Language::Reserved;
    user.Settings |= Firmware::Language::English;
    firmware.UpdateChecksums();
    NDSArgs args;
    args.Firmware = std::move(firmware);
    if (hybrid) args.JIT = std::nullopt; // recompiled calls hook into the interpreter
    auto nds = std::make_unique<NDS>(std::move(args), nullptr);
    auto cart = NDSCart::ParseROM(rom.data(), (u32)rom.size(), nullptr, std::nullopt);
    if (!cart) { fprintf(stderr, "bad rom\n"); return 2; }
    nds->SetNDSCart(std::move(cart));
    nds->Reset();
    nds->SetupDirectBoot("game.nds");
    nds->Start();
    if (hybrid) Hybrid::Enable(*nds, all);

    auto t0 = std::chrono::steady_clock::now();
    for (int f = 1; f <= frames; f++)
    {
        // START now and then through the intro and title, then A to advance text and
        // menus, with an occasional START (accepts the name entry screen).
        bool press = f < startFrames ? (f % 120) < 4 : (f % 24) < 3;
        u32 key = f < startFrames || (f / 24) % 10 == 9 ? 3 : 0;
        nds->SetKeyMask(press ? (0xFFF & ~(1u << key)) : 0xFFF);
        nds->RunFrame();
        if (!shots.empty() && shotEvery > 0 && f % shotEvery == 0)
        {
            char name[64];
            snprintf(name, sizeof(name), "/frame%05d.png", f);
            SavePng(shots + name, *nds);
        }
        if (hybrid && reportEvery > 0 && f % reportEvery == 0)
        {
            const Hybrid::Stats& s = Hybrid::GetStats();
            printf("frame %5d: %llu calls ran recompiled, %llu rolled back, %u functions used\n", f,
                   (unsigned long long)s.committed, (unsigned long long)s.aborted, s.used);
        }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("%d frames in %.1f s (%.1f fps)%s\n", frames, secs, frames / secs, hybrid ? " [hybrid]" : "");
    if (hybrid) printf("%s", Hybrid::Report(40).c_str());
    nds->Stop();
    return 0;
}
