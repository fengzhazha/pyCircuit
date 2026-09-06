#include "CompilerInternal.h"

#include "acir/Bindings/Registry.h"
#include "acir/Dialect/ACIR/GraphRegion.h"
#include "acir/Transforms/ResolveBindings.h"

#include "acir/CodeGen/Generator.h"
#include "acir/CodeGen/ModelPlan.h"
#include "acir/Conversion/ACIRToACSim/ACIRToACSim.h"
#include "acir/Dialect/ACSim/ACSimOps.h"
#include "acir/InitAllDialects.h"
#include "acir/InitAllPasses.h"
#include "acir/Transforms/Passes.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Location.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Pass/PassRegistry.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace acir::compiler {
namespace {

struct DriverState {
  mlir::MLIRContext context{mlir::MLIRContext::Threading::DISABLED};
  mlir::OwningOpRef<mlir::ModuleOp> module;
  std::optional<codegen::SourceBundle> sources;
  std::optional<codegen::BuildResult> build;
  std::string frozenAcir;
  std::string canonicalAcsim;
  std::string bindingLock = "[]";
  std::vector<std::string> providerInputs;
};

std::string severityName(mlir::DiagnosticSeverity severity) {
  switch (severity) {
  case mlir::DiagnosticSeverity::Error:
    return "error";
  case mlir::DiagnosticSeverity::Warning:
    return "warning";
  case mlir::DiagnosticSeverity::Remark:
    return "remark";
  case mlir::DiagnosticSeverity::Note:
    return "note";
  }
  return "error";
}

std::optional<SourceLocation> sourceLocation(mlir::Location location) {
  if (auto file = mlir::dyn_cast<mlir::FileLineColLoc>(location))
    return SourceLocation{file.getFilename().str(), file.getLine(),
                          file.getColumn()};
  return std::nullopt;
}

std::string diagnosticCode(llvm::StringRef message, llvm::StringRef fallback) {
  for (size_t start = message.find("AC"); start != llvm::StringRef::npos;
       start = message.find("AC", start + 2)) {
    size_t end = start;
    while (end < message.size() &&
           (std::isupper(static_cast<unsigned char>(message[end])) ||
            std::isdigit(static_cast<unsigned char>(message[end])) ||
            message[end] == '-'))
      ++end;
    llvm::StringRef candidate = message.slice(start, end);
    if (candidate.contains('-') && candidate.size() >= 6)
      return candidate.str();
  }
  return fallback.str();
}

std::string defaultDiagnosticCode(CompilerStage stage) {
  switch (stage) {
  case CompilerStage::AcirParse:
    return "ACIR-PARSE-001";
  case CompilerStage::AcirVerify:
    return "ACIR-VERIFY-001";
  case CompilerStage::AcirNormalize:
    return "ACIR-NORMALIZE-001";
  case CompilerStage::AcirFreeze:
    return "ACIR-FREEZE-001";
  case CompilerStage::AcsimLower:
    return "ACLOWER-001";
  case CompilerStage::AcsimVerify:
    return "ACSIM-VERIFY-001";
  case CompilerStage::CxxEmit:
    return "ACLOWER-CXX-001";
  case CompilerStage::CxxContract:
    return "ACLOWER-CXX-CONTRACT-001";
  case CompilerStage::Compile:
    return "ACLOWER-COMPILE-001";
  case CompilerStage::Link:
    return "ACLOWER-LINK-001";
  case CompilerStage::Publish:
    return "ACLOWER-PUBLISH-001";
  }
  return "ACIR-COMPILER-001";
}

CompilerDiagnostic makeDiagnostic(CompilerStage stage, llvm::StringRef code,
                                  llvm::StringRef message) {
  return CompilerDiagnostic{.stage = compilerStageName(stage).str(),
                            .code = code.str(),
                            .severity = "error",
                            .message = message.str()};
}

llvm::Error compilerFailure(CompilerStage stage, llvm::StringRef code,
                            llvm::StringRef message) {
  std::vector<CompilerDiagnostic> diagnostics;
  diagnostics.push_back(makeDiagnostic(stage, code, message));
  return llvm::make_error<CompilerError>(std::move(diagnostics));
}

llvm::Error compilerFailure(CompilerStage stage, llvm::Error error) {
  return compilerFailure(stage, defaultDiagnosticCode(stage),
                         llvm::toString(std::move(error)));
}

class DiagnosticCapture {
public:
  explicit DiagnosticCapture(mlir::MLIRContext &context)
      : handler_(&context, [&](mlir::Diagnostic &diagnostic) {
          std::string message = diagnostic.str();
          diagnostics_.push_back(CompilerDiagnostic{
              .stage = compilerStageName(stage_).str(),
              .code = diagnosticCode(message, defaultDiagnosticCode(stage_)),
              .severity = severityName(diagnostic.getSeverity()),
              .message = std::move(message),
              .source = sourceLocation(diagnostic.getLocation())});
          return mlir::success();
        }) {}

