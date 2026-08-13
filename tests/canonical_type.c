#include "../include/cobra.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static CobraTypeArena arena;
    cobra_type_arena_init(&arena);

    CobraType *i64 = cobra_type_new(&arena, COBRA_TYPE_I64);
    CobraType *f32 = cobra_type_new(&arena, COBRA_TYPE_F32);
    assert(i64 && f32);

    CobraType *option_a = cobra_type_new(&arena, COBRA_TYPE_OPTION);
    CobraType *option_b = cobra_type_new(&arena, COBRA_TYPE_OPTION);
    assert(option_a && option_b);
    assert(cobra_type_add_generic_arg(option_a, i64));
    assert(cobra_type_add_generic_arg(option_b, i64));
    assert(cobra_type_equal(option_a, option_b));
    assert(cobra_type_validate(&arena, option_a));
    assert(option_a->abi == COBRA_ABI_SUM_INDIRECT);
    assert(option_a->size == COBRA_NATIVE_SUM_TAG_SIZE + COBRA_NATIVE_SUM_SCALAR_SIZE);
    assert(!cobra_type_add_generic_arg(option_a, f32));
    CobraType *result = cobra_type_new(&arena, COBRA_TYPE_RESULT);
    assert(result);
    assert(cobra_type_add_generic_arg(result, i64));
    assert(cobra_type_add_generic_arg(result, f32));
    assert(cobra_type_validate(&arena, result));
    assert(cobra_type_element(result) == i64);
    assert(cobra_type_error(result) == f32);
    assert(result->abi == COBRA_ABI_SUM_INDIRECT);
    assert(result->size == COBRA_NATIVE_SUM_TAG_SIZE + 2 * COBRA_NATIVE_SUM_SCALAR_SIZE);

    CobraType *list = cobra_type_new(&arena, COBRA_TYPE_LIST);
    assert(list && cobra_type_add_generic_arg(list, f32));
    assert(cobra_type_validate(&arena, list));
    assert(cobra_type_element(list) == f32);
    assert(list->abi == COBRA_ABI_REFERENCE);

    CobraType *pair = cobra_type_named(&arena, COBRA_TYPE_STRUCT, "Pair");
    assert(pair);
    assert(cobra_type_add_field(pair, "value", f32, COBRA_OWNERSHIP_VALUE,
                                COBRA_MUTABILITY_MUTABLE, -1));
    assert(cobra_type_add_field(pair, "count", i64, COBRA_OWNERSHIP_VALUE,
                                COBRA_MUTABILITY_DEFAULT, -1));
    assert(cobra_type_validate(&arena, pair));
    assert(pair->fields[0].offset == 0);
    assert(pair->fields[1].offset == 4);
    assert(pair->size == 16);
    assert(pair->abi == COBRA_ABI_STRUCT_VALUE);

    CobraType *view = cobra_type_named(&arena, COBRA_TYPE_STRUCT, "RequestView");
    assert(view);
    assert(cobra_type_add_field(view, "body", list, COBRA_OWNERSHIP_BORROWED,
                                COBRA_MUTABILITY_READONLY, 7));
    assert(cobra_type_validate(&arena, view));
    assert(view->fields[0].ownership == COBRA_OWNERSHIP_BORROWED);
    assert(view->fields[0].mutability == COBRA_MUTABILITY_READONLY);
    assert(view->fields[0].region_id == 7);

    CobraType *dict = cobra_type_new(&arena, COBRA_TYPE_DICT);
    assert(dict && cobra_type_add_generic_arg(dict, cobra_type_named(&arena, COBRA_TYPE_STRING, NULL)));
    assert(dict && cobra_type_add_generic_arg(dict, i64));
    assert(cobra_type_key(dict) != NULL);
    assert(cobra_type_value(dict) == i64);
    assert(cobra_type_validate(&arena, dict));

    CobraType *cycle = cobra_type_named(&arena, COBRA_TYPE_STRUCT, "Cycle");
    assert(cycle);
    assert(cobra_type_add_field(cycle, "next", cycle, COBRA_OWNERSHIP_VALUE,
                                COBRA_MUTABILITY_DEFAULT, -1));
    assert(!cobra_type_validate(&arena, cycle));
    assert(strstr(arena.error, "recursive by-value type cycle") != NULL);

    /* ABI slot classification from the canonical descriptor. */
    assert(cobra_type_abi_slots(i64) == 1);
    assert(cobra_type_abi_slots(f32) == 0);
    assert(cobra_type_abi_slots(option_a) == 1);
    assert(cobra_type_abi_slots(list) == 3);
    assert(cobra_type_abi_slots(dict) == 2);
    CobraType *slice = cobra_type_new(&arena, COBRA_TYPE_SLICE_F32);
    assert(slice && cobra_type_add_generic_arg(slice, f32));
    assert(cobra_type_validate(&arena, slice));
    assert(cobra_type_abi_slots(slice) == 2);
    /* v256 is a vector view: two pointer+length slots at the call boundary
       even though its storage is handled by the vector emitter, not the
       canonical finalizer. */
    CobraType *v256 = cobra_type_new(&arena, COBRA_TYPE_V256);
    assert(v256);
    assert(cobra_type_abi_slots(v256) == 2);

    /* Layout regression: canonical packed sizes and per-field offsets for
       real declarations are pinned below as literals. */
    static CobraTypeArena layout_arena;
    cobra_type_arena_init(&layout_arena);
    ASTNode *program = ast_create_node(AST_PROGRAM, "RootProgram");
    ASTNode *point = ast_create_node(AST_STRUCT_DECL, "Point");
    ASTNode *point_x = ast_create_node(AST_PARAM, "x");
    point_x->declared_type = COBRA_TYPE_I64;
    point_x->canonical_type = cobra_type_make(&layout_arena, COBRA_TYPE_I64, NULL,
                                              NULL, NULL, NULL, NULL,
                                              COBRA_OWNERSHIP_VALUE,
                                              COBRA_MUTABILITY_DEFAULT, -1);
    ASTNode *point_y = ast_create_node(AST_PARAM, "y");
    point_y->declared_type = COBRA_TYPE_I64;
    point_y->canonical_type = cobra_type_make(&layout_arena, COBRA_TYPE_I64, NULL,
                                              NULL, NULL, NULL, NULL,
                                              COBRA_OWNERSHIP_VALUE,
                                              COBRA_MUTABILITY_DEFAULT, -1);
    ast_add_child(point, point_x);
    ast_add_child(point, point_y);
    ast_add_child(program, point);

    ASTNode *pair_decl = ast_create_node(AST_STRUCT_DECL, "Pair");
    ASTNode *pair_value = ast_create_node(AST_PARAM, "value");
    pair_value->declared_type = COBRA_TYPE_F32;
    pair_value->canonical_type = cobra_type_make(&layout_arena, COBRA_TYPE_F32, NULL,
                                                 NULL, NULL, NULL, NULL,
                                                 COBRA_OWNERSHIP_VALUE,
                                                 COBRA_MUTABILITY_DEFAULT, -1);
    ASTNode *pair_count = ast_create_node(AST_PARAM, "count");
    pair_count->declared_type = COBRA_TYPE_I64;
    pair_count->canonical_type = cobra_type_make(&layout_arena, COBRA_TYPE_I64, NULL,
                                                 NULL, NULL, NULL, NULL,
                                                 COBRA_OWNERSHIP_VALUE,
                                                 COBRA_MUTABILITY_DEFAULT, -1);
    ast_add_child(pair_decl, pair_value);
    ast_add_child(pair_decl, pair_count);
    ast_add_child(program, pair_decl);

    ASTNode *box = ast_create_node(AST_STRUCT_DECL, "Box");
    ASTNode *box_top = ast_create_node(AST_PARAM, "top");
    box_top->declared_type = COBRA_TYPE_STRUCT;
    box_top->canonical_type = cobra_type_make(&layout_arena, COBRA_TYPE_STRUCT, "Point",
                                              NULL, NULL, NULL, NULL,
                                              COBRA_OWNERSHIP_VALUE,
                                              COBRA_MUTABILITY_DEFAULT, -1);
    ASTNode *box_bottom = ast_create_node(AST_PARAM, "bottom");
    box_bottom->declared_type = COBRA_TYPE_STRUCT;
    box_bottom->canonical_type = cobra_type_make(&layout_arena, COBRA_TYPE_STRUCT, "Point",
                                                 NULL, NULL, NULL, NULL,
                                                 COBRA_OWNERSHIP_VALUE,
                                                 COBRA_MUTABILITY_DEFAULT, -1);
    ast_add_child(box, box_top);
    ast_add_child(box, box_bottom);
    ast_add_child(program, box);

    ASTNode *phase = ast_create_node(AST_ENUM_DECL, "Phase");
    ast_add_child(program, phase);
    ASTNode *state = ast_create_node(AST_STRUCT_DECL, "State");
    ASTNode *state_phase = ast_create_node(AST_PARAM, "phase");
    state_phase->declared_type = COBRA_TYPE_STRUCT;
    state_phase->canonical_type = cobra_type_make(&layout_arena, COBRA_TYPE_ENUM, "Phase",
                                                  NULL, NULL, NULL, NULL,
                                                  COBRA_OWNERSHIP_VALUE,
                                                  COBRA_MUTABILITY_DEFAULT, -1);
    ASTNode *state_level = ast_create_node(AST_PARAM, "level");
    state_level->declared_type = COBRA_TYPE_I64;
    state_level->canonical_type = cobra_type_make(&layout_arena, COBRA_TYPE_I64, NULL,
                                                  NULL, NULL, NULL, NULL,
                                                  COBRA_OWNERSHIP_VALUE,
                                                  COBRA_MUTABILITY_DEFAULT, -1);
    ast_add_child(state, state_phase);
    ast_add_child(state, state_level);
    ast_add_child(program, state);

    ASTNode *view_decl = ast_create_node(AST_STRUCT_DECL, "RequestView");
    ASTNode *view_body = ast_create_node(AST_PARAM, "body");
    view_body->declared_type = COBRA_TYPE_SLICE_U8;
    const CobraType *test_u8 = cobra_type_make(&layout_arena, COBRA_TYPE_U8, NULL,
                                               NULL, NULL, NULL, NULL,
                                               COBRA_OWNERSHIP_VALUE,
                                               COBRA_MUTABILITY_DEFAULT, -1);
    view_body->canonical_type = cobra_type_make(&layout_arena, COBRA_TYPE_SLICE_U8, NULL,
                                                test_u8, NULL, NULL, NULL,
                                                COBRA_OWNERSHIP_BORROWED,
                                                COBRA_MUTABILITY_READONLY, -1);
    ast_add_child(view_decl, view_body);
    ast_add_child(program, view_decl);

    /* Canonical layout is the only layout now. Expected packed sizes and
       offsets are pinned as literals, so a layout regression fails loudly
       instead of merely disagreeing with a second implementation. */
    {
        const CobraType *point = cobra_type_struct_layout(&layout_arena, program, "Point");
        assert(point != NULL);
        assert(point->size == 16);
        assert(point->fields[0].offset == 0);
        assert(point->fields[1].offset == 8);
        assert(point->field_count == 2);

        const CobraType *pair = cobra_type_struct_layout(&layout_arena, program, "Pair");
        assert(pair != NULL);
        assert(pair->size == 16);
        assert(pair->fields[0].offset == 0); /* f32 at 0 */
        assert(pair->fields[1].offset == 4); /* i64 directly after f32, packed */

        const CobraType *box = cobra_type_struct_layout(&layout_arena, program, "Box");
        assert(box != NULL);
        assert(box->size == 32);
        assert(box->fields[0].offset == 0);  /* top: Point */
        assert(box->fields[1].offset == 16); /* bottom: Point */
        assert(box->field_count == 2);

        const CobraType *state = cobra_type_struct_layout(&layout_arena, program, "State");
        assert(state != NULL);
        assert(state->size == 16);
        assert(state->fields[0].offset == 0); /* phase: enum promoted */
        assert(state->fields[1].offset == 8); /* level */

        const CobraType *view = cobra_type_struct_layout(&layout_arena, program, "RequestView");
        assert(view != NULL);
        assert(view->size == 16);
        assert(view->fields[0].offset == 0); /* body: readonly []u8 */
    }

    /* Ownership lowering contract: a readonly param maps to a borrowed
       readonly canonical type, an out param to out mutability, and a borrowed
       struct field keeps its ownership/mutability/region origin in the
       canonical descriptor. */
    ASTNode *ro_param = ast_create_node(AST_PARAM, "src");
    ro_param->declared_type = COBRA_TYPE_SLICE_F32;
    const CobraType *f32_component = cobra_type_make(&layout_arena, COBRA_TYPE_F32, NULL,
                                                     NULL, NULL, NULL, NULL,
                                                     COBRA_OWNERSHIP_VALUE,
                                                     COBRA_MUTABILITY_DEFAULT, -1);
    const CobraType *ro_type = cobra_type_make(&layout_arena, COBRA_TYPE_SLICE_F32, NULL,
                                               f32_component, NULL, NULL, NULL,
                                               COBRA_OWNERSHIP_BORROWED,
                                               COBRA_MUTABILITY_READONLY, -1);
    assert(ro_type != NULL);
    assert(ro_type->ownership == COBRA_OWNERSHIP_BORROWED);
    assert(ro_type->mutability == COBRA_MUTABILITY_READONLY);

    ASTNode *out_param = ast_create_node(AST_PARAM, "dst");
    out_param->declared_type = COBRA_TYPE_SLICE_F32;
    const CobraType *out_type = cobra_type_make(&layout_arena, COBRA_TYPE_SLICE_F32, NULL,
                                                f32_component, NULL, NULL, NULL,
                                                COBRA_OWNERSHIP_VALUE,
                                                COBRA_MUTABILITY_OUT, -1);
    assert(out_type != NULL);
    assert(out_type->mutability == COBRA_MUTABILITY_OUT);
    assert(out_type->ownership == COBRA_OWNERSHIP_VALUE);

    assert(cobra_type_struct_layout(&layout_arena, program, "RequestView")->fields[0].mutability == COBRA_MUTABILITY_READONLY);
    assert(cobra_type_struct_layout(&layout_arena, program, "RequestView")->fields[0].ownership == COBRA_OWNERSHIP_BORROWED);

    /* Qualified views never compare equal to owned or mutable values: the
       identity key includes ownership, mutability, and region origin, so a
       readonly or out slice is structurally distinct from a plain slice. */
    assert(!cobra_type_equal(slice, ro_type));
    assert(!cobra_type_equal(slice, out_type));
    assert(!cobra_type_equal(ro_type, out_type));
    const CobraType *ro_type_again = cobra_type_make(&layout_arena, COBRA_TYPE_SLICE_F32, NULL,
                                                      f32_component, NULL, NULL, NULL,
                                                      COBRA_OWNERSHIP_BORROWED,
                                                      COBRA_MUTABILITY_READONLY, -1);
    assert(cobra_type_equal(ro_type, ro_type_again));

    /* Interning collision guard: reusing a finalized name with a different
       field shape must be rejected, never silently merged. */
    static CobraTypeArena collision_arena;
    cobra_type_arena_init(&collision_arena);
    ASTNode *prog_a = ast_create_node(AST_PROGRAM, "A");
    ASTNode *point_a = ast_create_node(AST_STRUCT_DECL, "Point");
    ASTNode *pa_x = ast_create_node(AST_PARAM, "x");
    pa_x->declared_type = COBRA_TYPE_I64;
    pa_x->canonical_type = cobra_type_make(&collision_arena, COBRA_TYPE_I64, NULL,
                                           NULL, NULL, NULL, NULL,
                                           COBRA_OWNERSHIP_VALUE,
                                           COBRA_MUTABILITY_DEFAULT, -1);
    ASTNode *pa_y = ast_create_node(AST_PARAM, "y");
    pa_y->declared_type = COBRA_TYPE_I64;
    pa_y->canonical_type = cobra_type_make(&collision_arena, COBRA_TYPE_I64, NULL,
                                           NULL, NULL, NULL, NULL,
                                           COBRA_OWNERSHIP_VALUE,
                                           COBRA_MUTABILITY_DEFAULT, -1);
    ast_add_child(point_a, pa_x);
    ast_add_child(point_a, pa_y);
    ast_add_child(prog_a, point_a);
    ASTNode *prog_b = ast_create_node(AST_PROGRAM, "B");
    ASTNode *point_b = ast_create_node(AST_STRUCT_DECL, "Point");
    ASTNode *pb_z = ast_create_node(AST_PARAM, "z");
    pb_z->declared_type = COBRA_TYPE_I64;
    pb_z->canonical_type = cobra_type_make(&collision_arena, COBRA_TYPE_I64, NULL,
                                           NULL, NULL, NULL, NULL,
                                           COBRA_OWNERSHIP_VALUE,
                                           COBRA_MUTABILITY_DEFAULT, -1);
    ast_add_child(point_b, pb_z);
    ast_add_child(prog_b, point_b);
    assert(cobra_type_struct_layout(&collision_arena, prog_a, "Point") != NULL);
    assert(cobra_type_struct_layout(&collision_arena, prog_b, "Point") == NULL);
    assert(strstr(collision_arena.error, "interning collision") != NULL);

    printf("canonical-type-ok: identity, generics, layout, ownership, ABI, cycle rejection, literal layout regression\n");
    return 0;
}
