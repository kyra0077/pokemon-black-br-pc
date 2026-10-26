// Differential test: every recompiled static-module function is run from the
// same machine state as melonDS's ARM9 interpreter (the reference), and the
// resulting registers, flags and memory are compared.
//
// Usage: recomp_difftest <rom.nds> [--frames N] [--only ADDR] [--verbose]
//        [--overlays build/recomp/overlays.txt [--overlay-limit N] [--only-overlays]]
//        [--unknown misses.txt] [--pu] [--passlist whitelist.txt]

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "NDS.h"
#include "NDSCart.h"
#include "Args.h"
#include "ARM.h"
#include "SPI_Firmware.h"

#include "App.h"

extern "C" {
#include "recomp_runtime.h"
extern const RecompEntry recomp_table[];
extern const unsigned recomp_table_size;
extern const RecompCodeRange recomp_code_ranges[];
extern const unsigned recomp_code_ranges_size;
}

using namespace melonDS;

// ---- App hooks needed by Platform.cpp ------------------------------------------

std::string App::ExeDirectory() { return ""; }
void App::LogV(int level, const char* fmt, va_list args) { if (level >= 3) vfprintf(stderr, fmt, args); }
void App::OnEmuStop(Platform::StopReason) {}
void App::OnSaveWrite(const uint8_t*, uint32_t) {}

// ---- test runtime for the recompiled side -----------------------------------------