  void setStage(CompilerStage stage) { stage_ = stage; }

  llvm::Error takeFailure(CompilerStage stage) {
    if (diagnostics_.empty())
      diagnostics_.push_back(makeDiagnostic(stage, defaultDiagnosticCode(stage),
                                            "compiler stage failed"));
    if (stage == CompilerStage::AcirParse)
      for (CompilerDiagnostic &diagnostic : diagnostics_)
        diagnostic.code = "ACIR-PARSE-001";
    return llvm::make_error<CompilerError>(std::move(diagnostics_));
  }

  std::vector<CompilerDiagnostic> takeDiagnostics() {
    return std::move(diagnostics_);
  }

private:
  CompilerStage stage_ = CompilerStage::AcirParse;
  std::vector<CompilerDiagnostic> diagnostics_;
  mlir::ScopedDiagnosticHandler handler_;
};

std::string printModule(mlir::ModuleOp module) {
  std::string bytes;
  llvm::raw_string_ostream output(bytes);
  module.print(output);
  output.flush();
  return bytes;
}

bool requested(const CompilerRequest &request, codegen::ArtifactKind kind) {
  return llvm::is_contained(request.emits, kind);
}

bool namesStage(llvm::ArrayRef<std::string> names, CompilerStage stage) {
  return llvm::is_contained(names, compilerStageName(stage));
}

void addArtifact(CompilerResult &result, std::string path,
                 codegen::ArtifactKind kind, std::string bytes) {
  result.artifacts.push_back({std::move(path), kind, std::move(bytes), {}});
  CompilerArtifact &artifact = result.artifacts.back();
  artifact.sha256 = codegen::computeFingerprint(artifact.bytes);
}

unsigned stageOrdinal(CompilerStage stage) {
  return static_cast<unsigned>(stage);
}

llvm::Error validateRequest(const CompilerRequest &request,
                            llvm::ArrayRef<CompilerStage> pipeline) {
  if (request.acirBytes.empty())
    return compilerFailure(CompilerStage::AcirParse, "ACIR-PARSE-001",
                           "ACIR input is empty");
  std::set<codegen::ArtifactKind> unique;
  for (codegen::ArtifactKind kind : request.emits) {
    if (!unique.insert(kind).second)
      return compilerFailure(CompilerStage::AcirParse, "ACIR-EMIT-001",
                             "artifact emission request is duplicated");
    CompilerStage required = CompilerStage::Publish;
    switch (kind) {
    case codegen::ArtifactKind::Acir:
      required = CompilerStage::AcirFreeze;
      break;
    case codegen::ArtifactKind::Acsim:
      required = CompilerStage::AcsimVerify;
      break;
    case codegen::ArtifactKind::CppHeader:
    case codegen::ArtifactKind::CppSource:
      required = CompilerStage::CxxEmit;
      break;
    case codegen::ArtifactKind::Executable:
      required = CompilerStage::Publish;
      break;
    case codegen::ArtifactKind::Report:
      required = CompilerStage::CxxContract;
      break;
    case codegen::ArtifactKind::Acpy:
      return compilerFailure(CompilerStage::AcirParse, "ACIR-EMIT-001",
                             "the native compiler does not emit ACPy");
    }
    if (pipeline.empty() ||
        stageOrdinal(required) > stageOrdinal(pipeline.back()))
      return compilerFailure(required, "ACIR-EMIT-001",
                             "artifact requires a later compiler stage");
  }
  auto validateDumps = [&](llvm::ArrayRef<std::string> names) -> llvm::Error {
    std::set<std::string> uniqueNames;
    for (const std::string &name : names) {
      if (!uniqueNames.insert(name).second)
        return compilerFailure(CompilerStage::AcirParse, "ACIR-DUMP-001",
                               "dump stage is duplicated");
      bool known = llvm::any_of(pipeline, [&](CompilerStage stage) {
        return compilerStageName(stage) == name;
      });
      if (!known)
        return compilerFailure(CompilerStage::AcirParse, "ACIR-DUMP-001",
                               "dump stage is outside the selected pipeline");
    }
    return llvm::Error::success();
  };
  if (auto error = validateDumps(request.dumpBefore))
    return error;
  if (auto error = validateDumps(request.dumpAfter))
    return error;
  return llvm::Error::success();
}

llvm::LogicalResult runPass(DriverState &state,
                            std::unique_ptr<mlir::Pass> pass) {
  mlir::PassManager manager(&state.context);
  manager.addPass(std::move(pass));
  return manager.run(state.module.get());
}

llvm::LogicalResult runCustomPipeline(DriverState &state,
                                      llvm::StringRef pipeline,
                                      std::string &error) {
  mlir::PassManager manager(&state.context);
  llvm::raw_string_ostream stream(error);
  if (mlir::failed(mlir::parsePassPipeline(pipeline, manager, stream)))
    return mlir::failure();
  return manager.run(state.module.get());
}

llvm::LogicalResult runRulePipeline(DriverState &state) {
  mlir::PassManager manager(&state.context);
  addRuleLoweringPipeline(manager);
  return manager.run(state.module.get());
}

llvm::Error runStage(CompilerStage stage, const CompilerRequest &request,
                     DriverState &state, CompilerResult &result,
                     DiagnosticCapture &capture) {
  capture.setStage(stage);
  switch (stage) {
  case CompilerStage::AcirParse:
    state.module = mlir::parseSourceString<mlir::ModuleOp>(request.acirBytes,
                                                           &state.context);
    return state.module ? llvm::Error::success() : capture.takeFailure(stage);
  case CompilerStage::AcirVerify:
    if (mlir::failed(runPass(state, createVerifyACIRFilePass())))
      return capture.takeFailure(stage);
    return llvm::Error::success();
  case CompilerStage::AcirNormalize:
    if (request.profile == CompilerProfile::Custom) {
      std::string error;
      if (mlir::failed(
              runCustomPipeline(state, *request.customPipeline, error))) {
        if (!error.empty())
          return compilerFailure(stage, "ACIR-PIPELINE-001", error);
        return capture.takeFailure(stage);
      }
      if (mlir::failed(runPass(state, createVerifyRuleClosurePass())))
        return capture.takeFailure(stage);
      return llvm::Error::success();
    }
    if (mlir::failed(runRulePipeline(state)))
      return capture.takeFailure(stage);
    if (mlir::failed(runPass(state, createNormalizeACIRFilePass())))
      return capture.takeFailure(stage);
    return llvm::Error::success();
  case CompilerStage::AcirFreeze:
    if (mlir::failed(runPass(state, createFreezeTopologyPass())))
      return capture.takeFailure(stage);
    state.frozenAcir = printModule(*state.module);
    if (requested(request, codegen::ArtifactKind::Acir))
      addArtifact(result, "frozen.ac.mlir", codegen::ArtifactKind::Acir,
                  state.frozenAcir);
    return llvm::Error::success();
  case CompilerStage::AcsimLower: {
    bindings::BindingRegistryDocument registry;
    if (!request.bindingRegistryBytes.empty()) {
      auto parsed =
          bindings::parseBindingRegistry(request.bindingRegistryBytes);
      if (!parsed)
        return compilerFailure(stage, parsed.takeError());
      registry = std::move(*parsed);
    }
    ACIRToACSimPassOptions options;
    options.profile = request.profile == CompilerProfile::Validated
                          ? "validated"
                      : request.profile == CompilerProfile::Custom ? "custom"
                                                                   : "fast";
    options.target = request.build.toolchain.targetTriple.empty()
                         ? "unknown-unknown"
                         : request.build.toolchain.targetTriple;
    ResolveBindingsPassOptions resolutionOptions;
    resolutionOptions.candidates = registry.candidates;
    resolutionOptions.requests = registry.requests;
    resolutionOptions.profile = options.profile;
    resolutionOptions.target = options.target;
    auto resolution = resolveModuleBindings(*state.module, resolutionOptions);
    if (!resolution)
      return compilerFailure(stage, resolution.takeError());
    if (!resolution->selections().empty() || request.bindingLockBytes.empty())
      state.bindingLock = resolution->canonicalLock().str();
    std::set<std::string> providers;
    for (const bindings::ResolvedBinding &selection : resolution->selections())
      providers.insert(selection.record().provider().str());
    state.providerInputs.assign(providers.begin(), providers.end());
    options.candidates = std::move(registry.candidates);
    options.requests = std::move(registry.requests);
    if (mlir::failed(runPass(state, createACIRToACSimPass(std::move(options)))))
      return capture.takeFailure(stage);
    return llvm::Error::success();
  }
  case CompilerStage::AcsimVerify:
    if (mlir::failed(mlir::verify(state.module.get())) ||
        !llvm::hasSingleElement(state.module->getOps<acsim::ModelOp>()))
      return capture.takeFailure(stage);
    state.canonicalAcsim = printModule(*state.module);
    if (requested(request, codegen::ArtifactKind::Acsim))
      addArtifact(result, "model.acsim.mlir", codegen::ArtifactKind::Acsim,
                  state.canonicalAcsim);
    return llvm::Error::success();
  case CompilerStage::CxxEmit: {
    auto plan = codegen::buildModelPlan(*state.module);
    if (!plan)
      return compilerFailure(stage, plan.takeError());
    auto sources = codegen::generateModelSources(*plan);
    if (!sources)
      return compilerFailure(stage, sources.takeError());
    state.sources = std::move(*sources);
    for (const codegen::GeneratedFile &file : state.sources->files) {
      codegen::ArtifactKind kind =
          llvm::StringRef(file.relativePath).ends_with(".h")
              ? codegen::ArtifactKind::CppHeader
              : codegen::ArtifactKind::CppSource;
      if (requested(request, kind))
        addArtifact(result, file.relativePath, kind, file.content);
    }
    return llvm::Error::success();
  }
  case CompilerStage::CxxContract: {
    auto plan = codegen::buildModelPlan(*state.module);
    if (!plan)
      return compilerFailure(stage, plan.takeError());
    if (auto error = codegen::validateSourceBundle(*plan, *state.sources))
      return compilerFailure(stage, std::move(error));
    return llvm::Error::success();
  }
  case CompilerStage::Compile: {
    codegen::BuildRequest build = request.build;
    build.canonicalACSim = *state.module;
    build.frozenAcirBytes = state.frozenAcir;
    build.canonicalACSimBytes = state.canonicalAcsim;
    build.bindingLockBytes = state.bindingLock;
    build.providerInputs = state.providerInputs;
    if (build.profile.empty())
      build.profile = request.profile == CompilerProfile::Validated
                          ? "validated"
                      : request.profile == CompilerProfile::Custom ? "custom"
                                                                   : "fast";
    build.passPipeline = {
        "acir-verify",
        "ac-verify-value-constraints",
        "ac-lower-variable-state",
        "ac-verify-value-constraints",
        "ac-infer-rule-types",
        "ac-infer-rule-effects",
        "ac-infer-rule-activation",
        "ac-materialize-rule-checks",
        "ac-materialize-rule-handshake",
        "ac-discharge-rule-obligations",
        "ac-resolve-rule-schedule",
        "ac-lower-rules-to-firing",
        "ac-canonicalize-pure-firings",
        "ac-verify-rule-closure",
        request.customPipeline.value_or("acir-normalize"),
        "acir-freeze",
        "acsim-lower",
        "acsim-verify",
        "acsim-emit-cxx",
        "acsim-check-cxx-contract",
        "compile",
        "link",
    };
    auto built = codegen::buildGeneratedModel(build);
    if (!built)
      return compilerFailure(stage, built.takeError());
    state.build = std::move(*built);
    result.build = state.build;
    return llvm::Error::success();
  }
  case CompilerStage::Link:
    return state.build ? llvm::Error::success()
                       : compilerFailure(stage, "ACLOWER-LINK-001",
                                         "compile stage produced no build");
  case CompilerStage::Publish:
    if (!state.build)
      return compilerFailure(stage, "ACLOWER-PUBLISH-001",
                             "link stage produced no build");
    if (requested(request, codegen::ArtifactKind::Executable)) {
      auto executable = llvm::MemoryBuffer::getFile(state.build->executable);
      if (!executable)
        return compilerFailure(stage, "ACLOWER-PUBLISH-001",
                               "published executable cannot be read");
      addArtifact(result, "bin/model", codegen::ArtifactKind::Executable,
                  (*executable)->getBuffer().str());
    }
    return llvm::Error::success();
  }
  llvm_unreachable("closed CompilerStage is exhaustive");
} // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): DriverState owns the
  // parsed MLIR module across stage calls and destroys it after the pipeline.

} // namespace

