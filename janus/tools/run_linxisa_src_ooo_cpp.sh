#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"

LLVM_BIN="${LLVM_LINXISA_BIN:-${HOME}/llvm-project/build-linxisa-clang/bin}"
CLANG="${LLVM_BIN}/clang"
LD="${LLVM_BIN}/ld.lld"
OBJCOPY="${LLVM_BIN}/llvm-objcopy"

LINX_LD_SCRIPT="${LINX_LD_SCRIPT:-${HOME}/linx-libc/linx.ld}"

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <program.{c,S}> [expected_mem100] [expected_a0]" >&2
  exit 2
fi

SRC="$1"
shift

if [[ ! -x "${CLANG}" ]]; then
  echo "error: missing clang at ${CLANG} (set LLVM_LINXISA_BIN=...)" >&2
  exit 1
fi
if [[ ! -x "${LD}" ]]; then
  echo "error: missing ld.lld at ${LD} (set LLVM_LINXISA_BIN=...)" >&2
  exit 1
fi
if [[ ! -x "${OBJCOPY}" ]]; then
  echo "error: missing llvm-objcopy at ${OBJCOPY} (set LLVM_LINXISA_BIN=...)" >&2
  exit 1
fi
if [[ ! -f "${LINX_LD_SCRIPT}" ]]; then
  echo "error: missing linker script at ${LINX_LD_SCRIPT} (set LINX_LD_SCRIPT=...)" >&2
  exit 1
fi

WORK_DIR="$(mktemp -d -t linxisa_ooo_cpp.XXXXXX)"
trap 'rm -rf "${WORK_DIR}"' EXIT

BASE="$(basename -- "${SRC}")"
NAME="${BASE%.*}"

OBJ="${WORK_DIR}/${NAME}.o"
ELF="${WORK_DIR}/${NAME}.elf"
HEX="${WORK_DIR}/${NAME}.hex"
MEMH="${WORK_DIR}/${NAME}.memh"

"${CLANG}" --target=linx64-unknown-elf -nostdlib -ffreestanding -c -o "${OBJ}" "${SRC}"
"${LD}" -m elf64linx -T "${LINX_LD_SCRIPT}" -o "${ELF}" "${OBJ}"
"${OBJCOPY}" -O ihex "${ELF}" "${HEX}"
python3 "${ROOT_DIR}/hw/tools/ihex_to_memh.py" "${HEX}" "${MEMH}"

"${ROOT_DIR}/hw/tools/run_janus_bcc_ooo_pyc_cpp.sh" "${MEMH}" "$@"

