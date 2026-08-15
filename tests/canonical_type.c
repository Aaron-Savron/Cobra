#include "../include/cobra.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static CobraType *substitute_one(CobraTypeArena *arena, const CobraType *template_type,
                                 const CobraType *parameter, const CobraType *argument) {
    CobraTypeBinding binding = {parameter, argument};
    return cobra_type_substitute(arena, template_type, &binding, 1, NULL);
}

static CobraType *substitute_one_named(CobraTypeArena *arena, const CobraType *template_type,
                                       const CobraType *parameter, const CobraType *argument,
                                       const char *specialized_name) {
    CobraTypeBinding binding = {parameter, argument};
    return cobra_type_substitute(arena, template_type, &binding, 1, specialized_name);
}

int main(void) {
    static CobraTypeArena arena;
    cobra_type_arena_init(&arena);

    CobraType *i32 = cobra_type_new(&arena, COBRA_TYPE_I32);
    CobraType *i64 = cobra_type_new(&arena, COBRA_TYPE_I64);
    CobraType *f32 = cobra_type_new(&arena, COBRA_TYPE_F32);
    assert(i32 && i64 && f32);

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

    /* Nested scalar sums: an aggregate component occupies its real canonical
       size inside the enclosing sum, not the fixed scalar slot. */
    CobraType *inner_option = cobra_type_new(&arena, COBRA_TYPE_OPTION);
    assert(inner_option);
    assert(cobra_type_add_generic_arg(inner_option, i64));
    assert(cobra_type_validate(&arena, inner_option));
    assert(inner_option->size ==
           COBRA_NATIVE_SUM_TAG_SIZE + COBRA_NATIVE_SUM_SCALAR_SIZE);
    CobraType *outer_option = cobra_type_new(&arena, COBRA_TYPE_OPTION);
    assert(outer_option);
    assert(cobra_type_add_generic_arg(outer_option, inner_option));
    assert(cobra_type_validate(&arena, outer_option));
    assert(outer_option->size == COBRA_NATIVE_SUM_TAG_SIZE + inner_option->size);
    CobraType *nested_result = cobra_type_new(&arena, COBRA_TYPE_RESULT);
    assert(nested_result);
    assert(cobra_type_add_generic_arg(nested_result, inner_option));
    assert(cobra_type_add_generic_arg(nested_result, i64));
    assert(cobra_type_validate(&arena, nested_result));
    assert(nested_result->size ==
           COBRA_NATIVE_SUM_TAG_SIZE + inner_option->size +
           COBRA_NATIVE_SUM_SCALAR_SIZE);

    /* Narrow generic-instantiation milestone: one canonical placeholder can
       specialize scalar Option and Result descriptors. Instantiation finalizes
       the ABI and interns equivalent results in the same arena. */
    CobraType *parameter = cobra_type_make(&arena, COBRA_TYPE_GENERIC_PARAM, "T",
                                           NULL, NULL, NULL, NULL,
                                           COBRA_OWNERSHIP_VALUE,
                                           COBRA_MUTABILITY_DEFAULT, -1);
    CobraType *option_template = cobra_type_make(&arena, COBRA_TYPE_OPTION, NULL,
                                                 parameter, NULL, NULL, NULL,
                                                 COBRA_OWNERSHIP_VALUE,
                                                 COBRA_MUTABILITY_DEFAULT, -1);
    assert(parameter && option_template);
    CobraType *instantiated_option = substitute_one(&arena, option_template,
                                                             parameter, i64);
    CobraType *same_option = substitute_one(&arena, option_template,
                                                    parameter, i64);
    assert(instantiated_option && same_option);
    assert(instantiated_option == same_option);
    assert(instantiated_option->finalized);
    assert(cobra_type_element(instantiated_option) == i64);
    assert(instantiated_option->abi == COBRA_ABI_SUM_INDIRECT);
    assert(!cobra_type_add_generic_arg(instantiated_option, f32));

    CobraType *result_template = cobra_type_make(&arena, COBRA_TYPE_RESULT, NULL,
                                                 parameter, i32, NULL, NULL,
                                                 COBRA_OWNERSHIP_VALUE,
                                                 COBRA_MUTABILITY_DEFAULT, -1);
    assert(result_template);
    CobraType *instantiated_result = substitute_one(&arena, result_template,
                                                            parameter, f32);
    assert(instantiated_result && instantiated_result->finalized);
    assert(cobra_type_element(instantiated_result)->kind == COBRA_TYPE_F32);
    assert(cobra_type_error(instantiated_result) == i32);
    assert(instantiated_result->abi == COBRA_ABI_SUM_INDIRECT);

    /* Scalar-only generic structs substitute fields before finalization and
       intern repeated specializations by their complete canonical shape. */
    static CobraTypeArena generic_struct_arena;
    cobra_type_arena_init(&generic_struct_arena);
    CobraType *struct_parameter = cobra_type_make(&generic_struct_arena,
                                                  COBRA_TYPE_GENERIC_PARAM, "T",
                                                  NULL, NULL, NULL, NULL,
                                                  COBRA_OWNERSHIP_VALUE,
                                                  COBRA_MUTABILITY_DEFAULT, -1);
    CobraType *box_template = cobra_type_named(&generic_struct_arena,
                                                COBRA_TYPE_STRUCT, "Box");
    assert(struct_parameter && box_template);
    assert(cobra_type_add_generic_arg(box_template, struct_parameter));
    assert(cobra_type_add_field(box_template, "value", struct_parameter,
                                COBRA_OWNERSHIP_VALUE,
                                COBRA_MUTABILITY_DEFAULT, -1));
    CobraType *box_i64 = substitute_one_named(&generic_struct_arena,
                                                       box_template, struct_parameter,
                                                       i64, "Box__i64");
    CobraType *same_box_i64 = substitute_one_named(&generic_struct_arena,
                                                            box_template, struct_parameter,
                                                            i64, "Box__i64");
    CobraType *box_f32 = substitute_one_named(&generic_struct_arena,
                                                       box_template, struct_parameter,
                                                       f32, "Box__f32");
    assert(box_i64 && same_box_i64 && box_f32);
    assert(box_i64 == same_box_i64);
    assert(box_i64 != box_f32);
    assert(box_i64->finalized && box_i64->abi == COBRA_ABI_STRUCT_VALUE);
    assert(box_i64->fields[0].type == i64);
    assert(box_i64->fields[0].offset == 0 && box_i64->size == 8);
    assert(box_f32->fields[0].type == f32);
    assert(box_f32->fields[0].offset == 0 && box_f32->size == 8);
    assert(!cobra_type_add_field(box_i64, "extra", i64,
                                 COBRA_OWNERSHIP_VALUE,
                                 COBRA_MUTABILITY_DEFAULT, -1));

    /* Nested generic composition reuses the same recursive substitution and
       remains independent of the generated specialization spelling. */
    CobraType *outer_template = cobra_type_named(&generic_struct_arena,
                                                  COBRA_TYPE_STRUCT, "Outer");
    assert(outer_template);
    assert(cobra_type_add_generic_arg(outer_template, struct_parameter));
    assert(cobra_type_add_field(outer_template, "inner", box_template,
                                COBRA_OWNERSHIP_VALUE,
                                COBRA_MUTABILITY_DEFAULT, -1));
    CobraType *outer_i64 = substitute_one_named(&generic_struct_arena,
                                                         outer_template, struct_parameter,
                                                         i64, "Outer__i64");
    CobraType *outer_i64_alias = substitute_one_named(&generic_struct_arena,
                                                               outer_template, struct_parameter,
                                                               i64, "Outer_spelling_independent");
    assert(outer_i64 && outer_i64_alias && outer_i64 == outer_i64_alias);
    assert(outer_i64->fields[0].type == box_i64);
    assert(outer_i64->fields[0].offset == 0 && outer_i64->size == 8);
    assert(outer_i64->template_origin == outer_template);

    /* Generic borrowed-field structs substitute the element while preserving
       the field's borrowed/readonly contract and two-word slice layout. */
    CobraType *view_parameter = cobra_type_make(&generic_struct_arena,
                                                COBRA_TYPE_GENERIC_PARAM, "V",
                                                NULL, NULL, NULL, NULL,
                                                COBRA_OWNERSHIP_VALUE,
                                                COBRA_MUTABILITY_DEFAULT, -1);
    CobraType *view_template = cobra_type_named(&generic_struct_arena,
                                                COBRA_TYPE_STRUCT, "View");
    CobraType *view_slice = cobra_type_make(&generic_struct_arena,
                                            COBRA_TYPE_SLICE, NULL,
                                            view_parameter, NULL, NULL, NULL,
                                            COBRA_OWNERSHIP_BORROWED,
                                            COBRA_MUTABILITY_READONLY, -1);
    assert(view_parameter && view_template && view_slice);
    assert(cobra_type_add_generic_arg(view_template, view_parameter));
    assert(cobra_type_add_field(view_template, "data", view_slice,
                                COBRA_OWNERSHIP_BORROWED,
                                COBRA_MUTABILITY_READONLY, -1));
    CobraType *view_i64 = substitute_one_named(&generic_struct_arena,
                                                        view_template, view_parameter,
                                                        i64, "View__i64");
    CobraType *same_view_i64 = substitute_one_named(&generic_struct_arena,
                                                             view_template, view_parameter,
                                                             i64, "View__i64");
    assert(view_i64 && same_view_i64 && view_i64 == same_view_i64);
    assert(view_i64->abi == COBRA_ABI_STRUCT_VALUE && view_i64->size == 16);
    assert(view_i64->fields[0].offset == 0);
    assert(view_i64->fields[0].type->kind == COBRA_TYPE_SLICE);
    assert(cobra_type_element(view_i64->fields[0].type)->kind == COBRA_TYPE_I64);
    assert(view_i64->fields[0].ownership == COBRA_OWNERSHIP_BORROWED);
    assert(view_i64->fields[0].mutability == COBRA_MUTABILITY_READONLY);
    assert(view_i64->fields[0].region_id == -1);

    /* A borrowed View[T] nested inside another by-value generic struct is
       intentionally outside this frozen ownership contract. It must fail at
       canonical substitution rather than silently losing the view owner. */
    CobraType *view_holder = cobra_type_named(&generic_struct_arena,
                                               COBRA_TYPE_STRUCT, "ViewHolder");
    assert(view_holder);
    assert(cobra_type_add_generic_arg(view_holder, view_parameter));
    assert(cobra_type_add_field(view_holder, "view", view_template,
                                COBRA_OWNERSHIP_VALUE,
                                COBRA_MUTABILITY_DEFAULT, -1));
    CobraTypeBinding view_holder_binding = {view_parameter, i64};
    assert(!cobra_type_substitute(&generic_struct_arena, view_holder,
                                  &view_holder_binding, 1, "ViewHolder__i64"));
    assert(strstr(generic_struct_arena.error, "unsupported ownership or ABI contract") != NULL);

    /* Readonly generic slices keep the borrowed pointer-plus-length ABI while
       selecting an element-specific storage kind during substitution. */
    CobraType *readonly_slice_template = cobra_type_make(
        &arena, COBRA_TYPE_SLICE, NULL, parameter, NULL, NULL, NULL,
        COBRA_OWNERSHIP_BORROWED, COBRA_MUTABILITY_READONLY, -1);
    assert(readonly_slice_template);
    CobraType *readonly_f32 = substitute_one(&arena,
                                                     readonly_slice_template,
                                                     parameter, f32);
    CobraType *same_readonly_f32 = substitute_one(&arena,
                                                          readonly_slice_template,
                                                          parameter, f32);
    assert(readonly_f32 && same_readonly_f32);
    assert(readonly_f32 == same_readonly_f32);
    assert(readonly_f32->kind == COBRA_TYPE_SLICE_F32);
    assert(cobra_type_element(readonly_f32) == f32);
    assert(readonly_f32->ownership == COBRA_OWNERSHIP_BORROWED);
    assert(readonly_f32->mutability == COBRA_MUTABILITY_READONLY);
    assert(readonly_f32->abi == COBRA_ABI_SLICE);
    assert(readonly_f32->size == 16);

    static CobraTypeArena generic_negative;
    cobra_type_arena_init(&generic_negative);
    CobraType *negative_param = cobra_type_make(&generic_negative,
                                                COBRA_TYPE_GENERIC_PARAM, "T",
                                                NULL, NULL, NULL, NULL,
                                                COBRA_OWNERSHIP_VALUE,
                                                COBRA_MUTABILITY_DEFAULT, -1);
    CobraType *negative_template = cobra_type_make(&generic_negative,
                                                   COBRA_TYPE_OPTION, NULL,
                                                   negative_param, NULL, NULL, NULL,
                                                   COBRA_OWNERSHIP_VALUE,
                                                   COBRA_MUTABILITY_DEFAULT, -1);
    CobraType *list_argument = cobra_type_make(&generic_negative, COBRA_TYPE_LIST, NULL,
                                               i64, NULL, NULL, NULL,
                                               COBRA_OWNERSHIP_VALUE,
                                               COBRA_MUTABILITY_DEFAULT, -1);
    assert(!substitute_one(&generic_negative, negative_template,
                            negative_param, list_argument));
    assert(strstr(generic_negative.error, "scalar argument") != NULL);

    /* The public substitution operation intentionally rejects multiple
       bindings until multi-parameter specialization has a complete identity,
       naming, and ABI contract. */
    CobraType *second_param = cobra_type_make(&generic_negative,
                                              COBRA_TYPE_GENERIC_PARAM, "U",
                                              NULL, NULL, NULL, NULL,
                                              COBRA_OWNERSHIP_VALUE,
                                              COBRA_MUTABILITY_DEFAULT, -1);
    CobraTypeBinding multiple_bindings[2] = {
        {negative_param, i64}, {second_param, f32}
    };
    assert(second_param);
    assert(!cobra_type_substitute(&generic_negative, negative_template,
                                  multiple_bindings, 2, NULL));
    assert(strstr(generic_negative.error, "exactly one binding") != NULL);

    /* A generic specialization must detect by-value recursive templates before
       allocating an unbounded chain of generated descriptors. */
    static CobraTypeArena specialization_cycle;
    cobra_type_arena_init(&specialization_cycle);
    CobraType *cycle_param = cobra_type_make(&specialization_cycle,
                                             COBRA_TYPE_GENERIC_PARAM, "T",
                                             NULL, NULL, NULL, NULL,
                                             COBRA_OWNERSHIP_VALUE,
                                             COBRA_MUTABILITY_DEFAULT, -1);
    CobraType *cycle_template = cobra_type_named(&specialization_cycle,
                                                  COBRA_TYPE_STRUCT, "Recursive");
    assert(cycle_param && cycle_template);
    assert(cobra_type_add_generic_arg(cycle_template, cycle_param));
    assert(cobra_type_add_field(cycle_template, "next", cycle_template,
                                COBRA_OWNERSHIP_VALUE,
                                COBRA_MUTABILITY_DEFAULT, -1));
    CobraTypeBinding cycle_binding = {cycle_param, i64};
    assert(!cobra_type_substitute(&specialization_cycle, cycle_template,
                                  &cycle_binding, 1, "Recursive__i64"));
    assert(strstr(specialization_cycle.error, "recursive generic specialization") != NULL);

    /* Canonical arenas fail closed rather than returning a descriptor outside
       their fixed storage contract. */
    static CobraTypeArena exhausted;
    cobra_type_arena_init(&exhausted);
    for (size_t i = 0; i < COBRA_MAX_TYPE_NODES; i++)
        assert(cobra_type_new(&exhausted, COBRA_TYPE_I64) != NULL);
    assert(cobra_type_new(&exhausted, COBRA_TYPE_I64) == NULL);

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
