/*
 * @gpu kernel lowering: translates a restricted subset of a Cobra function
 * body into GLSL compute-shader source, which `cobra build` then hands to
 * glslangValidator (a build-time dependency, same as the tensor kernels in
 * runtime/cobra_gpu.c) to produce SPIR-V.
 *
 * Subset, deliberately narrow: any number of f32[] buffer parameters (the
 * first one's length is the kernel's implicit thread domain - one invocation
 * per element) in any position, any number of scalar i64/f32/bool
 * parameters (become push constants), and a body built from assignment,
 * if/while, arithmetic/comparison, array indexing on a buffer parameter, and
 * the `gpu_index()`/`len()` builtins. A kernel may also call ordinary Cobra
 * functions - same file or pulled in through an import, exactly like the
 * CPU path - as long as those functions are themselves scalar-only (no
 * buffer access): they get carried into the same SPIR-V module as ordinary
 * GLSL helper functions, transitively, with cycles rejected as a compile
 * error. Anything outside this subset is a compile error naming the
 * offending construct and line, not a silent CPU fallback - a kernel that
 * can't be expressed on the GPU must fail loudly at compile time.
 */
#include "cobra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GPU_MAX_BUFFERS 8
#define GPU_MAX_CARRIED 32

typedef struct {
    const ASTNode *program;     /* root AST, used to resolve carried calls */
    FILE *out;                  /* current emission target (main or a helper body) */
    FILE *f_proto;              /* forward prototypes for carried helpers */
    FILE *f_helpers;            /* carried helper bodies, in discovery order */
    const char *fn_name;        /* name of whichever function is being lowered right now */
    const char *source_file;
    bool in_kernel_main;        /* true while lowering the kernel entry point itself */
    bool backward_mode;         /* true when lowering a compiler-generated backward kernel */

    char buffer_names[GPU_MAX_BUFFERS][COBRA_MAX_IDENT_LEN];
    int buffer_count;

    char scalar_names[16][COBRA_MAX_IDENT_LEN];
    CobraTypeKind scalar_types[16];
    int scalar_count;

    char local_names[64][COBRA_MAX_IDENT_LEN];
    CobraTypeKind local_types[64];
    int local_count;

    /* Carried (non-kernel) helper functions, keyed by Cobra function name. */
    const ASTNode *carried_nodes[GPU_MAX_CARRIED];
    int carried_count;
    const ASTNode *in_progress[GPU_MAX_CARRIED];
    int in_progress_count;

    bool failed;
} GpuLowerCtx;

static void gpu_fail(GpuLowerCtx *ctx, const ASTNode *n, const char *what) {
    if (ctx->failed) return;
    ctx->failed = true;
    fprintf(stderr, "%s:%d:%d: error: @gpu kernel cannot use %s on the GPU (in '%s')\n",
            ctx->source_file, n ? n->source_line : 0, n ? n->source_col : 0, what, ctx->fn_name);
}

static int gpu_buffer_index(GpuLowerCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->buffer_count; i++) if (!strcmp(ctx->buffer_names[i], name)) return i;
    return -1;
}

static bool gpu_local_known(GpuLowerCtx *ctx, const char *name) {
    if (gpu_buffer_index(ctx, name) >= 0) return true;
    for (int i = 0; i < ctx->scalar_count; i++) if (!strcmp(ctx->scalar_names[i], name)) return true;
    for (int i = 0; i < ctx->local_count; i++) if (!strcmp(ctx->local_names[i], name)) return true;
    return false;
}

/* GLSL reserves a long list of identifiers (control keywords, type names,
   storage qualifiers) that Cobra doesn't - a param/local named e.g. `out`
   or `in` compiles fine as ordinary Cobra and then fails deep inside
   glslangValidator with a confusing syntax error pointing at generated
   code the user never wrote. Catch the common ones here instead, with a
   diagnostic that names the actual identifier and points at the user's own
   source line. Not exhaustive (GLSL reserves ~100+ words); covers the ones
   someone writing ordinary variable/parameter names is actually likely to
   pick. */
static bool gpu_is_glsl_reserved(const char *name) {
    static const char *reserved[] = {
        "in", "out", "inout", "uniform", "buffer", "shared", "layout",
        "const", "void", "main", "discard", "struct", "return",
        "if", "else", "for", "while", "do", "break", "continue",
        "true", "false", "int", "uint", "float", "double", "bool",
        "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4",
        "uvec2", "uvec3", "uvec4", "mat2", "mat3", "mat4",
        "sampler2D", "image2D", "precision", "highp", "mediump", "lowp",
        NULL
    };
    for (int i = 0; reserved[i]; i++) if (!strcmp(name, reserved[i])) return true;
    return false;
}

static const char *gpu_glsl_scalar_type(CobraTypeKind k) {
    if (k == COBRA_TYPE_F32 || k == COBRA_TYPE_F64) return "float";
    if (k == COBRA_TYPE_BOOL) return "bool";
    return "int"; /* i32/i64/u32/u64: GLSL has no 64-bit int in the core subset we target */
}

static const ASTNode *gpu_find_toplevel_function(const ASTNode *program, const char *name) {
    if (!program) return NULL;
    for (size_t i = 0; i < program->child_count; i++) {
        ASTNode *fn = program->children[i];
        if (fn->type == AST_FUNCTION && !strcmp(fn->name, name)) return fn;
    }
    return NULL;
}

static void gpu_emit_expr(GpuLowerCtx *ctx, const ASTNode *e);
static void gpu_emit_stmt(GpuLowerCtx *ctx, const ASTNode *s);
static bool gpu_carry_function(GpuLowerCtx *ctx, const ASTNode *fn);

/* Infers a carried helper's scalar return type: an explicit declared_type
   wins, otherwise the type of its first `return expr` (the v1 subset has no
   control-flow-merged return type inference beyond that). */
static CobraTypeKind gpu_infer_return_type(const ASTNode *fn) {
    if (fn->declared_type != COBRA_TYPE_UNTYPED && fn->declared_type != COBRA_TYPE_VOID) return fn->declared_type;
    for (size_t i = 0; i < fn->child_count; i++) {
        const ASTNode *s = fn->children[i];
        if (s->type == AST_RETURN && s->child_count > 0) return s->children[0]->value_type;
    }
    return COBRA_TYPE_I64;
}