namespace
{

// Return address handed to every tested function. It holds a real `b .` at the
// top of ITCM (unused by the game's code): melonDS sometimes runs two Thumb
// instructions per step, so the reference may execute it once before we look.
constexpr u32 kMagicReturn = 0x01FFFFF0;
constexpr u32 kMagicItcmOffset = kMagicReturn & 0x7FFF;
constexpr int kBudgetBlocks = 200000;
constexpr int kBudgetInstrs = 4000000;

struct Machine
{
    std::vector<u8> mainRam, itcm, dtcm;
    std::vector<u8> puMap; // privileged MPU permissions per 4 KB page (bit0 read, bit1 write)
    // Overlays loaded as a whole, by id. Bytes at an entry point aren't enough: an
    // overlay partly overwritten by another can still match at some addresses.
    std::vector<u8> resident;
    u32 mainMask = 0x3FFFFF;
    u32 itcmSize = 0, dtcmBase = 0, dtcmMask = 0;
};

Machine gSnap, gMem;
jmp_buf gAbort;
const char* gAbortReason = nullptr;
int gBudget = 0;
int gCallDepth = 0;
char* gStackBase = nullptr;
FILE* gUnknownLog = nullptr; // --unknown <file>: targets the dispatcher couldn't resolve
// --passlist <file>: static-module functions with no failing case (the hybrid runtime's whitelist)
FILE* gPassList = nullptr;
melonDS::NDS* gNds = nullptr;
bool gTrace = false; // --trace: log every dispatcher decision
// recomp_code_ranges indexed by 4 KB page of the canonical address (ITCM at
// 0x01FF8000, main RAM at 0x02000000), from kCodePageBase upwards.
constexpr u32 kCodePageBase = 0x01FF8000 >> 12;
std::vector<std::vector<unsigned>> gCodePages;

void IndexCodeRanges()
{
    gCodePages.assign((0x02400000 >> 12) - kCodePageBase, {});
    for (unsigned i = 0; i < recomp_code_ranges_size; i++)
    {
        const RecompCodeRange& r = recomp_code_ranges[i];
        for (u32 page = r.start >> 12; page <= (r.end - 1) >> 12; page++)
            if (page >= kCodePageBase && page - kCodePageBase < gCodePages.size())
                gCodePages[page - kCodePageBase].push_back(i);
    }
}

// Is this (canonical) address an instruction byte of code that's resident right now?
bool IsResidentCode(u32 addr)
{
    u32 page = addr >> 12;
    if (page < kCodePageBase || page - kCodePageBase >= gCodePages.size()) return false;
    for (unsigned i : gCodePages[page - kCodePageBase])
    {
        const RecompCodeRange& r = recomp_code_ranges[i];
        if (addr < r.start || addr >= r.end) continue;
        if (r.overlay < 0 || ((size_t)r.overlay < gMem.resident.size() && gMem.resident[r.overlay]))
            return true;
    }
    return false;
}

[[noreturn]] void Abort(const char* why)
{
    gAbortReason = why;
    longjmp(gAbort, 1);
}

u8* Map(u32 addr, bool write)
{
    // The game's own MPU setup forbids some pages (e.g. the DSi-only 0x02FFE000 area in
    // NTR mode); real hardware takes a data abort there, so the case can't be compared.
    if (!(gMem.puMap[addr >> 12] & (write ? 2 : 1))) Abort("MPU fault (game forbids this page)");
    // Writes over resident code (usually through a garbage pointer from the test's
    // inputs) would change what the reference executes but not the recompiled
    // code: not a comparable case. Mirrors are folded to the canonical address.
    if (addr < gMem.itcmSize)
    {
        if (write && IsResidentCode(0x01FF8000 | (addr & 0x7FFF))) Abort("write over code (self-modifying)");
        return &gMem.itcm[addr & 0x7FFF];
    }
    if ((addr & gMem.dtcmMask) == gMem.dtcmBase) return &gMem.dtcm[addr & 0x3FFF];
    if ((addr >> 24) == 0x02)
    {
        if (write && IsResidentCode(0x02000000 | (addr & gMem.mainMask))) Abort("write over code (self-modifying)");
        return &gMem.mainRam[addr & gMem.mainMask];
    }
    Abort("hardware/unmapped access");
}

// Hashes code bytes for the dispatcher's residency check without aborting the case.
bool PeekHash(u32 addr, u32 len, u32& out)
{
    u8 buf[64];
    if (len > sizeof(buf)) return false;
    for (u32 i = 0; i < len; i++)
    {
        u32 a = addr + i;
        if (a < gMem.itcmSize) buf[i] = gMem.itcm[a & 0x7FFF];
        else if ((a & gMem.dtcmMask) == gMem.dtcmBase) buf[i] = gMem.dtcm[a & 0x3FFF];
        else if ((a >> 24) == 0x02) buf[i] = gMem.mainRam[a & gMem.mainMask];
        else return false;
    }
    out = recomp_fnv1a(buf, len);
    return true;
}

// Several overlays can occupy the same address; the entry whose code bytes
// match what is in memory right now is the one that's loaded.
const RecompEntry* Lookup(u32 target)
{
    unsigned lo = 0, hi = recomp_table_size;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        if (recomp_table[mid].addr < target) lo = mid + 1;
        else hi = mid;
    }
    for (; lo < recomp_table_size && recomp_table[lo].addr == target; lo++)
    {
        const RecompEntry& e = recomp_table[lo];
        if (e.overlay >= 0 && ((size_t)e.overlay >= gMem.resident.size() || !gMem.resident[e.overlay]))
            continue;
        u32 h;
        if (PeekHash(target & ~3u, e.check_len, h) && h == e.check_hash)
            return &e;
    }
    return nullptr;
}

int gNeedOverlay = -1;

// The overlay id if exactly one overlay has a recompiled function at target, else -1.
int UniqueOverlayAt(u32 target)
{
    unsigned lo = 0, hi = recomp_table_size;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        if (recomp_table[mid].addr < target) lo = mid + 1;
        else hi = mid;
    }
    int owner = -1;
    for (; lo < recomp_table_size && recomp_table[lo].addr == target; lo++)
    {
        int ov = recomp_table[lo].overlay;
        if (ov < 0) return -1;
        if (owner >= 0 && owner != ov) return -1; // several overlays share it: ambiguous
        owner = ov;
    }
    return owner;
}

