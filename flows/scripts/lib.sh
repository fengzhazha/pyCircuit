#!/usr/bin/env bash
set -euo pipefail

PYC_ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

pyc_log() {
  echo "[pyc] $*"
}

pyc_warn() {
  echo "[pyc][warn] $*" >&2
}

pyc_die() {
  echo "[pyc][error] $*" >&2
  exit 1
}

pyc_toolchain_root() {
  if [[ -n "${PYC_TOOLCHAIN_ROOT:-}" && -d "${PYC_TOOLCHAIN_ROOT}" ]]; then
    echo "${PYC_TOOLCHAIN_ROOT}"
    return 0
  fi

  if [[ -n "${PYCC:-}" && -x "${PYCC}" ]]; then
    local pycc_dir
    pycc_dir="$(cd -- "$(dirname -- "${PYCC}")" && pwd)"
    if [[ "$(basename -- "${pycc_dir}")" == "bin" ]]; then
      echo "$(cd -- "${pycc_dir}/.." && pwd)"
      return 0
    fi
  fi

  local candidates=("${PYC_ROOT_DIR}/.pycircuit_out/toolchain/install")
  local c=""
  for c in "${candidates[@]}"; do
    if [[ -x "${c}/bin/pycc" || -x "${c}/bin/pycc.exe" ]]; then
      echo "${c}"
      return 0
    fi
  done

  return 1
}

pyc_find_pycc() {
  if [[ -n "${PYCC:-}" && -x "${PYCC}" ]]; then
    if root="$(pyc_toolchain_root 2>/dev/null)"; then
      export PYC_TOOLCHAIN_ROOT="${root}"
    fi
    return 0
  fi

  local exe_suffix=""
  case "$(uname -s 2>/dev/null || true)" in
    MINGW*|MSYS*|CYGWIN*) exe_suffix=".exe";;
  esac

  local toolchain_root=""
  if toolchain_root="$(pyc_toolchain_root 2>/dev/null)"; then
    if [[ -x "${toolchain_root}/bin/pycc${exe_suffix}" ]]; then
      export PYC_TOOLCHAIN_ROOT="${toolchain_root}"
      export PYCC="${toolchain_root}/bin/pycc${exe_suffix}"
      return 0
    fi
    if [[ -x "${toolchain_root}/bin/pycc" ]]; then
      export PYC_TOOLCHAIN_ROOT="${toolchain_root}"
      export PYCC="${toolchain_root}/bin/pycc"
      return 0
    fi
  fi

  local candidates=(
    # Canonical repository-local install tree.
    "${PYC_ROOT_DIR}/.pycircuit_out/toolchain/install/bin/pycc${exe_suffix}"
    "${PYC_ROOT_DIR}/.pycircuit_out/toolchain/install/bin/pycc"
  )

  # Pick the newest executable among the common build locations. This avoids
  # accidentally grabbing an older `pycc` from a stale build directory.
  local best=""
  local best_mtime=0
  for c in "${candidates[@]}"; do
    if [[ -x "${c}" ]]; then
      local mtime=0
      if mtime="$(stat -f %m "${c}" 2>/dev/null)"; then
        :
      elif mtime="$(stat -c %Y "${c}" 2>/dev/null)"; then
        :
      else
        mtime=0
      fi
      if (( mtime > best_mtime )); then
        best="${c}"
        best_mtime="${mtime}"
      fi
    fi
  done
  if [[ -n "${best}" ]]; then
    export PYCC="${best}"
    if root="$(pyc_toolchain_root 2>/dev/null)"; then
      export PYC_TOOLCHAIN_ROOT="${root}"
    fi
    return 0
  fi

  if command -v pycc >/dev/null 2>&1; then
    export PYCC
    PYCC="$(command -v pycc)"
    if root="$(pyc_toolchain_root 2>/dev/null)"; then
      export PYC_TOOLCHAIN_ROOT="${root}"
    fi
    return 0
  fi

  if command -v pycc.exe >/dev/null 2>&1; then
    export PYCC
    PYCC="$(command -v pycc.exe)"
    if root="$(pyc_toolchain_root 2>/dev/null)"; then
      export PYC_TOOLCHAIN_ROOT="${root}"
    fi
    return 0
  fi

  pyc_die "missing pycc (set PYCC=... or build it with: flows/scripts/pyc build)"
}

pyc_pythonpath() {
  if [[ "${PYC_USE_INSTALLED_PYTHON_PACKAGE:-0}" == "1" ]]; then
    echo "${PYC_PYTHONPATH:-}"
    return 0
  fi

  if [[ -n "${PYC_PYTHONPATH:-}" ]]; then
    echo "${PYC_PYTHONPATH}"
    return 0
  fi

  # Prefer editable install, but use the canonical in-tree package for
  # repository-local runs.
  echo "${PYC_ROOT_DIR}/python/semantic-core/src:${PYC_ROOT_DIR}/python/pycircuit/src:${PYC_ROOT_DIR}"
}

pyc_out_root() {
  echo "${PYC_ROOT_DIR}/.pycircuit_out"
}