static void gpu_emit_expr(GpuLowerCtx *ctx, const ASTNode *e) {
    if (ctx->failed || !e) return;
    switch (e->type) {
        case AST_INT_LITERAL:
            fprintf(ctx->out, "%lld", (long long)e->literal_i64);
            return;
        case AST_FLOAT_LITERAL:
            fprintf(ctx->out, "%g", e->float_val);
            return;
        case AST_BOOL_LITERAL:
            fprintf(ctx->out, e->int_val ? "true" : "false");
            return;
        case AST_VAR_REF:
            if (!gpu_local_known(ctx, e->name)) { gpu_fail(ctx, e, "an undeclared or non-local name"); return; }
            /* Kernel scalar params live in the push_constant block and are
               referenced as kpc.<name> directly (not via a #define): GLSL
               has no macro scoping, so a bare `#define <name> kpc.<name>`
               would silently clobber any identically-named identifier
               anywhere else in the file, including an unrelated carried
               helper function's own parameter of the same name. Helper
               params (in_kernel_main == false) are ordinary GLSL function
               parameters and stay bare. */
            if (ctx->in_kernel_main) {
                for (int i = 0; i < ctx->scalar_count; i++) {
                    if (!strcmp(ctx->scalar_names[i], e->name)) { fprintf(ctx->out, "kpc.%s", e->name); return; }
                }
            }
            fprintf(ctx->out, "%s", e->name);
            return;
        case AST_ARRAY_INDEX: {
            int idx = gpu_buffer_index(ctx, e->name);
            if (idx < 0) { gpu_fail(ctx, e, "indexing into anything but a kernel buffer parameter"); return; }
            if (e->child_count != 1) { gpu_fail(ctx, e, "multi-dimensional indexing"); return; }
            fprintf(ctx->out, "%s[", e->name);
            gpu_emit_expr(ctx, e->children[0]);
            fprintf(ctx->out, "]");
            return;
        }
        case AST_BINARY_OP: {
            if (e->child_count != 2) { gpu_fail(ctx, e, "a malformed binary expression"); return; }
            fprintf(ctx->out, "(");
            gpu_emit_expr(ctx, e->children[0]);
            fprintf(ctx->out, " %s ", e->name);
            gpu_emit_expr(ctx, e->children[1]);
            fprintf(ctx->out, ")");
            return;
        }
        case AST_LEN_EXPR: {
            if (e->child_count != 1 || e->children[0]->type != AST_VAR_REF) { gpu_fail(ctx, e, "len() on anything but a kernel buffer parameter"); return; }
            int idx = gpu_buffer_index(ctx, e->children[0]->name);
            if (idx < 0) { gpu_fail(ctx, e, "len() on anything but a kernel buffer parameter"); return; }
            if (ctx->backward_mode) fprintf(ctx->out, "int(kpc.klen)");
            else fprintf(ctx->out, "int(kpc.klen_%s)", e->children[0]->name);
            return;
        }
        case AST_FUNC_CALL: {
            if (!strcmp(e->name, "gpu_index") && e->child_count == 0) { fprintf(ctx->out, "int(kgid)"); return; }
            if ((!strcmp(e->name, "max") || !strcmp(e->name, "min") || !strcmp(e->name, "sqrt") ||
                 !strcmp(e->name, "abs") || !strcmp(e->name, "floor") || !strcmp(e->name, "ceil") ||
                 !strcmp(e->name, "exp") || !strcmp(e->name, "log") || !strcmp(e->name, "tanh") ||
                 !strcmp(e->name, "pow")) && e->child_count >= 1) {
                fprintf(ctx->out, "%s(", e->name);
                for (size_t i = 0; i < e->child_count; i++) {
                    if (i) fprintf(ctx->out, ", ");
                    gpu_emit_expr(ctx, e->children[i]);
                }
                fprintf(ctx->out, ")");
                return;
            }
            /* Anything else is a carry candidate: another Cobra function,
               same file or imported, resolved from the merged program AST
               exactly like the CPU path resolves it. Carrying writes into
               ctx->f_proto/f_helpers, which only exist while lowering a
               forward kernel (cobra_gpu_lower_function) - backward's own
               context (cobra_gpu_lower_backward) never sets them up, since
               differentiating through an arbitrary carried call isn't
               supported (see gpu_emit_grad's own FUNC_CALL case). Reaching
               here in backward mode means this kernel isn't autodiff-
               eligible; fail the way gpu_emit_grad already does elsewhere,
               not by writing into a null FILE*. */
            if (ctx->backward_mode) { gpu_fail(ctx, e, "a call to a non-differentiable function inside an autodiff-eligible kernel"); return; }
            const ASTNode *callee = gpu_find_toplevel_function(ctx->program, e->name);
            if (!callee) { gpu_fail(ctx, e, "a call to an unknown function (not a GLSL builtin and not found in the program)"); return; }
            if (!gpu_carry_function(ctx, callee)) return;
            fprintf(ctx->out, "%s(", e->name);
            for (size_t i = 0; i < e->child_count; i++) {
                if (i) fprintf(ctx->out, ", ");
                gpu_emit_expr(ctx, e->children[i]);
            }
            fprintf(ctx->out, ")");
            return;
        }
        default:
            gpu_fail(ctx, e, "this expression form");
            return;
    }
}

