// DWARF CFI unwinding for the sampling profiler (see ps5unwind.h).
//
// ps5wk-child's .eh_frame (WebKit build, 2026-10-31): 238k FDEs, three CIEs
// ("zR", "zPLR", ""), and only these call frame instructions: advance_loc
// (1/2/4), offset, restore, def_cfa, def_cfa_offset, def_cfa_register,
// remember/restore_state, register (once) and def_cfa_expression (63 FDEs,
// stack-realigning functions: not followed). 76% of the FDEs put the CFA on
// rbp (frame pointers); the rest are leaf functions or dependencies.
#include "ps5unwind.h"

#include <string.h>

enum {
    DW_EH_PE_absptr = 0x00,
    DW_EH_PE_uleb128 = 0x01,
    DW_EH_PE_udata2 = 0x02,
    DW_EH_PE_udata4 = 0x03,
    DW_EH_PE_udata8 = 0x04,
    DW_EH_PE_sleb128 = 0x09,
    DW_EH_PE_sdata2 = 0x0a,
    DW_EH_PE_sdata4 = 0x0b,
    DW_EH_PE_sdata8 = 0x0c,
    DW_EH_PE_pcrel = 0x10,
    DW_EH_PE_datarel = 0x30,
    DW_EH_PE_indirect = 0x80,
    DW_EH_PE_omit = 0xff,
};

#define REG_RBP 6
#define REG_RSP 7
#define REG_RA 16

static uint64_t uleb(const uint8_t** p, const uint8_t* end)
{
    uint64_t value = 0;
    unsigned shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        if (shift < 64)
            value |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80))
            break;
    }
    return value;
}

static int64_t sleb(const uint8_t** p, const uint8_t* end)
{
    int64_t value = 0;
    unsigned shift = 0;
    uint8_t b = 0;
    while (*p < end) {
        b = *(*p)++;
        if (shift < 64)
            value |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80))
            break;
    }
    if (shift < 64 && (b & 0x40))
        value |= -((int64_t)1 << shift);
    return value;
}

// Reads one pointer in the given encoding; datarel is relative to `data`.
static int readEncoded(const uint8_t** p, const uint8_t* end, uint8_t encoding, uintptr_t data, uintptr_t* out)
{
    if (encoding == DW_EH_PE_omit)
        return -1;
    const uint8_t* start = *p;
    uintptr_t value;
    int64_t s;
    uint64_t u;
    switch (encoding & 0x0f) {
    case DW_EH_PE_absptr:
    case DW_EH_PE_udata8:
    case DW_EH_PE_sdata8:
        if (end - start < 8)
            return -1;
        memcpy(&u, start, 8);
        value = (uintptr_t)u;
        *p += 8;
        break;
    case DW_EH_PE_udata4: {
        uint32_t v;
        if (end - start < 4)
            return -1;
        memcpy(&v, start, 4);
        value = v;
        *p += 4;
        break;
    }
    case DW_EH_PE_sdata4: {
        int32_t v;
        if (end - start < 4)
            return -1;
        memcpy(&v, start, 4);
        value = (uintptr_t)(intptr_t)v;
        *p += 4;
        break;
    }
    case DW_EH_PE_udata2: {
        uint16_t v;
        if (end - start < 2)
            return -1;
        memcpy(&v, start, 2);
        value = v;
        *p += 2;
        break;
    }
    case DW_EH_PE_sdata2: {
        int16_t v;
        if (end - start < 2)
            return -1;
        memcpy(&v, start, 2);
        value = (uintptr_t)(intptr_t)v;
        *p += 2;
        break;
    }
    case DW_EH_PE_uleb128:
        value = (uintptr_t)uleb(p, end);
        break;
    case DW_EH_PE_sleb128:
        s = sleb(p, end);
        value = (uintptr_t)s;
        break;
    default:
        return -1;
    }
    switch (encoding & 0x70) {
    case 0:
        break;
    case DW_EH_PE_pcrel:
        value += (uintptr_t)start;
        break;
    case DW_EH_PE_datarel:
        value += data;
        break;
    default:
        return -1;
    }
    if (encoding & DW_EH_PE_indirect)
        return -1; // not used for pc_begin/pc_range here
    *out = value;
    return 0;
}