struct OverlayImage
{
    int id;
    u32 addr, bss;
    std::vector<u8> data;
};

std::vector<OverlayImage> gOverlays;

const OverlayImage* FindOverlay(int id)
{
    for (const auto& o : gOverlays)
        if (o.id == id) return &o;
    return nullptr;
}

// Copies an overlay (and its zeroed .bss) into a memory image. Only main-RAM
// overlays are supported; a few of this game's overlays live in VRAM.
bool PlaceOverlay(Machine& m, const OverlayImage& o)
{
    if ((o.addr >> 24) != 0x02 || (o.addr & m.mainMask) + o.data.size() + o.bss > m.mainRam.size())
        return false;
    u32 off = o.addr & m.mainMask;
    memcpy(&m.mainRam[off], o.data.data(), o.data.size());
    memset(&m.mainRam[off + o.data.size()], 0, o.bss);
    u32 end = o.addr + (u32)o.data.size() + o.bss;
    for (const OverlayImage& other : gOverlays)
    {
        u32 oend = other.addr + (u32)other.data.size() + other.bss;
        if ((size_t)other.id < m.resident.size() && other.addr < end && o.addr < oend)
            m.resident[other.id] = 0; // overwritten (at least partly)
    }
    if ((size_t)o.id < m.resident.size()) m.resident[o.id] = 1;
    return true;
}

// At snapshot time: an overlay is resident if nearly all of its image is in memory
// (its initialised data may have been modified since it was loaded).
bool LooksResident(const Machine& m, const OverlayImage& o)
{
    if ((o.addr >> 24) != 0x02 || o.data.empty()) return false;
    u32 off = o.addr & m.mainMask;
    if (off + o.data.size() > m.mainRam.size()) return false;
    size_t same = 0;
    for (size_t i = 0; i < o.data.size(); i++) same += m.mainRam[off + i] == o.data[i];
    return same * 100 >= o.data.size() * 97;
}

} // namespace

