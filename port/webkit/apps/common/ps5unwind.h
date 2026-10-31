#pragma once
// Stack unwinding from a signal context with the executable's own
// .eh_frame/.eh_frame_hdr (DWARF CFI), for the sampling profiler
// (profiler.c). Async-signal-safe: no allocation, no locks, and every stack
// read is checked against the bounds the caller passes. Only what a return
// address needs is tracked: the CFA (from rsp or rbp), the saved rbp and
// the return address. Also builds on the Mac for a host test
// (test/prof/unwind-test.c).
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t* hdr; // .eh_frame_hdr
    const uint8_t* table; // its sorted (initial location, FDE) table
    size_t count;
    uint8_t tableEncoding;
    uintptr_t textStart, textEnd; // lowest FDE start .. highest FDE end
    const uint8_t* frameStart; // .eh_frame bounds (FDE/CIE pointers are checked against them)
    const uint8_t* frameEnd;
} ps5unwind_info_t;

typedef struct {
    int cfaRegister; // 7 = rsp, 6 = rbp (others: not supported)
    long cfaOffset;
    int rbpSaved; // rbp saved at CFA + rbpOffset
    long rbpOffset;
    long raOffset; // return address at CFA + raOffset
} ps5unwind_rule_t;

typedef struct {
    uintptr_t pc, sp, fp;
} ps5unwind_regs_t;

// 0 on success. frameEnd may be NULL (no upper bound check beyond the table).
int ps5unwind_init(ps5unwind_info_t*, const void* ehFrameHdr, const void* ehFrame, const void* ehFrameEnd);
// The unwind rule in effect at pc. 0 on success, -1 if no FDE covers pc,
// -2 if the CFI uses something this unwinder does not follow.
int ps5unwind_rule(const ps5unwind_info_t*, uintptr_t pc, ps5unwind_rule_t*);
// One step by CFI: regs is the frame at pc (exact for the interrupted frame,
// a return address otherwise: looked up at pc - 1). Stack reads must lie in
// [stackLow, stackHigh). 0 on success (regs is the caller), < 0 otherwise.
int ps5unwind_step(const ps5unwind_info_t*, ps5unwind_regs_t*, int exactPC, uintptr_t stackLow, uintptr_t stackHigh);

#ifdef __cplusplus
}
#endif