static size_t encodedSize(uint8_t encoding)
{
    switch (encoding & 0x0f) {
    case DW_EH_PE_udata2:
    case DW_EH_PE_sdata2:
        return 2;
    case DW_EH_PE_udata4:
    case DW_EH_PE_sdata4:
        return 4;
    case DW_EH_PE_absptr:
    case DW_EH_PE_udata8:
    case DW_EH_PE_sdata8:
        return 8;
    default:
        return 0;
    }
}

int ps5unwind_init(ps5unwind_info_t* info, const void* ehFrameHdr, const void* ehFrame, const void* ehFrameEnd)
{
    memset(info, 0, sizeof *info);
    const uint8_t* hdr = (const uint8_t*)ehFrameHdr;
    if (!hdr || hdr[0] != 1)
        return -1;
    uint8_t framePtrEncoding = hdr[1], countEncoding = hdr[2], tableEncoding = hdr[3];
    const uint8_t* p = hdr + 4;
    const uint8_t* limit = hdr + 64;
    uintptr_t framePtr, count;
    if (readEncoded(&p, limit, framePtrEncoding, (uintptr_t)hdr, &framePtr) || readEncoded(&p, limit, countEncoding, (uintptr_t)hdr, &count))
        return -1;
    if (encodedSize(tableEncoding) != 4 || (tableEncoding & 0x70) != DW_EH_PE_datarel || !count)
        return -1; // lld writes datarel|sdata4
    info->hdr = hdr;
    info->table = p;
    info->count = count;
    info->tableEncoding = tableEncoding;
    info->frameStart = ehFrame ? (const uint8_t*)ehFrame : (const uint8_t*)framePtr;
    info->frameEnd = (const uint8_t*)ehFrameEnd;
    int32_t first, lastLoc, lastFde;
    memcpy(&first, p, 4);
    memcpy(&lastLoc, p + (count - 1) * 8, 4);
    memcpy(&lastFde, p + (count - 1) * 8 + 4, 4);
    info->textStart = (uintptr_t)hdr + first;
    info->textEnd = (uintptr_t)hdr + lastLoc + 1;
    // The end of the text: the last FDE's pc_range (best effort).
    const uint8_t* fde = hdr + lastFde;
    uint32_t length;
    memcpy(&length, fde, 4);
    if (length && length != 0xffffffffu) {
        int32_t ciePointer;
        memcpy(&ciePointer, fde + 4, 4);
        const uint8_t* q = fde + 8;
        uintptr_t begin, range;
        // Assume the common pcrel|sdata4 encoding for this one lookup.
        if (!readEncoded(&q, fde + 4 + length, 0x1b, 0, &begin) && !readEncoded(&q, fde + 4 + length, 0x0b, 0, &range))
            info->textEnd = begin + range;
    }
    return 0;
}

typedef struct {
    uint64_t codeAlign;
    int64_t dataAlign;
    unsigned raRegister;
    uint8_t pointerEncoding;
    int hasAugmentationData;
    const uint8_t* instructions;
    const uint8_t* end;
} cie_t;

static int inFrame(const ps5unwind_info_t* info, const uint8_t* p, size_t size)
{
    if (p < info->frameStart)
        return 0;
    if (info->frameEnd && p + size > info->frameEnd)
        return 0;
    return 1;
}

