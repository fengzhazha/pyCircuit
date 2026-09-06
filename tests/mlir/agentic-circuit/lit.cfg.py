import os
import sys

import lit.formats


config.name = "AgenticCircuit"
config.test_format = lit.formats.ShTest(execute_external=True)
config.suffixes = [".mlir"]
config.test_source_root = os.path.dirname(__file__)

configured_paths = {
    "ACIR_TEST_EXEC_ROOT": getattr(config, "acir_test_exec_root", None)
    or os.environ.get("ACIR_TEST_EXEC_ROOT"),
    "ACIR_TOOLS_DIR": getattr(config, "acir_tools_dir", None)
    or os.environ.get("ACIR_TOOLS_DIR"),
    "LLVM_TOOLS_DIR": getattr(config, "llvm_tools_dir", None)
    or os.environ.get("LLVM_TOOLS_DIR"),
}
missing_paths = [name for name, value in configured_paths.items() if not value]
if missing_paths:
    lit_config.fatal(
        "standalone lit requires explicit paths: " + ", ".join(missing_paths)
    )

config.test_exec_root = configured_paths["ACIR_TEST_EXEC_ROOT"]
tools_dir = configured_paths["ACIR_TOOLS_DIR"]
llvm_tools_dir = configured_paths["LLVM_TOOLS_DIR"]

if sys.platform == "darwin":
    config.available_features.add("system-darwin")

config.substitutions.append(("%binary_root", config.acir_binary_root))
config.substitutions.append(("%cxx", config.acir_cxx))
config.substitutions.append(("%llvm_linker_flags", config.acir_llvm_linker_flags))
config.substitutions.append(("%python", config.acir_python))
config.substitutions.append(("%source_root", config.acir_source_root))

config.substitutions.append(("%acir_opt_public", os.path.join(tools_dir, "acir-opt")))
config.substitutions.append(("%acir_opt", os.path.join(tools_dir, "acir-opt-internal")))
config.substitutions.append(("%acir_build", os.path.join(tools_dir, "acir-build")))
config.substitutions.append(("%acir_cxxgen", os.path.join(tools_dir, "acir-cxxgen")))
config.substitutions.append(
    ("%acir_opcode_catalog", os.path.join(tools_dir, "acir-opcode-catalog"))
)
config.substitutions.append(
    ("%acir_queue_cxxgen", os.path.join(tools_dir, "acir-queue-cxxgen"))
)
config.substitutions.append(
    ("%acir_queue_plan", os.path.join(tools_dir, "acir-queue-plan"))
)
config.substitutions.append(
    ("%acir_queue_pycgen", os.path.join(tools_dir, "acir-queue-pycgen"))
)
config.substitutions.append(("%FileCheck", os.path.join(llvm_tools_dir, "FileCheck")))
config.substitutions.append(("%split_file", os.path.join(llvm_tools_dir, "split-file")))
config.substitutions.append(("%not", os.path.join(llvm_tools_dir, "not")))
