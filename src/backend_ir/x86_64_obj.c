/*
 * v1 direct-to-machine-code encoder. See x86_64_obj.h for scope.
 *
 * Single pass: instructions are appended to a growable byte buffer in
 * program order. Every branch/call target is a rel32 field whose value is
 * not yet known when it is written (the target block or function may come
 * later), so each such field is zero-filled and recorded in a fixup list;
 * once every function has been laid out, block and function start offsets
 * are all known and the fixups are patched in place. Because this lane
 * always uses fixed-width encodings (REX.W always present, disp32/rel32
 * always 4 bytes, never a shorter form), no instruction's length depends on
 * a not-yet-known target, so a single pass is sufficient.
 */
#include "x86_64_obj.h"
#include "elf64.h"
#include <stdarg.h>

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} X86ObjBuf;

typedef enum { FIXUP_BLOCK, FIXUP_FUNCTION } X86ObjFixupKind;

typedef struct {
    size_t patch_offset; /* offset of the 4-byte rel32 field */
    X86ObjFixupKind kind;
    uint32_t target;
} X86ObjFixup;

#define X86OBJ_MAX_EXTERNS 8

typedef struct {
    size_t text_offset;
    size_t extern_index;
} X86ObjExternCall;

typedef struct {
    size_t text_offset;
    size_t rodata_offset;
} X86ObjRodataRef;

typedef struct {
    const MirModule *module;
    const MirAllocation *allocation;
    X86ObjBuf buf;
    uint64_t *block_offset;   /* by MirBlockRef, UINT64_MAX until seen */
    uint64_t *func_offset;    /* by function index */
    uint64_t *func_end;       /* by function index, filled after layout */
    X86ObjFixup *fixups;
    size_t fixup_count;
    size_t fixup_cap;
    int64_t *spill_offsets;
    int64_t *view_length_offsets;
    int64_t temp_offsets[MIR_MAX_OPERANDS];
    int64_t callee_saved_offsets[5];
    int64_t memory_offsets[BIR_MAX_STACK_SLOTS];
    bool memory_seen[BIR_MAX_STACK_SLOTS];
    uint32_t frame_size;
    char extern_names[X86OBJ_MAX_EXTERNS][32];
    size_t extern_count;
    X86ObjExternCall *extern_calls;
    size_t extern_call_count;
    size_t extern_call_cap;
    X86ObjBuf rodata;
    X86ObjRodataRef *rodata_refs;
    size_t rodata_ref_count;
    size_t rodata_ref_cap;
    char *errbuf;
    size_t errbuf_size;
    bool failed;
} X86ObjContext;

static void x86obj_error(X86ObjContext *ctx, const char *fmt, ...) {
    if (ctx->failed) return;
    ctx->failed = true;
    if (!ctx->errbuf || ctx->errbuf_size == 0 || ctx->errbuf[0]) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(ctx->errbuf, ctx->errbuf_size, fmt, args);
    va_end(args);
}

static void buf_reserve(X86ObjBuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return;
    size_t next = b->cap ? b->cap * 2 : 256;
    while (next < b->len + extra) next *= 2;
    b->data = realloc(b->data, next);
    b->cap = next;
}

static void emit_u8(X86ObjContext *ctx, uint8_t v) {
    buf_reserve(&ctx->buf, 1);
    ctx->buf.data[ctx->buf.len++] = v;
}

static size_t emit_u32_placeholder(X86ObjContext *ctx) {
    buf_reserve(&ctx->buf, 4);
    size_t at = ctx->buf.len;
    for (int i = 0; i < 4; i++) ctx->buf.data[ctx->buf.len++] = 0;
    return at;
}

static void emit_u32(X86ObjContext *ctx, uint32_t v) {
    for (int i = 0; i < 4; i++) emit_u8(ctx, (uint8_t)(v >> (8 * i)));
}

static void emit_u64(X86ObjContext *ctx, uint64_t v) {
    for (int i = 0; i < 8; i++) emit_u8(ctx, (uint8_t)(v >> (8 * i)));
}

static void add_fixup(X86ObjContext *ctx, size_t patch_offset, X86ObjFixupKind kind, uint32_t target) {
    if (ctx->fixup_count == ctx->fixup_cap) {
        size_t next = ctx->fixup_cap ? ctx->fixup_cap * 2 : 32;
        ctx->fixups = realloc(ctx->fixups, next * sizeof(*ctx->fixups));
        ctx->fixup_cap = next;
    }
    ctx->fixups[ctx->fixup_count].patch_offset = patch_offset;
    ctx->fixups[ctx->fixup_count].kind = kind;
    ctx->fixups[ctx->fixup_count].target = target;
    ctx->fixup_count++;
}

/* Abstract allocator/ABI register indices 0..10 map 1:1 onto this table;
   indices 0..5 double as the six SysV integer argument registers. */
static int x86obj_enc(uint16_t abstract_index) {
    static const int enc[11] = {7, 6, 2, 1, 8, 9, 3, 12, 13, 14, 15};
    return abstract_index < 11 ? enc[abstract_index] : -1;
}

#define X86OBJ_RAX 0
#define X86OBJ_RCX 1
#define X86OBJ_RDX 2
#define X86OBJ_RSP 4
#define X86OBJ_RBP 5
#define X86OBJ_R9 9
#define X86OBJ_R10 10
#define X86OBJ_R11 11

static uint8_t x86obj_rex(bool w, int reg, int rm) {
    return (uint8_t)(0x40 | (w ? 8 : 0) | ((reg >= 8) ? 4 : 0) | ((rm >= 8) ? 1 : 0));
}

