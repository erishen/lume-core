# lume-llvm — Lume → AST → LLVM IR → Native
#
# Host toolchain is C11 + libc only. The LLVM IR is emitted as *text* (.ll)
# and handed to clang/cc for the native step, so this project never has to
# link libLLVM. See README.md for the rationale.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g
CFLAGS  += -Isrc -Iupstream

# The generated .ll must name the target, otherwise LLVM lowers for a generic
# target and the native binary misbehaves. Ask the host toolchain once.
TRIPLE  := $(shell $(CC) -print-target-triple 2>/dev/null)
CFLAGS  += -DTARGET_TRIPLE=\"$(TRIPLE)\"

# The runtime helpers the generated IR links against; the driver compiles and
# links this into the native binary (see --compile in src/main.c).
CFLAGS  += -DLUME_RT_SRC=\"$(CURDIR)/src/rt.c\"

BUILD    := build
BIN      := bin
TARGET   := $(BIN)/lume-llvm

# vendored frontend from ../lume (lexer / parser / typecheck + the Node+Type AST)
FRONTEND := upstream/token.c \
            upstream/lexer.c \
            upstream/parser.c \
            upstream/parser_expr.c \
            upstream/parser_stmt.c \
            upstream/typecheck.c \
            upstream/typecheck_expr.c \
            upstream/typecheck_stmt.c

# driver + LLVM IR backend
LOCAL    := src/main.c \
            src/codegen.c \
            src/irbuf.c

SRCS     := $(FRONTEND) $(LOCAL)
OBJS     := $(patsubst %.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all check dump test examples clean

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lm

# object dirs mirror the source tree (build/upstream/*.o, build/src/*.o)
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN):
	@mkdir -p $@

# --- type-check a source file against the real frontend ---------------------

check: $(TARGET)
	@for f in $(wildcard examples/*.lume); do \
	    printf '%-40s ' "$$f"; \
	    ./$(TARGET) --check "$$f" && echo OK || echo FAIL; \
	done

dump: $(TARGET)
	@[ -n "$(f)" ] || { echo "usage: make dump f=examples/foo.lume"; exit 2; }
	./$(TARGET) --dump "$(f)"

# --- end-to-end: .lume -> .ll -> native binary -------------------------------

# Every example ships an expected stdout in tests/<stem>.expected. `make test`
# diffs the compiled binary's real output against it, so a backend regression
# (a wrong GEP index, a lost alloca, a bad coerced operand) fails loudly.

# The stem is the file basename without ".lume" (matches src/main.c: stem_of()).
# basename(1) is used instead of ${src##*/} because '#' starts a make comment.
SPLIT_STEM = stem=$$(basename "$$src" .lume); 

# --- end-to-end: .lume -> .ll -> native binary, then run it -----------------

# make test [f=examples/foo.lume]
#   Build each example, run it, and diff the output against tests/<stem>.expected.
# make examples [f=examples/foo.lume]
#   Same build, but print the program's output instead of comparing it.

test: $(TARGET)
	@mkdir -p out; set -e; rc=0; \
	for src in $(if $(f),$(f),$(wildcard examples/*.lume)); do \
	    $(SPLIT_STEM) \
	    if ! ./$(TARGET) --compile "$$src" >/dev/null 2>out/$$stem.log; then \
	        printf '  FAIL %-24s build\n' "$$stem"; tail -5 out/$$stem.log; rc=1; continue; \
	    fi; \
	    ./out/$$stem >out/$$stem.out 2>&1; \
	    if diff -u tests/$$stem.expected out/$$stem.out >out/$$stem.diff; then \
	        printf '  ok   %-24s (%s lines)\n' "$$stem" "$$(wc -l < out/$$stem.out | tr -d ' ')"; \
	    else \
	        printf '  FAIL %-24s output\n' "$$stem"; cat out/$$stem.diff; rc=1; \
	    fi; \
	done; \
	[ $$rc -eq 0 ] && echo "all examples build and produce the expected output"

examples: $(TARGET)
	@mkdir -p out; set -e; \
	for src in $(if $(f),$(f),$(wildcard examples/*.lume)); do \
	    $(SPLIT_STEM) \
	    echo "===== $$src"; \
	    ./$(TARGET) --compile "$$src" >/dev/null; \
	    ./out/$$stem; \
	done

clean:
	rm -rf $(BUILD) $(BIN) out
