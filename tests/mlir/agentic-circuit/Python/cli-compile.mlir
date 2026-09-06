// RUN: rm -rf %t && mkdir -p %t
// RUN: cp %source_root/tests/python/agentic-circuit/cli/fixtures/compile/agentic-circuit.toml %t/
// RUN: cp %source_root/tests/python/agentic-circuit/cli/fixtures/compile/architecture.py %t/
// RUN: cd %t && env PYTHONPATH=%source_root/python/semantic-core/src:%source_root/python/agentic-circuit/src:%binary_root/python %python -m agentic_circuit._cli compile architecture.py --emit=frozen-acir,acsim --stop-after=acsim --output-dir=output --json | %FileCheck %s

// CHECK: "artifacts":["frozen.ac.mlir","model.acsim.mlir"]
// CHECK: "schema":"agentic-circuit-compile-result"
// CHECK: "stage":"acsim"
// CHECK: "status":"passed"
