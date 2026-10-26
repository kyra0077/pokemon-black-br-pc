// Hybrid runtime: implements recomp_runtime.h on top of melonDS's ARM9 so
// recompiled functions can run inside the emulated console. See hybrid.h.

#include "hybrid.h"

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "NDS.h"
#include "ARM.h"

extern "C" {
#include "recomp_runtime.h"
extern const RecompEntry recomp_table[];
extern const unsigned recomp_table_size;
extern const RecompCodeRange recomp_code_ranges[];
extern const unsigned recomp_code_ranges_size;
// Generated from tools/recomp/hybrid_whitelist_IRBO.txt (entry addresses, bit 0 = Thumb).
extern const u32 hybrid_whitelist[];
extern const unsigned hybrid_whitelist_size;
}

// Hooks compiled into the core by tools/recomp/patch_core_hybrid.py.
namespace melonDS
{
extern void (*HybridJumpHook)(u32 addr);
extern bool (*HybridRunHook)(ARMv5& cpu);
extern bool HybridPending;
}

namespace Hybrid
{
namespace
{

// Rough ARM9 cycles charged per recompiled basic block (~4 instructions with
// the instruction cache hitting), so emulated time keeps moving.
constexpr int kCyclesPerBlock = 10;
// A call longer than this is rolled back: the interpreter would service IRQs
// in the meantime, and the recompiled code can't (spin-waits on IRQ-set flags).
constexpr int kBudgetBlocks = 20000;
constexpr int kMaxCallDepth = 256;
constexpr long kMaxHostStack = 8 * 1024 * 1024;
// A function that aborts this often (and rarely commits) stops being tried.
constexpr uint32_t kDisableAfterAborts = 16;

// Entry bitmap over ITCM's canonical base up to the end of main RAM, one bit per halfword.
constexpr u32 kMapBase = 0x01FF8000;
constexpr u32 kMapSize = 0x02400000 - kMapBase;

enum Reason
{
    R_IO, R_MPU, R_CODE_WRITE, R_UNKNOWN_TARGET, R_SWI, R_MODE, R_SPSR, R_CP15, R_WFI,
    R_UNIMPL, R_EXC_RETURN, R_RETURN, R_BUDGET, R_STACK, R_DEPTH, R_COUNT
};
const char* const kReasonNames[R_COUNT] = {
    "IO / unmapped access", "MPU forbids access", "write over recompiled code",
    "indirect call to non-static or unknown target", "SWI (BIOS call)", "CPU mode switch",
    "SPSR access", "CP15 configuration write", "wait for interrupt", "unimplemented instruction",
    "exception return", "returned somewhere other than LR", "budget (ran too long)",
    "host stack depth", "call depth",
};

struct Func
{
    const RecompEntry* entry;
    uint64_t calls = 0, commits = 0, aborts = 0;
    bool disabled = false;
};

struct Undo
{
    u8* ptr; // direct memory, or nullptr for a bus address
    u32 addr;
    u32 old;
    u8 size;
};

melonDS::NDS* gNds = nullptr;
melonDS::ARMv5* gCpu = nullptr;
bool gEnabled = false;
Stats gStats;
uint64_t gReasons[R_COUNT] = {};

std::vector<u8> gEntryBits;
std::unordered_map<u32, unsigned> gFuncIndex; // entry (with Thumb bit) -> gFuncs
std::vector<Func> gFuncs;
std::vector<std::vector<unsigned>> gCodePages; // static code ranges per 4 KB page

// State of the call in progress.
jmp_buf gAbort;
Reason gAbortReason;
std::vector<Undo> gUndo;
int gBudget = 0;
int gDepth = 0;
char* gStackBase = nullptr;

[[noreturn]] void Abort(Reason r)
{
    gAbortReason = r;
    longjmp(gAbort, 1);
}

void IndexCodeRanges()
{
    gCodePages.assign(kMapSize >> 12, {});
    for (unsigned i = 0; i < recomp_code_ranges_size; i++)
    {
        const RecompCodeRange& r = recomp_code_ranges[i];
        if (r.overlay >= 0) continue;
        for (u32 page = r.start >> 12; page <= (r.end - 1) >> 12; page++)
            if (page >= (kMapBase >> 12) && page - (kMapBase >> 12) < gCodePages.size())
                gCodePages[page - (kMapBase >> 12)].push_back(i);
    }
}

bool IsStaticCode(u32 canon)
{
    u32 page = (canon - kMapBase) >> 12;
    if (canon < kMapBase || page >= gCodePages.size()) return false;
    for (unsigned i : gCodePages[page])
        if (canon >= recomp_code_ranges[i].start && canon < recomp_code_ranges[i].end) return true;
    return false;
}

// Plain memory (TCM, main RAM) is accessed directly; VRAM, palette, OAM, shared
// WRAM and the BIOS go through melonDS's bus (nullptr); IO and anything with
// side effects on access aborts the call.
u8* Map(u32 addr, bool write)
{
    melonDS::ARMv5& cpu = *gCpu;
    if (!(cpu.PU_Map[addr >> 12] & (write ? 0x02 : 0x01))) Abort(R_MPU); // the interpreter takes the data abort
    if (addr < cpu.ITCMSize)
    {
        if (write && IsStaticCode(kMapBase | (addr & 0x7FFF))) Abort(R_CODE_WRITE);
        return &cpu.ITCM[addr & (melonDS::ITCMPhysicalSize - 1)];
    }
    if ((addr & cpu.DTCMMask) == cpu.DTCMBase) return &cpu.DTCM[addr & (melonDS::DTCMPhysicalSize - 1)];
    switch (addr >> 24)
    {
    case 0x02:
        if (write && IsStaticCode(0x02000000 | (addr & gNds->MainRAMMask))) Abort(R_CODE_WRITE);
        return &gNds->MainRAM[addr & gNds->MainRAMMask];
    case 0x03: case 0x05: case 0x06: case 0x07:
        return nullptr;
    case 0xFF:
        if (write) Abort(R_IO);
        return nullptr;
    default:
        Abort(R_IO);
    }
}

template <typename T>
u32 Read(u32 addr)
{
    addr &= ~(u32)(sizeof(T) - 1);
    if (u8* p = Map(addr, false))
    {
        T v;
        memcpy(&v, p, sizeof(T));
        return v;
    }
    if constexpr (sizeof(T) == 1) return gNds->ARM9Read8(addr);
    else if constexpr (sizeof(T) == 2) return gNds->ARM9Read16(addr);
    else return gNds->ARM9Read32(addr);
}

template <typename T>
void Write(u32 addr, u32 val)
{
    addr &= ~(u32)(sizeof(T) - 1);
    if (u8* p = Map(addr, true))
    {
        T old;
        memcpy(&old, p, sizeof(T));
        gUndo.push_back({p, addr, old, (u8)sizeof(T)});
        T v = (T)val;
        memcpy(p, &v, sizeof(T));
        return;
    }
    if constexpr (sizeof(T) == 1)
    {
        gUndo.push_back({nullptr, addr, gNds->ARM9Read8(addr), 1});
        gNds->ARM9Write8(addr, (u8)val);
    }
    else if constexpr (sizeof(T) == 2)
    {
        gUndo.push_back({nullptr, addr, gNds->ARM9Read16(addr), 2});
        gNds->ARM9Write16(addr, (u16)val);
    }
    else
    {
        gUndo.push_back({nullptr, addr, gNds->ARM9Read32(addr), 4});
        gNds->ARM9Write32(addr, val);
    }
}

void Rollback()
{
    for (auto it = gUndo.rbegin(); it != gUndo.rend(); ++it)
    {
        if (it->ptr)
        {
            if (it->size == 1) *it->ptr = (u8)it->old;
            else if (it->size == 2) { u16 v = (u16)it->old; memcpy(it->ptr, &v, 2); }
            else memcpy(it->ptr, &it->old, 4);
        }
        else if (it->size == 1) gNds->ARM9Write8(it->addr, (u8)it->old);
        else if (it->size == 2) gNds->ARM9Write16(it->addr, (u16)it->old);
        else gNds->ARM9Write32(it->addr, it->old);
    }
    gUndo.clear();
}

// Is this entry's code what's in memory right now? (ITCM is filled at boot by
// the autoload; until then the static ITCM functions aren't there.)
bool Resident(const RecompEntry& e)
{
    u32 a = e.addr & ~3u;
    u8 buf[64];
    if (e.check_len > sizeof(buf)) return false;
    for (u32 i = 0; i < e.check_len; i++)
    {
        u32 x = a + i;
        if (x < gCpu->ITCMSize) buf[i] = gCpu->ITCM[x & (melonDS::ITCMPhysicalSize - 1)];
        else if ((x >> 24) == 0x02) buf[i] = gNds->MainRAM[x & gNds->MainRAMMask];
        else return false;
    }
    return recomp_fnv1a(buf, e.check_len) == e.check_hash;
}

// Static-module entry at target (bit 0 = Thumb), resident in memory.
const RecompEntry* LookupStatic(u32 target)
{
    unsigned lo = 0, hi = recomp_table_size;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        if (recomp_table[mid].addr < target) lo = mid + 1;
        else hi = mid;
    }
    for (; lo < recomp_table_size && recomp_table[lo].addr == target; lo++)
        if (recomp_table[lo].overlay < 0 && Resident(recomp_table[lo])) return &recomp_table[lo];
    return nullptr;
}

void OnJump(u32 addr)
{
    u32 off = (addr & ~1u) - kMapBase;
    if (off < kMapSize && (gEntryBits[off >> 4] >> ((off >> 1) & 7) & 1)) melonDS::HybridPending = true;
}

// Called by the interpreter at an instruction boundary after a jump to a
// (possible) entry. Returns true if the function ran as recompiled code.
bool Run(melonDS::ARMv5& cpu)
{
    melonDS::HybridPending = false;
    u32 thumb = (cpu.CPSR >> 5) & 1;
    u32 pc = cpu.R[15] - (thumb ? 2 : 4);
    auto it = gFuncIndex.find(pc | thumb);
    if (it == gFuncIndex.end()) return false;
    Func& f = gFuncs[it->second];
    if (f.disabled || !Resident(*f.entry)) return false;
    if (cpu.IRQ && !(cpu.CPSR & 0x80)) return false; // let the interpreter take the IRQ first

    f.calls++;
    gStats.calls++;
    Cpu c{};
    for (int i = 0; i < 15; i++) c.r[i] = cpu.R[i];
    c.r[15] = pc;
    c.n = cpu.CPSR >> 31 & 1; c.z = cpu.CPSR >> 30 & 1; c.c = cpu.CPSR >> 29 & 1;
    c.v = cpu.CPSR >> 28 & 1; c.q = cpu.CPSR >> 27 & 1;
    c.cpsr_ctrl = cpu.CPSR & 0xFF;
    const u32 entryLr = cpu.R[14];

    gUndo.clear();
    gBudget = kBudgetBlocks;
    gDepth = 0;
    char stackMark;
    gStackBase = &stackMark;
    if (setjmp(gAbort) != 0)
    {
        Rollback();
        f.aborts++;
        gStats.aborted++;
        gReasons[gAbortReason]++;
        if (f.aborts >= kDisableAfterAborts && f.commits * 4 < f.aborts && !f.disabled)
        {
            f.disabled = true;
            gStats.disabled++;
        }
        return false;
    }
    f.entry->fn(&c);

    // Commit. The mode can't have changed (rt_set_psr aborts on that); the T bit
    // comes from the return address, as BX/POP {PC} would set it.
    for (int i = 0; i < 15; i++) cpu.R[i] = c.r[i];
    cpu.CPSR = (cpu.CPSR & 0x07FFFF00) | (c.n << 31) | (c.z << 30) | (c.c << 29) | (c.v << 28) | (c.q << 27)
             | (c.cpsr_ctrl & 0xDF);
    int blocks = kBudgetBlocks - gBudget;
    cpu.Cycles += blocks * kCyclesPerBlock;
    gStats.blocks += blocks;
    gStats.committed++;
    if (f.commits++ == 0) gStats.used++;
    gUndo.clear();
    cpu.JumpTo(entryLr);
    return true;
}

} // namespace