char CompilerError::ID = 0;

CompilerError::CompilerError(std::vector<CompilerDiagnostic> diagnostics)
    : diagnostics_(std::move(diagnostics)) {}

void CompilerError::log(llvm::raw_ostream &output) const {
  if (diagnostics_.empty()) {
    output << "compiler failed without a diagnostic";
    return;
  }
  output << diagnostics_.front().code << ": " << diagnostics_.front().message;
}

std::error_code CompilerError::convertToErrorCode() const {
  return llvm::inconvertibleErrorCode();
}

llvm::StringRef compilerStageName(CompilerStage stage) {
  switch (stage) {
  case CompilerStage::AcirParse:
    return "acir-parse";
  case CompilerStage::AcirVerify:
    return "acir-verify";
  case CompilerStage::AcirNormalize:
    return "acir-normalize";
  case CompilerStage::AcirFreeze:
    return "acir-freeze";
  case CompilerStage::AcsimLower:
    return "acsim-lower";
  case CompilerStage::AcsimVerify:
    return "acsim-verify";
  case CompilerStage::CxxEmit:
    return "cxx-emit";
  case CompilerStage::CxxContract:
    return "cxx-contract";
  case CompilerStage::Compile:
    return "compile";
  case CompilerStage::Link:
    return "link";
  case CompilerStage::Publish:
    return "publish";
  }
  llvm_unreachable("closed CompilerStage is exhaustive");
}

