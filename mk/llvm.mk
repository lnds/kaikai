# LLVM static prep — factored out of the root Makefile so the prebuilt
# libLLVM asset is named after this file and scripts/llvm-prebuilt.sh,
# nothing else. Editing a test target in the root Makefile must not name a
# new asset and force a ~25-min libLLVM build; only a real change here
# (version, cmake flags, target list) should. Keep everything that governs
# the libLLVM build in this file.

.PHONY: llvm-info llvm-fetch llvm-configure llvm-build llvm-size llvm-clean \
        llvm-prebuilt llvm-pack

# ---- LLVM static prep (libLLVM for the in-process native backend) ----
#
# These targets prepare an out-of-tree, statically-linked libLLVM that
# the in-process `native` backend links stage2 against (built with
# `make -C stage2 KAI_LLVM=1`): it constructs the LLVM module via the C
# API in-process and emits a native object — no external `clang`, no
# `.ll` text. None of these run from `all`, `tier0`, or `tier1` —
# invocation is explicit. The components below are the libLLVM surface
# the C-API path needs (codegen + target + object writer + linker).
#
# See docs/lane-experience-l0-llvm-static-prep.md for the full retro,
# size measurements, and CI plan.

LLVM_VERSION ?= 22.1.8
LLVM_THIRD_PARTY := stage0/third_party
LLVM_SRC_DIR := $(LLVM_THIRD_PARTY)/llvm
LLVM_BUILD_DIR := $(LLVM_SRC_DIR)/build
# LLVM publishes one source tarball for the whole monorepo. The llvm tree
# reaches its siblings by relative path (`../cmake`, `../third-party`), so
# those three are extracted side by side and nothing else is.
LLVM_TARBALL_ROOT := llvm-project-$(LLVM_VERSION).src
LLVM_TARBALL := $(LLVM_THIRD_PARTY)/$(LLVM_TARBALL_ROOT).tar.xz
LLVM_TARBALL_URL := https://github.com/llvm/llvm-project/releases/download/llvmorg-$(LLVM_VERSION)/$(LLVM_TARBALL_ROOT).tar.xz
LLVM_TARBALL_MEMBERS := llvm cmake third-party
LLVM_CMAKE_DIR := $(LLVM_THIRD_PARTY)/cmake
# The version the extracted source is: a tree left by an earlier pin would
# otherwise build under the new pin's name.
LLVM_SOURCE_STAMP := $(LLVM_SRC_DIR)/.source-version
# CMake targets we actually need (parse text IR + emit x86-64 + arm64
# objects). Kept narrow on purpose; expanding this list expands the
# final static-link footprint roughly linearly.
LLVM_CMAKE_TARGETS := LLVMCore LLVMSupport LLVMIRReader LLVMAsmParser \
                      LLVMBitReader LLVMBitWriter \
                      LLVMTarget LLVMTargetParser LLVMMC LLVMMCParser \
                      LLVMObject LLVMOption LLVMBinaryFormat \
                      LLVMX86CodeGen LLVMX86AsmParser LLVMX86Desc LLVMX86Info \
                      LLVMAArch64CodeGen LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info \
                      LLVMCodeGen LLVMAnalysis LLVMTransformUtils LLVMScalarOpts \
                      LLVMSelectionDAG LLVMGlobalISel LLVMAsmPrinter \
                      LLVMipo LLVMInstCombine LLVMInstrumentation \
                      LLVMVectorize LLVMLinker LLVMPasses \
                      LLVMDemangle LLVMRemarks LLVMDebugInfoDWARF LLVMDebugInfoCodeView \
                      llvm-config

