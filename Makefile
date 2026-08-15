CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -O2
# -MMD -MP tracks header dependencies so touching include/cobra.h rebuilds every object
DEPFLAGS = -MMD -MP
SRCS = src/lexer.c src/ast.c src/interpreter.c src/parser.c src/ir.c src/type.c src/codegen.c src/gpu_lower.c src/main.c
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)
TARGET = cobra
TYPE_TEST_TARGET = cobra-type-tests
BIR_TEST_TARGET = cobra-backend-ir-tests

# Isolated backend-IR foundation (docs/BACKEND_IR.md). Linked into the
# production compiler behind `--backend=native` (src/main.c); the direct
# emitter (src/codegen.c) remains the default.
BIR_SRCS = src/backend_ir/ssa.c src/backend_ir/hir.c src/backend_ir/ssa_pass.c \
           src/backend_ir/verify.c src/backend_ir/print.c src/backend_ir/eval.c \
           src/backend_ir/mir.c src/backend_ir/alloc.c src/backend_ir/x86_64.c \
           src/backend_ir/x86_64_alloc.c src/backend_ir/elf64.c \
           src/backend_ir/x86_64_obj.c src/backend_ir/driver.c
BIR_OBJS = $(BIR_SRCS:.c=.o)

# --- Cobra Release Packaging Rule ---
VERSION = 1.0.0
DIST_NAME = cobra-v$(VERSION)-linux-x86_64
DIST_DIR = dist/$(DIST_NAME)
PROFILE_SOURCE ?= benchmarks/gemm_repeat.cb

all: $(TARGET)

