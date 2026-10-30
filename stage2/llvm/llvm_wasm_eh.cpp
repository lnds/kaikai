// The C API has no exception-model option, and WebAssembly codegen needs it to keep invoke and landingpad.
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include <llvm-c/TargetMachine.h>

extern "C" void kai_llvm_select_wasm_exception_model(LLVMTargetMachineRef ref) {
    auto *tm = reinterpret_cast<llvm::TargetMachine *>(ref);
    tm->Options.ExceptionModel = llvm::ExceptionHandling::Wasm;
    const_cast<llvm::MCAsmInfo *>(tm->getMCAsmInfo())->setExceptionsType(llvm::ExceptionHandling::Wasm);
}