static void gpu_emit_stmt(GpuLowerCtx *ctx, const ASTNode *s) {
    if (ctx->failed || !s) return;
    switch (s->type) {
        case AST_VAR_DECL: {
            if (gpu_is_glsl_reserved(s->name)) { gpu_fail(ctx, s, "a reserved GLSL keyword as a local variable name"); return; }
            if (ctx->local_count >= 64) { gpu_fail(ctx, s, "this many local variables"); return; }
            snprintf(ctx->local_names[ctx->local_count], COBRA_MAX_IDENT_LEN, "%s", s->name);
            ctx->local_types[ctx->local_count++] = s->declared_type;
            fprintf(ctx->out, "    %s %s = ", gpu_glsl_scalar_type(s->declared_type), s->name);
            if (s->child_count > 0) gpu_emit_expr(ctx, s->children[0]); else fprintf(ctx->out, "0");
            fprintf(ctx->out, ";\n");
            return;
        }
        case AST_ASSIGN: {
            /* Cobra has no `let`/`var` keyword: `name = expr` is a fresh
               local on first use and a plain reassignment afterward. IR has
               already run by the time gpu_lower sees this, so value_type is
               known even without an explicit declared_type. */
            bool first_use = !gpu_local_known(ctx, s->name);
            if (first_use) {
                if (gpu_is_glsl_reserved(s->name)) { gpu_fail(ctx, s, "a reserved GLSL keyword as a local variable name"); return; }
                if (ctx->local_count >= 64) { gpu_fail(ctx, s, "this many local variables"); return; }
                snprintf(ctx->local_names[ctx->local_count], COBRA_MAX_IDENT_LEN, "%s", s->name);
                CobraTypeKind t = s->declared_type != COBRA_TYPE_UNTYPED ? s->declared_type
                                 : (s->child_count > 0 ? s->children[0]->value_type : COBRA_TYPE_I64);
                ctx->local_types[ctx->local_count++] = t;
                fprintf(ctx->out, "    %s %s = ", gpu_glsl_scalar_type(t), s->name);
            } else {
                fprintf(ctx->out, "    %s = ", s->name);
            }
            if (s->child_count > 0) gpu_emit_expr(ctx, s->children[0]); else fprintf(ctx->out, "0");
            fprintf(ctx->out, ";\n");
            return;
        }
        case AST_INDEX_ASSIGN: {
            if (gpu_buffer_index(ctx, s->name) < 0) { gpu_fail(ctx, s, "writing to anything but a kernel buffer parameter"); return; }
            if (!ctx->in_kernel_main) { gpu_fail(ctx, s, "buffer writes in a carried (non-kernel) helper function"); return; }
            if (s->child_count != 2) { gpu_fail(ctx, s, "multi-dimensional index assignment"); return; }
            fprintf(ctx->out, "    %s[", s->name);
            gpu_emit_expr(ctx, s->children[0]);
            fprintf(ctx->out, "] = ");
            gpu_emit_expr(ctx, s->children[1]);
            fprintf(ctx->out, ";\n");
            return;
        }
        case AST_IF_STMT: {
            if (s->child_count < 2) { gpu_fail(ctx, s, "a malformed if statement"); return; }
            fprintf(ctx->out, "    if (");
            gpu_emit_expr(ctx, s->children[0]);
            fprintf(ctx->out, ") {\n");
            gpu_emit_stmt(ctx, s->children[1]);
            fprintf(ctx->out, "    }");
            if (s->child_count > 2) {
                fprintf(ctx->out, " else {\n");
                gpu_emit_stmt(ctx, s->children[2]);
                fprintf(ctx->out, "    }");
            }
            fprintf(ctx->out, "\n");
            return;
        }
        case AST_WHILE_STMT: {
            if (s->child_count < 2) { gpu_fail(ctx, s, "a malformed while statement"); return; }
            fprintf(ctx->out, "    while (");
            gpu_emit_expr(ctx, s->children[0]);
            fprintf(ctx->out, ") {\n");
            gpu_emit_stmt(ctx, s->children[1]);
            fprintf(ctx->out, "    }\n");
            return;
        }
        case AST_RETURN:
            if (ctx->in_kernel_main) {
                if (s->child_count > 0) { gpu_fail(ctx, s, "a value-returning return inside the kernel entry point"); return; }
                fprintf(ctx->out, "    return;\n");
            } else {
                if (s->child_count == 0) { gpu_fail(ctx, s, "a bare return in a carried helper (it must return a scalar value)"); return; }
                fprintf(ctx->out, "    return ");
                gpu_emit_expr(ctx, s->children[0]);
                fprintf(ctx->out, ";\n");
            }
            return;
        default:
            /* A block node (no explicit AST_BLOCK type in this AST - bodies
               are represented as a parent whose children are statements) is
               reached via AST_FUNCTION iteration, not here; anything else in
               statement position is out of the subset. */
            if (s->child_count > 0 && s->type != AST_FUNC_CALL) {
                for (size_t i = 0; i < s->child_count; i++) gpu_emit_stmt(ctx, s->children[i]);
                return;
            }
            gpu_fail(ctx, s, "this statement form");
            return;
    }
}

/* Lowers `fn` (a non-kernel helper - no f32[] params, no buffer access) into
   a GLSL function and appends it to ctx->f_proto/ctx->f_helpers. Returns
   false (with an error already reported) on failure, including recursion
   and buffer-parameter use. Idempotent: a helper already carried or in
   progress is a no-op success. */
static bool gpu_carry_function(GpuLowerCtx *ctx, const ASTNode *fn) {
    for (int i = 0; i < ctx->carried_count; i++) if (ctx->carried_nodes[i] == fn) return true;
    for (int i = 0; i < ctx->in_progress_count; i++) {
        if (ctx->in_progress[i] == fn) {
            gpu_fail(ctx, fn, "a recursive call chain (not representable on the GPU)");
            return false;
        }
    }
    if (ctx->carried_count >= GPU_MAX_CARRIED || ctx->in_progress_count >= GPU_MAX_CARRIED) {
        gpu_fail(ctx, fn, "too many carried helper functions");
        return false;
    }

    /* Save/restore the caller's local scope: a helper gets its own fresh
       scope built from its own parameters, not the kernel's. */
    GpuLowerCtx saved = *ctx;
    ctx->scalar_count = 0;
    ctx->local_count = 0;
    ctx->buffer_count = 0;
    ctx->fn_name = fn->name;
    ctx->in_kernel_main = false;
    ctx->in_progress[ctx->in_progress_count++] = fn;

    for (size_t i = 0; i < fn->child_count; i++) {
        ASTNode *p = fn->children[i];
        if (p->type != AST_PARAM) continue;
        if (gpu_is_glsl_reserved(p->name)) {
            gpu_fail(ctx, p, "a reserved GLSL keyword as a carried helper function's parameter name");
            *ctx = saved;
            return false;
        }
        if (p->declared_type == COBRA_TYPE_SLICE_F32) {
            gpu_fail(ctx, p, "a buffer parameter in a carried (non-kernel) helper function");
            *ctx = saved;
            return false;
        }
        if (p->declared_type != COBRA_TYPE_F32 && p->declared_type != COBRA_TYPE_I64 &&
            p->declared_type != COBRA_TYPE_I32 && p->declared_type != COBRA_TYPE_BOOL) {
            gpu_fail(ctx, p, "a parameter type other than f32/i64/i32/bool in a carried helper function");
            *ctx = saved;
            return false;
        }
        if (ctx->scalar_count < 16) {
            snprintf(ctx->scalar_names[ctx->scalar_count], COBRA_MAX_IDENT_LEN, "%s", p->name);
            ctx->scalar_types[ctx->scalar_count] = p->declared_type;
            ctx->scalar_count++;
        }
    }

    CobraTypeKind ret_type = gpu_infer_return_type(fn);
    const char *glsl_ret = gpu_glsl_scalar_type(ret_type);

    fprintf(ctx->f_proto, "%s %s(", glsl_ret, fn->name);
    for (int i = 0; i < ctx->scalar_count; i++)
        fprintf(ctx->f_proto, "%s%s %s", i ? ", " : "", gpu_glsl_scalar_type(ctx->scalar_types[i]), ctx->scalar_names[i]);
    if (ctx->scalar_count == 0) fprintf(ctx->f_proto, "void");
    fprintf(ctx->f_proto, ");\n");

    fprintf(ctx->f_helpers, "%s %s(", glsl_ret, fn->name);
    for (int i = 0; i < ctx->scalar_count; i++)
        fprintf(ctx->f_helpers, "%s%s %s", i ? ", " : "", gpu_glsl_scalar_type(ctx->scalar_types[i]), ctx->scalar_names[i]);
    if (ctx->scalar_count == 0) fprintf(ctx->f_helpers, "void");
    fprintf(ctx->f_helpers, ") {\n");
    ctx->out = ctx->f_helpers;
    for (size_t i = 0; i < fn->child_count; i++) {
        ASTNode *stmt = fn->children[i];
        if (stmt->type == AST_PARAM) continue;
        gpu_emit_stmt(ctx, stmt);
        if (ctx->failed) { *ctx = saved; return false; }
    }
    fprintf(ctx->f_helpers, "}\n");

    bool ok = !ctx->failed;
    /* Restore the caller's scope/output but keep the growing carried list,
       prototype stream and helper-body stream (those are shared/global for
       the whole lowering pass, not per-scope). */
    const ASTNode *carried_snapshot[GPU_MAX_CARRIED];
    int carried_snapshot_count = ctx->carried_count;
    memcpy(carried_snapshot, ctx->carried_nodes, sizeof(carried_snapshot));
    *ctx = saved;
    ctx->carried_count = carried_snapshot_count;
    memcpy(ctx->carried_nodes, carried_snapshot, sizeof(carried_snapshot));
    if (ok) ctx->carried_nodes[ctx->carried_count++] = fn;
    return ok;
}