static uint8_t x86obj_modrm(int mod, int reg, int rm) {
    return (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

static void emit_mov_rr(X86ObjContext *ctx, int dst, int src) {
    emit_u8(ctx, x86obj_rex(true, src, dst));
    emit_u8(ctx, 0x89);
    emit_u8(ctx, x86obj_modrm(3, src, dst));
}

static void emit_mov_load(X86ObjContext *ctx, int dst, int64_t disp) {
    emit_u8(ctx, x86obj_rex(true, dst, X86OBJ_RBP));
    emit_u8(ctx, 0x8B);
    emit_u8(ctx, x86obj_modrm(2, dst, X86OBJ_RBP));
    emit_u32(ctx, (uint32_t)(int32_t)disp);
}

static void emit_mov_store(X86ObjContext *ctx, int64_t disp, int src) {
    emit_u8(ctx, x86obj_rex(true, src, X86OBJ_RBP));
    emit_u8(ctx, 0x89);
    emit_u8(ctx, x86obj_modrm(2, src, X86OBJ_RBP));
    emit_u32(ctx, (uint32_t)(int32_t)disp);
}

static void emit_movabs(X86ObjContext *ctx, int reg, uint64_t imm) {
    emit_u8(ctx, (uint8_t)(0x48 | ((reg >= 8) ? 1 : 0)));
    emit_u8(ctx, (uint8_t)(0xB8 + (reg & 7)));
    emit_u64(ctx, imm);
}

static void emit_add_rr(X86ObjContext *ctx, int dst, int src) {
    emit_u8(ctx, x86obj_rex(true, src, dst));
    emit_u8(ctx, 0x01);
    emit_u8(ctx, x86obj_modrm(3, src, dst));
}

static void emit_sub_rr(X86ObjContext *ctx, int dst, int src) {
    emit_u8(ctx, x86obj_rex(true, src, dst));
    emit_u8(ctx, 0x29);
    emit_u8(ctx, x86obj_modrm(3, src, dst));
}

static void emit_imul_rr(X86ObjContext *ctx, int dst, int src) {
    emit_u8(ctx, x86obj_rex(true, dst, src));
    emit_u8(ctx, 0x0F);
    emit_u8(ctx, 0xAF);
    emit_u8(ctx, x86obj_modrm(3, dst, src));
}

static void emit_neg_r(X86ObjContext *ctx, int reg) {
    emit_u8(ctx, x86obj_rex(true, 0, reg));
    emit_u8(ctx, 0xF7);
    emit_u8(ctx, x86obj_modrm(3, 3, reg));
}

static void emit_cmp_rr(X86ObjContext *ctx, int lhs, int rhs) {
    emit_u8(ctx, x86obj_rex(true, rhs, lhs));
    emit_u8(ctx, 0x39);
    emit_u8(ctx, x86obj_modrm(3, rhs, lhs));
}

static void emit_test_rr(X86ObjContext *ctx, int reg) {
    emit_u8(ctx, x86obj_rex(true, reg, reg));
    emit_u8(ctx, 0x85);
    emit_u8(ctx, x86obj_modrm(3, reg, reg));
}

static void emit_setcc(X86ObjContext *ctx, int cc, int reg) {
    emit_u8(ctx, (uint8_t)(0x40 | ((reg >= 8) ? 1 : 0)));
    emit_u8(ctx, 0x0F);
    emit_u8(ctx, (uint8_t)(0x90 + cc));
    emit_u8(ctx, x86obj_modrm(3, 0, reg));
}

static void emit_movzx64_8(X86ObjContext *ctx, int dst, int src8) {
    emit_u8(ctx, x86obj_rex(true, dst, src8));
    emit_u8(ctx, 0x0F);
    emit_u8(ctx, 0xB6);
    emit_u8(ctx, x86obj_modrm(3, dst, src8));
}

static void emit_xor_rr(X86ObjContext *ctx, int reg) {
    emit_u8(ctx, x86obj_rex(true, reg, reg));
    emit_u8(ctx, 0x31);
    emit_u8(ctx, x86obj_modrm(3, reg, reg));
}

static void emit_xor_rr2(X86ObjContext *ctx, int dst, int src) {
    emit_u8(ctx, x86obj_rex(true, src, dst));
    emit_u8(ctx, 0x31);
    emit_u8(ctx, x86obj_modrm(3, src, dst));
}

/* 8-bit AND/OR on the low byte of two of {al,cl,dl,bl} (reg < 4), used only
   to combine SETcc results for NaN-safe float comparisons; no REX needed. */
static void emit_and_r8(X86ObjContext *ctx, int dst, int src) {
    emit_u8(ctx, 0x20);
    emit_u8(ctx, x86obj_modrm(3, src, dst));
}

static void emit_or_r8(X86ObjContext *ctx, int dst, int src) {
    emit_u8(ctx, 0x08);
    emit_u8(ctx, x86obj_modrm(3, src, dst));
}

static void emit_cqo(X86ObjContext *ctx) {
    emit_u8(ctx, 0x48);
    emit_u8(ctx, 0x99);
}

static void emit_idiv(X86ObjContext *ctx, int reg, bool is_signed) {
    emit_u8(ctx, x86obj_rex(true, 0, reg));
    emit_u8(ctx, 0xF7);
    emit_u8(ctx, x86obj_modrm(3, is_signed ? 7 : 6, reg));
}

static void emit_jmp(X86ObjContext *ctx, MirBlockRef target) {
    emit_u8(ctx, 0xE9);
    add_fixup(ctx, emit_u32_placeholder(ctx), FIXUP_BLOCK, target);
}

static void emit_jcc(X86ObjContext *ctx, int cc, MirBlockRef target) {
    emit_u8(ctx, 0x0F);
    emit_u8(ctx, (uint8_t)(0x80 + cc));
    add_fixup(ctx, emit_u32_placeholder(ctx), FIXUP_BLOCK, target);
}

/* Short-lived intra-instruction control flow (bounds-check fail/done
   labels): both the branch and its target are emitted within the same
   opcode handler, so these patch immediately against the current buffer
   position instead of going through the module-wide fixup list. */
static size_t emit_jmp_fwd(X86ObjContext *ctx) {
    emit_u8(ctx, 0xE9);
    return emit_u32_placeholder(ctx);
}
static size_t emit_jcc_fwd(X86ObjContext *ctx, int cc) {
    emit_u8(ctx, 0x0F);
    emit_u8(ctx, (uint8_t)(0x80 + cc));
    return emit_u32_placeholder(ctx);
}
static void patch_fwd(X86ObjContext *ctx, size_t patch_offset) {
    uint32_t target = (uint32_t)ctx->buf.len;
    int32_t rel = (int32_t)((int64_t)target - (int64_t)(patch_offset + 4));
    uint32_t bits = (uint32_t)rel;
    for (int b = 0; b < 4; b++) ctx->buf.data[patch_offset + b] = (uint8_t)(bits >> (8 * b));
}
static void emit_ud2(X86ObjContext *ctx) {
    emit_u8(ctx, 0x0F);
    emit_u8(ctx, 0x0B);
}

/* Unconditional jump to an already-known earlier position (a loop back-edge)
   - unlike emit_jmp_fwd, the target is known immediately, so this patches
   the rel32 field itself instead of deferring to a later patch_fwd call. */
static void emit_jmp_to(X86ObjContext *ctx, size_t target_offset) {
    emit_u8(ctx, 0xE9);
    size_t patch_offset = emit_u32_placeholder(ctx);
    int32_t rel = (int32_t)((int64_t)target_offset - (int64_t)(patch_offset + 4));
    uint32_t bits = (uint32_t)rel;
    for (int b = 0; b < 4; b++) ctx->buf.data[patch_offset + b] = (uint8_t)(bits >> (8 * b));
}

static size_t x86obj_extern_index(X86ObjContext *ctx, const char *name) {
    for (size_t i = 0; i < ctx->extern_count; i++)
        if (strcmp(ctx->extern_names[i], name) == 0) return i;
    if (ctx->extern_count >= X86OBJ_MAX_EXTERNS) return (size_t)-1;
    snprintf(ctx->extern_names[ctx->extern_count], sizeof(ctx->extern_names[0]), "%s", name);
    return ctx->extern_count++;
}

/* Calls an external symbol (malloc/free/...) resolved by the linker via a
   R_X86_64_PLT32 relocation - unlike emit_call, the target isn't known or
   knowable at emission time, so this always defers to elf64_write_object's
   relocation table instead of the intra-module fixup list. */
static bool emit_call_extern(X86ObjContext *ctx, const char *name) {
    size_t index = x86obj_extern_index(ctx, name);
    if (index == (size_t)-1) return false;
    emit_u8(ctx, 0xE8);
    size_t patch_offset = emit_u32_placeholder(ctx);
    if (ctx->extern_call_count == ctx->extern_call_cap) {
        size_t next = ctx->extern_call_cap ? ctx->extern_call_cap * 2 : 16;
        ctx->extern_calls = realloc(ctx->extern_calls, next * sizeof(*ctx->extern_calls));
        ctx->extern_call_cap = next;
    }
    ctx->extern_calls[ctx->extern_call_count].text_offset = patch_offset;
    ctx->extern_calls[ctx->extern_call_count].extern_index = index;
    ctx->extern_call_count++;
    return true;
}

/* Appends a NUL-terminated copy of `text` to the .rodata buffer and returns
   its byte offset within that section (raw bytes - no assembler-style
   escaping needed since this writes directly to the section, not through a
   `.string` directive). */
static size_t x86obj_rodata_string(X86ObjContext *ctx, const char *text) {
    size_t len = strlen(text) + 1;
    buf_reserve(&ctx->rodata, len);
    size_t at = ctx->rodata.len;
    memcpy(ctx->rodata.data + ctx->rodata.len, text, len);
    ctx->rodata.len += len;
    return at;
}

/* `lea dst, [rip+disp32]` targeting a .rodata offset, resolved at link time
   via an R_X86_64_PC32 relocation against the local .rodata section symbol
   (elf64_write_object always emits exactly one). mod=00,rm=101 is the
   dedicated RIP-relative encoding in 64-bit mode. */
static void emit_lea_rodata(X86ObjContext *ctx, int dst, size_t rodata_offset) {
    emit_u8(ctx, x86obj_rex(true, dst, 0));
    emit_u8(ctx, 0x8D);
    emit_u8(ctx, x86obj_modrm(0, dst, 5));
    size_t patch_offset = emit_u32_placeholder(ctx);
    if (ctx->rodata_ref_count == ctx->rodata_ref_cap) {
        size_t next = ctx->rodata_ref_cap ? ctx->rodata_ref_cap * 2 : 16;
        ctx->rodata_refs = realloc(ctx->rodata_refs, next * sizeof(*ctx->rodata_refs));
        ctx->rodata_ref_cap = next;
    }
    ctx->rodata_refs[ctx->rodata_ref_count].text_offset = patch_offset;
    ctx->rodata_refs[ctx->rodata_ref_count].rodata_offset = rodata_offset;
    ctx->rodata_ref_count++;
}

static void emit_call(X86ObjContext *ctx, uint32_t callee_index) {
    emit_u8(ctx, 0xE8);
    add_fixup(ctx, emit_u32_placeholder(ctx), FIXUP_FUNCTION, callee_index);
}

static void emit_ret(X86ObjContext *ctx) { emit_u8(ctx, 0xC3); }
static void emit_leave(X86ObjContext *ctx) { emit_u8(ctx, 0xC9); }
static void emit_push_rbp(X86ObjContext *ctx) { emit_u8(ctx, 0x55); }

static void emit_lea_rbp(X86ObjContext *ctx, int dst, int64_t disp) {
    emit_u8(ctx, x86obj_rex(true, dst, X86OBJ_RBP));
    emit_u8(ctx, 0x8D);
    emit_u8(ctx, x86obj_modrm(2, dst, X86OBJ_RBP));
    emit_u32(ctx, (uint32_t)(int32_t)disp);
}

/* Indirect [addr_reg] forms. addr_reg is always the fixed r10 scratch here,
   whose low 3 bits (2) never collide with the rm=4 (SIB) or rm=5
   (RIP-relative when mod=00) special encodings, so mod=00 with no SIB byte
   is always a plain [addr_reg] dereference. */
static void emit_mov_load_indirect(X86ObjContext *ctx, int dst, int addr_reg) {
    emit_u8(ctx, x86obj_rex(true, dst, addr_reg));
    emit_u8(ctx, 0x8B);
    emit_u8(ctx, x86obj_modrm(0, dst, addr_reg));
}

static void emit_mov_store_indirect(X86ObjContext *ctx, int addr_reg, int src) {
    emit_u8(ctx, x86obj_rex(true, src, addr_reg));
    emit_u8(ctx, 0x89);
    emit_u8(ctx, x86obj_modrm(0, src, addr_reg));
}

static void emit_movzx64_8_indirect(X86ObjContext *ctx, int dst, int addr_reg) {
    emit_u8(ctx, x86obj_rex(true, dst, addr_reg));
    emit_u8(ctx, 0x0F);
    emit_u8(ctx, 0xB6);
    emit_u8(ctx, x86obj_modrm(0, dst, addr_reg));
}

static void emit_movb_store_indirect(X86ObjContext *ctx, int addr_reg, int src8) {
    emit_u8(ctx, x86obj_rex(false, src8, addr_reg));
    emit_u8(ctx, 0x88);
    emit_u8(ctx, x86obj_modrm(0, src8, addr_reg));
}

/* General [base+disp32] access at width 8/4/1 bytes, used for struct field
   addressing and whole-struct copies where the base is a runtime pointer
   (not always %rbp), unlike the fixed spill/stack-slot helpers above. */
static void emit_mov_mem_disp(X86ObjContext *ctx, bool load, int width, int reg, int base, int64_t disp) {
    if (width == 1) {
        emit_u8(ctx, x86obj_rex(false, reg, base));
        emit_u8(ctx, load ? 0x8A : 0x88);
    } else {
        emit_u8(ctx, x86obj_rex(width == 8, reg, base));
        emit_u8(ctx, load ? 0x8B : 0x89);
    }
    emit_u8(ctx, x86obj_modrm(2, reg, base));
    emit_u32(ctx, (uint32_t)(int32_t)disp);
}

static void emit_add_r_imm32(X86ObjContext *ctx, int reg, int32_t imm) {
    emit_u8(ctx, x86obj_rex(true, 0, reg));
    emit_u8(ctx, 0x81);
    emit_u8(ctx, x86obj_modrm(3, 0, reg));
    emit_u32(ctx, (uint32_t)imm);
}

static void emit_imul_r_imm32(X86ObjContext *ctx, int reg, uint32_t imm) {
    emit_u8(ctx, x86obj_rex(true, reg, reg));
    emit_u8(ctx, 0x69);
    emit_u8(ctx, x86obj_modrm(3, reg, reg));
    emit_u32(ctx, imm);
}

static void emit_sub_rsp_imm(X86ObjContext *ctx, uint32_t imm) {
    emit_u8(ctx, x86obj_rex(true, 0, X86OBJ_RSP));
    emit_u8(ctx, 0x81);
    emit_u8(ctx, x86obj_modrm(3, 5, X86OBJ_RSP));
    emit_u32(ctx, imm);
}

/* ---- SSE2 scalar float encodings. Binary/compare/neg ops always route
   through xmm14/xmm15, mirroring the text emitter's float scratch choice -
   both lie outside the allocator's xmm0-7 class, so they can never collide
   with a live allocated value. ---- */
#define X86OBJ_XMM_SCRATCH0 14
#define X86OBJ_XMM_SCRATCH1 15

static void emit_xmm_rr(X86ObjContext *ctx, uint8_t prefix, uint8_t opcode, int dst, int src) {
    if (prefix) emit_u8(ctx, prefix);
    if (dst >= 8 || src >= 8) emit_u8(ctx, x86obj_rex(false, dst, src));
    emit_u8(ctx, 0x0F);
    emit_u8(ctx, opcode);
    emit_u8(ctx, x86obj_modrm(3, dst, src));
}

static void emit_xmm_mem(X86ObjContext *ctx, uint8_t prefix, uint8_t opcode, int xmm_reg, int64_t disp) {
    if (prefix) emit_u8(ctx, prefix);
    if (xmm_reg >= 8) emit_u8(ctx, x86obj_rex(false, xmm_reg, X86OBJ_RBP));
    emit_u8(ctx, 0x0F);
    emit_u8(ctx, opcode);
    emit_u8(ctx, x86obj_modrm(2, xmm_reg, X86OBJ_RBP));
    emit_u32(ctx, (uint32_t)(int32_t)disp);
}

static uint8_t x86obj_float_prefix(bool is_f64) { return is_f64 ? 0xF2 : 0xF3; }

static void emit_movx_load(X86ObjContext *ctx, bool is_f64, int dst, int64_t disp) {
    emit_xmm_mem(ctx, x86obj_float_prefix(is_f64), 0x10, dst, disp);
}
static void emit_movx_store(X86ObjContext *ctx, bool is_f64, int64_t disp, int src) {
    emit_xmm_mem(ctx, x86obj_float_prefix(is_f64), 0x11, src, disp);
}
static void emit_movx_rr(X86ObjContext *ctx, bool is_f64, int dst, int src) {
    emit_xmm_rr(ctx, x86obj_float_prefix(is_f64), 0x10, dst, src);
}
static void emit_addx(X86ObjContext *ctx, bool is_f64, int dst, int src) {
    emit_xmm_rr(ctx, x86obj_float_prefix(is_f64), 0x58, dst, src);
}
static void emit_subx(X86ObjContext *ctx, bool is_f64, int dst, int src) {
    emit_xmm_rr(ctx, x86obj_float_prefix(is_f64), 0x5C, dst, src);
}
static void emit_mulx(X86ObjContext *ctx, bool is_f64, int dst, int src) {
    emit_xmm_rr(ctx, x86obj_float_prefix(is_f64), 0x59, dst, src);
}
static void emit_divx(X86ObjContext *ctx, bool is_f64, int dst, int src) {
    emit_xmm_rr(ctx, x86obj_float_prefix(is_f64), 0x5E, dst, src);
}
static void emit_ucomix(X86ObjContext *ctx, bool is_f64, int a, int b) {
    /* ucomiss/ucomisd use 66 as a mandatory prefix only for the sd form,
       not F2/F3 like the arithmetic ops. */
    emit_xmm_rr(ctx, is_f64 ? 0x66 : 0x00, 0x2E, a, b);
}
static void emit_pxor_x(X86ObjContext *ctx, int dst, int src) {
    emit_xmm_rr(ctx, 0x66, 0xEF, dst, src);
}
/* cvtss2sd/cvtsd2ss: converts float precision, xmm <- xmm. The source
   precision picks the mandatory prefix (F3 reads a single, F2 a double). */
static void emit_cvt_float_width(X86ObjContext *ctx, bool from_f64, int dst, int src) {
    emit_xmm_rr(ctx, from_f64 ? 0xF2 : 0xF3, 0x5A, dst, src);
}
/* cvtsi2ss/cvtsi2sd: xmm <- r/m64 (always the 64-bit GPR form here, REX.W
   set explicitly since emit_xmm_rr's implicit REX omits W). */
static void emit_cvtsi2sx(X86ObjContext *ctx, bool is_f64, int xmm_dst, int gpr_src) {
    emit_u8(ctx, x86obj_float_prefix(is_f64));
    emit_u8(ctx, x86obj_rex(true, xmm_dst, gpr_src));
    emit_u8(ctx, 0x0F);
    emit_u8(ctx, 0x2A);
    emit_u8(ctx, x86obj_modrm(3, xmm_dst, gpr_src));
}
/* cvttss2si/cvttsd2si: r64 <- xmm, truncating toward zero (matches the
   direct backend's own cast semantics, never the rounding cvtsi2si form). */
static void emit_cvttsx2si(X86ObjContext *ctx, bool is_f64, int gpr_dst, int xmm_src) {
    emit_u8(ctx, x86obj_float_prefix(is_f64));
    emit_u8(ctx, x86obj_rex(true, gpr_dst, xmm_src));
    emit_u8(ctx, 0x0F);
    emit_u8(ctx, 0x2C);
    emit_u8(ctx, x86obj_modrm(3, gpr_dst, xmm_src));
}

/* movd/movq between a GPR and the low bits of an xmm register. reg/rm follow
   the same slots as the integer encoders: xmm goes in the ModRM.reg field,
   the GPR in ModRM.rm, for both load (0x6E) and store (0x7E) directions. */
static void emit_movd_gpr_to_xmm(X86ObjContext *ctx, bool wide, int xmm_dst, int gpr_src) {
    emit_u8(ctx, 0x66);
    emit_u8(ctx, x86obj_rex(wide, xmm_dst, gpr_src));
    emit_u8(ctx, 0x0F);
    emit_u8(ctx, 0x6E);
    emit_u8(ctx, x86obj_modrm(3, xmm_dst, gpr_src));
}
static void emit_xmm_mem_indirect(X86ObjContext *ctx, uint8_t prefix, uint8_t opcode, int xmm_reg, int addr_reg) {
    if (prefix) emit_u8(ctx, prefix);
    if (xmm_reg >= 8 || addr_reg >= 8) emit_u8(ctx, x86obj_rex(false, xmm_reg, addr_reg));
    emit_u8(ctx, 0x0F);
    emit_u8(ctx, opcode);
    emit_u8(ctx, x86obj_modrm(0, xmm_reg, addr_reg));
}
static void emit_movx_load_indirect(X86ObjContext *ctx, bool is_f64, int dst, int addr_reg) {
    emit_xmm_mem_indirect(ctx, x86obj_float_prefix(is_f64), 0x10, dst, addr_reg);
}
static void emit_movx_store_indirect(X86ObjContext *ctx, bool is_f64, int addr_reg, int src) {
    emit_xmm_mem_indirect(ctx, x86obj_float_prefix(is_f64), 0x11, src, addr_reg);
}

static void emit_movd_xmm_to_gpr(X86ObjContext *ctx, bool wide, int gpr_dst, int xmm_src) {
    emit_u8(ctx, 0x66);
    emit_u8(ctx, x86obj_rex(wide, xmm_src, gpr_dst));
    emit_u8(ctx, 0x0F);
    emit_u8(ctx, 0x7E);
    emit_u8(ctx, x86obj_modrm(3, xmm_src, gpr_dst));
}

/* -------------------------------------------------------------------- */

static bool x86obj_is_float(MirMachineType type) { return type == MIR_TYPE_F32 || type == MIR_TYPE_F64; }

/* Buffer opcodes carry a CobraType element descriptor (inst->memory_type)
   rather than a MirMachineType, since the element is never itself a
   register value. Maps to a machine type only so x86obj_supported_scalar
   can gate it the same way as everything else; kinds outside the v1 scalar
   set deliberately map to AGGREGATE so that gate rejects them. */
static MirMachineType x86obj_machine_type_for_cobra(const CobraType *type) {
    if (!type) return MIR_TYPE_VOID;
    switch (type->kind) {
        case COBRA_TYPE_I64: return MIR_TYPE_I64;
        case COBRA_TYPE_U64: return MIR_TYPE_U64;
        case COBRA_TYPE_BOOL: return MIR_TYPE_BOOL;
        case COBRA_TYPE_F32: return MIR_TYPE_F32;
        case COBRA_TYPE_F64: return MIR_TYPE_F64;
        case COBRA_TYPE_POINTER: return MIR_TYPE_ADDRESS;
        default: return MIR_TYPE_AGGREGATE;
    }
}

/* v1 struct support: flat structs whose fields are all plain scalars - no
   nested aggregates, arrays, sums, or owned payloads. AGG_COPY/FIELD_ADDR
   never need to know field layout beyond what MIR already computed
   (memory_offset, memory_width); this predicate only gates which struct
   shapes the emitter is willing to touch at all. */
static bool x86obj_supported_struct(const CobraType *type);

/* Shared by struct fields and fixed-array elements: everything this emitter
   can place inline as flat bytes inside an aggregate. */
static bool x86obj_supported_field_type(const CobraType *field) {
    if (!field) return false;
    switch (field->kind) {
        case COBRA_TYPE_I64: case COBRA_TYPE_U64: case COBRA_TYPE_BOOL:
        case COBRA_TYPE_F32: case COBRA_TYPE_F64: case COBRA_TYPE_POINTER:
            return true;
        case COBRA_TYPE_STRUCT:
            /* Nested value structs are just more flat bytes to AGG_COPY and
               more constant offsets for FIELD_ADDR - recurse. */
            return x86obj_supported_struct(field);
        case COBRA_TYPE_ARRAY:
            /* Fixed arrays as struct fields: bounds-checked element access
               goes through ARRAY_INDEX_ADDR; whole-field copies are still
               just more flat bytes to AGG_COPY. */
            return field->generic_arg_count == 1 && field->array_length > 0 &&
                   field->array_length <= COBRA_MAX_ARRAY_ELEMENTS &&
                   x86obj_supported_field_type(field->generic_args[0]);
        default:
            /* An owned slice/string field (e.g. a struct holding a string)
               is still just flat bytes to move/copy; only AGG_MOVE/AGG_DROP
               need to know it needs freeing on drop and zeroing on move,
               which x86obj_emit_drop_owned_value handles generically via
               bir_type_has_owned_payload. */
            return bir_is_owned_slice_type(field);
    }
}

static bool x86obj_supported_struct(const CobraType *type) {
    if (!type || !type->finalized || type->kind != COBRA_TYPE_STRUCT || type->field_count == 0) return false;
    for (size_t i = 0; i < type->field_count; i++)
        if (!x86obj_supported_field_type(type->fields[i].type)) return false;
    return true;
}

/* v1 sum/enum support: flat tag+payload layouts (Option/Result/user enums)
   whose variant payloads are unit, plain scalars, or (recursively) a v1
   struct - no owned payloads, views, or further-nested sums. Like structs,
   the emitter only ever moves these as opaque bytes (AGG_COPY) or reads the
   tag (SUM_CHECK/FIELD_ADDR+LOAD), so this predicate only gates shape. */
static bool x86obj_supported_sum(const CobraType *type) {
    if (!type || !type->finalized || !bir_is_sum_type(type)) return false;
    /* An owned-view payload component (e.g. Option[string]) is only
       supported for the two-component Option/Result shape: that is the
       only shape x86obj_emit_drop_owned_value's sum branch below knows how
       to recurse into (selector 1 -> generic_args[0], else -> [1]). An
       arbitrary N-variant user enum with an owned payload would recurse
       into the wrong component there, so it must stay rejected - matching
       the text emitter's own "supported layouts" scope for owning sums. */
    bool two_component_sum = type->kind == COBRA_TYPE_OPTION || type->kind == COBRA_TYPE_RESULT;
    for (size_t i = 0; i < type->generic_arg_count; i++) {
        const CobraType *component = type->generic_args[i];
        if (!component) continue; /* unit variant */
        switch (component->kind) {
            case COBRA_TYPE_I64: case COBRA_TYPE_U64: case COBRA_TYPE_BOOL:
            case COBRA_TYPE_F32: case COBRA_TYPE_F64: case COBRA_TYPE_POINTER:
                continue;
            default:
                if (x86obj_supported_struct(component)) continue;
                if (two_component_sum && bir_is_owned_slice_type(component)) continue;
                return false;
        }
    }
    return true;
}

static bool x86obj_supported_scalar(MirMachineType type) {
    return type == MIR_TYPE_I64 || type == MIR_TYPE_U64 ||
           type == MIR_TYPE_BOOL || type == MIR_TYPE_ADDRESS || x86obj_is_float(type);
}

/* I8 (the language's u8 byte type) can be moved, spilled, and stored through
   registers/memory exactly like BOOL - always carried zero-extended in a
   full 64-bit slot - but this emitter does not yet truncate ADD/SUB/MUL/DIV/
   CMP to 8 bits, so those opcodes must keep rejecting it via
   x86obj_supported_scalar. This wider predicate is only for opcodes that
   only ever move a value (register-shape gating, CONST, ABI_MOVE, LOAD,
   STORE), never compute with it. */
static bool x86obj_movable_scalar(MirMachineType type) {
    return x86obj_supported_scalar(type) || type == MIR_TYPE_I8;
}

static bool x86obj_signed(MirMachineType type) { return type == MIR_TYPE_I64; }

static int64_t x86obj_spill(X86ObjContext *ctx, MirReg reg) {
    return reg < ctx->module->arena.reg_count ? ctx->spill_offsets[reg] : 0;
}

static const MirRegAllocation *x86obj_loc(X86ObjContext *ctx, MirReg reg) {
    if (reg == MIR_REG_NONE || reg >= ctx->allocation->reg_count) return NULL;
    return &ctx->allocation->regs[reg];
}

static bool x86obj_load(X86ObjContext *ctx, MirReg reg, int scratch) {
    const MirRegAllocation *loc = x86obj_loc(ctx, reg);
    if (!loc) return false;
    if (loc->kind == MIR_ALLOC_REGISTER) {
        int phys = x86obj_enc(loc->register_index);
        if (phys < 0) return false;
        emit_mov_rr(ctx, scratch, phys);
        return true;
    }
    if (loc->kind != MIR_ALLOC_SPILL) return false;
    emit_mov_load(ctx, scratch, x86obj_spill(ctx, reg));
    return true;
}

static bool x86obj_store(X86ObjContext *ctx, MirReg reg, int scratch) {
    const MirRegAllocation *loc = x86obj_loc(ctx, reg);
    if (!loc) return false;
    if (loc->kind == MIR_ALLOC_REGISTER) {
        int phys = x86obj_enc(loc->register_index);
        if (phys < 0) return false;
        emit_mov_rr(ctx, phys, scratch);
        return true;
    }
    if (loc->kind != MIR_ALLOC_SPILL) return false;
    emit_mov_store(ctx, x86obj_spill(ctx, reg), scratch);
    return true;
}

static bool x86obj_load_float(X86ObjContext *ctx, MirReg reg, int scratch) {
    const MirRegAllocation *loc = x86obj_loc(ctx, reg);
    if (!loc) return false;
    bool is_f64 = ctx->module->arena.regs[reg].machine_type == MIR_TYPE_F64;
    if (loc->kind == MIR_ALLOC_REGISTER) {
        emit_movx_rr(ctx, is_f64, scratch, loc->register_index);
        return true;
    }
    if (loc->kind != MIR_ALLOC_SPILL) return false;
    emit_movx_load(ctx, is_f64, scratch, x86obj_spill(ctx, reg));
    return true;
}

static bool x86obj_store_float(X86ObjContext *ctx, MirReg reg, int scratch) {
    const MirRegAllocation *loc = x86obj_loc(ctx, reg);
    if (!loc) return false;
    bool is_f64 = ctx->module->arena.regs[reg].machine_type == MIR_TYPE_F64;
    if (loc->kind == MIR_ALLOC_REGISTER) {
        emit_movx_rr(ctx, is_f64, loc->register_index, scratch);
        return true;
    }
    if (loc->kind != MIR_ALLOC_SPILL) return false;
    emit_movx_store(ctx, is_f64, x86obj_spill(ctx, reg), scratch);
    return true;
}

static int64_t x86obj_view_length_spill(X86ObjContext *ctx, MirReg reg) {
    return reg < ctx->module->arena.reg_count ? ctx->view_length_offsets[reg] : 0;
}

/* Views are always paired-spill (never register-allocated - see alloc.c),
   so these only ever touch memory. */
static bool x86obj_load_view_component(X86ObjContext *ctx, MirReg reg, bool length, int scratch) {
    const MirRegAllocation *loc = x86obj_loc(ctx, reg);
    if (!loc || loc->kind != MIR_ALLOC_SPILL ||
        ctx->module->arena.regs[reg].machine_type != MIR_TYPE_VIEW) return false;
    emit_mov_load(ctx, scratch, length ? x86obj_view_length_spill(ctx, reg) : x86obj_spill(ctx, reg));
    return true;
}

static bool x86obj_store_view_component(X86ObjContext *ctx, MirReg reg, bool length, int scratch) {
    const MirRegAllocation *loc = x86obj_loc(ctx, reg);
    if (!loc || loc->kind != MIR_ALLOC_SPILL ||
        ctx->module->arena.regs[reg].machine_type != MIR_TYPE_VIEW) return false;
    emit_mov_store(ctx, length ? x86obj_view_length_spill(ctx, reg) : x86obj_spill(ctx, reg), scratch);
    return true;
}

static bool x86obj_prepare_frame(X86ObjContext *ctx, size_t function_index) {
    const MirArena *arena = &ctx->module->arena;
    const MirFunction *function = &ctx->module->functions[function_index];
    memset(ctx->memory_seen, 0, sizeof(ctx->memory_seen));
    ctx->spill_offsets = calloc(arena->reg_count ? arena->reg_count : 1, sizeof(*ctx->spill_offsets));
    ctx->view_length_offsets = calloc(arena->reg_count ? arena->reg_count : 1, sizeof(*ctx->view_length_offsets));
    if (!ctx->spill_offsets || !ctx->view_length_offsets) {
        x86obj_error(ctx, "out of memory allocating object spill slots");
        return false;
    }
    uint32_t cursor = 0;
    for (MirReg reg = 1; reg < arena->reg_count; reg++) {
        if (arena->regs[reg].function_index != function_index) continue;
        MirMachineType type = arena->regs[reg].machine_type;
        if (!x86obj_movable_scalar(type) && type != MIR_TYPE_VIEW) {
            x86obj_error(ctx, "object emitter supports only i64/u64/bool/address/f32/f64/view values (function %s, reg %u, type %s)",
                        function->name, reg, mir_machine_type_name(type));
            return false;
        }
        const MirRegAllocation *loc = &ctx->allocation->regs[reg];
        if (loc->kind != MIR_ALLOC_SPILL) continue;
        cursor += 8;
        ctx->spill_offsets[reg] = -(int64_t)cursor;
        if (type == MIR_TYPE_VIEW) {
            cursor += 8;
            ctx->view_length_offsets[reg] = -(int64_t)cursor;
        }
    }
    for (size_t offset = 0; offset < function->block_count; offset++) {
        MirBlockRef block_ref = function->first_block + (MirBlockRef)offset;
        const MirBlock *block = &arena->blocks[block_ref];
        for (size_t i = 0; i < block->inst_count; i++) {
            const MirInst *inst = &arena->insts[block->insts[i]];
            if (inst->op != MIR_OP_STACK_SLOT) continue;
            bool is_scalar_width = inst->memory_width == 8 || inst->memory_width == 4 || inst->memory_width == 1;
            bool is_aggregate_slot = inst->memory_type &&
                                     (x86obj_supported_struct(inst->memory_type) ||
                                      x86obj_supported_sum(inst->memory_type) ||
                                      (inst->memory_type->kind == COBRA_TYPE_ARRAY &&
                                       x86obj_supported_field_type(inst->memory_type))) &&
                                     inst->memory_width == inst->memory_type->size;
            if (inst->stack_slot >= BIR_MAX_STACK_SLOTS || !(is_scalar_width || is_aggregate_slot)) {
                x86obj_error(ctx, "object emitter supports only scalar, flat-scalar-struct, flat-sum, and fixed-array stack slots (function %s)",
                            function->name);
                return false;
            }
            if (ctx->memory_seen[inst->stack_slot]) continue;
            ctx->memory_seen[inst->stack_slot] = true;
            uint32_t slot_size = is_aggregate_slot ? (uint32_t)((inst->memory_width + 7) & ~7u) : 8;
            cursor += slot_size;
            ctx->memory_offsets[inst->stack_slot] = -(int64_t)cursor;
        }
    }
    for (size_t i = 0; i < MIR_MAX_OPERANDS; i++) {
        cursor += 8;
        ctx->temp_offsets[i] = -(int64_t)cursor;
    }
    const MirFunctionAllocation *fn_alloc = &ctx->allocation->functions[function_index];
    for (size_t i = 0; i < 5; i++) {
        uint16_t reg_index = (uint16_t)(6 + i);
        if (fn_alloc->used_gpr_mask & (UINT64_C(1) << reg_index)) {
            cursor += 8;
            ctx->callee_saved_offsets[i] = -(int64_t)cursor;
        } else ctx->callee_saved_offsets[i] = 0;
    }
    ctx->frame_size = (cursor % 16) ? cursor + (16 - cursor % 16) : cursor;
    return true;
}

static void x86obj_save_callee_saved(X86ObjContext *ctx, size_t function_index) {
    static const int phys[5] = {3, 12, 13, 14, 15};
    const MirFunctionAllocation *fn_alloc = &ctx->allocation->functions[function_index];
    for (size_t i = 0; i < 5; i++) {
        if (!(fn_alloc->used_gpr_mask & (UINT64_C(1) << (6 + i)))) continue;
        emit_mov_store(ctx, ctx->callee_saved_offsets[i], phys[i]);
    }
}

static void x86obj_restore_callee_saved(X86ObjContext *ctx, size_t function_index) {
    static const int phys[5] = {3, 12, 13, 14, 15};
    const MirFunctionAllocation *fn_alloc = &ctx->allocation->functions[function_index];
    for (size_t i = 0; i < 5; i++) {
        if (!(fn_alloc->used_gpr_mask & (UINT64_C(1) << (6 + i)))) continue;
        emit_mov_load(ctx, phys[i], ctx->callee_saved_offsets[i]);
    }
}

static int x86obj_cmp_cc(MirOpcode op, bool is_signed) {
    switch (op) {
        case MIR_OP_EQ: return 0x4;
        case MIR_OP_NE: return 0x5;
        case MIR_OP_LT: return is_signed ? 0xC : 0x2;
        case MIR_OP_GE: return is_signed ? 0xD : 0x3;
        case MIR_OP_LE: return is_signed ? 0xE : 0x6;
        case MIR_OP_GT: return is_signed ? 0xF : 0x7;
        default: return -1;
    }
}

/* Verifies that [pointer, pointer+width) lies within view_source's bounds.
   pointer must already be loaded into X86OBJ_R10; on success r10 is restored
   to its original value (the check only validates it, never adjusts it).
   Traps with ud2 on failure, mirroring the text emitter's contract. */
static bool x86obj_emit_view_bounds_check(X86ObjContext *ctx, MirReg view_source, uint32_t width) {
    const CobraType *view_type = ctx->module->arena.regs[view_source].type;
    const CobraType *element = view_type ? cobra_type_element(view_type) : NULL;
    if (!element || element->size == 0) return false;
    emit_mov_store(ctx, ctx->temp_offsets[0], X86OBJ_R10);
    if (!x86obj_load_view_component(ctx, view_source, false, X86OBJ_RAX) ||
        !x86obj_load_view_component(ctx, view_source, true, X86OBJ_RDX)) return false;
    emit_cmp_rr(ctx, X86OBJ_R10, X86OBJ_RAX);
    size_t fail_below = emit_jcc_fwd(ctx, 0x2 /* jb */);
    emit_sub_rr(ctx, X86OBJ_R10, X86OBJ_RAX);
    emit_imul_r_imm32(ctx, X86OBJ_RDX, (uint32_t)element->size);
    emit_add_r_imm32(ctx, X86OBJ_R10, (int32_t)width);
    emit_cmp_rr(ctx, X86OBJ_R10, X86OBJ_RDX);
    size_t fail_above = emit_jcc_fwd(ctx, 0x7 /* ja */);
    size_t to_done = emit_jmp_fwd(ctx);
    patch_fwd(ctx, fail_below);
    patch_fwd(ctx, fail_above);
    emit_ud2(ctx);
    patch_fwd(ctx, to_done);
    emit_mov_load(ctx, X86OBJ_R10, ctx->temp_offsets[0]);
    return true;
}

static bool x86obj_call_arg_moves_ownership(const X86ObjContext *ctx, const MirInst *inst, size_t arg) {
    if (!ctx->module->source || inst->callee_index >= ctx->module->source->function_count) return false;
    const BirFunctionInfo *callee = &ctx->module->source->functions[inst->callee_index];
    if (callee->has_hidden_return_storage && arg == 0) return false;
    size_t user_arg = arg - (callee->has_hidden_return_storage ? 1U : 0U);
    return user_arg < callee->param_count && bir_is_owned_slice_type(callee->param_value_types[user_arg]);
}

static bool x86obj_zero_view(X86ObjContext *ctx, MirReg view) {
    emit_movabs(ctx, X86OBJ_R10, 0);
    return x86obj_store_view_component(ctx, view, false, X86OBJ_R10) &&
           x86obj_store_view_component(ctx, view, true, X86OBJ_R10);
}

static bool x86obj_emit_copy_bytes(X86ObjContext *ctx, MirReg destination, MirReg source, uint32_t width) {
    if (!x86obj_load(ctx, destination, X86OBJ_R10) || !x86obj_load(ctx, source, X86OBJ_R11)) return false;
    uint32_t offset = 0;
    while (width - offset >= 8) {
        emit_mov_mem_disp(ctx, true, 8, X86OBJ_RAX, X86OBJ_R11, offset);
        emit_mov_mem_disp(ctx, false, 8, X86OBJ_RAX, X86OBJ_R10, offset);
        offset += 8;
    }
    if (width - offset >= 4) {
        emit_mov_mem_disp(ctx, true, 4, X86OBJ_RAX, X86OBJ_R11, offset);
        emit_mov_mem_disp(ctx, false, 4, X86OBJ_RAX, X86OBJ_R10, offset);
        offset += 4;
    }
    while (offset < width) {
        emit_mov_mem_disp(ctx, true, 1, X86OBJ_RAX, X86OBJ_R11, offset);
        emit_mov_mem_disp(ctx, false, 1, X86OBJ_RAX, X86OBJ_R10, offset);
        offset++;
    }
    return true;
}

static bool x86obj_emit_zero_bytes(X86ObjContext *ctx, MirReg source, uint32_t width) {
    if (width == 0 || !x86obj_load(ctx, source, X86OBJ_R10)) return false;
    emit_movabs(ctx, X86OBJ_RAX, 0);
    uint32_t offset = 0;
    while (width - offset >= 8) { emit_mov_mem_disp(ctx, false, 8, X86OBJ_RAX, X86OBJ_R10, offset); offset += 8; }
    if (width - offset >= 4) { emit_mov_mem_disp(ctx, false, 4, X86OBJ_RAX, X86OBJ_R10, offset); offset += 4; }
    while (offset < width) { emit_mov_mem_disp(ctx, false, 1, X86OBJ_RAX, X86OBJ_R10, offset); offset++; }
    return true;
}

/* Recursively frees any owned-slice bytes reachable inside `type` at
   `source + base_offset`, mirroring the text emitter's
   x86_alloc_emit_drop_owned_value. Under x86obj_supported_struct's current
   gate, an owned component can only be a direct field (never nested inside
   an unsupported sum shape), so only the struct-recursion and owned-slice-
   leaf cases are reachable; bir_type_has_owned_payload is still checked
   generically so this stays correct if that gate is ever widened further. */
static bool x86obj_emit_drop_owned_value(X86ObjContext *ctx, MirReg source,
                                         const CobraType *type, size_t base_offset) {
    if (!type || !bir_type_has_owned_payload(type)) return true;
    if (bir_is_owned_slice_type(type)) {
        if (!x86obj_load(ctx, source, X86OBJ_R10)) return false;
        if (base_offset != 0) {
            if (base_offset > (size_t)INT32_MAX) return false;
            emit_add_r_imm32(ctx, X86OBJ_R10, (int32_t)base_offset);
        }
        emit_mov_load_indirect(ctx, X86OBJ_R11, X86OBJ_R10);
        emit_test_rr(ctx, X86OBJ_R11);
        size_t done = emit_jcc_fwd(ctx, 0x4 /* je */);
        emit_movabs(ctx, X86OBJ_RAX, 0);
        emit_mov_mem_disp(ctx, false, 8, X86OBJ_RAX, X86OBJ_R10, 0);
        emit_mov_mem_disp(ctx, false, 8, X86OBJ_RAX, X86OBJ_R10, 8);
        emit_mov_rr(ctx, 7 /* rdi */, X86OBJ_R11);
        if (!emit_call_extern(ctx, "free")) return false;
        patch_fwd(ctx, done);
        return true;
    }
    if (type->kind == COBRA_TYPE_STRUCT) {
        for (size_t i = 0; i < type->field_count; i++)
            if (!x86obj_emit_drop_owned_value(ctx, source, type->fields[i].type,
                                              base_offset + type->fields[i].offset)) return false;
        return true;
    }
    if (bir_is_sum_type(type)) {
        /* Only reachable for Option/Result (x86obj_supported_sum gates any
           other owning sum shape out entirely - see its comment). */
        if (!x86obj_load(ctx, source, X86OBJ_R10)) return false;
        if (base_offset != 0) {
            if (base_offset > (size_t)INT32_MAX) return false;
            emit_add_r_imm32(ctx, X86OBJ_R10, (int32_t)base_offset);
        }
        emit_mov_load_indirect(ctx, X86OBJ_R11, X86OBJ_R10);
        emit_movabs(ctx, X86OBJ_RAX, 1);
        emit_cmp_rr(ctx, X86OBJ_R11, X86OBJ_RAX);
        size_t to_second = emit_jcc_fwd(ctx, 0x5 /* jne */);
        const CobraType *first = type->generic_args[0];
        if (!x86obj_emit_drop_owned_value(ctx, source, first,
                                          base_offset + bir_sum_component_offset(type, 1))) return false;
        size_t to_done = emit_jmp_fwd(ctx);
        patch_fwd(ctx, to_second);
        if (type->kind != COBRA_TYPE_OPTION) {
            const CobraType *second = type->generic_args[1];
            if (!x86obj_emit_drop_owned_value(ctx, source, second,
                                              base_offset + bir_sum_component_offset(type, 2))) return false;
        }
        patch_fwd(ctx, to_done);
        return true;
    }
    return true; /* unreachable under the current supported-shape gates */
}

/* Stores/loads an owned view (string/list[T]) into/out of a struct or sum
   field, transferring ownership: the source view is zeroed on store, and
   the field is zeroed on load (matching the text emitter's
   x86_alloc_emit_owned_payload_store/load exactly). Used for both struct
   fields (MIR_OP_FIELD_PAYLOAD_*) and sum payloads (MIR_OP_SUM_PAYLOAD_*) -
   the two opcode pairs share this one implementation there too. */
static bool x86obj_emit_owned_payload_store(X86ObjContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->memory_offset < 0) return false;
    MirReg destination = ctx->module->arena.operands[inst->operand_start];
    MirReg payload = ctx->module->arena.operands[inst->operand_start + 1];
    if (ctx->module->arena.regs[destination].machine_type != MIR_TYPE_ADDRESS ||
        ctx->module->arena.regs[payload].machine_type != MIR_TYPE_VIEW) return false;
    if (!x86obj_load(ctx, destination, X86OBJ_R10) ||
        !x86obj_load_view_component(ctx, payload, false, X86OBJ_R11) ||
        !x86obj_load_view_component(ctx, payload, true, X86OBJ_RAX)) return false;
    if (inst->memory_offset != 0) {
        if (inst->memory_offset > INT32_MAX) return false;
        emit_add_r_imm32(ctx, X86OBJ_R10, (int32_t)inst->memory_offset);
    }
    emit_mov_mem_disp(ctx, false, 8, X86OBJ_R11, X86OBJ_R10, 0);
    emit_mov_mem_disp(ctx, false, 8, X86OBJ_RAX, X86OBJ_R10, 8);
    return x86obj_zero_view(ctx, payload);
}

static bool x86obj_emit_owned_payload_load(X86ObjContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || inst->result == MIR_REG_NONE || inst->memory_offset < 0) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    if (ctx->module->arena.regs[source].machine_type != MIR_TYPE_ADDRESS ||
        ctx->module->arena.regs[inst->result].machine_type != MIR_TYPE_VIEW) return false;
    if (!x86obj_load(ctx, source, X86OBJ_R10)) return false;
    if (inst->memory_offset != 0) {
        if (inst->memory_offset > INT32_MAX) return false;
        emit_add_r_imm32(ctx, X86OBJ_R10, (int32_t)inst->memory_offset);
    }
    emit_mov_mem_disp(ctx, true, 8, X86OBJ_R11, X86OBJ_R10, 0);
    emit_mov_mem_disp(ctx, true, 8, X86OBJ_RAX, X86OBJ_R10, 8);
    if (!x86obj_store_view_component(ctx, inst->result, false, X86OBJ_R11) ||
        !x86obj_store_view_component(ctx, inst->result, true, X86OBJ_RAX)) return false;
    emit_movabs(ctx, X86OBJ_RCX, 0);
    emit_mov_mem_disp(ctx, false, 8, X86OBJ_RCX, X86OBJ_R10, 0);
    emit_mov_mem_disp(ctx, false, 8, X86OBJ_RCX, X86OBJ_R10, 8);
    return true;
}

