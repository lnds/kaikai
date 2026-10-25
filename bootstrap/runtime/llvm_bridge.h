/* ===================================================================
 * KIR Lane 1 — in-process libLLVM C-API forwarders (docs/kir-design.md
 * §7.2). OPT-IN ONLY: compiled only under `-DKAI_LLVM` (set by
 * `make KAI_LLVM=1`). The default build and the whole bootstrap chain
 * never see these — no libLLVM header, no libLLVM link.
 *
 * ABI: a forwarder is a plain C function whose handle params/returns
 * are raw `void *` (an LLVM C-API object pointer). On the kaikai side
 * these are typed `TyHandle` (raw, MUnboxed, non-RC); the C path emits
 * a raw UFn call with `void *` args and a `void *` result, so a handle
 * NEVER passes through `kai_int` / the tagged-Int boxing (which would
 * corrupt the pointer). This is the spike that confirms UFn-raw is the
 * right vehicle over the extern-C/FFI shim (which always reboxes the
 * result to `KaiValue *`).
 * =================================================================== */
/* Compiler-only. runtime.h includes it unless the TU defines KAI_NO_LLVM_BRIDGE (and
 * is not built with KAI_LLVM, whose kai_exit needs the emit pool). */
#ifndef KAI_RUNTIME_LLVM_BRIDGE_H
#define KAI_RUNTIME_LLVM_BRIDGE_H
#ifdef KAI_LLVM
#include "llvm_ctx.h"
#include "llvm_codegen.h"
#include "llvm_emit.h"
#else /* !KAI_LLVM */
#include "llvm_stubs.h"
#endif /* KAI_LLVM */
#endif /* KAI_RUNTIME_LLVM_BRIDGE_H */