/* Emits a complete GLSL compute shader for `fn` into `out`. `fn` must be
   TARGET_DEV_GPU_KERNEL-tagged (checked by the caller). `program` is the
   full merged program AST (imports already resolved, same as codegen sees),
   used to carry any ordinary functions the kernel calls. Returns true on
   success; on failure, an error naming the offending construct/line has
   already gone to stderr and `out`'s contents must be discarded. */
bool cobra_gpu_lower_function(const ASTNode *program, const ASTNode *fn, FILE *out) {
    GpuLowerCtx ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.program = program;
    ctx.fn_name = fn->name;
    ctx.source_file = fn->source_file;
    ctx.in_kernel_main = true;

    char proto_buf[65536] = {0}, helpers_buf[262144] = {0}, main_buf[131072] = {0};
    size_t proto_len = 0, helpers_len = 0, main_len = 0;
    FILE *f_proto = fmemopen(proto_buf, sizeof(proto_buf), "w");
    FILE *f_helpers = fmemopen(helpers_buf, sizeof(helpers_buf), "w");
    FILE *f_main = fmemopen(main_buf, sizeof(main_buf), "w");
    if (!f_proto || !f_helpers || !f_main) {
        fprintf(stderr, "%s:%d: error: @gpu kernel '%s': failed to allocate lowering buffers\n",
                fn->source_file, fn->source_line, fn->name);
        if (f_proto) fclose(f_proto);
        if (f_helpers) fclose(f_helpers);
        if (f_main) fclose(f_main);
        return false;
    }
    ctx.f_proto = f_proto;
    ctx.f_helpers = f_helpers;
    ctx.out = f_main;

    for (size_t i = 0; i < fn->child_count; i++) {
        ASTNode *p = fn->children[i];
        if (p->type != AST_PARAM) continue;
        if (gpu_is_glsl_reserved(p->name)) {
            fprintf(stderr, "%s:%d:%d: error: @gpu kernel '%s' parameter '%s' is a reserved GLSL keyword; pick a different name\n",
                    p->source_file, p->source_line, p->source_col, fn->name, p->name);
            goto done_fail;
        }
        if (p->declared_type == COBRA_TYPE_SLICE_F32) {
            if (ctx.buffer_count >= GPU_MAX_BUFFERS) { gpu_fail(&ctx, p, "too many buffer parameters"); goto done_fail; }
            snprintf(ctx.buffer_names[ctx.buffer_count], COBRA_MAX_IDENT_LEN, "%s", p->name);
            ctx.buffer_count++;
        } else if (p->declared_type == COBRA_TYPE_F32 || p->declared_type == COBRA_TYPE_I64 ||
                   p->declared_type == COBRA_TYPE_I32 || p->declared_type == COBRA_TYPE_BOOL) {
            if (ctx.scalar_count >= 16) { gpu_fail(&ctx, p, "this many scalar parameters"); goto done_fail; }
            snprintf(ctx.scalar_names[ctx.scalar_count], COBRA_MAX_IDENT_LEN, "%s", p->name);
            ctx.scalar_types[ctx.scalar_count] = p->declared_type;
            ctx.scalar_count++;
        } else {
            gpu_fail(&ctx, p, "a parameter type other than f32[]/f32/i64/i32/bool");
            goto done_fail;
        }
    }
    if (ctx.buffer_count == 0) {
        fprintf(stderr, "%s:%d: error: @gpu kernel '%s' needs at least one f32[] buffer parameter\n",
                fn->source_file, fn->source_line, fn->name);
        goto done_fail;
    }

    for (size_t i = 0; i < fn->child_count; i++) {
        ASTNode *stmt = fn->children[i];
        if (stmt->type == AST_PARAM) continue;
        gpu_emit_stmt(&ctx, stmt);
        if (ctx.failed) goto done_fail;
    }

    fflush(f_proto); fflush(f_helpers); fflush(f_main);
    proto_len = strlen(proto_buf); helpers_len = strlen(helpers_buf); main_len = strlen(main_buf);
    fclose(f_proto); fclose(f_helpers); fclose(f_main);

    fprintf(out, "#version 450\nlayout(local_size_x = 256) in;\n");
    for (int i = 0; i < ctx.buffer_count; i++)
        fprintf(out, "layout(binding = %d) buffer Buf%d { float %s[]; };\n", i, i, ctx.buffer_names[i]);
    fprintf(out, "layout(push_constant) uniform PC {");
    for (int i = 0; i < ctx.buffer_count; i++) fprintf(out, " uint klen_%s;", ctx.buffer_names[i]);
    for (int i = 0; i < ctx.scalar_count; i++)
        fprintf(out, " %s %s;", gpu_glsl_scalar_type(ctx.scalar_types[i]), ctx.scalar_names[i]);
    fprintf(out, " } kpc;\n");

    fwrite(proto_buf, 1, proto_len, out);
    fwrite(helpers_buf, 1, helpers_len, out);

    fprintf(out, "void main() {\n    uint kgid = gl_GlobalInvocationID.x;\n    if (kgid >= kpc.klen_%s) return;\n",
            ctx.buffer_names[0]);
    fwrite(main_buf, 1, main_len, out);
    fprintf(out, "}\n");
    return true;

done_fail:
    fclose(f_proto); fclose(f_helpers); fclose(f_main);
    return false;
}