llvm::Expected<CompilerResult> runCompiler(const CompilerRequest &request) {
  static std::once_flag passesRegistered;
  std::call_once(passesRegistered, [] { registerAllPasses(); });

  auto pipeline = detail::selectPipeline(request);
  if (!pipeline)
    return compilerFailure(CompilerStage::AcirParse, "ACIR-PIPELINE-001",
                           llvm::toString(pipeline.takeError()));
  if (auto error = validateRequest(request, *pipeline))
    return std::move(error);

  mlir::DialectRegistry registry;
  registerAllDialects(registry);
  DriverState state;
  state.bindingLock =
      request.bindingLockBytes.empty() ? "[]" : request.bindingLockBytes;
  state.context.appendDialectRegistry(registry);
  state.context.loadAllAvailableDialects();
  if (!request.bindingRegistryBytes.empty()) {
    auto bindingRegistry =
        bindings::parseBindingRegistry(request.bindingRegistryBytes);
    if (!bindingRegistry)
      return compilerFailure(CompilerStage::AcirParse,
                             bindingRegistry.takeError());
    auto &providers = ac::getStructuralProviderRegistry(&state.context);
    for (const bindings::BindingRequest &bindingRequest :
         bindingRegistry->requests)
      providers.registerExternal(bindingRequest.binding);
  }
  DiagnosticCapture capture(state.context);
  CompilerResult result;

  for (CompilerStage stage : *pipeline) {
    if (state.module && namesStage(request.dumpBefore, stage))
      addArtifact(result,
                  "dumps/" + compilerStageName(stage).str() + "-before.mlir",
                  codegen::ArtifactKind::Report, printModule(*state.module));
    if (auto error = runStage(stage, request, state, result, capture))
      return std::move(error);
    if (request.verifyAfterEach && state.module &&
        mlir::failed(mlir::verify(state.module.get())))
      return capture.takeFailure(stage);
    if (state.module &&
        (request.dumpAfterEach || namesStage(request.dumpAfter, stage)))
      addArtifact(result,
                  "dumps/" + compilerStageName(stage).str() + "-after.mlir",
                  codegen::ArtifactKind::Report, printModule(*state.module));
  }
  result.diagnostics = capture.takeDiagnostics();
  return result;
}

} // namespace acir::compiler