static int parseCIE(const ps5unwind_info_t* info, const uint8_t* cie, cie_t* out)
{
    if (!inFrame(info, cie, 8))
        return -1;
    uint32_t length;
    memcpy(&length, cie, 4);
    if (!length || length == 0xffffffffu)
        return -1;
    const uint8_t* end = cie + 4 + length;
    if (!inFrame(info, cie, 4 + length))
        return -1;
    uint32_t id;
    memcpy(&id, cie + 4, 4);
    if (id != 0)
        return -1;
    const uint8_t* p = cie + 8;
    uint8_t version = *p++;
    const char* augmentation = (const char*)p;
    while (p < end && *p)
        p++;
    if (p >= end)
        return -1;
    p++;
    if (version >= 4)
        p += 2; // address_size, segment_size
    memset(out, 0, sizeof *out);
    out->pointerEncoding = 0; // absptr
    out->codeAlign = uleb(&p, end);
    out->dataAlign = sleb(&p, end);
    out->raRegister = version == 1 ? *p++ : (unsigned)uleb(&p, end);
    if (augmentation[0] == 'z') {
        out->hasAugmentationData = 1;
        uint64_t size = uleb(&p, end);
        const uint8_t* data = p;
        const uint8_t* dataEnd = p + size;
        if (dataEnd > end)
            return -1;
        for (const char* a = augmentation + 1; *a; a++) {
            if (*a == 'R')
                out->pointerEncoding = *data++;
            else if (*a == 'L')
                data++;
            else if (*a == 'P') {
                uint8_t encoding = *data++;
                uintptr_t ignored;
                if (readEncoded(&data, dataEnd, encoding & 0x7f, 0, &ignored))
                    return -1;
            } else if (*a == 'S' || *a == 'B')
                ;
            else
                break;
        }
        p = dataEnd;
    } else if (augmentation[0])
        return -1;
    out->instructions = p;
    out->end = end;
    return 0;
}

typedef struct {
    int cfaRegister;
    int64_t cfaOffset;
    int8_t rbpRule; // 0 same value, 1 offset(N), 2 unsupported
    int64_t rbpOffset;
    int8_t raRule; // 1 offset(N), 0 undefined, 2 unsupported
    int64_t raOffset;
    int cfaUnsupported;
} state_t;

#define STATE_STACK 4