static bool x86obj_emit_call(X86ObjContext *ctx, const MirInst *inst) {
    const MirFunction *callee = &ctx->module->functions[inst->callee_index];
    if (inst->operand_count > MIR_MAX_OPERANDS) return false;
    for (size_t arg = 0; arg < inst->operand_count; arg++) {
        MirReg value = ctx->module->arena.operands[inst->operand_start + arg];
        MirMachineType type = ctx->module->arena.regs[value].machine_type;
        if (type == MIR_TYPE_VIEW) continue; /* always memory-resident; no staging hazard, see below */
        if (callee->call_abi.params[arg].count != 1 ||
            callee->call_abi.params[arg].parts[0].storage != BIR_ABI_STORAGE_REGISTER)
            return false; /* stack-passed / multi-part scalar args are outside the v1 lane */
        if (x86obj_is_float(type)) {
            if (!x86obj_load_float(ctx, value, X86OBJ_XMM_SCRATCH0)) return false;
            emit_movx_store(ctx, type == MIR_TYPE_F64, ctx->temp_offsets[arg], X86OBJ_XMM_SCRATCH0);
        } else {
            if (!x86obj_load(ctx, value, X86OBJ_R10)) return false;
            emit_mov_store(ctx, ctx->temp_offsets[arg], X86OBJ_R10);
        }
    }
    for (size_t arg = 0; arg < inst->operand_count; arg++) {
        MirReg value = ctx->module->arena.operands[inst->operand_start + arg];
        MirMachineType type = ctx->module->arena.regs[value].machine_type;
        if (type == MIR_TYPE_VIEW) {
            /* Owned buffers are 3-part (ptr, len, cap); the native view model
               has no capacity, so the 3rd part is fed the length again,
               matching the text emitter's contract. Borrow-only: the source
               view is never zeroed, so this does not yet support moving
               ownership of an owned slice/buffer through a call boundary. */
            if (callee->call_abi.params[arg].count != 2 && callee->call_abi.params[arg].count != 3) return false;
            for (size_t part = 0; part < callee->call_abi.params[arg].count; part++) {
                const BirAbiLocation *location = &callee->call_abi.params[arg].parts[part];
                if (location->storage != BIR_ABI_STORAGE_REGISTER) return false;
                int phys = x86obj_enc(location->register_index);
                if (phys < 0 || !x86obj_load_view_component(ctx, value, part != 0, phys)) return false;
            }
            continue;
        }
        const BirAbiLocation *location = &callee->call_abi.params[arg].parts[0];
        if (x86obj_is_float(type)) {
            if (location->register_class != BIR_ABI_REGISTER_XMM) return false;
            emit_movx_load(ctx, type == MIR_TYPE_F64, location->register_index, ctx->temp_offsets[arg]);
        } else {
            int phys = x86obj_enc(location->register_index);
            if (phys < 0) return false;
            emit_mov_load(ctx, X86OBJ_R10, ctx->temp_offsets[arg]);
            emit_mov_rr(ctx, phys, X86OBJ_R10);
        }
    }
    for (size_t arg = 0; arg < inst->operand_count; arg++) {
        MirReg value = ctx->module->arena.operands[inst->operand_start + arg];
        if (ctx->module->arena.regs[value].machine_type == MIR_TYPE_VIEW &&
            x86obj_call_arg_moves_ownership(ctx, inst, arg) &&
            !x86obj_zero_view(ctx, value)) return false;
    }
    emit_call(ctx, inst->callee_index);
    if (inst->result != MIR_REG_NONE) {
        if (!x86obj_supported_scalar(inst->machine_type)) return false;
        if (x86obj_is_float(inst->machine_type)) {
            if (!x86obj_store_float(ctx, inst->result, 0 /* xmm0 */)) return false;
        } else if (!x86obj_store(ctx, inst->result, X86OBJ_RAX)) return false;
    }
    return true;
}

