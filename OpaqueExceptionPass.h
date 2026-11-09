#pragma once

#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"

struct OpaqueExceptionPass : llvm::PassInfoMixin<OpaqueExceptionPass>
{
    llvm::PreservedAnalyses run(llvm::Module& m, llvm::ModuleAnalysisManager&);
};

bool RunOpaqueException(llvm::Module& m);

extern llvm::cl::opt<bool> OpaqueExceptionEnable;