extern "C" {

u32 rt_read8(Cpu*, u32 a) { return *Map(a, false); }
u32 rt_read16(Cpu*, u32 a) { a &= ~1u; u16 v; memcpy(&v, Map(a, false), 2); return v; }
u32 rt_read32(Cpu*, u32 a) { a &= ~3u; u32 v; memcpy(&v, Map(a, false), 4); return v; }
void rt_write8(Cpu*, u32 a, u32 v) { *Map(a, true) = (u8)v; }
void rt_write16(Cpu*, u32 a, u32 v) { a &= ~1u; u16 x = (u16)v; memcpy(Map(a, true), &x, 2); }
void rt_write32(Cpu*, u32 a, u32 v) { a &= ~3u; memcpy(Map(a, true), &v, 4); }

void rt_call(Cpu* c, u32 target)
{
    const RecompEntry* e = Lookup(target);
    if (!e)
    {
        // A call into an overlay that isn't resident in this test image: if exactly one
        // overlay has a function there, the case is retried with that overlay loaded too.
        int owner = UniqueOverlayAt(target);
        if (owner >= 0)
        {
            gNeedOverlay = owner;
            Abort("needs another overlay");
        }
        if (gUnknownLog) fprintf(gUnknownLog, "%08X\n", target);
        Abort("indirect call to unknown target");
    }
    if (++gCallDepth > 512) Abort("call depth");
    if (gTrace) fprintf(stderr, "  dispatch %08X -> ov%d (r0=%08X)\n", target, e->overlay, c->r[0]);
    e->fn(c);
    gCallDepth--;
}

void rt_jump(Cpu* c, u32 target, u32)
{
    if ((target & ~1u) == kMagicReturn) return;
    rt_call(c, target);
}

void rt_swi(Cpu*, u32, u32) { Abort("swi"); }

u32 rt_get_psr(Cpu* c, int spsr)
{
    if (spsr) Abort("spsr");
    return (c->n << 31) | (c->z << 30) | (c->c << 29) | (c->v << 28) | (c->q << 27) | c->cpsr_ctrl;
}

void rt_set_psr(Cpu* c, u32 val, u32 mask, int spsr)
{
    if (spsr) Abort("spsr");
    if (mask & 8)
    {
        c->n = val >> 31 & 1; c->z = val >> 30 & 1; c->c = val >> 29 & 1; c->v = val >> 28 & 1; c->q = val >> 27 & 1;
    }
    if (mask & 1)
    {
        if ((val & 0x1F) != (c->cpsr_ctrl & 0x1F)) Abort("mode switch");
        c->cpsr_ctrl = (c->cpsr_ctrl & 0x20) | (val & 0xDF);
    }
}

// CP15 ids from the recompiler: cp<<16 | op1<<12 | CRn<<8 | CRm<<4 | op2.
// melonDS's CP15Read takes the low 12 bits in the same layout.
u32 rt_mrc(Cpu*, u32 id)
{
    if ((id >> 16) != 15 || (id & 0xF000)) Abort("coprocessor read");
    return gNds->ARM9.CP15Read(id & 0xFFF); // CP15 state never changes during a case
}

void rt_mcr(Cpu*, u32 id, u32)
{
    if ((id >> 16) != 15 || (id & 0xF000)) Abort("coprocessor write");
    u32 crn = (id >> 8) & 15, crm = (id >> 4) & 15, op2 = id & 7;
    if (crn != 7) Abort("cp15 config write");
    if ((crm == 0 && op2 == 4) || (crm == 8 && op2 == 2)) Abort("wait for interrupt");
    // Remaining c7 operations are cache/write-buffer maintenance: no effect on
    // flat memory, and melonDS doesn't emulate a data cache either.
}
void rt_unimpl(Cpu*, u32, u32) { Abort("unimplemented instruction"); }
void rt_exception_return(Cpu*, u32) { Abort("exception return"); }

void rt_return_check(Cpu* c, u32 popped, u32 entry_lr)
{
    if (gTrace && gCallDepth == 0 && (entry_lr & ~1u) == kMagicReturn)
    {
        fprintf(stderr, "  recomp top-level return: popped=%08X", popped);
        for (int i = 0; i < 15; i++) fprintf(stderr, " r%d=%08X", i, c->r[i]);
        fprintf(stderr, "\n");
    }
    // The reference would jump to whatever was popped; the recompiled code returns.
    if ((popped & ~1u) != (entry_lr & ~1u)) Abort("return address on the stack overwritten");
}

void rt_block_hook(Cpu*)
{
    if (--gBudget <= 0) Abort("budget");
    // Direct calls are plain C calls, so runaway recursion would overflow the host stack.
    char here;
    if (gStackBase - &here > 4 * 1024 * 1024) Abort("host stack depth");
}

} // extern "C"

// ---- reference side (melonDS interpreter) -------------------------------------------

