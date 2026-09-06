#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
llvm_prefix=${LLVM_PREFIX:-/opt/homebrew/opt/llvm}
python_bin=${PYTHON:-python3}

if [[ ! -x "${llvm_prefix}/bin/llvm-config" ]]; then
  echo "LLVM 22.1.8 was not found at ${llvm_prefix}" >&2
  exit 1
fi

actual_version=$("${llvm_prefix}/bin/llvm-config" --version)
if [[ "${actual_version}" != "22.1.8" ]]; then
  echo "LLVM 22.1.8 is required; found ${actual_version}" >&2
  exit 1
fi

"${python_bin}" -m venv "${repo_root}/.venv"
"${repo_root}/.venv/bin/python" -m pip install \
  --disable-pip-version-check \
  -r "${repo_root}/python/agentic-circuit/requirements-dev.lock"
"${repo_root}/.venv/bin/python" -m pip install \
  --disable-pip-version-check \
  -e "${repo_root}/python/semantic-core"
"${repo_root}/.venv/bin/python" -m pip install \
  --disable-pip-version-check \
  --no-deps \
  -e "${repo_root}/python/agentic-circuit"

echo "Development environment ready."
echo "Run: source ${repo_root}/.venv/bin/activate"
