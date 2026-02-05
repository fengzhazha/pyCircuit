#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"

PYC_COMPILE="${PYC_COMPILE:-${ROOT_DIR}/pyc/mlir/build/bin/pyc-compile}"

if [[ ! -x "${PYC_COMPILE}" ]]; then
  echo "error: missing pyc-compile at ${PYC_COMPILE}" >&2
  echo "build it first (see ${ROOT_DIR}/README.md)" >&2
  exit 1
fi

GEN_DIR="${ROOT_DIR}/janus/generated/janus_bcc_pyc"
HDR="${GEN_DIR}/janus_bcc_pyc_gen.hpp"
if [[ ! -f "${HDR}" ]]; then
  bash "${ROOT_DIR}/janus/update_generated.sh"
fi

WORK_DIR="$(mktemp -d -t janus_bcc_pyc_tb.XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT

clang++ -std=c++17 -O2 \
  -I "${ROOT_DIR}/include" \
  -I "${GEN_DIR}" \
  -o "${WORK_DIR}/tb_janus_bcc_pyc" \
  "${ROOT_DIR}/janus/tb/tb_janus_bcc_pyc.cpp"

if [[ $# -gt 0 ]]; then
  "${WORK_DIR}/tb_janus_bcc_pyc" "$@"
else
  "${WORK_DIR}/tb_janus_bcc_pyc"
fi
