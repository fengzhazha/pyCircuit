#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
# shellcheck source=../../scripts/lib.sh
source "${ROOT_DIR}/scripts/lib.sh"
pyc_find_pyc_compile

GEN_DIR="${ROOT_DIR}/janus/generated/janus_bcc_ooo_pyc"
HDR="${GEN_DIR}/janus_bcc_ooo_pyc_gen.hpp"
if [[ ! -f "${HDR}" ]]; then
  bash "${ROOT_DIR}/janus/update_generated.sh"
fi

WORK_DIR="$(mktemp -d -t janus_bcc_ooo_pyc_tb.XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT

"${CXX:-clang++}" -std=c++17 -O2 \
  -I "${ROOT_DIR}/include" \
  -I "${GEN_DIR}" \
  -o "${WORK_DIR}/tb_janus_bcc_ooo_pyc" \
  "${ROOT_DIR}/janus/tb/tb_janus_bcc_ooo_pyc.cpp"

if [[ $# -gt 0 ]]; then
  "${WORK_DIR}/tb_janus_bcc_ooo_pyc" "$@"
else
  "${WORK_DIR}/tb_janus_bcc_ooo_pyc"
fi