void Enable(melonDS::NDS& nds, bool all)
{
    gNds = &nds;
    gCpu = &nds.ARM9;
    IndexCodeRanges();
    gEntryBits.assign(kMapSize >> 4, 0);
    gFuncs.clear();
    gFuncIndex.clear();
    gStats = Stats{};
    memset(gReasons, 0, sizeof(gReasons));

    std::vector<u32> wanted;
    if (all)
    {
        for (unsigned i = 0; i < recomp_table_size; i++)
            if (recomp_table[i].overlay < 0) wanted.push_back(recomp_table[i].addr);
    }
    else
        wanted.assign(hybrid_whitelist, hybrid_whitelist + hybrid_whitelist_size);

    for (u32 addr : wanted)
    {
        if (gFuncIndex.count(addr)) continue;
        const RecompEntry* e = std::lower_bound(recomp_table, recomp_table + recomp_table_size, addr,
                                                [](const RecompEntry& x, u32 a) { return x.addr < a; });
        while (e != recomp_table + recomp_table_size && e->addr == addr && e->overlay >= 0) e++;
        if (e == recomp_table + recomp_table_size || e->addr != addr) e = nullptr;
        // Functions returning through their caller's frame can't be entered on their own.
        if (!e || (e->flags & RECOMP_FLAG_CALLER_FRAME)) continue;
        u32 off = (addr & ~1u) - kMapBase;
        if (off >= kMapSize) continue;
        gEntryBits[off >> 4] |= 1 << ((off >> 1) & 7);
        gFuncIndex[addr] = (unsigned)gFuncs.size();
        gFuncs.push_back({e});
    }
    gStats.whitelisted = (uint32_t)gFuncs.size();

    melonDS::HybridPending = false;
    melonDS::HybridRunHook = Run;
    melonDS::HybridJumpHook = OnJump;
    gEnabled = true;
}