/* ============================================================================
 * Reverse-mode autodiff for elementwise @gpu kernels.
 *
 * Eligible kernels: every buffer access (read or write) indexes at the
 * kernel's own thread index (gpu_index()/its local alias) - i.e. a pointwise
 * map, the shape of every activation function, elementwise loss, and custom
 * elementwise layer. Exactly one buffer is written (the "output"); every
 * other buffer parameter, plus every scalar parameter, gets a gradient.
 * No while loops, no carried helper calls, no multiple output buffers -
 * backward generation is silently skipped (not an error) for kernels
 * outside this subset, since not every @gpu kernel needs to be
 * differentiable.
 *
 * Implementation: two passes over the same restricted AST gpu_emit_stmt/
 * gpu_emit_expr already walk for the forward kernel.
 *   Pass A (seed): mirrors the kernel's if/else skeleton; at the single
 *     output buffer's index-assignment site(s), seeds reverse accumulation
 *     with the incoming grad_out[i] via emit_grad().
 *   Pass B (propagate): every local variable's own defining expression is
 *     differentiated, in reverse declaration order, each wrapped in the
 *     same if/else guard it was originally declared under - this is
 *     reverse-mode AD's standard "replay the tape backward" step, done
 *     directly via recursive codegen instead of an actual runtime tape.
 * emit_grad() implements the chain rule for the subset's operators
 * (+ - * /), the whitelisted GLSL math builtins, and buffer/scalar/local
 * leaves; anything else it encounters makes the kernel ineligible.
 * ========================================================================= */

#define GPU_GRAD_MAX_DEFS 64
#define GPU_GRAD_MAX_DEPTH 8

typedef struct {
    char cond[512];
    bool is_else;
} GpuGuardFrame;

typedef struct {
    char name[COBRA_MAX_IDENT_LEN];
    const ASTNode *rhs;
    GpuGuardFrame guard[GPU_GRAD_MAX_DEPTH];
    int guard_depth;
} GpuGradDef;

typedef struct {
    GpuLowerCtx *fctx;              /* forward ctx: buffer/scalar name lookup, program, source_file */
    FILE *out;                      /* pass A output stream (kernel main body) */
    char output_buf[COBRA_MAX_IDENT_LEN];
    bool have_output_buf;

    GpuGradDef defs[GPU_GRAD_MAX_DEFS];
    int def_count;

    GpuGuardFrame guard[GPU_GRAD_MAX_DEPTH];
    int guard_depth;

    bool touched_dg[GPU_MAX_BUFFERS];   /* which buffers received a dg_ accumulator */
    bool touched_d_scalar[16];          /* which scalar params received a d_ accumulator */

    bool ineligible;                    /* silent - not a user-facing error */
} GpuGradCtx;

static bool gpu_grad_local_is_def(GpuGradCtx *g, const char *name) {
    for (int i = 0; i < g->def_count; i++) if (!strcmp(g->defs[i].name, name)) return true;
    return false;
}

/* Renders `e` as plain GLSL expression text (no side effects) by replaying
   gpu_emit_expr into a scratch buffer. Caller does not own the returned
   pointer past the next call - it points into a small rotating static pool,
   which is enough for the short, non-recursive-across-calls uses below
   (each call site consumes the string immediately, before nesting another). */
static const char *gpu_expr_text(GpuLowerCtx *fctx, const ASTNode *e) {
    static char pool[4][1024];
    static int slot = 0;
    slot = (slot + 1) % 4;
    char *buf = pool[slot];
    FILE *f = fmemopen(buf, sizeof(pool[0]), "w");
    if (!f) { buf[0] = '\0'; return buf; }
    FILE *saved_out = fctx->out;
    fctx->out = f;
    gpu_emit_expr(fctx, e);
    fctx->out = saved_out;
    fflush(f);
    fclose(f);
    return buf;
}

static void gpu_emit_grad(GpuGradCtx *g, const ASTNode *e, const char *adjoint);

static void gpu_guard_open(FILE *out, const GpuGuardFrame *guard, int depth) {
    for (int i = 0; i < depth; i++) fprintf(out, "    if (%s%s) {\n", guard[i].is_else ? "!" : "", guard[i].cond);
}
static void gpu_guard_close(FILE *out, int depth) {
    for (int i = 0; i < depth; i++) fprintf(out, "    }\n");
}