static bool x86obj_emit_inst(X86ObjContext *ctx, size_t function_index, const MirInst *inst) {
    switch (inst->op) {
        case MIR_OP_CONST: {
            if (!inst->has_immediate || inst->result == MIR_REG_NONE ||
                !x86obj_movable_scalar(inst->machine_type)) return false;
            if (inst->machine_type == MIR_TYPE_F32) {
                emit_movabs(ctx, X86OBJ_R10, (uint64_t)inst->immediate.payload.f32_bits);
                emit_movd_gpr_to_xmm(ctx, false, X86OBJ_XMM_SCRATCH0, X86OBJ_R10);
                return x86obj_store_float(ctx, inst->result, X86OBJ_XMM_SCRATCH0);
            }
            if (inst->machine_type == MIR_TYPE_F64) {
                emit_movabs(ctx, X86OBJ_R10, inst->immediate.payload.f64_bits);
                emit_movd_gpr_to_xmm(ctx, true, X86OBJ_XMM_SCRATCH0, X86OBJ_R10);
                return x86obj_store_float(ctx, inst->result, X86OBJ_XMM_SCRATCH0);
            }
            emit_movabs(ctx, X86OBJ_R10, (uint64_t)inst->immediate.payload.i64);
            return x86obj_store(ctx, inst->result, X86OBJ_R10);
        }
        case MIR_OP_ABI_MOVE: {
            if (inst->result == MIR_REG_NONE) return false;
            if (inst->machine_type == MIR_TYPE_VIEW) {
                /* Owned buffers are 3-part (ptr, len, cap); the native view
                   model keeps only ptr and len, so a 3rd part is received
                   but discarded, matching the text emitter's contract. */
                if (inst->abi_locations.count != 2 && inst->abi_locations.count != 3) return false;
                for (size_t part = 0; part < inst->abi_locations.count && part < 2; part++) {
                    const BirAbiLocation *location = &inst->abi_locations.parts[part];
                    if (location->storage != BIR_ABI_STORAGE_REGISTER) return false;
                    int phys = x86obj_enc(location->register_index);
                    if (phys < 0) return false;
                    if (!x86obj_store_view_component(ctx, inst->result, part == 1, phys)) return false;
                }
                return true;
            }
            if (inst->abi_locations.count != 1 || !x86obj_movable_scalar(inst->machine_type)) return false;
            const BirAbiLocation *location = &inst->abi_locations.parts[0];
            if (location->storage != BIR_ABI_STORAGE_REGISTER) return false;
            if (x86obj_is_float(inst->machine_type)) {
                if (location->register_class != BIR_ABI_REGISTER_XMM) return false;
                return x86obj_store_float(ctx, inst->result, location->register_index);
            }
            int phys = x86obj_enc(location->register_index);
            if (phys < 0) return false;
            return x86obj_store(ctx, inst->result, phys);
        }
        case MIR_OP_ADD:
        case MIR_OP_SUB:
        case MIR_OP_MUL: {
            if (!x86obj_supported_scalar(inst->machine_type)) return false;
            MirReg lhs = ctx->module->arena.operands[inst->operand_start];
            MirReg rhs = ctx->module->arena.operands[inst->operand_start + 1];
            if (x86obj_is_float(inst->machine_type)) {
                bool is_f64 = inst->machine_type == MIR_TYPE_F64;
                if (!x86obj_load_float(ctx, lhs, X86OBJ_XMM_SCRATCH0) ||
                    !x86obj_load_float(ctx, rhs, X86OBJ_XMM_SCRATCH1)) return false;
                if (inst->op == MIR_OP_ADD) emit_addx(ctx, is_f64, X86OBJ_XMM_SCRATCH0, X86OBJ_XMM_SCRATCH1);
                else if (inst->op == MIR_OP_SUB) emit_subx(ctx, is_f64, X86OBJ_XMM_SCRATCH0, X86OBJ_XMM_SCRATCH1);
                else emit_mulx(ctx, is_f64, X86OBJ_XMM_SCRATCH0, X86OBJ_XMM_SCRATCH1);
                return x86obj_store_float(ctx, inst->result, X86OBJ_XMM_SCRATCH0);
            }
            if (!x86obj_load(ctx, lhs, X86OBJ_R10) || !x86obj_load(ctx, rhs, X86OBJ_R11)) return false;
            if (inst->op == MIR_OP_ADD) emit_add_rr(ctx, X86OBJ_R10, X86OBJ_R11);
            else if (inst->op == MIR_OP_SUB) emit_sub_rr(ctx, X86OBJ_R10, X86OBJ_R11);
            else emit_imul_rr(ctx, X86OBJ_R10, X86OBJ_R11);
            return x86obj_store(ctx, inst->result, X86OBJ_R10);
        }
        case MIR_OP_NEG: {
            if (!x86obj_supported_scalar(inst->machine_type)) return false;
            MirReg operand = ctx->module->arena.operands[inst->operand_start];
            if (x86obj_is_float(inst->machine_type)) {
                bool is_f64 = inst->machine_type == MIR_TYPE_F64;
                if (!x86obj_load_float(ctx, operand, X86OBJ_XMM_SCRATCH0)) return false;
                emit_movd_xmm_to_gpr(ctx, is_f64, X86OBJ_R10, X86OBJ_XMM_SCRATCH0);
                emit_movabs(ctx, X86OBJ_R11, is_f64 ? UINT64_C(0x8000000000000000) : UINT64_C(0x80000000));
                emit_xor_rr2(ctx, X86OBJ_R10, X86OBJ_R11);
                emit_movd_gpr_to_xmm(ctx, is_f64, X86OBJ_XMM_SCRATCH0, X86OBJ_R10);
                return x86obj_store_float(ctx, inst->result, X86OBJ_XMM_SCRATCH0);
            }
            if (!x86obj_load(ctx, operand, X86OBJ_R10)) return false;
            emit_neg_r(ctx, X86OBJ_R10);
            return x86obj_store(ctx, inst->result, X86OBJ_R10);
        }
        case MIR_OP_CONVERT: {
            /* Same v1 scope line as every other compute opcode in this
               emitter (see x86obj_supported_scalar/x86obj_movable_scalar
               above): I32/U32 aren't encoded here yet, so a convert
               touching either width cleanly fails the object build instead
               of emitting a value that lost its width truncation. */
            MirReg operand = ctx->module->arena.operands[inst->operand_start];
            MirMachineType from = ctx->module->arena.regs[operand].machine_type;
            MirMachineType to = inst->machine_type;
            if (!x86obj_supported_scalar(from) || !x86obj_supported_scalar(to)) return false;
            if (x86obj_is_float(from) && x86obj_is_float(to)) {
                if (!x86obj_load_float(ctx, operand, X86OBJ_XMM_SCRATCH0)) return false;
                if (from != to) emit_cvt_float_width(ctx, from == MIR_TYPE_F64,
                                                     X86OBJ_XMM_SCRATCH0, X86OBJ_XMM_SCRATCH0);
                return x86obj_store_float(ctx, inst->result, X86OBJ_XMM_SCRATCH0);
            }
            if (x86obj_is_float(from)) {
                if (!x86obj_load_float(ctx, operand, X86OBJ_XMM_SCRATCH0)) return false;
                if (to == MIR_TYPE_BOOL) {
                    emit_pxor_x(ctx, X86OBJ_XMM_SCRATCH1, X86OBJ_XMM_SCRATCH1);
                    emit_ucomix(ctx, from == MIR_TYPE_F64, X86OBJ_XMM_SCRATCH0, X86OBJ_XMM_SCRATCH1);
                    emit_setcc(ctx, 0x5 /* setne */, X86OBJ_R10);
                    emit_movzx64_8(ctx, X86OBJ_R10, X86OBJ_R10);
                    return x86obj_store(ctx, inst->result, X86OBJ_R10);
                }
                emit_cvttsx2si(ctx, from == MIR_TYPE_F64, X86OBJ_R10, X86OBJ_XMM_SCRATCH0);
                return x86obj_store(ctx, inst->result, X86OBJ_R10);
            }
            /* Every supported int/bool width here is already carried as a
               clean, fully-extended 64-bit slot (see x86obj_movable_scalar's
               comment), so no extra widening load is needed before using it. */
            if (!x86obj_load(ctx, operand, X86OBJ_R10)) return false;
            if (x86obj_is_float(to)) {
                emit_cvtsi2sx(ctx, to == MIR_TYPE_F64, X86OBJ_XMM_SCRATCH0, X86OBJ_R10);
                return x86obj_store_float(ctx, inst->result, X86OBJ_XMM_SCRATCH0);
            }
            if (to == MIR_TYPE_BOOL) {
                emit_test_rr(ctx, X86OBJ_R10);
                emit_setcc(ctx, 0x5, X86OBJ_R10);
                emit_movzx64_8(ctx, X86OBJ_R10, X86OBJ_R10);
                return x86obj_store(ctx, inst->result, X86OBJ_R10);
            }
            /* i64<->u64 share a bit pattern; the clean 64-bit slot needs
               nothing further. */
            return x86obj_store(ctx, inst->result, X86OBJ_R10);
        }
        case MIR_OP_DIV:
        case MIR_OP_REM: {
            if (!x86obj_supported_scalar(inst->machine_type)) return false;
            MirReg lhs = ctx->module->arena.operands[inst->operand_start];
            MirReg rhs = ctx->module->arena.operands[inst->operand_start + 1];
            if (x86obj_is_float(inst->machine_type)) {
                if (inst->op == MIR_OP_REM) return false; /* not supported at this layer */
                bool is_f64 = inst->machine_type == MIR_TYPE_F64;
                if (!x86obj_load_float(ctx, lhs, X86OBJ_XMM_SCRATCH0) ||
                    !x86obj_load_float(ctx, rhs, X86OBJ_XMM_SCRATCH1)) return false;
                emit_divx(ctx, is_f64, X86OBJ_XMM_SCRATCH0, X86OBJ_XMM_SCRATCH1);
                return x86obj_store_float(ctx, inst->result, X86OBJ_XMM_SCRATCH0);
            }
            bool is_signed = x86obj_signed(inst->machine_type);
            if (!x86obj_load(ctx, lhs, X86OBJ_RAX) || !x86obj_load(ctx, rhs, X86OBJ_R10)) return false;
            if (is_signed) emit_cqo(ctx); else emit_xor_rr(ctx, X86OBJ_RDX);
            emit_idiv(ctx, X86OBJ_R10, is_signed);
            return x86obj_store(ctx, inst->result, inst->op == MIR_OP_REM ? X86OBJ_RDX : X86OBJ_RAX);
        }
        case MIR_OP_EQ: case MIR_OP_NE: case MIR_OP_LT:
        case MIR_OP_LE: case MIR_OP_GT: case MIR_OP_GE: {
            MirReg lhs = ctx->module->arena.operands[inst->operand_start];
            MirReg rhs = ctx->module->arena.operands[inst->operand_start + 1];
            MirMachineType operand_type = ctx->module->arena.regs[lhs].machine_type;
            if (!x86obj_supported_scalar(operand_type)) return false;
            if (x86obj_is_float(operand_type)) {
                bool is_f64 = operand_type == MIR_TYPE_F64;
                if (!x86obj_load_float(ctx, lhs, X86OBJ_XMM_SCRATCH0) ||
                    !x86obj_load_float(ctx, rhs, X86OBJ_XMM_SCRATCH1)) return false;
                emit_ucomix(ctx, is_f64, X86OBJ_XMM_SCRATCH0, X86OBJ_XMM_SCRATCH1);
                /* Ordered/unordered condition pairs mirror the text emitter:
                   NaN must make EQ/LT/LE/GT/GE false and NE true. */
                int ordered_cc, unordered_cc; bool combine_or;
                switch (inst->op) {
                    case MIR_OP_EQ: ordered_cc = 0x4; unordered_cc = 0xB /*setnp*/; combine_or = false; break;
                    case MIR_OP_NE: ordered_cc = 0x5; unordered_cc = 0xA /*setp*/; combine_or = true; break;
                    case MIR_OP_LT: ordered_cc = 0x2; unordered_cc = 0xB; combine_or = false; break;
                    case MIR_OP_LE: ordered_cc = 0x6; unordered_cc = 0xB; combine_or = false; break;
                    case MIR_OP_GT: ordered_cc = 0x7; unordered_cc = 0xB; combine_or = false; break;
                    case MIR_OP_GE: ordered_cc = 0x3; unordered_cc = 0xB; combine_or = false; break;
                    default: return false;
                }
                emit_setcc(ctx, ordered_cc, X86OBJ_RAX);
                emit_setcc(ctx, unordered_cc, X86OBJ_RDX);
                if (combine_or) emit_or_r8(ctx, X86OBJ_RAX, X86OBJ_RDX);
                else emit_and_r8(ctx, X86OBJ_RAX, X86OBJ_RDX);
                emit_movzx64_8(ctx, X86OBJ_R10, X86OBJ_RAX);
                return x86obj_store(ctx, inst->result, X86OBJ_R10);
            }
            int cc = x86obj_cmp_cc(inst->op, x86obj_signed(operand_type));
            if (cc < 0) return false;
            if (!x86obj_load(ctx, lhs, X86OBJ_R10) || !x86obj_load(ctx, rhs, X86OBJ_R11)) return false;
            emit_cmp_rr(ctx, X86OBJ_R10, X86OBJ_R11);
            emit_setcc(ctx, cc, X86OBJ_R10);
            emit_movzx64_8(ctx, X86OBJ_R10, X86OBJ_R10);
            return x86obj_store(ctx, inst->result, X86OBJ_R10);
        }
        case MIR_OP_JUMP:
            emit_jmp(ctx, inst->target);
            return true;
        case MIR_OP_BRANCH: {
            MirReg cond = ctx->module->arena.operands[inst->operand_start];
            if (!x86obj_load(ctx, cond, X86OBJ_R10)) return false;
            emit_test_rr(ctx, X86OBJ_R10);
            emit_jcc(ctx, 0x5 /* jne */, inst->target);
            emit_jmp(ctx, inst->target2);
            return true;
        }
        case MIR_OP_STACK_SLOT: {
            if (inst->stack_slot >= BIR_MAX_STACK_SLOTS || !ctx->memory_seen[inst->stack_slot]) return false;
            emit_lea_rbp(ctx, X86OBJ_R10, ctx->memory_offsets[inst->stack_slot]);
            return x86obj_store(ctx, inst->result, X86OBJ_R10);
        }
        case MIR_OP_LOAD: {
            if (inst->result == MIR_REG_NONE || !x86obj_movable_scalar(inst->machine_type)) return false;
            MirReg pointer = ctx->module->arena.operands[inst->operand_start];
            if (!x86obj_load(ctx, pointer, X86OBJ_R10)) return false;
            if (inst->view_source != MIR_REG_NONE &&
                !x86obj_emit_view_bounds_check(ctx, inst->view_source, inst->memory_width)) return false;
            if (x86obj_is_float(inst->machine_type)) {
                emit_movx_load_indirect(ctx, inst->machine_type == MIR_TYPE_F64,
                                        X86OBJ_XMM_SCRATCH0, X86OBJ_R10);
                return x86obj_store_float(ctx, inst->result, X86OBJ_XMM_SCRATCH0);
            }
            if ((inst->machine_type == MIR_TYPE_BOOL || inst->machine_type == MIR_TYPE_I8) && inst->memory_width == 1)
                emit_movzx64_8_indirect(ctx, X86OBJ_R11, X86OBJ_R10);
            else
                emit_mov_load_indirect(ctx, X86OBJ_R11, X86OBJ_R10);
            return x86obj_store(ctx, inst->result, X86OBJ_R11);
        }
        case MIR_OP_STORE: {
            if (inst->operand_count != 2) return false;
            MirReg pointer = ctx->module->arena.operands[inst->operand_start];
            MirReg value = ctx->module->arena.operands[inst->operand_start + 1];
            MirMachineType value_type = ctx->module->arena.regs[value].machine_type;
            if (!x86obj_movable_scalar(value_type)) return false;
            if (!x86obj_load(ctx, pointer, X86OBJ_R10)) return false;
            if (inst->view_source != MIR_REG_NONE &&
                !x86obj_emit_view_bounds_check(ctx, inst->view_source, inst->memory_width)) return false;
            if (x86obj_is_float(value_type)) {
                if (!x86obj_load_float(ctx, value, X86OBJ_XMM_SCRATCH0)) return false;
                emit_movx_store_indirect(ctx, value_type == MIR_TYPE_F64, X86OBJ_R10, X86OBJ_XMM_SCRATCH0);
                return true;
            }
            if (!x86obj_load(ctx, value, X86OBJ_R11)) return false;
            if ((value_type == MIR_TYPE_BOOL || value_type == MIR_TYPE_I8) && inst->memory_width == 1)
                emit_movb_store_indirect(ctx, X86OBJ_R10, X86OBJ_R11);
            else
                emit_mov_store_indirect(ctx, X86OBJ_R10, X86OBJ_R11);
            return true;
        }
        case MIR_OP_PTR_ADD: {
            if (inst->result == MIR_REG_NONE || inst->operand_count != 2) return false;
            MirReg pointer = ctx->module->arena.operands[inst->operand_start];
            MirReg offset = ctx->module->arena.operands[inst->operand_start + 1];
            if (!x86obj_load(ctx, pointer, X86OBJ_R10) || !x86obj_load(ctx, offset, X86OBJ_R11)) return false;
            if (inst->view_source != MIR_REG_NONE) {
                const CobraType *view_type = ctx->module->arena.regs[inst->view_source].type;
                const CobraType *element = view_type ? cobra_type_element(view_type) : NULL;
                if (!element || element->size == 0 ||
                    !x86obj_load_view_component(ctx, inst->view_source, true, X86OBJ_RDX)) return false;
                emit_test_rr(ctx, X86OBJ_R11);
                size_t fail_neg = emit_jcc_fwd(ctx, 0x8 /* js */);
                emit_imul_r_imm32(ctx, X86OBJ_RDX, (uint32_t)element->size);
                emit_cmp_rr(ctx, X86OBJ_R11, X86OBJ_RDX);
                size_t fail_above = emit_jcc_fwd(ctx, 0x7 /* ja */);
                size_t to_done = emit_jmp_fwd(ctx);
                patch_fwd(ctx, fail_neg);
                patch_fwd(ctx, fail_above);
                emit_ud2(ctx);
                patch_fwd(ctx, to_done);
            }
            emit_add_rr(ctx, X86OBJ_R10, X86OBJ_R11);
            return x86obj_store(ctx, inst->result, X86OBJ_R10);
        }
        case MIR_OP_VIEW_MAKE: {
            if (inst->machine_type != MIR_TYPE_VIEW || inst->operand_count != 2 ||
                inst->result == MIR_REG_NONE || !inst->memory_type || inst->memory_type->size == 0) return false;
            MirReg pointer = ctx->module->arena.operands[inst->operand_start];
            MirReg length = ctx->module->arena.operands[inst->operand_start + 1];
            MirReg source = inst->view_source;
            if (!x86obj_load(ctx, pointer, X86OBJ_R10) || !x86obj_load(ctx, length, X86OBJ_R11)) return false;
            emit_mov_store(ctx, ctx->temp_offsets[0], X86OBJ_R10);
            emit_test_rr(ctx, X86OBJ_R11);
            size_t fail_neg = emit_jcc_fwd(ctx, 0x8 /* js */);
            size_t fail_below = 0, fail_above = 0;
            bool has_source_checks = false;
            if (source != MIR_REG_NONE) {
                if (!x86obj_load_view_component(ctx, source, false, X86OBJ_RAX) ||
                    !x86obj_load_view_component(ctx, source, true, X86OBJ_RDX)) return false;
                emit_cmp_rr(ctx, X86OBJ_R10, X86OBJ_RAX);
                fail_below = emit_jcc_fwd(ctx, 0x2 /* jb */);
                emit_sub_rr(ctx, X86OBJ_R10, X86OBJ_RAX);
                emit_imul_r_imm32(ctx, X86OBJ_RDX, (uint32_t)inst->memory_type->size);
                emit_mov_rr(ctx, X86OBJ_RCX, X86OBJ_R11);
                emit_imul_r_imm32(ctx, X86OBJ_RCX, (uint32_t)inst->memory_type->size);
                emit_add_rr(ctx, X86OBJ_R10, X86OBJ_RCX);
                emit_cmp_rr(ctx, X86OBJ_R10, X86OBJ_RDX);
                fail_above = emit_jcc_fwd(ctx, 0x7 /* ja */);
                has_source_checks = true;
            }
            size_t to_done = emit_jmp_fwd(ctx);
            patch_fwd(ctx, fail_neg);
            if (has_source_checks) { patch_fwd(ctx, fail_below); patch_fwd(ctx, fail_above); }
            emit_ud2(ctx);
            patch_fwd(ctx, to_done);
            emit_mov_load(ctx, X86OBJ_R10, ctx->temp_offsets[0]);
            return x86obj_store_view_component(ctx, inst->result, false, X86OBJ_R10) &&
                   x86obj_store_view_component(ctx, inst->result, true, X86OBJ_R11);
        }
        case MIR_OP_VIEW_PTR: {
            if (inst->result == MIR_REG_NONE || inst->operand_count != 1) return false;
            MirReg view = ctx->module->arena.operands[inst->operand_start];
            if (!x86obj_load_view_component(ctx, view, false, X86OBJ_R10)) return false;
            return x86obj_store(ctx, inst->result, X86OBJ_R10);
        }
        case MIR_OP_VIEW_LEN: {
            if (inst->result == MIR_REG_NONE || inst->operand_count != 1) return false;
            MirReg view = ctx->module->arena.operands[inst->operand_start];
            if (!x86obj_load_view_component(ctx, view, true, X86OBJ_R10)) return false;
            return x86obj_store(ctx, inst->result, X86OBJ_R10);
        }
        case MIR_OP_SUM_CHECK: {
            if (inst->operand_count != 1) return false;
            MirReg tag = ctx->module->arena.operands[inst->operand_start];
            int64_t expected = inst->sum_check_kind == 2 ? 0
                : (inst->sum_check_kind == 3 ? inst->sum_check_expected : 1);
            if (!x86obj_load(ctx, tag, X86OBJ_R10)) return false;
            emit_movabs(ctx, X86OBJ_R11, (uint64_t)expected);
            emit_cmp_rr(ctx, X86OBJ_R10, X86OBJ_R11);
            size_t fail = emit_jcc_fwd(ctx, 0x5 /* jne */);
            size_t to_done = emit_jmp_fwd(ctx);
            patch_fwd(ctx, fail);
            emit_ud2(ctx);
            patch_fwd(ctx, to_done);
            return true;
        }
        case MIR_OP_PRINT_I64: {
            if (inst->operand_count != 1) return false;
            MirReg value = ctx->module->arena.operands[inst->operand_start];
            size_t fmt_offset = x86obj_rodata_string(ctx, "%ld\n");
            if (!x86obj_load(ctx, value, 6 /* rsi */)) return false;
            emit_lea_rodata(ctx, 7 /* rdi */, fmt_offset);
            emit_xor_rr2(ctx, X86OBJ_RAX, X86OBJ_RAX);
            return emit_call_extern(ctx, "printf");
        }
        case MIR_OP_PRINT_STRING: {
            /* Every scalar (including u8) occupies an 8-byte canonical slot
               in this memory model, so a string view's bytes are not packed
               and NUL-terminated the way printf("%s", ...) needs - see the
               matching comment in x86_alloc_emit_print_string. Print via a
               length-bounded putchar loop instead. */
            if (inst->operand_count != 1) return false;
            MirReg value = ctx->module->arena.operands[inst->operand_start];
            if (ctx->module->arena.regs[value].machine_type != MIR_TYPE_VIEW) return false;
            if (!x86obj_load_view_component(ctx, value, false, X86OBJ_R10) ||
                !x86obj_load_view_component(ctx, value, true, X86OBJ_R11)) return false;
            emit_mov_store(ctx, ctx->temp_offsets[0], X86OBJ_R10); /* ptr */
            emit_mov_store(ctx, ctx->temp_offsets[1], X86OBJ_R11); /* len */
            emit_movabs(ctx, X86OBJ_RAX, 0);
            emit_mov_store(ctx, ctx->temp_offsets[2], X86OBJ_RAX); /* index */
            size_t top = ctx->buf.len;
            emit_mov_load(ctx, X86OBJ_RAX, ctx->temp_offsets[2]);
            emit_mov_load(ctx, X86OBJ_R11, ctx->temp_offsets[1]);
            emit_cmp_rr(ctx, X86OBJ_RAX, X86OBJ_R11);
            size_t loop_done = emit_jcc_fwd(ctx, 0x3 /* jae, unsigned */);
            emit_mov_load(ctx, X86OBJ_R10, ctx->temp_offsets[0]);
            emit_mov_load(ctx, X86OBJ_R11, ctx->temp_offsets[2]);
            emit_imul_r_imm32(ctx, X86OBJ_R11, 8);
            emit_add_rr(ctx, X86OBJ_R10, X86OBJ_R11);
            emit_movzx64_8_indirect(ctx, X86OBJ_R11, X86OBJ_R10);
            emit_mov_rr(ctx, 7 /* rdi */, X86OBJ_R11);
            if (!emit_call_extern(ctx, "putchar")) return false;
            emit_mov_load(ctx, X86OBJ_RAX, ctx->temp_offsets[2]);
            emit_add_r_imm32(ctx, X86OBJ_RAX, 1);
            emit_mov_store(ctx, ctx->temp_offsets[2], X86OBJ_RAX);
            emit_jmp_to(ctx, top);
            patch_fwd(ctx, loop_done);
            emit_movabs(ctx, 7 /* rdi */, 10 /* '\n' */);
            return emit_call_extern(ctx, "putchar");
        }
        case MIR_OP_ASSERT: {
            if (inst->operand_count != 1) return false;
            MirReg cond = ctx->module->arena.operands[inst->operand_start];
            if (!x86obj_load(ctx, cond, X86OBJ_R10)) return false;
            emit_test_rr(ctx, X86OBJ_R10);
            size_t ok = emit_jcc_fwd(ctx, 0x5 /* jne */);
            size_t msg_offset = x86obj_rodata_string(ctx, "[cobra] assertion failed");
            emit_lea_rodata(ctx, 7 /* rdi */, msg_offset);
            if (!emit_call_extern(ctx, "puts")) return false;
            emit_movabs(ctx, 7 /* rdi */, 1);
            if (!emit_call_extern(ctx, "exit")) return false;
            patch_fwd(ctx, ok);
            return true;
        }
        case MIR_OP_ARRAY_INDEX_ADDR: {
            if (inst->result == MIR_REG_NONE || inst->operand_count != 2 ||
                !inst->memory_type || inst->memory_type->kind != COBRA_TYPE_ARRAY ||
                !inst->memory_type->generic_args[0] || inst->memory_type->array_length == 0 ||
                inst->memory_width != inst->memory_type->generic_args[0]->size) return false;
            MirReg base = ctx->module->arena.operands[inst->operand_start];
            MirReg index = ctx->module->arena.operands[inst->operand_start + 1];
            if (!x86obj_load(ctx, base, X86OBJ_R10) || !x86obj_load(ctx, index, X86OBJ_R11)) return false;
            emit_test_rr(ctx, X86OBJ_R11);
            size_t fail_neg = emit_jcc_fwd(ctx, 0x8 /* js */);
            emit_movabs(ctx, X86OBJ_RAX, inst->memory_type->array_length);
            emit_cmp_rr(ctx, X86OBJ_R11, X86OBJ_RAX);
            size_t fail_range = emit_jcc_fwd(ctx, 0x3 /* jae, unsigned >= */);
            emit_imul_r_imm32(ctx, X86OBJ_R11, (uint32_t)inst->memory_width);
            emit_add_rr(ctx, X86OBJ_R10, X86OBJ_R11);
            size_t to_done = emit_jmp_fwd(ctx);
            patch_fwd(ctx, fail_neg);
            patch_fwd(ctx, fail_range);
            emit_ud2(ctx);
            patch_fwd(ctx, to_done);
            return x86obj_store(ctx, inst->result, X86OBJ_R10);
        }
        case MIR_OP_FIELD_ADDR: {
            if (inst->result == MIR_REG_NONE || inst->operand_count != 1) return false;
            MirReg base = ctx->module->arena.operands[inst->operand_start];
            if (!x86obj_load(ctx, base, X86OBJ_R10)) return false;
            if (inst->memory_offset != 0) {
                if (inst->memory_offset > INT32_MAX || inst->memory_offset < INT32_MIN) return false;
                emit_add_r_imm32(ctx, X86OBJ_R10, (int32_t)inst->memory_offset);
            }
            return x86obj_store(ctx, inst->result, X86OBJ_R10);
        }
        case MIR_OP_AGG_COPY: {
            if (!inst->memory_type ||
                !(x86obj_supported_struct(inst->memory_type) || x86obj_supported_sum(inst->memory_type) ||
                  (inst->memory_type->kind == COBRA_TYPE_ARRAY && x86obj_supported_field_type(inst->memory_type))) ||
                inst->operand_count != 2 || inst->memory_width == 0 ||
                inst->memory_width != inst->memory_type->size) return false;
            MirReg destination = ctx->module->arena.operands[inst->operand_start];
            MirReg source = ctx->module->arena.operands[inst->operand_start + 1];
            if (!x86obj_load(ctx, destination, X86OBJ_R10) || !x86obj_load(ctx, source, X86OBJ_R11)) return false;
            uint32_t offset = 0;
            while (inst->memory_width - offset >= 8) {
                emit_mov_mem_disp(ctx, true, 8, X86OBJ_RAX, X86OBJ_R11, offset);
                emit_mov_mem_disp(ctx, false, 8, X86OBJ_RAX, X86OBJ_R10, offset);
                offset += 8;
            }
            if (inst->memory_width - offset >= 4) {
                emit_mov_mem_disp(ctx, true, 4, X86OBJ_RAX, X86OBJ_R11, offset);
                emit_mov_mem_disp(ctx, false, 4, X86OBJ_RAX, X86OBJ_R10, offset);
                offset += 4;
            }
            while (offset < inst->memory_width) {
                emit_mov_mem_disp(ctx, true, 1, X86OBJ_RAX, X86OBJ_R11, offset);
                emit_mov_mem_disp(ctx, false, 1, X86OBJ_RAX, X86OBJ_R10, offset);
                offset++;
            }
            return true;
        }
        case MIR_OP_SLICE_ALLOC: {
            if (inst->result == MIR_REG_NONE || inst->operand_count != 1 ||
                inst->machine_type != MIR_TYPE_VIEW || !inst->memory_type ||
                inst->memory_type->size == 0) return false;
            MirReg length = ctx->module->arena.operands[inst->operand_start];
            if (!x86obj_load(ctx, length, X86OBJ_R10)) return false;
            if (!x86obj_store_view_component(ctx, inst->result, true, X86OBJ_R10)) return false;
            emit_test_rr(ctx, X86OBJ_R10);
            size_t fail_neg = emit_jcc_fwd(ctx, 0x8 /* js */);
            emit_imul_r_imm32(ctx, X86OBJ_R10, (uint32_t)inst->memory_type->size);
            emit_mov_rr(ctx, 7 /* rdi */, X86OBJ_R10);
            if (!emit_call_extern(ctx, "malloc")) return false;
            emit_test_rr(ctx, X86OBJ_RAX);
            size_t fail_null = emit_jcc_fwd(ctx, 0x4 /* je */);
            if (!x86obj_store_view_component(ctx, inst->result, false, X86OBJ_RAX)) return false;
            size_t to_done = emit_jmp_fwd(ctx);
            patch_fwd(ctx, fail_neg);
            patch_fwd(ctx, fail_null);
            emit_ud2(ctx);
            patch_fwd(ctx, to_done);
            return true;
        }
        case MIR_OP_SLICE_FREE: {
            if (inst->operand_count != 1) return false;
            MirReg view = ctx->module->arena.operands[inst->operand_start];
            if (ctx->module->arena.regs[view].machine_type != MIR_TYPE_VIEW) return false;
            if (!x86obj_load_view_component(ctx, view, false, X86OBJ_R10)) return false;
            emit_test_rr(ctx, X86OBJ_R10);
            size_t skip_free = emit_jcc_fwd(ctx, 0x4 /* je */);
            emit_mov_rr(ctx, 7 /* rdi */, X86OBJ_R10);
            if (!emit_call_extern(ctx, "free")) return false;
            patch_fwd(ctx, skip_free);
            emit_movabs(ctx, X86OBJ_R10, 0);
            return x86obj_store_view_component(ctx, view, false, X86OBJ_R10) &&
                   x86obj_store_view_component(ctx, view, true, X86OBJ_R10);
        }
        case MIR_OP_DICT_ALLOC: {
            if (inst->result == MIR_REG_NONE || inst->machine_type != MIR_TYPE_VIEW ||
                !inst->type || !bir_is_owned_dict_type(inst->type)) return false;
            return x86obj_zero_view(ctx, inst->result);
        }
        case MIR_OP_DICT_SET: {
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE || inst->dict_key[0] == '\0') return false;
            MirReg source = ctx->module->arena.operands[inst->operand_start];
            MirReg value = ctx->module->arena.operands[inst->operand_start + 1];
            size_t key_offset = x86obj_rodata_string(ctx, inst->dict_key);
            /* Load the value first: the source-address lea below reuses
               scratch regs that the key/value setup must not have touched
               yet, matching the text emitter's clobber-avoidance comment. */
            if (!x86obj_load(ctx, value, X86OBJ_RDX)) return false;
            emit_lea_rbp(ctx, 7 /* rdi */, x86obj_spill(ctx, source));
            emit_lea_rodata(ctx, 6 /* rsi */, key_offset);
            if (!emit_call_extern(ctx, "cobra_dict_set_i64")) return false;
            if (!x86obj_load_view_component(ctx, source, false, X86OBJ_R10) ||
                !x86obj_store_view_component(ctx, inst->result, false, X86OBJ_R10)) return false;
            emit_mov_rr(ctx, 7 /* rdi */, X86OBJ_R10);
            if (!emit_call_extern(ctx, "cobra_dict_len")) return false;
            if (!x86obj_store_view_component(ctx, inst->result, true, X86OBJ_RAX)) return false;
            return x86obj_zero_view(ctx, source);
        }
        case MIR_OP_DICT_GET: {
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE || inst->dict_key[0] == '\0') return false;
            MirReg source = ctx->module->arena.operands[inst->operand_start];
            MirReg fallback = ctx->module->arena.operands[inst->operand_start + 1];
            size_t key_offset = x86obj_rodata_string(ctx, inst->dict_key);
            if (!x86obj_load(ctx, fallback, X86OBJ_RDX)) return false;
            if (!x86obj_load_view_component(ctx, source, false, 7 /* rdi */)) return false;
            emit_lea_rodata(ctx, 6 /* rsi */, key_offset);
            if (!emit_call_extern(ctx, "cobra_dict_get_i64")) return false;
            return x86obj_store(ctx, inst->result, X86OBJ_RAX);
        }
        case MIR_OP_DICT_HAS: {
            if (inst->operand_count != 1 || inst->result == MIR_REG_NONE || inst->dict_key[0] == '\0') return false;
            MirReg source = ctx->module->arena.operands[inst->operand_start];
            size_t key_offset = x86obj_rodata_string(ctx, inst->dict_key);
            if (!x86obj_load_view_component(ctx, source, false, 7 /* rdi */)) return false;
            emit_lea_rodata(ctx, 6 /* rsi */, key_offset);
            if (!emit_call_extern(ctx, "cobra_dict_has")) return false;
            return x86obj_store(ctx, inst->result, X86OBJ_RAX);
        }
        case MIR_OP_DICT_DELETE: {
            if (inst->operand_count != 1 || inst->result == MIR_REG_NONE || inst->dict_key[0] == '\0') return false;
            MirReg source = ctx->module->arena.operands[inst->operand_start];
            size_t key_offset = x86obj_rodata_string(ctx, inst->dict_key);
            emit_lea_rbp(ctx, 7 /* rdi */, x86obj_spill(ctx, source));
            emit_lea_rodata(ctx, 6 /* rsi */, key_offset);
            if (!emit_call_extern(ctx, "cobra_dict_delete")) return false;
            if (!x86obj_load_view_component(ctx, source, false, X86OBJ_R10) ||
                !x86obj_store_view_component(ctx, inst->result, false, X86OBJ_R10)) return false;
            emit_mov_rr(ctx, 7 /* rdi */, X86OBJ_R10);
            if (!emit_call_extern(ctx, "cobra_dict_len")) return false;
            if (!x86obj_store_view_component(ctx, inst->result, true, X86OBJ_RAX)) return false;
            return x86obj_zero_view(ctx, source);
        }
        case MIR_OP_DICT_POP: {
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE || inst->dict_key[0] == '\0') return false;
            MirReg source = ctx->module->arena.operands[inst->operand_start];
            MirReg fallback = ctx->module->arena.operands[inst->operand_start + 1];
            size_t key_offset = x86obj_rodata_string(ctx, inst->dict_key);
            if (!x86obj_load(ctx, fallback, X86OBJ_RDX)) return false;
            emit_lea_rbp(ctx, 7 /* rdi */, x86obj_spill(ctx, source));
            emit_lea_rodata(ctx, 6 /* rsi */, key_offset);
            if (!emit_call_extern(ctx, "cobra_dict_pop")) return false;
            if (!x86obj_store(ctx, inst->result, X86OBJ_RAX)) return false;
            if (!x86obj_load_view_component(ctx, source, false, 7 /* rdi */)) return false;
            if (!emit_call_extern(ctx, "cobra_dict_len")) return false;
            return x86obj_store_view_component(ctx, source, true, X86OBJ_RAX);
        }
        case MIR_OP_DICT_LEN: {
            if (inst->operand_count != 1 || inst->result == MIR_REG_NONE) return false;
            MirReg source = ctx->module->arena.operands[inst->operand_start];
            if (!x86obj_load_view_component(ctx, source, false, 7 /* rdi */)) return false;
            if (!emit_call_extern(ctx, "cobra_dict_len")) return false;
            return x86obj_store(ctx, inst->result, X86OBJ_RAX);
        }
        case MIR_OP_DICT_FREE: {
            if (inst->operand_count != 1 || inst->result != MIR_REG_NONE) return false;
            MirReg source = ctx->module->arena.operands[inst->operand_start];
            emit_lea_rbp(ctx, 7 /* rdi */, x86obj_spill(ctx, source));
            emit_lea_rbp(ctx, 6 /* rsi */, x86obj_view_length_spill(ctx, source));
            if (!emit_call_extern(ctx, "cobra_dict_free")) return false;
            return x86obj_zero_view(ctx, source);
        }
        case MIR_OP_STRING_CONCAT: {
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
                inst->machine_type != MIR_TYPE_VIEW || !inst->memory_type ||
                inst->memory_type->kind != COBRA_TYPE_U8 || inst->memory_type->size == 0) return false;
            MirReg left = ctx->module->arena.operands[inst->operand_start];
            MirReg right = ctx->module->arena.operands[inst->operand_start + 1];
            if (ctx->module->arena.regs[left].machine_type != MIR_TYPE_VIEW ||
                ctx->module->arena.regs[right].machine_type != MIR_TYPE_VIEW) return false;
            if (!x86obj_load_view_component(ctx, left, true, X86OBJ_R10) ||
                !x86obj_load_view_component(ctx, right, true, X86OBJ_R11)) return false;
            emit_add_rr(ctx, X86OBJ_R10, X86OBJ_R11);
            size_t fail_overflow = emit_jcc_fwd(ctx, 0x2 /* jc */);
            if (!x86obj_store_view_component(ctx, inst->result, true, X86OBJ_R10)) return false;
            emit_imul_r_imm32(ctx, X86OBJ_R10, (uint32_t)inst->memory_type->size);
            emit_mov_rr(ctx, 7 /* rdi */, X86OBJ_R10);
            if (!emit_call_extern(ctx, "malloc")) return false;
            emit_test_rr(ctx, X86OBJ_RAX);
            size_t fail_null = emit_jcc_fwd(ctx, 0x4 /* je */);
            if (!x86obj_store_view_component(ctx, inst->result, false, X86OBJ_RAX)) return false;
            if (!x86obj_load_view_component(ctx, inst->result, false, 7 /* rdi */) ||
                !x86obj_load_view_component(ctx, left, false, 6 /* rsi */) ||
                !x86obj_load_view_component(ctx, left, true, X86OBJ_RDX)) return false;
            emit_imul_r_imm32(ctx, X86OBJ_RDX, (uint32_t)inst->memory_type->size);
            if (!emit_call_extern(ctx, "memcpy")) return false;
            if (!x86obj_load_view_component(ctx, inst->result, false, 7 /* rdi */) ||
                !x86obj_load_view_component(ctx, left, true, X86OBJ_R10) ||
                !x86obj_load_view_component(ctx, right, false, 6 /* rsi */) ||
                !x86obj_load_view_component(ctx, right, true, X86OBJ_RDX)) return false;
            emit_imul_r_imm32(ctx, X86OBJ_R10, (uint32_t)inst->memory_type->size);
            emit_add_rr(ctx, 7 /* rdi */, X86OBJ_R10);
            emit_imul_r_imm32(ctx, X86OBJ_RDX, (uint32_t)inst->memory_type->size);
            if (!emit_call_extern(ctx, "memcpy")) return false;
            size_t to_done = emit_jmp_fwd(ctx);
            patch_fwd(ctx, fail_overflow);
            patch_fwd(ctx, fail_null);
            emit_ud2(ctx);
            patch_fwd(ctx, to_done);
            return true;
        }
        case MIR_OP_STRING_EQ: {
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
                inst->machine_type != MIR_TYPE_BOOL) return false;
            MirReg left = ctx->module->arena.operands[inst->operand_start];
            MirReg right = ctx->module->arena.operands[inst->operand_start + 1];
            if (ctx->module->arena.regs[left].machine_type != MIR_TYPE_VIEW ||
                ctx->module->arena.regs[right].machine_type != MIR_TYPE_VIEW) return false;
            if (!x86obj_load_view_component(ctx, left, true, X86OBJ_R10) ||
                !x86obj_load_view_component(ctx, right, true, X86OBJ_R11)) return false;
            emit_cmp_rr(ctx, X86OBJ_R10, X86OBJ_R11);
            size_t mismatch = emit_jcc_fwd(ctx, 0x5 /* jne */);
            if (!x86obj_load_view_component(ctx, left, false, 7 /* rdi */) ||
                !x86obj_load_view_component(ctx, right, false, 6 /* rsi */) ||
                !x86obj_load_view_component(ctx, left, true, X86OBJ_RDX)) return false;
            if (!emit_call_extern(ctx, "memcmp")) return false;
            /* memcmp returns a 32-bit int; the SysV ABI does not guarantee
               the upper 32 bits of rax are clean, so a plain movl zero-
               extends before the 64-bit test below. */
            emit_u8(ctx, 0x89);
            emit_u8(ctx, x86obj_modrm(3, X86OBJ_RAX, X86OBJ_RAX));
            emit_test_rr(ctx, X86OBJ_RAX);
            emit_setcc(ctx, 0x4 /* sete */, X86OBJ_R10);
            emit_movzx64_8(ctx, X86OBJ_R10, X86OBJ_R10);
            if (!x86obj_store(ctx, inst->result, X86OBJ_R10)) return false;
            size_t to_done = emit_jmp_fwd(ctx);
            patch_fwd(ctx, mismatch);
            emit_xor_rr(ctx, X86OBJ_R10);
            if (!x86obj_store(ctx, inst->result, X86OBJ_R10)) return false;
            patch_fwd(ctx, to_done);
            return true;
        }
        case MIR_OP_BUFFER_ALLOC: {
            if (inst->result == MIR_REG_NONE || inst->operand_count != 1 ||
                inst->machine_type != MIR_TYPE_VIEW || !inst->memory_type ||
                inst->memory_type->size == 0) return false;
            MirReg length = ctx->module->arena.operands[inst->operand_start];
            if (!x86obj_load(ctx, length, X86OBJ_R10)) return false;
            if (!x86obj_store_view_component(ctx, inst->result, true, X86OBJ_R10)) return false;
            emit_test_rr(ctx, X86OBJ_R10);
            size_t fail_neg = emit_jcc_fwd(ctx, 0x8 /* js */);
            /* A zero-length buffer still allocates one element's worth so a
               later append never has to distinguish "empty" from "unbacked". */
            size_t skip_bump = emit_jcc_fwd(ctx, 0x5 /* jne, i.e. r10 != 0 */);
            emit_movabs(ctx, X86OBJ_R10, 1);
            patch_fwd(ctx, skip_bump);
            emit_imul_r_imm32(ctx, X86OBJ_R10, (uint32_t)inst->memory_type->size);
            emit_mov_rr(ctx, 7 /* rdi */, X86OBJ_R10);
            if (!emit_call_extern(ctx, "malloc")) return false;
            emit_test_rr(ctx, X86OBJ_RAX);
            size_t fail_null = emit_jcc_fwd(ctx, 0x4 /* je */);
            if (!x86obj_store_view_component(ctx, inst->result, false, X86OBJ_RAX)) return false;
            size_t to_done = emit_jmp_fwd(ctx);
            patch_fwd(ctx, fail_neg);
            patch_fwd(ctx, fail_null);
            emit_ud2(ctx);
            patch_fwd(ctx, to_done);
            return true;
        }
        case MIR_OP_BUFFER_APPEND: {
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
                !inst->memory_type || inst->memory_type->size == 0) return false;
            bool is_float_elem = x86obj_is_float(x86obj_machine_type_for_cobra(inst->memory_type));
            bool is_struct_elem = !is_float_elem && inst->memory_type->kind == COBRA_TYPE_STRUCT &&
                                  x86obj_supported_struct(inst->memory_type) &&
                                  !bir_type_has_owned_payload(inst->memory_type);
            if (!is_float_elem && !is_struct_elem &&
                !x86obj_supported_scalar(x86obj_machine_type_for_cobra(inst->memory_type)))
                return false; /* owning-struct-element buffers are outside the v1 lane */
            MirReg source = ctx->module->arena.operands[inst->operand_start];
            MirReg value = ctx->module->arena.operands[inst->operand_start + 1];
            if (!x86obj_load_view_component(ctx, source, false, X86OBJ_R10) ||
                !x86obj_load_view_component(ctx, source, true, X86OBJ_R11)) return false;
            emit_test_rr(ctx, X86OBJ_R11);
            size_t fail_neg = emit_jcc_fwd(ctx, 0x8 /* js */);
            emit_mov_rr(ctx, X86OBJ_RAX, X86OBJ_R11);
            emit_add_r_imm32(ctx, X86OBJ_RAX, 1);
            size_t fail_overflow = emit_jcc_fwd(ctx, 0x0 /* jo */);
            emit_mov_rr(ctx, X86OBJ_RCX, X86OBJ_RAX);
            if (!x86obj_store_view_component(ctx, inst->result, true, X86OBJ_RCX)) return false;
            emit_imul_r_imm32(ctx, X86OBJ_RCX, (uint32_t)inst->memory_type->size);
            emit_mov_rr(ctx, 7 /* rdi */, X86OBJ_RCX);
            if (!emit_call_extern(ctx, "malloc")) return false;
            emit_test_rr(ctx, X86OBJ_RAX);
            size_t fail_null = emit_jcc_fwd(ctx, 0x4 /* je */);
            if (!x86obj_store_view_component(ctx, inst->result, false, X86OBJ_RAX)) return false;
            if (!x86obj_load_view_component(ctx, inst->result, false, 7 /* rdi */) ||
                !x86obj_load_view_component(ctx, source, false, 6 /* rsi */) ||
                !x86obj_load_view_component(ctx, source, true, X86OBJ_RDX)) return false;
            emit_imul_r_imm32(ctx, X86OBJ_RDX, (uint32_t)inst->memory_type->size);
            if (!emit_call_extern(ctx, "memcpy")) return false;
            if (!x86obj_load_view_component(ctx, inst->result, false, X86OBJ_R10) ||
                !x86obj_load_view_component(ctx, source, true, X86OBJ_R11)) return false;
            emit_imul_r_imm32(ctx, X86OBJ_R11, (uint32_t)inst->memory_type->size);
            emit_add_rr(ctx, X86OBJ_R10, X86OBJ_R11);
            if (is_float_elem) {
                if (!x86obj_load_float(ctx, value, X86OBJ_XMM_SCRATCH0)) return false;
                emit_movx_store_indirect(ctx, inst->memory_type->kind == COBRA_TYPE_F64,
                                         X86OBJ_R10, X86OBJ_XMM_SCRATCH0);
            } else if (is_struct_elem) {
                /* r10 already holds the append destination; value is a
                   pointer to the caller's struct instance. */
                if (!x86obj_load(ctx, value, X86OBJ_R11)) return false;
                uint32_t width = (uint32_t)inst->memory_type->size, off = 0;
                while (width - off >= 8) { emit_mov_mem_disp(ctx, true, 8, X86OBJ_RAX, X86OBJ_R11, off);
                                            emit_mov_mem_disp(ctx, false, 8, X86OBJ_RAX, X86OBJ_R10, off); off += 8; }
                if (width - off >= 4) { emit_mov_mem_disp(ctx, true, 4, X86OBJ_RAX, X86OBJ_R11, off);
                                         emit_mov_mem_disp(ctx, false, 4, X86OBJ_RAX, X86OBJ_R10, off); off += 4; }
                while (off < width) { emit_mov_mem_disp(ctx, true, 1, X86OBJ_RAX, X86OBJ_R11, off);
                                       emit_mov_mem_disp(ctx, false, 1, X86OBJ_RAX, X86OBJ_R10, off); off++; }
            } else {
                if (!x86obj_load(ctx, value, X86OBJ_R11)) return false;
                if (inst->memory_type->size == 1) emit_movb_store_indirect(ctx, X86OBJ_R10, X86OBJ_R11);
                else emit_mov_store_indirect(ctx, X86OBJ_R10, X86OBJ_R11);
            }
            if (!x86obj_load_view_component(ctx, source, false, 7 /* rdi */)) return false;
            if (!emit_call_extern(ctx, "free")) return false;
            if (!x86obj_zero_view(ctx, source)) return false;
            size_t to_done = emit_jmp_fwd(ctx);
            patch_fwd(ctx, fail_neg);
            patch_fwd(ctx, fail_overflow);
            patch_fwd(ctx, fail_null);
            emit_ud2(ctx);
            patch_fwd(ctx, to_done);
            return true;
        }
        case MIR_OP_BUFFER_POP: {
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
                !inst->memory_type || inst->memory_type->size == 0) return false;
            bool is_float_elem = x86obj_is_float(x86obj_machine_type_for_cobra(inst->memory_type));
            if (!is_float_elem && !x86obj_supported_scalar(x86obj_machine_type_for_cobra(inst->memory_type)))
                return false;
            MirReg source = ctx->module->arena.operands[inst->operand_start];
            MirReg fallback = ctx->module->arena.operands[inst->operand_start + 1];
            if (!x86obj_load_view_component(ctx, source, false, X86OBJ_R10) ||
                !x86obj_load_view_component(ctx, source, true, X86OBJ_R11)) return false;
            emit_movabs(ctx, X86OBJ_RAX, 1);
            emit_cmp_rr(ctx, X86OBJ_R11, X86OBJ_RAX);
            size_t fail = emit_jcc_fwd(ctx, 0xC /* jl, signed */);
            emit_add_r_imm32(ctx, X86OBJ_R11, -1);
            if (!x86obj_store_view_component(ctx, source, true, X86OBJ_R11)) return false;
            emit_imul_r_imm32(ctx, X86OBJ_R11, (uint32_t)inst->memory_type->size);
            emit_add_rr(ctx, X86OBJ_R10, X86OBJ_R11);
            if (is_float_elem) {
                emit_movx_load_indirect(ctx, inst->memory_type->kind == COBRA_TYPE_F64,
                                        X86OBJ_XMM_SCRATCH0, X86OBJ_R10);
                if (!x86obj_store_float(ctx, inst->result, X86OBJ_XMM_SCRATCH0)) return false;
            } else {
                if (inst->memory_type->kind == COBRA_TYPE_BOOL || inst->memory_type->size == 1)
                    emit_movzx64_8_indirect(ctx, X86OBJ_R9, X86OBJ_R10);
                else emit_mov_load_indirect(ctx, X86OBJ_R9, X86OBJ_R10);
                if (!x86obj_store(ctx, inst->result, X86OBJ_R9)) return false;
            }
            size_t to_done = emit_jmp_fwd(ctx);
            patch_fwd(ctx, fail);
            if (is_float_elem) {
                if (!x86obj_load_float(ctx, fallback, X86OBJ_XMM_SCRATCH0)) return false;
                if (!x86obj_store_float(ctx, inst->result, X86OBJ_XMM_SCRATCH0)) return false;
            } else {
                if (!x86obj_load(ctx, fallback, X86OBJ_R9)) return false;
                if (!x86obj_store(ctx, inst->result, X86OBJ_R9)) return false;
            }
            patch_fwd(ctx, to_done);
            return true;
        }
        case MIR_OP_BUFFER_FREE:
        case MIR_OP_TRANSFER: {
            if (inst->op == MIR_OP_TRANSFER) {
                if (inst->operand_count != 1 || inst->result == MIR_REG_NONE) return false;
                MirReg source = ctx->module->arena.operands[inst->operand_start];
                if (ctx->module->arena.regs[source].machine_type == MIR_TYPE_VIEW) {
                    return x86obj_load_view_component(ctx, source, false, X86OBJ_R10) &&
                           x86obj_load_view_component(ctx, source, true, X86OBJ_R11) &&
                           x86obj_store_view_component(ctx, inst->result, false, X86OBJ_R10) &&
                           x86obj_store_view_component(ctx, inst->result, true, X86OBJ_R11);
                }
                if (!x86obj_supported_scalar(ctx->module->arena.regs[source].machine_type)) return false;
                return x86obj_load(ctx, source, X86OBJ_R10) && x86obj_store(ctx, inst->result, X86OBJ_R10);
            }
            /* MIR_OP_BUFFER_FREE */
            if (inst->operand_count != 1) return false;
            MirReg view = ctx->module->arena.operands[inst->operand_start];
            if (ctx->module->arena.regs[view].machine_type != MIR_TYPE_VIEW) return false;
            if (!x86obj_load_view_component(ctx, view, false, X86OBJ_R10)) return false;
            emit_test_rr(ctx, X86OBJ_R10);
            size_t skip_free = emit_jcc_fwd(ctx, 0x4 /* je */);
            emit_mov_rr(ctx, 7 /* rdi */, X86OBJ_R10);
            if (!emit_call_extern(ctx, "free")) return false;
            patch_fwd(ctx, skip_free);
            return x86obj_zero_view(ctx, view);
        }
        case MIR_OP_DESTROY: {
            if (inst->operand_count != 1) return false;
            MirReg source = ctx->module->arena.operands[inst->operand_start];
            MirMachineType type = ctx->module->arena.regs[source].machine_type;
            if (type == MIR_TYPE_ADDRESS) return true;
            if (type != MIR_TYPE_VIEW) return false;
            if (!x86obj_load_view_component(ctx, source, false, X86OBJ_R10)) return false;
            emit_test_rr(ctx, X86OBJ_R10);
            size_t skip_free = emit_jcc_fwd(ctx, 0x4 /* je */);
            emit_mov_rr(ctx, 7 /* rdi */, X86OBJ_R10);
            if (!emit_call_extern(ctx, "free")) return false;
            patch_fwd(ctx, skip_free);
            return x86obj_zero_view(ctx, source);
        }
        case MIR_OP_SUM_PAYLOAD_STORE:
        case MIR_OP_FIELD_PAYLOAD_STORE:
            return x86obj_emit_owned_payload_store(ctx, inst);
        case MIR_OP_SUM_PAYLOAD_LOAD:
        case MIR_OP_FIELD_PAYLOAD_LOAD:
            return x86obj_emit_owned_payload_load(ctx, inst);
        case MIR_OP_SUM_MOVE:
        case MIR_OP_AGG_MOVE: {
            if (inst->operand_count != 2 || !inst->memory_type || inst->memory_width == 0) return false;
            MirReg destination = ctx->module->arena.operands[inst->operand_start];
            MirReg source = ctx->module->arena.operands[inst->operand_start + 1];
            return x86obj_emit_copy_bytes(ctx, destination, source, inst->memory_width) &&
                   x86obj_emit_zero_bytes(ctx, source, inst->memory_width);
        }
        case MIR_OP_SUM_DROP:
        case MIR_OP_AGG_DROP: {
            if (inst->operand_count != 1 || !inst->memory_type) return false;
            MirReg source = ctx->module->arena.operands[inst->operand_start];
            return x86obj_emit_drop_owned_value(ctx, source, inst->memory_type, 0) &&
                   x86obj_emit_zero_bytes(ctx, source, (uint32_t)inst->memory_type->size);
        }
        case MIR_OP_CALL:
            return x86obj_emit_call(ctx, inst);
        case MIR_OP_RETURN: {
            MirReg value = inst->operand_count == 1
                ? ctx->module->arena.operands[inst->operand_start] : MIR_REG_NONE;
            if (value != MIR_REG_NONE) {
                MirMachineType type = ctx->module->arena.regs[value].machine_type;
                if (!x86obj_supported_scalar(type)) return false;
                if (x86obj_is_float(type)) {
                    if (!x86obj_load_float(ctx, value, 0 /* xmm0 */)) return false;
                } else if (!x86obj_load(ctx, value, X86OBJ_RAX)) return false;
            }
            x86obj_restore_callee_saved(ctx, function_index);
            emit_leave(ctx);
            emit_ret(ctx);
            return true;
        }
        default:
            return false;
    }
}

