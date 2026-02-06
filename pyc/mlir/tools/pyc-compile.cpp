#include "pyc/Dialect/PYC/PYCDialect.h"
#include "pyc/Emit/CppEmitter.h"
#include "pyc/Emit/VerilogEmitter.h"
#include "pyc/Transforms/Passes.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Func/Extensions/InlinerExtension.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;

static llvm::cl::opt<std::string> inputFilename(llvm::cl::Positional, llvm::cl::desc("<input .pyc>"),
                                                llvm::cl::init("-"));

static llvm::cl::opt<std::string> outputFilename("o", llvm::cl::desc("Output file"), llvm::cl::init("-"));

static llvm::cl::opt<std::string> emitKind("emit", llvm::cl::desc("Emission target: verilog|cpp"),
                                           llvm::cl::init("verilog"));

static llvm::cl::opt<bool> includePrims("include-primitives",
                                        llvm::cl::desc("Emit `include` for PYC Verilog primitives"),
                                        llvm::cl::init(true));

int main(int argc, char **argv) {
  llvm::InitLLVM y(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv, "pyc-compile\n");

  DialectRegistry registry;
  registry.insert<pyc::PYCDialect, mlir::arith::ArithDialect, mlir::func::FuncDialect, mlir::scf::SCFDialect>();
  mlir::func::registerInlinerExtension(registry);

  MLIRContext ctx(registry);
  ctx.loadAllAvailableDialects();

  llvm::SourceMgr sm;
  auto fileOrErr = llvm::MemoryBuffer::getFileOrSTDIN(inputFilename);
  if (!fileOrErr) {
    llvm::errs() << "error: cannot read " << inputFilename << "\n";
    return 1;
  }
  sm.AddNewSourceBuffer(std::move(*fileOrErr), llvm::SMLoc());

  OwningOpRef<ModuleOp> module = parseSourceFile<ModuleOp>(sm, &ctx);
  if (!module) {
    llvm::errs() << "error: failed to parse MLIR\n";
    return 1;
  }

  // Cleanup + optimization pipeline tuned for netlist-style emission.
  PassManager pm(&ctx);
  pm.addPass(createInlinerPass());
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createCSEPass());

  // Constprop / DCE before lowering and comb fusion (keep IR small).
  pm.addPass(createSCCPPass());
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createCSEPass());
  pm.addPass(createRemoveDeadValuesPass());
  pm.addPass(createSymbolDCEPass());

  pm.addNestedPass<func::FuncOp>(pyc::createLowerSCFToPYCStaticPass());
  pm.addNestedPass<func::FuncOp>(pyc::createEliminateWiresPass());
  pm.addNestedPass<func::FuncOp>(pyc::createCombCanonicalizePass());
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createCSEPass());

  // Re-run simplification after CFG lowering.
  pm.addPass(createSCCPPass());
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createCSEPass());
  pm.addPass(createSymbolDCEPass());
  pm.addPass(createRemoveDeadValuesPass());

  pm.addNestedPass<func::FuncOp>(pyc::createFuseCombPass());
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createCSEPass());
  pm.addPass(createRemoveDeadValuesPass());
  pm.addPass(createSymbolDCEPass());
  pm.addNestedPass<func::FuncOp>(pyc::createCheckFlatTypesPass());
  if (failed(pm.run(*module))) {
    llvm::errs() << "error: pass pipeline failed\n";
    return 1;
  }

  std::error_code ec;
  llvm::raw_fd_ostream os(outputFilename, ec, llvm::sys::fs::OF_Text);
  if (ec) {
    llvm::errs() << "error: cannot open " << outputFilename << ": " << ec.message() << "\n";
    return 1;
  }

  if (emitKind == "verilog") {
    pyc::VerilogEmitterOptions opts;
    opts.includePrimitives = includePrims;
    if (failed(pyc::emitVerilog(*module, os, opts)))
      return 1;
    return 0;
  }
  if (emitKind == "cpp") {
    if (failed(pyc::emitCpp(*module, os)))
      return 1;
    return 0;
  }

  llvm::errs() << "error: unknown --emit kind: " << emitKind << "\n";
  return 1;
}
