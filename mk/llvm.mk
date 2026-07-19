# LLVM static prep — factored out of the root Makefile so the release
# libLLVM cache key (release.yml) can hash THIS file alone. Editing a
# test target in the root Makefile must not invalidate the ~25-min
# libLLVM build cache; only a real change here (version, cmake flags,
# target list) should. Keep everything that governs the libLLVM build
# in this file.

.PHONY: llvm-info llvm-fetch llvm-configure llvm-build llvm-size llvm-clean

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

LLVM_VERSION ?= 18.1.8
LLVM_SRC_DIR := stage0/third_party/llvm
LLVM_BUILD_DIR := $(LLVM_SRC_DIR)/build
LLVM_TARBALL := stage0/third_party/llvm-$(LLVM_VERSION).src.tar.xz
LLVM_TARBALL_URL := https://github.com/llvm/llvm-project/releases/download/llvmorg-$(LLVM_VERSION)/llvm-$(LLVM_VERSION).src.tar.xz
# The llvm source tree's configure step `include`s shared CMake modules
# (ExtendPath, FindPrefixFromConfig) from `../cmake/Modules` — these ship in
# a SEPARATE sibling tarball since the monorepo split, not in llvm-*.src. We
# extract it to stage0/third_party/cmake so `../cmake` resolves at configure.
LLVM_CMAKE_DIR := stage0/third_party/cmake
LLVM_CMAKE_TARBALL := stage0/third_party/cmake-$(LLVM_VERSION).src.tar.xz
LLVM_CMAKE_TARBALL_URL := https://github.com/llvm/llvm-project/releases/download/llvmorg-$(LLVM_VERSION)/cmake-$(LLVM_VERSION).src.tar.xz
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
	@echo "TARGETS         = X86 + AArch64 (MinSizeRel, no zlib/zstd/terminfo)"
	@echo "CMAKE_TARGETS   = $(words $(LLVM_CMAKE_TARGETS)) libraries"
	@echo "Disk required   = ~3.5 GB build tree, ~150-300 MB sum-of-.a (L3 measurement)"
	@echo "First build     = ~10-30 min on a modern laptop, ~30-60 min on cold CI"
	@echo ""
	@echo "Workflow:"
	@echo "  make llvm-fetch       # download + extract tarball (one-shot)"
	@echo "  make llvm-configure   # cmake -B build with MinSizeRel"
	@echo "  make llvm-build       # cmake --build (the long step)"
	@echo "  make llvm-size        # sum sizes of the .a archives"
	@echo "  make llvm-clean       # remove the build tree (keep source)"

# llvm-fetch: download + verify the LLVM source tarball, then extract
# into $(LLVM_SRC_DIR). The tarball + tree are gitignored; we never
# commit LLVM source. Re-running is a no-op when the source is present.
llvm-fetch:
	@mkdir -p stage0/third_party
	@if [ -f "$(LLVM_SRC_DIR)/CMakeLists.txt" ] && [ -f "$(LLVM_CMAKE_DIR)/Modules/ExtendPath.cmake" ]; then \
	  echo "llvm-fetch: $(LLVM_SRC_DIR) + $(LLVM_CMAKE_DIR) already populated, skipping"; \
	  exit 0; \
	fi; \
	if [ ! -f "$(LLVM_SRC_DIR)/CMakeLists.txt" ]; then \
	  if [ ! -f "$(LLVM_TARBALL)" ]; then \
	    echo "llvm-fetch: downloading $(LLVM_TARBALL_URL)"; \
	    curl -fL -o "$(LLVM_TARBALL).part" "$(LLVM_TARBALL_URL)" \
	      && mv "$(LLVM_TARBALL).part" "$(LLVM_TARBALL)" \
	      || { echo "llvm-fetch FAIL — download error"; rm -f "$(LLVM_TARBALL).part"; exit 1; }; \
	  fi; \
	  echo "llvm-fetch: extracting tarball into $(LLVM_SRC_DIR)"; \
	  mkdir -p "$(LLVM_SRC_DIR)"; \
	  tar -xJf "$(LLVM_TARBALL)" --strip-components=1 -C "$(LLVM_SRC_DIR)" \
	    || { echo "llvm-fetch FAIL — extract error"; exit 1; }; \
	fi; \
	if [ ! -f "$(LLVM_CMAKE_DIR)/Modules/ExtendPath.cmake" ]; then \
	  if [ ! -f "$(LLVM_CMAKE_TARBALL)" ]; then \
	    echo "llvm-fetch: downloading $(LLVM_CMAKE_TARBALL_URL)"; \
	    curl -fL -o "$(LLVM_CMAKE_TARBALL).part" "$(LLVM_CMAKE_TARBALL_URL)" \
	      && mv "$(LLVM_CMAKE_TARBALL).part" "$(LLVM_CMAKE_TARBALL)" \
	      || { echo "llvm-fetch FAIL — cmake-module download error"; rm -f "$(LLVM_CMAKE_TARBALL).part"; exit 1; }; \
	  fi; \
	  echo "llvm-fetch: extracting cmake modules into $(LLVM_CMAKE_DIR)"; \
	  mkdir -p "$(LLVM_CMAKE_DIR)"; \
	  tar -xJf "$(LLVM_CMAKE_TARBALL)" --strip-components=1 -C "$(LLVM_CMAKE_DIR)" \
	    || { echo "llvm-fetch FAIL — cmake-module extract error"; exit 1; }; \
	fi; \
	echo "llvm-fetch OK — source at $(LLVM_SRC_DIR), cmake modules at $(LLVM_CMAKE_DIR)"

# llvm-configure: run cmake. MinSizeRel + only X86 + AArch64 targets +
# disable optional features that bloat the static link (zlib, zstd,
# terminfo, libxml2). Requires cmake + ninja in PATH; we don't add
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
	  -DLLVM_ENABLE_TERMINFO=OFF \
	  -DLLVM_ENABLE_LIBXML2=OFF \
	  -DLLVM_ENABLE_LIBEDIT=OFF \
	  -DLLVM_ENABLE_OCAMLDOC=OFF \
	  -DLLVM_ENABLE_BINDINGS=OFF \
	  -DLLVM_ENABLE_ASSERTIONS=OFF
	@echo "llvm-configure OK — build/ ready under $(LLVM_SRC_DIR)"

# llvm-build: actually compile the static libs. This is the long step
# (10-30 min cold). The target list is narrow on purpose; expanding it
# raises the linked binary size in L3 roughly linearly.
llvm-build: llvm-configure
	cd $(LLVM_SRC_DIR) && cmake --build build --target $(LLVM_CMAKE_TARGETS)
	@echo "llvm-build OK — static .a archives under $(LLVM_BUILD_DIR)/lib"
	@$(MAKE) llvm-size

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