# llvm-info: prints what would be downloaded / built, without doing it.
# Safe to run on any machine.
llvm-info:
	@echo "LLVM_VERSION    = $(LLVM_VERSION)"
	@echo "LLVM_SRC_DIR    = $(LLVM_SRC_DIR)"
	@echo "LLVM_BUILD_DIR  = $(LLVM_BUILD_DIR)"
	@echo "LLVM_TARBALL    = $(LLVM_TARBALL)"
	@echo "LLVM_TARBALL_URL= $(LLVM_TARBALL_URL)"
	@echo "TARGETS         = X86 + AArch64 (MinSizeRel, no zlib/zstd/libxml2)"
	@echo "CMAKE_TARGETS   = $(words $(LLVM_CMAKE_TARGETS)) libraries"
	@echo "Disk required   = ~3.5 GB build tree, ~150-300 MB sum-of-.a (L3 measurement)"
	@echo "First build     = ~10-30 min on a modern laptop, ~30-60 min on cold CI"
	@echo ""
	@echo "Workflow:"
	@echo "  make llvm-prebuilt    # fetch the published archives, else build from source"
	@echo "  make llvm-fetch       # download + extract tarball (one-shot)"
	@echo "  make llvm-configure   # cmake -B build with MinSizeRel"
	@echo "  make llvm-build       # cmake --build (the long step)"
	@echo "  make llvm-pack        # archive a finished build as the prebuilt asset"
	@echo "  make llvm-size        # sum sizes of the .a archives"
	@echo "  make llvm-clean       # remove the build tree (keep source)"

# llvm-fetch: download the LLVM source tarball and extract the trees the
# build needs under $(LLVM_THIRD_PARTY). The tarball + trees are gitignored;
# we never commit LLVM source. Re-running is a no-op when the source is
# present.
llvm-fetch:
	@mkdir -p $(LLVM_THIRD_PARTY)
	@if [ -f "$(LLVM_SRC_DIR)/CMakeLists.txt" ]; then \
	  have="$$(cat $(LLVM_SOURCE_STAMP) 2>/dev/null || echo unknown)"; \
	  if [ "$$have" = "$(LLVM_VERSION)" ]; then \
	    echo "llvm-fetch: $(LLVM_SRC_DIR) already holds LLVM $(LLVM_VERSION), skipping"; \
	    exit 0; \
	  fi; \
	  echo "llvm-fetch FAIL — $(LLVM_SRC_DIR) holds LLVM $$have, the pin is $(LLVM_VERSION); remove $(LLVM_THIRD_PARTY) and re-run"; \
	  exit 1; \
	fi; \
	if [ ! -f "$(LLVM_TARBALL)" ]; then \
	  echo "llvm-fetch: downloading $(LLVM_TARBALL_URL)"; \
	  curl -fL --retry 3 -o "$(LLVM_TARBALL).part" "$(LLVM_TARBALL_URL)" \
	    && mv "$(LLVM_TARBALL).part" "$(LLVM_TARBALL)" \
	    || { echo "llvm-fetch FAIL — download error"; rm -f "$(LLVM_TARBALL).part"; exit 1; }; \
	fi; \
	echo "llvm-fetch: extracting $(LLVM_TARBALL_MEMBERS) into $(LLVM_THIRD_PARTY)"; \
	tar -xJf "$(LLVM_TARBALL)" --strip-components=1 -C "$(LLVM_THIRD_PARTY)" \
	  $(addprefix $(LLVM_TARBALL_ROOT)/,$(LLVM_TARBALL_MEMBERS)) \
	  || { echo "llvm-fetch FAIL — extract error"; exit 1; }; \
	echo "$(LLVM_VERSION)" > $(LLVM_SOURCE_STAMP); \
	echo "llvm-fetch OK — source at $(LLVM_SRC_DIR), cmake modules at $(LLVM_CMAKE_DIR)"

