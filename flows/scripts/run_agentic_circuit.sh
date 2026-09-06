#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

gate_run_id="${PYC_GATE_RUN_ID:-$(date +%Y%m%d-%H%M%S)}"
docs_gate_dir="${PYC_ROOT_DIR}/docs/gates/logs/${gate_run_id}"
gate_out_dir="$(pyc_out_root)/gates/${gate_run_id}/agentic-circuit"
ac_source="${PYC_ROOT_DIR}/compiler/acir"
ac_python="${PYC_ROOT_DIR}/python/agentic-circuit"
ac_test_python="${PYC_ROOT_DIR}/tests/python/agentic-circuit"
ac_build="$(pyc_out_root)/acir/dev-llvm22"
ac_tests="${PYC_ROOT_DIR}/tests"
ac_tools="${PYC_ROOT_DIR}/compiler/acir/tools"
ac_lock="${PYC_ROOT_DIR}/toolchains/agentic-circuit/pyc.lock.json"
venv="$(pyc_out_root)/agentic-circuit/venv"
mkdir -p "${docs_gate_dir}" "${gate_out_dir}" "$(dirname "${venv}")"

exec > >(tee -a "${docs_gate_dir}/agentic_circuit.stdout") \
  2> >(tee -a "${docs_gate_dir}/agentic_circuit.stderr" >&2)

cat > "${docs_gate_dir}/agentic_circuit_commands.txt" <<EOF
bash flows/scripts/run_agentic_circuit.sh
python3 -m unittest discover -s tests/python/agentic-circuit/python_frontend -p 'test_*.py'
python3 -m unittest discover -s tests/python/agentic-circuit/cli -p 'test_*.py'
cmake --build .pycircuit_out/acir/dev-llvm22 --target check-acir
ctest --test-dir .pycircuit_out/acir/dev-llvm22 --output-on-failure
bash flows/scripts/pyc build
acir-opt --pass-pipeline='builtin.module(ac-freeze-topology)' <raw-queue-graph>
compiler/acir/tools/ac-queue-pyc-build.py <ACIR> ...
pytest tests/unit -m unit
python3 flows/tools/check_api_hygiene.py python/pycircuit/src/pycircuit examples/pycircuit docs README.md
python3 flows/tools/check_decision_status.py --require-no-deferred --require-all-verified --require-concrete-evidence --require-existing-evidence
mkdocs build --strict
EOF

pyc_log "Agentic Circuit closure run-id=${gate_run_id}"

resume_from="${AC_GATE_RESUME_FROM:-g0}"
if [[ "${resume_from}" != "g0" && "${resume_from}" != "g2" ]]; then
  pyc_die "AC_GATE_RESUME_FROM must be g0 or g2"
fi
if [[ "${resume_from}" == "g0" ]]; then
  completed_lanes='["AC G0", "AC G1", "AC G2"]'
  completion_message="Agentic Circuit G0/G1/G2 closure passed"
else
  completed_lanes='["AC G2"]'
  completion_message="Agentic Circuit G2 closure passed (G0/G1 explicitly skipped)"
fi

if [[ ! -x "${venv}/bin/python" ]]; then
  python3 -m venv "${venv}"
fi
"${venv}/bin/python" -m pip install -e "${PYC_ROOT_DIR}/python/semantic-core"
"${venv}/bin/python" -m pip install -e "${ac_python}[test]"

if [[ "${resume_from}" == "g0" ]]; then
  llvm_config="${LLVM_CONFIG:-}"
  if [[ -z "${llvm_config}" ]]; then
    for candidate in llvm-config-22 llvm-config; do
      if command -v "${candidate}" >/dev/null 2>&1; then
        llvm_config="$(command -v "${candidate}")"
        break
      fi
    done
  fi
  if [[ -z "${llvm_config}" ]]; then
    for candidate in \
      /opt/homebrew/opt/llvm/bin/llvm-config \
      /usr/local/opt/llvm/bin/llvm-config; do
      if [[ -x "${candidate}" ]]; then
        llvm_config="${candidate}"
        break
      fi
    done
  fi
  [[ -n "${llvm_config}" ]] || pyc_die "LLVM 22 llvm-config is required"
  [[ "$("${llvm_config}" --version | cut -d. -f1)" == "22" ]] || \
    pyc_die "Agentic Circuit requires LLVM 22"
  export LLVM_DIR="${LLVM_DIR:-$("${llvm_config}" --cmakedir)}"
  export MLIR_DIR="${MLIR_DIR:-$(dirname "${LLVM_DIR}")/mlir}"

  pyc_log "AC G0/G1: configure integrated ACIR compiler"
  PATH="${venv}/bin:${PATH}" cmake --preset dev-llvm22 \
    -S "${ac_source}" -DACIR_BUILD_TESTING=ON
  cmake --build "${ac_build}" -j "${PYC_BUILD_JOBS:-6}"

  site_packages="$("${venv}/bin/python" -c \
    'import sysconfig; print(sysconfig.get_paths()["purelib"])')"
  (
    cd "${PYC_ROOT_DIR}"
    env -u AC_GATE_TOOLCHAIN_ROOT \
      PYTHONPATH="${PYC_ROOT_DIR}/python/semantic-core/src:${ac_python}/src:${ac_test_python}:${ac_build}/python" \
      "${venv}/bin/python" -m unittest discover \
        -s tests/python/agentic-circuit/python_frontend -p 'test_*.py'
    env -u AC_GATE_TOOLCHAIN_ROOT \
      PYTHONPATH="${PYC_ROOT_DIR}/python/semantic-core/src:${ac_python}/src:${ac_test_python}:${ac_build}/python" \
      "${venv}/bin/python" -m unittest discover \
        -s tests/python/agentic-circuit/cli -p 'test_*.py'
    PYTHONPATH="${site_packages}" \
      cmake --build "${ac_build}" --target check-acir -j "${PYC_BUILD_JOBS:-6}"
    ctest --test-dir "${ac_build}" --output-on-failure \
      -j "${PYC_TEST_JOBS:-6}"
  )