static bool x86obj_emit_function(X86ObjContext *ctx, size_t function_index) {
    const MirFunction *function = &ctx->module->functions[function_index];
    /* Struct-by-value returns use hidden caller-provided storage: the ABI
       gives the hidden pointer a single indirect GPR location exactly like
       an ordinary ADDRESS-typed parameter (see bir_abi_fill_return in
       ssa.c), so it flows through the same ABI_MOVE/FIELD_ADDR/STORE path
       as any other pointer parameter, and MIR_OP_RETURN for such a function
       always has zero operands (the callee writes through the hidden
       pointer directly, then returns void) - already handled below. No
       special casing is needed here beyond not rejecting it. */
    if (!x86obj_prepare_frame(ctx, function_index)) return false;

    ctx->func_offset[function_index] = ctx->buf.len;
    emit_push_rbp(ctx);
    emit_mov_rr(ctx, X86OBJ_RBP, X86OBJ_RSP);
    if (ctx->frame_size) emit_sub_rsp_imm(ctx, ctx->frame_size);
    x86obj_save_callee_saved(ctx, function_index);

    for (size_t offset = 0; offset < function->block_count; offset++) {
        MirBlockRef block_ref = function->first_block + (MirBlockRef)offset;
        const MirBlock *block = &ctx->module->arena.blocks[block_ref];
        ctx->block_offset[block_ref] = ctx->buf.len;
        for (size_t i = 0; i < block->inst_count; i++) {
            const MirInst *inst = &ctx->module->arena.insts[block->insts[i]];
            if (!x86obj_emit_inst(ctx, function_index, inst)) {
                x86obj_error(ctx, "object emitter does not support MIR opcode '%s' in function '%s'",
                            mir_opcode_name(inst->op), function->name);
                return false;
            }
        }
    }
    free(ctx->spill_offsets);
    ctx->spill_offsets = NULL;
    free(ctx->view_length_offsets);
    ctx->view_length_offsets = NULL;
    ctx->func_end[function_index] = ctx->buf.len;
    return true;
}

