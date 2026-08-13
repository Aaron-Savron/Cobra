CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -O2
# -MMD -MP tracks header dependencies so touching include/cobra.h rebuilds every object
DEPFLAGS = -MMD -MP
SRCS = src/lexer.c src/ast.c src/interpreter.c src/parser.c src/ir.c src/type.c src/codegen.c src/main.c
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)
TARGET = cobra
TYPE_TEST_TARGET = cobra-type-tests

# --- Cobra Release Packaging Rule ---
VERSION = 1.0.0
DIST_NAME = cobra-v$(VERSION)-linux-x86_64
DIST_DIR = dist/$(DIST_NAME)
PROFILE_SOURCE ?= benchmarks/gemm_repeat.cb

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

-include $(DEPS)

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

clean:
	rm -rf src/*.o src/*.d $(TARGET) $(TYPE_TEST_TARGET) output output.s *.s *.wat dist/ $(DIST_NAME).tar.gz

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

.PHONY: all clean dist type-tests deploy-check deploy-package deploy-publish deploy-release perf-contracts perf-baseline perf-profile
