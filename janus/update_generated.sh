#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -z "${PYC_COMPILE:-}" ]]; then
  if [[ -x "${ROOT_DIR}/pyc/mlir/build/bin/pyc-compile" ]]; then
    PYC_COMPILE="${ROOT_DIR}/pyc/mlir/build/bin/pyc-compile"
  elif [[ -x "${ROOT_DIR}/build/bin/pyc-compile" ]]; then
    PYC_COMPILE="${ROOT_DIR}/build/bin/pyc-compile"
  elif command -v pyc-compile >/dev/null 2>&1; then
    PYC_COMPILE="$(command -v pyc-compile)"
  else
    PYC_COMPILE=""
  fi
fi

if [[ -z "${PYC_COMPILE}" || ! -x "${PYC_COMPILE}" ]]; then
  echo "error: missing pyc-compile (PYC_COMPILE=${PYC_COMPILE:-<unset>})" >&2
  echo "Build MLIR tools first (see ${ROOT_DIR}/README.md)." >&2
  exit 1
fi

OUT_ROOT="${ROOT_DIR}/janus/generated"
mkdir -p "${OUT_ROOT}"

emit_one() {
  local name="$1"
  local src="$2"
  local outdir="${OUT_ROOT}/${name}"

  mkdir -p "${outdir}"
  echo "[emit] ${name}: ${src}"

  local tmp_pyc
  tmp_pyc="$(mktemp -t "pycircuit.janus.${name}.pyc")"

  PYTHONDONTWRITEBYTECODE=1 \
  PYTHONPATH="${ROOT_DIR}/binding/python:${ROOT_DIR}/janus/pyc" \
    python3 -m pycircuit.cli emit "${src}" -o "${tmp_pyc}"

  "${PYC_COMPILE}" "${tmp_pyc}" --emit=verilog -o "${outdir}/${name}.v"
  "${PYC_COMPILE}" "${tmp_pyc}" --emit=cpp -o "${outdir}/${name}.hpp"
}

emit_one janus_bcc_pyc "${ROOT_DIR}/janus/pyc/janus/bcc/janus_bcc_pyc.py"
emit_one janus_bcc_ooo_pyc "${ROOT_DIR}/janus/pyc/janus/bcc/janus_bcc_ooo_pyc.py"
emit_one janus_cube_pyc "${ROOT_DIR}/janus/pyc/janus/cube/cube.py"

mv -f "${OUT_ROOT}/janus_bcc_pyc/janus_bcc_pyc.hpp" "${OUT_ROOT}/janus_bcc_pyc/janus_bcc_pyc_gen.hpp"
mv -f "${OUT_ROOT}/janus_bcc_ooo_pyc/janus_bcc_ooo_pyc.hpp" "${OUT_ROOT}/janus_bcc_ooo_pyc/janus_bcc_ooo_pyc_gen.hpp"
mv -f "${OUT_ROOT}/janus_cube_pyc/janus_cube_pyc.hpp" "${OUT_ROOT}/janus_cube_pyc/janus_cube_pyc_gen.hpp"

echo "OK: wrote outputs under ${OUT_ROOT}"