void Disable()
{
    melonDS::HybridJumpHook = nullptr;
    melonDS::HybridPending = false;
    gEnabled = false;
}

bool Enabled() { return gEnabled; }

const Stats& GetStats() { return gStats; }

std::string Report(int topFunctions)
{
    std::string out;
    char line[256];
    const Stats& s = gStats;
    snprintf(line, sizeof(line),
             "hybrid: %u functions whitelisted, %u used, %u disabled\n"
             "calls: %llu reached, %llu ran recompiled (%.1f%%), %llu rolled back to the interpreter\n"
             "recompiled blocks executed: %llu\n",
             s.whitelisted, s.used, s.disabled, (unsigned long long)s.calls, (unsigned long long)s.committed,
             s.calls ? 100.0 * s.committed / s.calls : 0.0, (unsigned long long)s.aborted,
             (unsigned long long)s.blocks);
    out += line;
    for (int r = 0; r < R_COUNT; r++)
    {
        if (!gReasons[r]) continue;
        snprintf(line, sizeof(line), "  rollback %10llu  %s\n", (unsigned long long)gReasons[r], kReasonNames[r]);
        out += line;
    }
    std::vector<const Func*> order;
    for (const Func& f : gFuncs)
        if (f.calls) order.push_back(&f);
    std::sort(order.begin(), order.end(), [](const Func* a, const Func* b) { return a->calls > b->calls; });
    if ((int)order.size() > topFunctions) order.resize(topFunctions);
    if (!order.empty()) out += "most called (address, calls, recompiled, rolled back):\n";
    for (const Func* f : order)
    {
        snprintf(line, sizeof(line), "  %08X%c %12llu %12llu %10llu%s\n", f->entry->addr & ~1u,
                 (f->entry->addr & 1) ? 't' : 'a', (unsigned long long)f->calls, (unsigned long long)f->commits,
                 (unsigned long long)f->aborts, f->disabled ? "  (disabled)" : "");
        out += line;
    }
    return out;
}

} // namespace Hybrid