static void gpu_emit_grad(GpuGradCtx *g, const ASTNode *e, const char *adjoint) {
    /* gpu_expr_text (used below for operand/condition text) can fail via
       g->fctx's own gpu_fail (e.g. backward mode hitting a non-carryable
       call) - that sets fctx->failed, not g->ineligible, since gpu_expr_text
       replays the forward emitter, which knows nothing about the grad
       walk. Propagate it here so a failure never goes unnoticed. */
    if (g->fctx->failed) g->ineligible = true;
    if (g->ineligible || !e) return;
    switch (e->type) {
        case AST_INT_LITERAL:
        case AST_FLOAT_LITERAL:
        case AST_BOOL_LITERAL:
            return; /* constants are not gradient sinks */
        case AST_VAR_REF: {
            int buf_idx = gpu_buffer_index(g->fctx, e->name);
            if (buf_idx >= 0) return; /* a bare buffer name (no index) can't appear in a value position */
            for (int i = 0; i < g->fctx->scalar_count; i++) {
                if (!strcmp(g->fctx->scalar_names[i], e->name)) {
                    fprintf(g->out, "    d_%s += %s;\n", e->name, adjoint);
                    g->touched_d_scalar[i] = true;
                    return;
                }
            }
            if (gpu_grad_local_is_def(g, e->name)) {
                fprintf(g->out, "    d_%s += %s;\n", e->name, adjoint);
                return;
            }
            g->ineligible = true;
            return;
        }
        case AST_ARRAY_INDEX: {
            int buf_idx = gpu_buffer_index(g->fctx, e->name);
            if (buf_idx < 0 || e->child_count != 1) { g->ineligible = true; return; }
            /* Elementwise restriction: the index must be exactly gpu_index()
               (or its local alias) - anything else means this kernel isn't
               a pointwise map and autodiff isn't attempted. */
            const ASTNode *idx = e->children[0];
            bool is_kgid = (idx->type == AST_FUNC_CALL && !strcmp(idx->name, "gpu_index")) ||
                           (idx->type == AST_VAR_REF && g->fctx->local_count > 0 &&
                            !strcmp(idx->name, g->fctx->local_names[0]));
            if (!is_kgid) { g->ineligible = true; return; }
            fprintf(g->out, "    dg_%s += %s;\n", e->name, adjoint);
            g->touched_dg[buf_idx] = true;
            return;
        }
        case AST_BINARY_OP: {
            if (e->child_count != 2) { g->ineligible = true; return; }
            const char *op = e->name;
            if (!strcmp(op, "+")) {
                gpu_emit_grad(g, e->children[0], adjoint);
                gpu_emit_grad(g, e->children[1], adjoint);
                return;
            }
            if (!strcmp(op, "-")) {
                char neg[1200]; snprintf(neg, sizeof(neg), "-(%s)", adjoint);
                gpu_emit_grad(g, e->children[0], adjoint);
                gpu_emit_grad(g, e->children[1], neg);
                return;
            }
            if (!strcmp(op, "*")) {
                const char *rtext = gpu_expr_text(g->fctx, e->children[1]);
                char da[1200]; snprintf(da, sizeof(da), "(%s) * (%s)", adjoint, rtext);
                gpu_emit_grad(g, e->children[0], da);
                const char *ltext = gpu_expr_text(g->fctx, e->children[0]);
                char db[1200]; snprintf(db, sizeof(db), "(%s) * (%s)", adjoint, ltext);
                gpu_emit_grad(g, e->children[1], db);
                return;
            }
            if (!strcmp(op, "/")) {
                const char *rtext = gpu_expr_text(g->fctx, e->children[1]);
                char da[1200]; snprintf(da, sizeof(da), "(%s) / (%s)", adjoint, rtext);
                gpu_emit_grad(g, e->children[0], da);
                const char *ltext = gpu_expr_text(g->fctx, e->children[0]);
                char db[1200]; snprintf(db, sizeof(db), "-(%s) * (%s) / ((%s) * (%s))", adjoint, ltext, rtext, rtext);
                gpu_emit_grad(g, e->children[1], db);
                return;
            }
            /* Comparisons only ever belong in an `if` condition in this
               subset; reaching one here means it fed a differentiable
               value, which this kernel's shape doesn't support. */
            g->ineligible = true;
            return;
        }
        case AST_FUNC_CALL: {
            if (e->child_count == 0) { g->ineligible = true; return; } /* gpu_index() etc: not a gradient sink */
            const ASTNode *u = e->children[0];
            const char *utext = gpu_expr_text(g->fctx, u);
            if (!strcmp(e->name, "sqrt") && e->child_count == 1) {
                char da[1200]; snprintf(da, sizeof(da), "(%s) * 0.5 / sqrt(%s)", adjoint, utext);
                gpu_emit_grad(g, u, da);
                return;
            }
            if (!strcmp(e->name, "exp") && e->child_count == 1) {
                char da[1200]; snprintf(da, sizeof(da), "(%s) * exp(%s)", adjoint, utext);
                gpu_emit_grad(g, u, da);
                return;
            }
            if (!strcmp(e->name, "log") && e->child_count == 1) {
                char da[1200]; snprintf(da, sizeof(da), "(%s) / (%s)", adjoint, utext);
                gpu_emit_grad(g, u, da);
                return;
            }
            if (!strcmp(e->name, "tanh") && e->child_count == 1) {
                char da[1200]; snprintf(da, sizeof(da), "(%s) * (1.0 - tanh(%s) * tanh(%s))", adjoint, utext, utext);
                gpu_emit_grad(g, u, da);
                return;
            }
            if (!strcmp(e->name, "abs") && e->child_count == 1) {
                char da[1200]; snprintf(da, sizeof(da), "(%s) * sign(%s)", adjoint, utext);
                gpu_emit_grad(g, u, da);
                return;
            }
            if ((!strcmp(e->name, "floor") || !strcmp(e->name, "ceil")) && e->child_count == 1) {
                return; /* zero derivative almost everywhere; no propagation */
            }
            if (!strcmp(e->name, "max") && e->child_count == 2) {
                const char *vtext = gpu_expr_text(g->fctx, e->children[1]);
                char da[1200]; snprintf(da, sizeof(da), "((%s) >= (%s) ? (%s) : 0.0)", utext, vtext, adjoint);
                gpu_emit_grad(g, e->children[0], da);
                char db[1200]; snprintf(db, sizeof(db), "((%s) > (%s) ? 0.0 : (%s))", utext, vtext, adjoint);
                gpu_emit_grad(g, e->children[1], db);
                return;
            }
            if (!strcmp(e->name, "min") && e->child_count == 2) {
                const char *vtext = gpu_expr_text(g->fctx, e->children[1]);
                char da[1200]; snprintf(da, sizeof(da), "((%s) <= (%s) ? (%s) : 0.0)", utext, vtext, adjoint);
                gpu_emit_grad(g, e->children[0], da);
                char db[1200]; snprintf(db, sizeof(db), "((%s) < (%s) ? 0.0 : (%s))", utext, vtext, adjoint);
                gpu_emit_grad(g, e->children[1], db);
                return;
            }
            if (!strcmp(e->name, "pow") && e->child_count == 2) {
                double n;
                if (e->children[1]->type == AST_INT_LITERAL) n = (double)e->children[1]->literal_i64;
                else if (e->children[1]->type == AST_FLOAT_LITERAL) n = e->children[1]->float_val;
                else { g->ineligible = true; return; } /* variable exponent: not supported */
                char da[1200]; snprintf(da, sizeof(da), "(%s) * %g * pow(%s, %g)", adjoint, n, utext, n - 1.0);
                gpu_emit_grad(g, u, da);
                return;
            }
            g->ineligible = true; /* carried helper calls: not differentiated in v1 */
            return;
        }
        default:
            g->ineligible = true;
            return;
    }
}

/* Pass A: mirrors the kernel's control-flow skeleton, collecting local defs
   (with their guard context, for pass B) and seeding gradient accumulation
   at the output buffer's write site(s). Returns false (ineligible) if the
   kernel falls outside the autodiff subset. */