// Runs CFA instructions from p to end, stopping before the row that starts
// after target. loc is the location the row starts at.
static int execute(const uint8_t* p, const uint8_t* end, const cie_t* cie, uintptr_t* loc, uintptr_t target, state_t* state,
    const state_t* initial)
{
    state_t stack[STATE_STACK];
    int depth = 0;
    while (p < end) {
        uint8_t op = *p++;
        uint8_t high = op & 0xc0, low = op & 0x3f;
        uint64_t reg, delta;
        int64_t offset;
        if (high == 0x40) { // advance_loc
            delta = low * cie->codeAlign;
            if (*loc + delta > target)
                return 0;
            *loc += delta;
            continue;
        }
        if (high == 0x80) { // offset
            offset = (int64_t)uleb(&p, end) * cie->dataAlign;
            if (low == REG_RBP) {
                state->rbpRule = 1;
                state->rbpOffset = offset;
            } else if (low == cie->raRegister) {
                state->raRule = 1;
                state->raOffset = offset;
            }
            continue;
        }
        if (high == 0xc0) { // restore
            if (low == REG_RBP && initial) {
                state->rbpRule = initial->rbpRule;
                state->rbpOffset = initial->rbpOffset;
            } else if (low == cie->raRegister && initial) {
                state->raRule = initial->raRule;
                state->raOffset = initial->raOffset;
            }
            continue;
        }
        switch (op) {
        case 0x00: // nop
            break;
        case 0x01: { // set_loc
            uintptr_t newLoc;
            if (readEncoded(&p, end, cie->pointerEncoding, 0, &newLoc))
                return -2;
            if (newLoc > target)
                return 0;
            *loc = newLoc;
            break;
        }
        case 0x02: // advance_loc1
            if (end - p < 1)
                return -2;
            delta = p[0] * cie->codeAlign;
            p += 1;
            if (*loc + delta > target)
                return 0;
            *loc += delta;
            break;
        case 0x03: { // advance_loc2
            uint16_t v;
            if (end - p < 2)
                return -2;
            memcpy(&v, p, 2);
            p += 2;
            delta = v * cie->codeAlign;
            if (*loc + delta > target)
                return 0;
            *loc += delta;
            break;
        }
        case 0x04: { // advance_loc4
            uint32_t v;
            if (end - p < 4)
                return -2;
            memcpy(&v, p, 4);
            p += 4;
            delta = v * cie->codeAlign;
            if (*loc + delta > target)
                return 0;
            *loc += delta;
            break;
        }
        case 0x05: // offset_extended
        case 0x11: // offset_extended_sf
            reg = uleb(&p, end);
            offset = op == 0x05 ? (int64_t)uleb(&p, end) * cie->dataAlign : sleb(&p, end) * cie->dataAlign;
            if (reg == REG_RBP) {
                state->rbpRule = 1;
                state->rbpOffset = offset;
            } else if (reg == cie->raRegister) {
                state->raRule = 1;
                state->raOffset = offset;
            }
            break;
        case 0x06: // restore_extended
            reg = uleb(&p, end);
            if (reg == REG_RBP && initial) {
                state->rbpRule = initial->rbpRule;
                state->rbpOffset = initial->rbpOffset;
            } else if (reg == cie->raRegister && initial) {
                state->raRule = initial->raRule;
                state->raOffset = initial->raOffset;
            }
            break;
        case 0x07: // undefined
        case 0x08: // same_value
            reg = uleb(&p, end);
            if (reg == REG_RBP)
                state->rbpRule = 0;
            else if (reg == cie->raRegister)
                state->raRule = op == 0x07 ? 0 : 2;
            break;
        case 0x09: // register
            reg = uleb(&p, end);
            (void)uleb(&p, end);
            if (reg == REG_RBP)
                state->rbpRule = 2;
            else if (reg == cie->raRegister)
                state->raRule = 2;
            break;
        case 0x0a: // remember_state
            if (depth >= STATE_STACK)
                return -2;
            stack[depth++] = *state;
            break;
        case 0x0b: // restore_state
            if (!depth)
                return -2;
            *state = stack[--depth];
            break;
        case 0x0c: // def_cfa
            state->cfaRegister = (int)uleb(&p, end);
            state->cfaOffset = (int64_t)uleb(&p, end);
            state->cfaUnsupported = 0;
            break;
        case 0x12: // def_cfa_sf
            state->cfaRegister = (int)uleb(&p, end);
            state->cfaOffset = sleb(&p, end) * cie->dataAlign;
            state->cfaUnsupported = 0;
            break;
        case 0x0d: // def_cfa_register
            state->cfaRegister = (int)uleb(&p, end);
            break;
        case 0x0e: // def_cfa_offset
            state->cfaOffset = (int64_t)uleb(&p, end);
            break;
        case 0x13: // def_cfa_offset_sf
            state->cfaOffset = sleb(&p, end) * cie->dataAlign;
            break;
        case 0x0f: // def_cfa_expression
            state->cfaUnsupported = 1;
            p += uleb(&p, end);
            break;
        case 0x10: // expression
        case 0x16: // val_expression
            reg = uleb(&p, end);
            if (reg == REG_RBP)
                state->rbpRule = 2;
            else if (reg == cie->raRegister)
                state->raRule = 2;
            p += uleb(&p, end);
            break;
        case 0x14: // val_offset
        case 0x15: // val_offset_sf
            reg = uleb(&p, end);
            if (op == 0x14)
                (void)uleb(&p, end);
            else
                (void)sleb(&p, end);
            if (reg == REG_RBP)
                state->rbpRule = 2;
            else if (reg == cie->raRegister)
                state->raRule = 2;
            break;
        case 0x2e: // GNU_args_size
            (void)uleb(&p, end);
            break;
        case 0x2f: // GNU_negative_offset_extended
            reg = uleb(&p, end);
            offset = -(int64_t)uleb(&p, end) * cie->dataAlign;
            if (reg == REG_RBP) {
                state->rbpRule = 1;
                state->rbpOffset = offset;
            } else if (reg == cie->raRegister) {
                state->raRule = 1;
                state->raOffset = offset;
            }
            break;
        default:
            return -2;
        }
    }
    return 0;
}