// ---- recomp_runtime.h -------------------------------------------------------------

using namespace Hybrid;

extern "C" {

u32 rt_read8(Cpu*, u32 a) { return Read<u8>(a); }
u32 rt_read16(Cpu*, u32 a) { return Read<u16>(a); }
u32 rt_read32(Cpu*, u32 a) { return Read<u32>(a); }
void rt_write8(Cpu*, u32 a, u32 v) { Write<u8>(a, v); }
void rt_write16(Cpu*, u32 a, u32 v) { Write<u16>(a, v); }
void rt_write32(Cpu*, u32 a, u32 v) { Write<u32>(a, v); }

void rt_call(Cpu* c, u32 target)
{
    // Overlay functions aren't run: their residency isn't tracked well enough yet.
    const RecompEntry* e = LookupStatic(target);
    if (!e) Abort(R_UNKNOWN_TARGET);
    if (++gDepth > kMaxCallDepth) Abort(R_DEPTH);
    e->fn(c);
    gDepth--;
}

// Jumps that aren't returns are tail calls; a return in disguise (e.g. BX to a
// copy of LR) isn't a function entry, so the lookup fails and the call rolls back.
void rt_jump(Cpu* c, u32 target, u32) { rt_call(c, target); }

void rt_swi(Cpu*, u32, u32) { Abort(R_SWI); }

u32 rt_get_psr(Cpu* c, int spsr)
{
    if (spsr) Abort(R_SPSR);
    return (c->n << 31) | (c->z << 30) | (c->c << 29) | (c->v << 28) | (c->q << 27) | c->cpsr_ctrl;
}

void rt_set_psr(Cpu* c, u32 val, u32 mask, int spsr)
{
    if (spsr) Abort(R_SPSR);
    if (mask & 8)
    {
        c->n = val >> 31 & 1; c->z = val >> 30 & 1; c->c = val >> 29 & 1; c->v = val >> 28 & 1; c->q = val >> 27 & 1;
    }
    if (mask & 1)
    {
        if ((val & 0x1F) != (c->cpsr_ctrl & 0x1F)) Abort(R_MODE);
        c->cpsr_ctrl = (c->cpsr_ctrl & 0x20) | (val & 0xDF); // I/F changes are fine
    }
}

// CP15 ids from the recompiler: cp<<16 | op1<<12 | CRn<<8 | CRm<<4 | op2.
u32 rt_mrc(Cpu*, u32 id)
{
    if ((id >> 16) != 15 || (id & 0xF000)) Abort(R_CP15);
    return gCpu->CP15Read(id & 0xFFF);
}

void rt_mcr(Cpu*, u32 id, u32 val)
{
    if ((id >> 16) != 15 || (id & 0xF000)) Abort(R_CP15);
    u32 crn = (id >> 8) & 15, crm = (id >> 4) & 15, op2 = id & 7;
    if (crn != 7) Abort(R_CP15);
    if ((crm == 0 && op2 == 4) || (crm == 8 && op2 == 2)) Abort(R_WFI);
    // Cache maintenance: melonDS emulates the instruction cache, so pass it on.
    // Not undoable, but repeating an invalidation after a rollback is harmless.
    gCpu->CP15Write(id & 0xFFF, val);
}

void rt_unimpl(Cpu*, u32, u32) { Abort(R_UNIMPL); }
void rt_exception_return(Cpu*, u32) { Abort(R_EXC_RETURN); }

void rt_return_check(Cpu*, u32 popped, u32 entry_lr)
{
    // Recompiled code returns to its C caller; that's only right if the target
    // is the LR the function was entered with (Thumb bit included).
    if (popped != entry_lr) Abort(R_RETURN);
}

void rt_block_hook(Cpu*)
{
    if (--gBudget <= 0) Abort(R_BUDGET);
    // Direct calls are plain C calls, so deep recursion would overflow the host stack.
    char here;
    if (gStackBase - &here > kMaxHostStack) Abort(R_STACK);
}

} // extern "C"