bool bir_x86_64_emit_object(const MirModule *module, const MirAllocation *allocation,
                            const char *output_object_path,
                            char *errbuf, size_t errbuf_size) {
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    if (!module || !allocation || allocation->source != module || !output_object_path ||
        !mir_allocation_verify(allocation, errbuf, errbuf_size)) return false;

    X86ObjContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.module = module;
    ctx.allocation = allocation;
    ctx.errbuf = errbuf;
    ctx.errbuf_size = errbuf_size;
    ctx.block_offset = calloc(module->arena.block_count ? module->arena.block_count : 1, sizeof(*ctx.block_offset));
    ctx.func_offset = calloc(module->function_count ? module->function_count : 1, sizeof(*ctx.func_offset));
    ctx.func_end = calloc(module->function_count ? module->function_count : 1, sizeof(*ctx.func_end));
    if (!ctx.block_offset || !ctx.func_offset || !ctx.func_end) {
        x86obj_error(&ctx, "out of memory preparing object emission");
        free(ctx.block_offset); free(ctx.func_offset); free(ctx.func_end);
        return false;
    }

    bool ok = true;
    for (size_t f = 0; ok && f < module->function_count; f++)
        ok = x86obj_emit_function(&ctx, f);

    if (ok) {
        for (size_t i = 0; i < ctx.fixup_count; i++) {
            const X86ObjFixup *fixup = &ctx.fixups[i];
            uint64_t target = fixup->kind == FIXUP_BLOCK
                ? ctx.block_offset[fixup->target] : ctx.func_offset[fixup->target];
            int32_t rel = (int32_t)((int64_t)target - (int64_t)(fixup->patch_offset + 4));
            uint32_t bits = (uint32_t)rel;
            for (int b = 0; b < 4; b++) ctx.buf.data[fixup->patch_offset + b] = (uint8_t)(bits >> (8 * b));
        }

        Elf64ObjectSymbol *symbols = calloc(module->function_count ? module->function_count : 1, sizeof(*symbols));
        const char *extern_names[X86OBJ_MAX_EXTERNS];
        Elf64ObjectReloc *relocs = calloc(ctx.extern_call_count ? ctx.extern_call_count : 1, sizeof(*relocs));
        Elf64ObjectRodataReloc *rodata_relocs =
            calloc(ctx.rodata_ref_count ? ctx.rodata_ref_count : 1, sizeof(*rodata_relocs));
        if (!symbols || !relocs || !rodata_relocs) {
            x86obj_error(&ctx, "out of memory building object symbol/relocation tables");
            ok = false;
        } else {
            for (size_t f = 0; f < module->function_count; f++) {
                symbols[f].name = module->functions[f].name;
                symbols[f].offset = ctx.func_offset[f];
                symbols[f].size = ctx.func_end[f] - ctx.func_offset[f];
            }
            for (size_t i = 0; i < ctx.extern_count; i++) extern_names[i] = ctx.extern_names[i];
            for (size_t i = 0; i < ctx.extern_call_count; i++) {
                relocs[i].text_offset = ctx.extern_calls[i].text_offset;
                relocs[i].extern_index = ctx.extern_calls[i].extern_index;
                relocs[i].addend = -4; /* R_X86_64_PLT32: S + A - P with A = -4 for a call rel32 */
            }
            for (size_t i = 0; i < ctx.rodata_ref_count; i++) {
                rodata_relocs[i].text_offset = ctx.rodata_refs[i].text_offset;
                /* R_X86_64_PC32: S + A - P with A = target_offset - 4 for a
                   rip-relative disp32 field (see emit_lea_rodata). */
                rodata_relocs[i].addend = (int64_t)ctx.rodata_refs[i].rodata_offset - 4;
            }
            ok = elf64_write_object(output_object_path, ctx.buf.data, ctx.buf.len,
                                    ctx.rodata.data, ctx.rodata.len,
                                    symbols, module->function_count,
                                    extern_names, ctx.extern_count,
                                    relocs, ctx.extern_call_count,
                                    rodata_relocs, ctx.rodata_ref_count, errbuf, errbuf_size);
        }
        free(symbols);
        free(relocs);
        free(rodata_relocs);
    }

    free(ctx.buf.data);
    free(ctx.rodata.data);
    free(ctx.rodata_refs);
    free(ctx.block_offset);
    free(ctx.func_offset);
    free(ctx.func_end);
    free(ctx.fixups);
    free(ctx.extern_calls);
    return ok && !ctx.failed;
}