static bool gpu_grad_walk_stmt(GpuGradCtx *g, const ASTNode *s) {
    if (g->fctx->failed) g->ineligible = true; /* see gpu_emit_grad's matching guard */
    if (g->ineligible || !s) return !g->ineligible;
    switch (s->type) {
        case AST_VAR_DECL:
        case AST_ASSIGN: {
            /* The kernel's own thread-index local (`i = gpu_index()`) is an
               integer, not a gradient-bearing quantity - register it for
               name resolution only, never as a differentiable def. */
            bool is_index_var = s->child_count > 0 && s->children[0]->type == AST_FUNC_CALL &&
                                 !strcmp(s->children[0]->name, "gpu_index");
            if (is_index_var) {
                if (!gpu_local_known(g->fctx, s->name) && g->fctx->local_count < 64) {
                    snprintf(g->fctx->local_names[g->fctx->local_count], COBRA_MAX_IDENT_LEN, "%s", s->name);
                    g->fctx->local_types[g->fctx->local_count] = s->declared_type;
                    g->fctx->local_count++;
                }
                return true;
            }
            if (g->def_count >= GPU_GRAD_MAX_DEFS || g->guard_depth >= GPU_GRAD_MAX_DEPTH) { g->ineligible = true; return false; }
            /* SSA requirement: a name may be defined once. */
            if (gpu_grad_local_is_def(g, s->name)) { g->ineligible = true; return false; }
            GpuGradDef *d = &g->defs[g->def_count++];
            snprintf(d->name, COBRA_MAX_IDENT_LEN, "%s", s->name);
            d->rhs = s->child_count > 0 ? s->children[0] : NULL;
            d->guard_depth = g->guard_depth;
            memcpy(d->guard, g->guard, sizeof(GpuGuardFrame) * (size_t)g->guard_depth);
            /* Register with fctx too (if not already, e.g. the index-var
               alias), so gpu_expr_text/gpu_emit_expr can resolve this name
               when rendering guard-condition text and operand text - those
               go through the forward emitter, which only knows fctx's own
               local_names, not g->defs. */
            if (!gpu_local_known(g->fctx, s->name) && g->fctx->local_count < 64) {
                snprintf(g->fctx->local_names[g->fctx->local_count], COBRA_MAX_IDENT_LEN, "%s", s->name);
                g->fctx->local_types[g->fctx->local_count] = s->declared_type;
                g->fctx->local_count++;
            }
            return true;
        }
        case AST_INDEX_ASSIGN: {
            if (s->child_count != 2) { g->ineligible = true; return false; }
            if (!g->have_output_buf) {
                snprintf(g->output_buf, COBRA_MAX_IDENT_LEN, "%s", s->name);
                g->have_output_buf = true;
            } else if (strcmp(g->output_buf, s->name)) {
                g->ineligible = true; /* more than one buffer written: not supported */
                return false;
            }
            const ASTNode *idx = s->children[0];
            bool is_kgid = (idx->type == AST_FUNC_CALL && !strcmp(idx->name, "gpu_index")) ||
                           (idx->type == AST_VAR_REF && g->fctx->local_count > 0 &&
                            !strcmp(idx->name, g->fctx->local_names[0]));
            if (!is_kgid) { g->ineligible = true; return false; }
            /* if/else guards around this write are already open in the
               output stream via the AST_IF_STMT recursion below. */
            gpu_emit_grad(g, s->children[1], "grad_out[i]");
            return !g->ineligible;
        }
        case AST_IF_STMT: {
            if (s->child_count < 2 || g->guard_depth >= GPU_GRAD_MAX_DEPTH) { g->ineligible = true; return false; }
            const char *cond_text = gpu_expr_text(g->fctx, s->children[0]);
            snprintf(g->guard[g->guard_depth].cond, sizeof(g->guard[0].cond), "%s", cond_text);
            g->guard[g->guard_depth].is_else = false;
            fprintf(g->out, "    if (%s) {\n", g->guard[g->guard_depth].cond);
            g->guard_depth++;
            if (!gpu_grad_walk_stmt(g, s->children[1])) return false;
            g->guard_depth--;
            fprintf(g->out, "    }\n");
            if (s->child_count > 2) {
                g->guard[g->guard_depth].is_else = true;
                fprintf(g->out, "    if (!(%s)) {\n", g->guard[g->guard_depth].cond);
                g->guard_depth++;
                if (!gpu_grad_walk_stmt(g, s->children[2])) return false;
                g->guard_depth--;
                fprintf(g->out, "    }\n");
            }
            return true;
        }
        case AST_WHILE_STMT:
            g->ineligible = true; /* not supported in v1 */
            return false;
        case AST_RETURN:
            return true; /* an early-exit return carries no gradient itself */
        default:
            if (s->child_count > 0 && s->type != AST_FUNC_CALL) {
                for (size_t i = 0; i < s->child_count; i++) {
                    if (!gpu_grad_walk_stmt(g, s->children[i])) return false;
                }
                return true;
            }
            g->ineligible = true;
            return false;
    }
}

/* Emits `<kernel>_backward`'s GLSL body into `out` if `fn` is eligible for
   autodiff (see file header). Returns false silently (no eligible backward
   pass; not an error) when the kernel falls outside the subset. On success,
   reports the buffer/scalar parameter names/types (same order as `fn`'s own
   params) so main.c can generate a matching wrapper; `*out_output_buf` is
   set to the name of the buffer `fn` writes. */