$(TARGET): $(OBJS) $(BIR_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

src/backend_ir/%.o: src/backend_ir/%.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

-include $(DEPS)
-include $(BIR_OBJS:.o=.d)

dist: all
	@echo "[dist] Packaging Cobra v$(VERSION) release archive..."
	@mkdir -p $(DIST_DIR)/bin $(DIST_DIR)/lib $(DIST_DIR)/docs
	@cp ./cobra $(DIST_DIR)/bin/
	@strip --strip-unneeded $(DIST_DIR)/bin/cobra
	@cp ./lib/std.cb ./lib/nn.cb ./lib/fs.cb ./lib/time.cb ./lib/mem.cb ./lib/cpu.cb ./runtime/cobra_parallel.c ./runtime/cobra_collections.c $(DIST_DIR)/lib/
	@cp ./install.sh $(DIST_DIR)/
	@cp ./README.md ./RELEASE_NOTES.md $(DIST_DIR)/docs/ 2>/dev/null || true
	@tar -czvf $(DIST_NAME).tar.gz -C dist $(DIST_NAME)
	@echo "[dist] Distribution package created: $(DIST_NAME).tar.gz"

type-tests: $(TYPE_TEST_TARGET)
	./$(TYPE_TEST_TARGET)

$(TYPE_TEST_TARGET): tests/canonical_type.c src/type.o src/ast.o
	$(CC) $(CFLAGS) -o $@ $^

# Backend-IR unit + differential tests (tests/backend_ir.c). The test binary
# links the real lexer/parser/type/interpreter so differential cases parse
# actual Cobra subset source and compare against the host interpreter.
backend-ir-tests: $(BIR_TEST_TARGET)
	./$(BIR_TEST_TARGET)

backend-native-tests: backend-ir-tests tests/backend_native_runner.c
	@set -e; tmp=$$(mktemp -d "$${TMPDIR:-/tmp}/cobra-backend-native.XXXXXX"); \
	trap 'rm -rf "$$tmp"' EXIT; \
	BIR_X86_OWNED_ALLOC_ASM_OUT="$$tmp/allocated.s" \
	BIR_X86_OWNED_SPILL_ASM_OUT="$$tmp/spill.s" \
	BIR_X86_GRID_ALLOC_ASM_OUT="$$tmp/grid-allocated.s" \
	BIR_X86_GRID_SPILL_ASM_OUT="$$tmp/grid-spill.s" \
	BIR_X86_NESTED_ALLOC_ASM_OUT="$$tmp/nested-allocated.s" \
	BIR_X86_NESTED_SPILL_ASM_OUT="$$tmp/nested-spill.s" \
	BIR_X86_STRUCT_ARR_ALLOC_ASM_OUT="$$tmp/structarr-allocated.s" \
	BIR_X86_STRUCT_ARR_SPILL_ASM_OUT="$$tmp/structarr-spill.s" \
	BIR_X86_STRUCT_BUF_ALLOC_ASM_OUT="$$tmp/structbuf-allocated.s" \
	BIR_X86_STRUCT_BUF_SPILL_ASM_OUT="$$tmp/structbuf-spill.s" \
	BIR_X86_DICT_ALLOC_ASM_OUT="$$tmp/dict-allocated.s" \
	BIR_X86_DICT_SPILL_ASM_OUT="$$tmp/dict-spill.s" \
	BIR_X86_SUM_STRUCT_ALLOC_ASM_OUT="$$tmp/sumstruct-allocated.s" \
	BIR_X86_SUM_STRUCT_SPILL_ASM_OUT="$$tmp/sumstruct-spill.s" \
	BIR_X86_OWNING_SUM_ALLOC_ASM_OUT="$$tmp/owningsum-allocated.s" \
	BIR_X86_OWNING_SUM_SPILL_ASM_OUT="$$tmp/owningsum-spill.s" \
	BIR_X86_NESTED_STRUCT_ALLOC_ASM_OUT="$$tmp/nestedstruct-allocated.s" \
	BIR_X86_NESTED_STRUCT_SPILL_ASM_OUT="$$tmp/nestedstruct-spill.s" \
	BIR_X86_SUM_MATCH_ALLOC_ASM_OUT="$$tmp/summatch-allocated.s" \
	BIR_X86_SUM_MATCH_SPILL_ASM_OUT="$$tmp/summatch-spill.s" \
	BIR_X86_ENUM_PAYLOAD_ALLOC_ASM_OUT="$$tmp/enumpayload-allocated.s" \
	BIR_X86_ENUM_PAYLOAD_SPILL_ASM_OUT="$$tmp/enumpayload-spill.s" \
	BIR_X86_OWNING_ENUM_ALLOC_ASM_OUT="$$tmp/owningenum-allocated.s" \
	BIR_X86_OWNING_ENUM_SPILL_ASM_OUT="$$tmp/owningenum-spill.s" \
	BIR_X86_CALLEE_SAVED_ALLOC_ASM_OUT="$$tmp/calleesaved-allocated.s" \
	BIR_X86_CALLEE_SAVED_SPILL_ASM_OUT="$$tmp/calleesaved-spill.s" \
	BIR_X86_OBJECT_OUT="$$tmp/object.o" \
	BIR_X86_OBJECT2_OUT="$$tmp/object2.o" \
	BIR_X86_OBJECT3_OUT="$$tmp/object3.o" \
	BIR_X86_OBJECT4_OUT="$$tmp/object4.o" \
	./$(BIR_TEST_TARGET) >/dev/null; \
	$(CC) -no-pie -o "$$tmp/grid-allocated" "$$tmp/grid-allocated.s" tests/backend_grid_runner.c; \
	"$$tmp/grid-allocated"; \
	$(CC) -no-pie -o "$$tmp/grid-spill" "$$tmp/grid-spill.s" tests/backend_grid_runner.c; \
	"$$tmp/grid-spill"; \
	$(CC) -no-pie -o "$$tmp/nested-allocated" "$$tmp/nested-allocated.s" tests/backend_nested_runner.c; \
	"$$tmp/nested-allocated"; \
	$(CC) -no-pie -o "$$tmp/nested-spill" "$$tmp/nested-spill.s" tests/backend_nested_runner.c; \
	"$$tmp/nested-spill"; \
	$(CC) -no-pie -o "$$tmp/structarr-allocated" "$$tmp/structarr-allocated.s" tests/backend_structarr_runner.c; \
	"$$tmp/structarr-allocated"; \
	$(CC) -no-pie -o "$$tmp/structarr-spill" "$$tmp/structarr-spill.s" tests/backend_structarr_runner.c; \
	"$$tmp/structarr-spill"; \
	$(CC) -no-pie -o "$$tmp/structbuf-allocated" "$$tmp/structbuf-allocated.s" tests/backend_structbuf_runner.c; \
	"$$tmp/structbuf-allocated"; \
	$(CC) -no-pie -o "$$tmp/structbuf-spill" "$$tmp/structbuf-spill.s" tests/backend_structbuf_runner.c; \
	"$$tmp/structbuf-spill"; \
	$(CC) -no-pie -o "$$tmp/dict-allocated" "$$tmp/dict-allocated.s" tests/backend_dict_runner.c runtime/cobra_collections.c; \
	"$$tmp/dict-allocated"; \
	$(CC) -no-pie -o "$$tmp/dict-spill" "$$tmp/dict-spill.s" tests/backend_dict_runner.c runtime/cobra_collections.c; \
	"$$tmp/dict-spill"; \
	$(CC) -no-pie -o "$$tmp/sumstruct-allocated" "$$tmp/sumstruct-allocated.s" tests/backend_sum_struct_runner.c; \
	"$$tmp/sumstruct-allocated"; \
	$(CC) -no-pie -o "$$tmp/sumstruct-spill" "$$tmp/sumstruct-spill.s" tests/backend_sum_struct_runner.c; \
	"$$tmp/sumstruct-spill"; \
	$(CC) -no-pie -o "$$tmp/owningsum-allocated" "$$tmp/owningsum-allocated.s" tests/backend_owning_sum_runner.c; \
	"$$tmp/owningsum-allocated"; \
	$(CC) -no-pie -o "$$tmp/owningsum-spill" "$$tmp/owningsum-spill.s" tests/backend_owning_sum_runner.c; \
	"$$tmp/owningsum-spill"; \
	$(CC) -no-pie -o "$$tmp/nestedstruct-allocated" "$$tmp/nestedstruct-allocated.s" tests/backend_nested_struct_runner.c; \
	"$$tmp/nestedstruct-allocated"; \
	$(CC) -no-pie -o "$$tmp/nestedstruct-spill" "$$tmp/nestedstruct-spill.s" tests/backend_nested_struct_runner.c; \
	"$$tmp/nestedstruct-spill"; \
	$(CC) -no-pie -o "$$tmp/summatch-allocated" "$$tmp/summatch-allocated.s" tests/backend_sum_match_runner.c; \
	"$$tmp/summatch-allocated"; \
	$(CC) -no-pie -o "$$tmp/summatch-spill" "$$tmp/summatch-spill.s" tests/backend_sum_match_runner.c; \
	"$$tmp/summatch-spill"; \
	$(CC) -no-pie -o "$$tmp/enumpayload-allocated" "$$tmp/enumpayload-allocated.s" tests/backend_enum_payload_runner.c; \
	"$$tmp/enumpayload-allocated"; \
	$(CC) -no-pie -o "$$tmp/enumpayload-spill" "$$tmp/enumpayload-spill.s" tests/backend_enum_payload_runner.c; \
	"$$tmp/enumpayload-spill"; \
	$(CC) -no-pie -o "$$tmp/owningenum-allocated" "$$tmp/owningenum-allocated.s" tests/backend_owning_enum_runner.c; \
	"$$tmp/owningenum-allocated"; \
	$(CC) -no-pie -o "$$tmp/owningenum-spill" "$$tmp/owningenum-spill.s" tests/backend_owning_enum_runner.c; \
	"$$tmp/owningenum-spill"; \
	$(CC) -no-pie -o "$$tmp/calleesaved-allocated" "$$tmp/calleesaved-allocated.s" tests/backend_callee_saved_runner.c; \
	"$$tmp/calleesaved-allocated"; \
	$(CC) -no-pie -o "$$tmp/calleesaved-spill" "$$tmp/calleesaved-spill.s" tests/backend_callee_saved_runner.c; \
	"$$tmp/calleesaved-spill"; \
	$(CC) -no-pie -o "$$tmp/object" "$$tmp/object.o" tests/backend_object_runner.c; \
	"$$tmp/object"; \
	$(CC) -no-pie -o "$$tmp/object2" "$$tmp/object2.o" tests/backend_object2_runner.c runtime/cobra_collections.c; \
	"$$tmp/object2"; \
	$(CC) -no-pie -o "$$tmp/object3" "$$tmp/object3.o" tests/backend_object3_runner.c; \
	"$$tmp/object3"; \
	$(CC) -no-pie -o "$$tmp/object4" "$$tmp/object4.o" tests/backend_object4_runner.c; \
	"$$tmp/object4"; \
	$(CC) -no-pie -o "$$tmp/allocated" "$$tmp/allocated.s" tests/backend_native_runner.c; \
	"$$tmp/allocated"; \
	$(CC) -no-pie -o "$$tmp/spill" "$$tmp/spill.s" tests/backend_native_runner.c; \
	"$$tmp/spill"

$(BIR_TEST_TARGET): tests/backend_ir.c $(BIR_OBJS) src/lexer.o src/ast.o src/type.o src/parser.o src/interpreter.o
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -rf src/*.o src/*.d src/backend_ir/*.o src/backend_ir/*.d $(TARGET) $(TYPE_TEST_TARGET) $(BIR_TEST_TARGET) output output.s *.s *.wat dist/ $(DIST_NAME).tar.gz

deploy-check:
	python3 tools/cobra_deploy.py site-check

deploy-package:
	python3 tools/cobra_deploy.py package

deploy-publish:
	python3 tools/cobra_deploy.py publish

deploy-release:
	python3 tools/cobra_deploy.py release

perf-contracts: all
	bash benchmarks/check_contracts.sh

perf-baseline: all
	python3 benchmarks/performance_baseline.py

perf-profile: all
	bash benchmarks/profile_generated.sh $(PROFILE_SOURCE)

.PHONY: all clean dist type-tests backend-ir-tests backend-native-tests deploy-check deploy-package deploy-publish deploy-release perf-contracts perf-baseline perf-profile