# llvm-configure: run cmake. MinSizeRel + only X86 + AArch64 targets +
# disable optional features that bloat the static link (zlib, zstd,
# libxml2). Requires cmake + ninja in PATH; we don't add
# them to stage0 deps. If cmake/ninja are missing the failure is loud.
llvm-configure: llvm-fetch
	@command -v cmake >/dev/null 2>&1 || { echo "llvm-configure FAIL — cmake not in PATH"; exit 2; }
	@command -v ninja >/dev/null 2>&1 || { echo "llvm-configure FAIL — ninja not in PATH"; exit 2; }
	cd $(LLVM_SRC_DIR) && cmake -B build -G Ninja \
	  -DCMAKE_BUILD_TYPE=MinSizeRel \
	  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
	  -DLLVM_ENABLE_PROJECTS="" \
	  -DLLVM_BUILD_TOOLS=OFF \
	  -DLLVM_BUILD_UTILS=OFF \
	  -DLLVM_BUILD_EXAMPLES=OFF \
	  -DLLVM_INCLUDE_EXAMPLES=OFF \
	  -DLLVM_INCLUDE_TESTS=OFF \
	  -DLLVM_INCLUDE_BENCHMARKS=OFF \
	  -DLLVM_INCLUDE_DOCS=OFF \
	  -DLLVM_ENABLE_BACKTRACES=OFF \
	  -DLLVM_ENABLE_ZLIB=OFF \
	  -DLLVM_ENABLE_ZSTD=OFF \
	  -DLLVM_ENABLE_LIBXML2=OFF \
	  -DLLVM_ENABLE_LIBEDIT=OFF \
	  -DLLVM_ENABLE_OCAMLDOC=OFF \
	  -DLLVM_ENABLE_BINDINGS=OFF \
	  -DLLVM_ENABLE_ASSERTIONS=OFF
	@echo "llvm-configure OK — build/ ready under $(LLVM_SRC_DIR)"

# llvm-build: actually compile the static libs. This is the long step
# (10-30 min cold). The target list is narrow on purpose; expanding it
# raises the linked binary size in L3 roughly linearly. Prebuilt archives
# for this exact configuration are the same output, so they are left alone.
llvm-build:
	@if [ "$$(cat $(LLVM_BUILD_DIR)/.prebuilt 2>/dev/null)" = "$$(scripts/llvm-prebuilt.sh name)" ]; then \
	  echo "llvm-build: prebuilt archives in place under $(LLVM_BUILD_DIR), nothing to build"; \
	  exit 0; \
	fi; \
	$(MAKE) llvm-configure \
	  && (cd $(LLVM_SRC_DIR) && cmake --build build --target $(LLVM_CMAKE_TARGETS)) \
	  && echo "llvm-build OK — static .a archives under $(LLVM_BUILD_DIR)/lib" \
	  && $(MAKE) llvm-size

# llvm-prebuilt: the same archives without the compile. Fetches the asset
# published for this host and configuration; where none exists (a fork, a
# new LLVM version, an edited flag) it builds from source instead.
llvm-prebuilt:
	@scripts/llvm-prebuilt.sh fetch || { rc=$$?; [ $$rc -eq 3 ] || exit $$rc; $(MAKE) llvm-build; }

# llvm-pack: archive a finished source build into dist/ as that asset.
llvm-pack:
	@scripts/llvm-prebuilt.sh pack dist

# llvm-size: sum-of-.a measurement. The number L3 needs to estimate
# the linked-kaic2 binary size. Static link drops a lot via dead-code
# elimination, so the linked footprint is typically 30-50% of the
# sum-of-.a, not 100%.
llvm-size:
	@if [ ! -d "$(LLVM_BUILD_DIR)/lib" ]; then \
	  echo "llvm-size: no build yet, run \`make llvm-build\` first"; \
	  exit 1; \
	fi; \
	echo "Static archive sizes under $(LLVM_BUILD_DIR)/lib:"; \
	du -sh $(LLVM_BUILD_DIR)/lib/*.a 2>/dev/null | sort -h | tail -20; \
	total=$$(du -sk $(LLVM_BUILD_DIR)/lib/*.a 2>/dev/null | awk '{s+=$$1} END {print s}'); \
	echo "Sum of .a archives: $$total KB (~$$((total / 1024)) MB)"

# llvm-clean: drop the build tree, keep the unpacked source. Use this
# between configure-tuning runs. To drop everything (source + tarball)
# delete stage0/third_party/ directly.
llvm-clean:
	rm -rf $(LLVM_BUILD_DIR)
	@echo "llvm-clean OK — source tree preserved at $(LLVM_SRC_DIR)"
