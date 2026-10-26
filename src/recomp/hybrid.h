// Hybrid execution: melonDS keeps emulating the whole console (ARM7, GPU,
// SPU, DMA, timers, IRQs) and its ARM9 interpreter, but whenever the ARM9
// reaches the entry of a whitelisted function, the statically recompiled C
// version runs instead, up to the function's return.
//
// Each recompiled call is a transaction: memory writes are logged, and if the
// function does anything the runtime can't do atomically (touch IO, SWI, mode
// switch, an indirect call it can't resolve, run too long...), every write is
// undone and the interpreter executes the function as usual.
//
// Requires the core hooks from tools/recomp/patch_core_hybrid.py (ENABLE_HYBRID)
// and the interpreter (JIT off).
#pragma once

#include <cstdint>
#include <string>

namespace melonDS { class NDS; }

namespace Hybrid
{

struct Stats
{
    uint64_t calls = 0;       // whitelisted entries reached
    uint64_t committed = 0;   // calls that ran fully as recompiled code
    uint64_t aborted = 0;     // calls rolled back and handed to the interpreter
    uint64_t blocks = 0;      // recompiled basic blocks executed in committed calls
    uint32_t whitelisted = 0; // functions eligible for recompiled execution
    uint32_t used = 0;        // distinct functions with at least one committed call
    uint32_t disabled = 0;    // functions switched off after aborting too often
};

// all: every static-module function instead of only the ones the differential
// test proved identical (tools/recomp/hybrid_whitelist_IRBO.txt).
void Enable(melonDS::NDS& nds, bool all);
void Disable();
bool Enabled();

const Stats& GetStats();
// Multi-line report: totals, abort reasons and the most-called functions.
std::string Report(int topFunctions = 25);

} // namespace Hybrid