bool cobra_gpu_lower_backward(const ASTNode *program, const ASTNode *fn, FILE *out,
                               int *out_buffer_count, char out_buffer_names[][COBRA_MAX_IDENT_LEN],
                               int *out_scalar_count, char out_scalar_names[][COBRA_MAX_IDENT_LEN],
                               CobraTypeKind *out_scalar_types, char *out_output_buf) {
    GpuLowerCtx fctx; memset(&fctx, 0, sizeof(fctx));
    fctx.program = program;
    fctx.fn_name = fn->name;
    fctx.source_file = fn->source_file;
    fctx.in_kernel_main = true;
    fctx.backward_mode = true;

    for (size_t i = 0; i < fn->child_count; i++) {
        ASTNode *p = fn->children[i];
        if (p->type != AST_PARAM) continue;
        if (p->declared_type == COBRA_TYPE_SLICE_F32) {
            if (fctx.buffer_count >= GPU_MAX_BUFFERS) return false;
            snprintf(fctx.buffer_names[fctx.buffer_count], COBRA_MAX_IDENT_LEN, "%s", p->name);
            fctx.buffer_count++;
        } else if (p->declared_type == COBRA_TYPE_F32) {
            if (fctx.scalar_count >= 16) return false;
            snprintf(fctx.scalar_names[fctx.scalar_count], COBRA_MAX_IDENT_LEN, "%s", p->name);
            fctx.scalar_types[fctx.scalar_count] = p->declared_type;
            fctx.scalar_count++;
        } else {
            return false; /* i64/i32/bool params: no meaningful gradient, not supported as differentiable inputs */
        }
    }
    /* The kernel's own index-alias local (`i = gpu_index()`), if any, is
       tracked as local_names[0] by convention - find it so gpu_emit_grad's
       is_kgid check matches. */
    for (size_t i = 0; i < fn->child_count && fctx.local_count == 0; i++) {
        const ASTNode *s = fn->children[i];
        if ((s->type == AST_VAR_DECL || s->type == AST_ASSIGN) && s->child_count > 0 &&
            s->children[0]->type == AST_FUNC_CALL && !strcmp(s->children[0]->name, "gpu_index")) {
            snprintf(fctx.local_names[0], COBRA_MAX_IDENT_LEN, "%s", s->name);
            fctx.local_count = 1;
        }
    }
    if (fctx.buffer_count == 0) return false;
    /* Backward's generated GLSL names gradient bindings grad_<param> and
       grad_<param>_partial; if the kernel already has a parameter with one
       of those exact names (e.g. an optimizer-step kernel whose own second
       argument is literally called `grad_w`), the generated shader would
       declare the same GLSL identifier twice. Bail out cleanly rather than
       emit a shader that fails to compile. */
    for (size_t i = 0; i < fn->child_count; i++) {
        ASTNode *p = fn->children[i];
        if (p->type != AST_PARAM) continue;
        char probe[COBRA_MAX_IDENT_LEN];
        for (int b = 0; b < fctx.buffer_count; b++) {
            snprintf(probe, sizeof(probe), "grad_%s", fctx.buffer_names[b]);
            if (!strcmp(p->name, probe)) return false;
        }
        for (int s = 0; s < fctx.scalar_count; s++) {
            snprintf(probe, sizeof(probe), "grad_%s_partial", fctx.scalar_names[s]);
            if (!strcmp(p->name, probe)) return false;
        }
    }

    char main_buf[131072] = {0};
    FILE *f_main = fmemopen(main_buf, sizeof(main_buf), "w");
    if (!f_main) return false;

    GpuGradCtx g; memset(&g, 0, sizeof(g));
    g.fctx = &fctx;
    g.out = f_main;

    bool ok = true;
    for (size_t i = 0; i < fn->child_count && ok; i++) {
        ASTNode *stmt = fn->children[i];
        if (stmt->type == AST_PARAM) continue;
        if (!gpu_grad_walk_stmt(&g, stmt)) ok = false;
    }
    if (!ok || g.ineligible || !g.have_output_buf) { fclose(f_main); return false; }

    /* Pass B: propagate each local's own gradient into its defining
       expression, in reverse declaration order, each wrapped in the same
       if/else guard it was declared under. */
    for (int i = g.def_count - 1; i >= 0 && !g.ineligible; i--) {
        GpuGradDef *d = &g.defs[i];
        gpu_guard_open(f_main, d->guard, d->guard_depth);
        char seed[64]; snprintf(seed, sizeof(seed), "d_%s", d->name);
        gpu_emit_grad(&g, d->rhs, seed);
        gpu_guard_close(f_main, d->guard_depth);
    }
    if (g.ineligible) { fclose(f_main); return false; }

    fflush(f_main);
    size_t main_len = strlen(main_buf);
    fclose(f_main);

    /* Emit the complete backward shader: grad_out (input) + every original
       buffer (readonly, same binding order as forward) + one grad_<name>
       output per original buffer + one grad_<scalar>_partial output per
       scalar param, all sharing the forward kernel's single dispatch
       domain (klen), plus the original scalar values as push constants. */
    fprintf(out, "#version 450\nlayout(local_size_x = 256) in;\n");
    int binding = 0;
    fprintf(out, "layout(binding = %d) readonly buffer GBufOut { float grad_out[]; };\n", binding++);
    for (int i = 0; i < fctx.buffer_count; i++)
        fprintf(out, "layout(binding = %d) readonly buffer GBufIn%d { float %s[]; };\n", binding++, i, fctx.buffer_names[i]);
    for (int i = 0; i < fctx.buffer_count; i++)
        fprintf(out, "layout(binding = %d) writeonly buffer GBufGrad%d { float grad_%s[]; };\n", binding++, i, fctx.buffer_names[i]);
    for (int i = 0; i < fctx.scalar_count; i++)
        fprintf(out, "layout(binding = %d) writeonly buffer GBufGradS%d { float grad_%s_partial[]; };\n", binding++, i, fctx.scalar_names[i]);

    fprintf(out, "layout(push_constant) uniform PC { uint klen;");
    for (int i = 0; i < fctx.scalar_count; i++) fprintf(out, " float %s;", fctx.scalar_names[i]);
    fprintf(out, " } kpc;\n");

    fprintf(out, "void main() {\n    uint kgid = gl_GlobalInvocationID.x;\n    int i = int(kgid);\n    if (kgid >= kpc.klen) return;\n");
    for (int i = 0; i < fctx.buffer_count; i++) fprintf(out, "    float dg_%s = 0.0;\n", fctx.buffer_names[i]);
    for (int i = 0; i < fctx.scalar_count; i++) fprintf(out, "    float d_%s = 0.0;\n", fctx.scalar_names[i]);
    for (int i = 0; i < g.def_count; i++) fprintf(out, "    float d_%s = 0.0;\n", g.defs[i].name);

    /* Recompute the forward kernel's local values: the backward pass needs
       them (both for branch conditions and as operands in gradient
       formulas, e.g. d/dx(x*w) = w needs w, d/dw(x*w) = x needs x[i], but
       an intermediate like `v = x*w` used further downstream needs its own
       recomputed value too). Elementwise kernels make this cheap - just
       replay the same statements from the same original inputs. Declared
       at function scope (not inside the guard) so later Pass A/B blocks,
       which re-test the same conditions separately, can still see them. */
    for (int i = 0; i < g.def_count; i++) fprintf(out, "    float %s;\n", g.defs[i].name);
    for (int i = 0; i < g.def_count; i++) {
        GpuGradDef *d = &g.defs[i];
        gpu_guard_open(out, d->guard, d->guard_depth);
        fprintf(out, "    %s = ", d->name);
        FILE *saved_out = fctx.out;
        fctx.out = out;
        gpu_emit_expr(&fctx, d->rhs);
        fctx.out = saved_out;
        fprintf(out, ";\n");
        gpu_guard_close(out, d->guard_depth);
    }

    fwrite(main_buf, 1, main_len, out);

    for (int i = 0; i < fctx.buffer_count; i++) fprintf(out, "    grad_%s[i] = dg_%s;\n", fctx.buffer_names[i], fctx.buffer_names[i]);
    for (int i = 0; i < fctx.scalar_count; i++) fprintf(out, "    grad_%s_partial[i] = d_%s;\n", fctx.scalar_names[i], fctx.scalar_names[i]);
    fprintf(out, "}\n");

    *out_buffer_count = fctx.buffer_count;
    for (int i = 0; i < fctx.buffer_count; i++) snprintf(out_buffer_names[i], COBRA_MAX_IDENT_LEN, "%s", fctx.buffer_names[i]);
    *out_scalar_count = fctx.scalar_count;
    for (int i = 0; i < fctx.scalar_count; i++) {
        snprintf(out_scalar_names[i], COBRA_MAX_IDENT_LEN, "%s", fctx.scalar_names[i]);
        out_scalar_types[i] = fctx.scalar_types[i];
    }
    snprintf(out_output_buf, COBRA_MAX_IDENT_LEN, "%s", g.output_buf);
    return true;
}