// The FDE whose range holds pc, by binary search of the header's table.
static const uint8_t* findFDE(const ps5unwind_info_t* info, uintptr_t pc)
{
    if (!info->hdr || pc < info->textStart || pc >= info->textEnd)
        return NULL;
    size_t low = 0, high = info->count;
    intptr_t key = (intptr_t)(pc - (uintptr_t)info->hdr);
    while (high - low > 1) {
        size_t middle = low + (high - low) / 2;
        int32_t loc;
        memcpy(&loc, info->table + middle * 8, 4);
        if ((intptr_t)loc <= key)
            low = middle;
        else
            high = middle;
    }
    int32_t loc, fde;
    memcpy(&loc, info->table + low * 8, 4);
    memcpy(&fde, info->table + low * 8 + 4, 4);
    if ((intptr_t)loc > key)
        return NULL;
    return info->hdr + fde;
}

int ps5unwind_rule(const ps5unwind_info_t* info, uintptr_t pc, ps5unwind_rule_t* rule)
{
    const uint8_t* fde = findFDE(info, pc);
    if (!fde || !inFrame(info, fde, 8))
        return -1;
    uint32_t length;
    memcpy(&length, fde, 4);
    if (!length || length == 0xffffffffu || !inFrame(info, fde, 4 + length))
        return -1;
    const uint8_t* end = fde + 4 + length;
    int32_t ciePointer;
    memcpy(&ciePointer, fde + 4, 4);
    if (!ciePointer)
        return -1;
    cie_t cie;
    if (parseCIE(info, fde + 4 - ciePointer, &cie))
        return -1;
    const uint8_t* p = fde + 8;
    uintptr_t begin, range;
    if (readEncoded(&p, end, cie.pointerEncoding, 0, &begin) || readEncoded(&p, end, cie.pointerEncoding & 0x0f, 0, &range))
        return -1;
    if (pc < begin || pc >= begin + range)
        return -1;
    if (cie.hasAugmentationData)
        p += uleb(&p, end);
    if (p > end)
        return -1;

    state_t state;
    memset(&state, 0, sizeof state);
    state.cfaRegister = REG_RSP;
    state.cfaOffset = 8;
    uintptr_t loc = begin;
    if (execute(cie.instructions, cie.end, &cie, &loc, (uintptr_t)-1, &state, NULL))
        return -2;
    state_t initial = state;
    loc = begin;
    if (execute(p, end, &cie, &loc, pc, &state, &initial))
        return -2;
    if (state.cfaUnsupported || (state.cfaRegister != REG_RSP && state.cfaRegister != REG_RBP) || state.raRule != 1 || state.rbpRule == 2)
        return -2;
    rule->cfaRegister = state.cfaRegister;
    rule->cfaOffset = (long)state.cfaOffset;
    rule->rbpSaved = state.rbpRule == 1;
    rule->rbpOffset = (long)state.rbpOffset;
    rule->raOffset = (long)state.raOffset;
    return 0;
}

static int readStack(uintptr_t address, uintptr_t low, uintptr_t high, uintptr_t* out)
{
    if ((address & 7) || address < low || address + 8 > high || address + 8 < address)
        return -1;
    *out = *(const uintptr_t*)address;
    return 0;
}

int ps5unwind_step(const ps5unwind_info_t* info, ps5unwind_regs_t* regs, int exactPC, uintptr_t stackLow, uintptr_t stackHigh)
{
    ps5unwind_rule_t rule;
    int r = ps5unwind_rule(info, exactPC ? regs->pc : regs->pc - 1, &rule);
    if (r)
        return r;
    uintptr_t cfa = (rule.cfaRegister == REG_RSP ? regs->sp : regs->fp) + rule.cfaOffset;
    uintptr_t ra, fp = regs->fp;
    if (readStack(cfa + rule.raOffset, stackLow, stackHigh, &ra))
        return -3;
    if (rule.rbpSaved && readStack(cfa + rule.rbpOffset, stackLow, stackHigh, &fp))
        return -3;
    if (cfa <= regs->sp)
        return -4; // the stack must unwind upwards
    regs->pc = ra;
    regs->sp = cfa;
    regs->fp = fp;
    return 0;
}