namespace
{

struct Regs
{
    u32 r[16];
    u32 nzcv;
};

void LoadMelon(NDS& nds, const Machine& m)
{
    memcpy(nds.MainRAM, m.mainRam.data(), m.mainRam.size());
    memcpy(nds.ARM9.ITCM, m.itcm.data(), m.itcm.size());
    memcpy(nds.ARM9.DTCM, m.dtcm.data(), m.dtcm.size());
}

// Returns false if the reference didn't return within the budget.
bool RunMelon(NDS& nds, u32 entry, const Regs& in, u32 ctrl, Regs& out)
{
    ARMv5& cpu = nds.ARM9;
    for (int i = 0; i < 15; i++) cpu.R[i] = in.r[i];
    cpu.CPSR = (in.nzcv << 28) | (ctrl & ~0x20u) | 0x80;
    // CPSR was written directly (no mode-switch side effects), so point the
    // protection unit at the privileged map that System mode uses.
    cpu.PU_Map = cpu.PU_PrivMap;
    cpu.Halted = 0;
    cpu.IRQ = 0;
    cpu.JumpTo(entry);
    struct Step { u32 pc, sp, r0, cpsr; };
    Step history[16] = {};
    for (int n = 0; n < kBudgetInstrs; n++)
    {
        u32 pc = cpu.R[15] - ((cpu.CPSR & 0x20) ? 2 : 4);
        history[n % 16] = {pc | ((cpu.CPSR >> 5) & 1), cpu.R[13], cpu.R[0], cpu.CPSR};
        if (gTrace && (cpu.CPSR & 0x1F) != 0x1F && n > 0)
        {
            fprintf(stderr, "  reference left System mode; last steps, oldest first (pc|thumb sp r0 cpsr):\n");
            for (int k = std::min(n, 15); k >= 0; k--)
            {
                const Step& s = history[(n - k + 32) % 16];
                fprintf(stderr, "    %08X sp=%08X r0=%08X cpsr=%08X\n", s.pc, s.sp, s.r0, s.cpsr);
            }
            return false;
        }
        if ((pc & ~1u) == kMagicReturn)
        {
            for (int i = 0; i < 16; i++) out.r[i] = cpu.R[i];
            out.nzcv = cpu.CPSR >> 28;
            return true;
        }
        nds.ARM9Timestamp = 0;
        nds.ARM9Target = 1;
        cpu.Execute<CPUExecuteMode::Interpreter>();
    }
    return false;
}

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

struct Diff
{
    std::string text;
    bool any = false;
    void Add(const char* fmt, ...)
    {
        char buf[256];
        va_list a;
        va_start(a, fmt);
        vsnprintf(buf, sizeof(buf), fmt, a);
        va_end(a);
        if (text.size() < 600) text += buf;
        any = true;
    }
};

void CompareMem(Diff& d, const char* name, u32 base, const std::vector<u8>& mine, const u8* ref, size_t len)
{
    int shown = 0;
    for (size_t i = 0; i < len; i++)
    {
        if (mine[i] != ref[i])
        {
            if (shown++ < 4) d.Add(" %s[%08X] recomp=%02X ref=%02X;", name, (u32)(base + i), mine[i], ref[i]);
            else { d.any = true; break; }
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <rom.nds> [--frames N] [--only HEXADDR] [--verbose] [--overlays FILE]"
                        " [--overlay-limit N] [--only-overlays] [--unknown FILE] [--pu] [--passlist FILE]\n", argv[0]);
        return 2;
    }
    int frames = 600;
    u32 only = 0;
    bool verbose = false;
    bool dumpPU = false;
    std::string overlayList; // overlays.txt written by tools/recomp/recomp.py
    int overlayLimit = -1;
    bool onlyOverlays = false;
    for (int i = 2; i < argc; i++)
    {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = strtoul(argv[++i], nullptr, 16);
        else if (!strcmp(argv[i], "--verbose")) verbose = true;
        else if (!strcmp(argv[i], "--pu")) dumpPU = true;
        else if (!strcmp(argv[i], "--unknown") && i + 1 < argc) gUnknownLog = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--passlist") && i + 1 < argc) gPassList = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--overlays") && i + 1 < argc) overlayList = argv[++i];
        else if (!strcmp(argv[i], "--overlay-limit") && i + 1 < argc) overlayLimit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--only-overlays")) onlyOverlays = true;
        else if (!strcmp(argv[i], "--trace")) gTrace = true;
    }

    setvbuf(stdout, nullptr, _IONBF, 0);
    std::vector<u8> rom = ReadFile(argv[1]);
    if (rom.empty()) { fprintf(stderr, "can't read %s\n", argv[1]); return 2; }

    if (FILE* list = overlayList.empty() ? nullptr : fopen(overlayList.c_str(), "r"))
    {
        int id;
        unsigned addr, bss;
        char path[1024];
        while (fscanf(list, "%d %x %u %1023[^\n]", &id, &addr, &bss, path) == 4)
            gOverlays.push_back({id, addr, bss, ReadFile(path)});
        fclose(list);
    }

    NDSArgs args;
    args.JIT = std::nullopt; // the interpreter is the reference
    args.Firmware = Firmware(0);
    auto nds = std::make_unique<NDS>(std::move(args), nullptr);
    gNds = nds.get();
    auto cart = NDSCart::ParseROM(rom.data(), (u32)rom.size(), nullptr, std::nullopt);
    if (!cart) { fprintf(stderr, "bad rom\n"); return 2; }
    nds->SetNDSCart(std::move(cart));
    nds->Reset();
    nds->SetupDirectBoot("game.nds");
    nds->Start();

    fprintf(stderr, "booting %d frames for a realistic memory snapshot...\n", frames);
    for (int f = 0; f < frames; f++)
    {
        // Tap START now and then so the game gets past its intro.
        nds->SetKeyMask((f % 120) < 4 ? (0xFFF & ~(1u << 3)) : 0xFFF);
        nds->RunFrame();
    }

    gSnap.mainMask = nds->MainRAMMask;
    gSnap.mainRam.assign(nds->MainRAM, nds->MainRAM + nds->MainRAMMask + 1);
    gSnap.itcm.assign(nds->ARM9.ITCM, nds->ARM9.ITCM + 0x8000);
    gSnap.dtcm.assign(nds->ARM9.DTCM, nds->ARM9.DTCM + 0x4000);
    const u32 branchToSelf = 0xEAFFFFFE; // ARM `b .`
    memcpy(&gSnap.itcm[kMagicItcmOffset], &branchToSelf, 4);
    gSnap.itcmSize = nds->ARM9.ITCMSize;
    gSnap.dtcmBase = nds->ARM9.DTCMBase;
    gSnap.dtcmMask = nds->ARM9.DTCMMask;
    gSnap.puMap.assign(nds->ARM9.PU_PrivMap, nds->ARM9.PU_PrivMap + 0x100000);
    IndexCodeRanges();
    int maxOverlay = -1;
    for (const auto& o : gOverlays) maxOverlay = std::max(maxOverlay, o.id);
    gSnap.resident.assign(maxOverlay + 1, 0);
    int residentCount = 0;
    for (const auto& o : gOverlays)
        if (LooksResident(gSnap, o)) { gSnap.resident[o.id] = 1; residentCount++; }
    if (!gOverlays.empty()) fprintf(stderr, "snapshot: %d overlays resident\n", residentCount);
    Regs snapRegs{};
    for (int i = 0; i < 16; i++) snapRegs.r[i] = nds->ARM9.R[i];
    u32 ctrl = nds->ARM9.CPSR & 0xFF;
    // Both sides run in System mode. All of r0-r14 are loaded explicitly before
    // each case, so it doesn't matter which bank the snapshot was taken in.
    ctrl = (ctrl & ~0x1Fu) | 0x1F;
    u32 sp = nds->ARM9.R[13];
    fprintf(stderr, "snapshot: ITCM<%08X DTCM@%08X sp=%08X\n", gSnap.itcmSize, gSnap.dtcmBase, sp);
    if (dumpPU)
    {
        ARMv5& a9 = nds->ARM9;
        fprintf(stderr, "cpsr=%08X cp15ctrl=%08X datarw=%08X coderw=%08X map=%s\n", a9.CPSR, a9.CP15Control,
                a9.PU_DataRW, a9.PU_CodeRW, a9.PU_Map == a9.PU_PrivMap ? "priv" : "user");
        for (int i = 0; i < 8; i++)
            fprintf(stderr, "  region%d %08X\n", i, a9.PU_Region[i]);
        for (u32 page : {0x02000u, 0x023FFu, 0x02400u, 0x027FFu, 0x02FFEu, 0x02FFFu})
            fprintf(stderr, "  page %05X000 priv=%02X user=%02X\n", page, a9.PU_PrivMap[page], a9.PU_UserMap[page]);
    }

    const u32 variants[][4] = {
        {snapRegs.r[0], snapRegs.r[1], snapRegs.r[2], snapRegs.r[3]},
        {0, 0, 0, 0},
        {1, 2, 3, 4},
        {0x02200000, 0x02210000, 0x10, 0x20},
    };

    int pass = 0, fail = 0, skipped = 0, refTimeout = 0;
    int funcsPass = 0, funcsFail = 0, callerFrame = 0, depRetries = 0, overlaysNotRam = 0;
    std::vector<std::pair<std::string, int>> skipReasons;

    // Tests every function of one module (-1 = static) over a base memory image.
    auto testModule = [&](int module, const Machine& base) {
    for (unsigned t = 0; t < recomp_table_size; t++)
    {
        const RecompEntry& e = recomp_table[t];
        if (e.overlay != module) continue;
        if (only && (e.addr & ~1u) != only) continue;
        bool thumb = e.addr & 1;
        bool fnFailed = false, fnPassed = false;
        if (e.flags & RECOMP_FLAG_CALLER_FRAME)
        {
            callerFrame++;
            continue;
        }

        for (const auto& v : variants)
        {
            Regs in{};
            for (int i = 0; i < 13; i++) in.r[i] = snapRegs.r[i];
            for (int i = 0; i < 4; i++) in.r[i] = v[i];
            in.r[13] = (sp - 0x400) & ~7u; // leave the live frames below the snapshot SP alone
            in.r[14] = kMagicReturn;
            in.nzcv = 0x6; // Z and C set

            // Recompiled side. Calls into overlays that aren't resident get their
            // overlay added to the image (for both sides) and the case is rerun.
            const Machine* caseBase = &base;
            Machine withDeps;
            Cpu c{};
            for (int attempt = 0;; attempt++)
            {
                gMem = *caseBase;
                c = Cpu{};
                for (int i = 0; i < 15; i++) c.r[i] = in.r[i];
                c.n = in.nzcv >> 3 & 1; c.z = in.nzcv >> 2 & 1; c.c = in.nzcv >> 1 & 1; c.v = in.nzcv & 1;
                c.cpsr_ctrl = (ctrl & ~0x20u) | 0x80 | (thumb ? 0x20 : 0);
                gBudget = kBudgetBlocks;
                gCallDepth = 0;
                gAbortReason = nullptr;
                gNeedOverlay = -1;
                char stackMark;
                gStackBase = &stackMark;
                if (setjmp(gAbort) == 0)
                    e.fn(&c);
                if (!gAbortReason || gNeedOverlay < 0 || attempt >= 4) break;
                const OverlayImage* dep = FindOverlay(gNeedOverlay);
                if (caseBase != &withDeps) withDeps = base;
                if (!dep || !PlaceOverlay(withDeps, *dep)) break;
                if (module >= 0 && !withDeps.resident[module])
                {
                    // The dependency would overwrite the overlay under test: the two
                    // are never resident together, so there's nothing to compare.
                    gAbortReason = "needed overlay overlaps the tested one";
                    break;
                }
                caseBase = &withDeps;
                if (attempt == 0) depRetries++;
            }
            if (gAbortReason)
            {
                skipped++;
                bool found = false;
                for (auto& r : skipReasons)
                    if (r.first == gAbortReason) { r.second++; found = true; break; }
                if (!found) skipReasons.push_back({gAbortReason, 1});
                continue;
            }

            // Reference side.
            LoadMelon(*nds, *caseBase);
            Regs out{};
            if (!RunMelon(*nds, e.addr, in, ctrl, out))
            {
                if (gTrace)
                {
                    Diff d;
                    CompareMem(d, "ram", 0x02000000, gMem.mainRam, nds->MainRAM, gMem.mainRam.size());
                    CompareMem(d, "dtcm", gMem.dtcmBase, gMem.dtcm, nds->ARM9.DTCM, gMem.dtcm.size());
                    fprintf(stderr, "  memory recomp vs reference at timeout:%s\n", d.text.c_str());
                }
                if (refTimeout++ < 8 || verbose)
                    printf("TIMEOUT ov%d %08X%s: reference stuck at pc=%08X cpsr=%08X lr=%08X r0-r3=%08X %08X %08X %08X sp=%08X\n",
                           module, e.addr & ~1u, thumb ? "t" : "a", nds->ARM9.R[15], nds->ARM9.CPSR, nds->ARM9.R[14],
                           in.r[0], in.r[1], in.r[2], in.r[3], in.r[13]);
                continue;
            }

            Diff d;
            for (int i = 0; i < 14; i++)
                if (c.r[i] != out.r[i]) d.Add(" r%d recomp=%08X ref=%08X;", i, c.r[i], out.r[i]);
            u32 mine = (c.n << 3) | (c.z << 2) | (c.c << 1) | c.v;
            if (mine != out.nzcv) d.Add(" nzcv recomp=%X ref=%X;", mine, out.nzcv);
            CompareMem(d, "ram", 0x02000000, gMem.mainRam, nds->MainRAM, gMem.mainRam.size());
            CompareMem(d, "itcm", 0, gMem.itcm, nds->ARM9.ITCM, gMem.itcm.size());
            CompareMem(d, "dtcm", gMem.dtcmBase, gMem.dtcm, nds->ARM9.DTCM, gMem.dtcm.size());

            if (d.any)
            {
                fail++;
                if (!fnFailed)
                    printf("FAIL ov%d %08X%s (r0-r3 %08X %08X %08X %08X):%s\n", module, e.addr & ~1u, thumb ? "t" : "a",
                           in.r[0], in.r[1], in.r[2], in.r[3], d.text.c_str());
                fnFailed = true;
            }
            else
            {
                pass++;
                fnPassed = true;
                if (verbose) printf("ok   ov%d %08X%s\n", module, e.addr & ~1u, thumb ? "t" : "a");
            }
        }
        if (fnFailed) funcsFail++;
        else if (fnPassed)
        {
            funcsPass++;
            if (gPassList && module < 0) fprintf(gPassList, "%08X\n", e.addr);
        }
    }
    };

    if (!onlyOverlays) testModule(-1, gSnap);

    // Overlays: place each image (plus zeroed .bss) over the snapshot, then test its functions.
    int tested = 0;
    for (const OverlayImage& o : gOverlays)
    {
        if (overlayLimit >= 0 && tested >= overlayLimit) break;
        Machine m = gSnap;
        if (!PlaceOverlay(m, o))
        {
            overlaysNotRam++;
            continue;
        }
        int before = pass + fail;
        testModule(o.id, m);
        tested++;
        fprintf(stderr, "overlay %3d: %d cases compared (%d pass, %d fail so far)\n", o.id, pass + fail - before, pass, fail);
    }

    printf("\ncases: %d pass, %d fail, %d skipped (recomp side), %d reference timeouts\n", pass, fail, skipped, refTimeout);
    printf("functions: %d fully passing, %d with failures, %d not testable alone (return through caller's frame)\n",
           funcsPass, funcsFail, callerFrame);
    printf("overlays: %d tested, %d skipped (not in main RAM); %d cases needed other overlays loaded\n",
           tested, overlaysNotRam, depRetries);
    for (auto& r : skipReasons) printf("  skipped %6d  %s\n", r.second, r.first.c_str());
    if (gPassList) fclose(gPassList);
    return fail ? 1 : 0;
}