fi

pyc_log "AC G2: build and install the repo-local pyc6 + AC toolchain"
if [[ -n "${AC_GATE_TOOLCHAIN_ROOT:-}" ]]; then
  toolchain="${AC_GATE_TOOLCHAIN_ROOT}"
  pyc_log "AC G2: reusing integrated toolchain ${toolchain}"
else
  gate_toolchain="${gate_out_dir}/toolchain"
  PYC_BUILD_DIR="${gate_toolchain}/build" \
  PYC_INSTALL_PREFIX="${gate_toolchain}/install" \
  PYC_BUILD_AGENTIC_CIRCUIT=ON \
    bash "${PYC_ROOT_DIR}/flows/scripts/pyc" build
  toolchain="${gate_toolchain}/install"
fi
pycgen="${toolchain}/bin/acir-queue-pycgen"
acir_opt="${toolchain}/bin/acir-opt"
pycc="${toolchain}/bin/pycc"
metadata="${toolchain}/share/pycircuit/toolchain-metadata.json"
for required in "${pycgen}" "${acir_opt}" "${pycc}" "${metadata}"; do
  [[ -f "${required}" ]] || pyc_die "missing integrated toolchain artifact: ${required}"
done

cxx="$(command -v c++ || true)"
verilator="$(command -v verilator || true)"
[[ -n "${cxx}" ]] || pyc_die "C++ compiler is required for AC G2"
[[ -n "${verilator}" ]] || pyc_die "Verilator is required for AC G2"

for case_name in arbiter atomic-transform bit-widths masked-match popcount; do
  case_dir="${gate_out_dir}/${case_name}"
  if [[ -e "${case_dir}" ]]; then
    pyc_die "AC G2 output already exists: ${case_dir}"
  fi
  mkdir -p "${case_dir}"
  "${acir_opt}" \
    --pass-pipeline='builtin.module(ac-freeze-topology)' \
    "${ac_tests}/mlir/agentic-circuit/CodeGen/${case_name}.mlir" \
    -o "${case_dir}/model.frozen.ac.mlir"
  "${ac_tools}/ac-queue-pyc-build.py" \
    "${case_dir}/model.frozen.ac.mlir" \
    --pycgen-tool "${pycgen}" \
    --pycc "${pycc}" \
    --toolchain-lock "${ac_lock}" \
    --toolchain-metadata "${metadata}" \
    --cxx "${cxx}" \
    --verilator "${verilator}" \
    --pyc-output "${case_dir}/model.pyc" \
    --cpp-output-dir "${case_dir}/cpp" \
    --verilog-output-dir "${case_dir}/verilog" \
    --manifest "${case_dir}/manifest.json"
done

PYC_TOOLCHAIN_ROOT="${toolchain}" \
ACIR_OPT="${acir_opt}" \
ACIR_QUEUE_PYCGEN="${pycgen}" \
PYTHONPATH="${PYC_ROOT_DIR}/python/semantic-core/src:${ac_python}/src:${ac_build}/python" \
  "${venv}/bin/python" \
  "${PYC_ROOT_DIR}/tests/integration/agentic-circuit/e2e/test_pyc_backend.py" \
  PycBackendTest.test_rule_retirement_builds_pyc_and_verilog \
  PycBackendTest.test_bitfield_scalar_is_cycle_equivalent_in_pyc_cpp_and_verilog \
  PycBackendTest.test_masked_decode_is_cycle_equivalent_in_pyc_cpp_and_verilog \
  PycBackendTest.test_nested_payload_is_cycle_equivalent_in_pyc_cpp_and_verilog \
  PycBackendTest.test_nominal_enum_is_cycle_equivalent_in_pyc_cpp_and_verilog \
  PycBackendTest.test_aggregate_payload_is_cycle_equivalent_in_pyc_cpp_and_verilog \
  PycBackendTest.test_recursive_aggregate_payload_is_cycle_equivalent_in_pyc_cpp_and_verilog \
  -v

pyc_log "pyCircuit 6 root contracts and documentation"
(
  cd "${PYC_ROOT_DIR}"
  pytest tests/unit -m unit -q
  python3 flows/tools/check_api_hygiene.py \
    python/pycircuit/src/pycircuit examples/pycircuit docs README.md
  python3 flows/tools/check_decision_status.py \
    --rfc docs/rfcs/pyc6-decisions.md \
    --status docs/gates/decision_status_v6.md \
    --out "${docs_gate_dir}/decision_status_report.json" \
    --require-no-deferred \
    --require-all-verified \
    --require-concrete-evidence \
    --require-existing-evidence
  mkdocs build --strict
)

cat > "${docs_gate_dir}/agentic_circuit_summary.json" <<EOF
{
  "run_id": "${gate_run_id}",
  "script": "run_agentic_circuit.sh",
  "status": "pass",
  "lanes": ${completed_lanes},
  "resume_from": "${resume_from}",
  "contract_epoch": "0.5",
  "pyc_interface": "pyc6",
  "cases": ["arbiter", "atomic-transform", "bit-widths", "masked-match", "popcount", "rule-retirement", "bitfield", "masked-decode", "nested-payload", "enum-payload", "aggregate-payload", "recursive-aggregate-payload"]
}
EOF

pyc_log "${completion_message}"
