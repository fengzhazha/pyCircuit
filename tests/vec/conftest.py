from __future__ import annotations

import os
import shutil
from pathlib import Path

import pytest


def pytest_configure(config: pytest.Config) -> None:
    config.addinivalue_line("markers", "vec: Vec operator generated tests")
    config.addinivalue_line(
        "markers", "verilator: tests that build/run generated Verilog with Verilator"
    )
    config.addinivalue_line("markers", "slow: slower backend integration tests")
    # Each xdist worker already builds an independent pycc case. Avoid nested
    # parallel builds exhausting CPU and memory; callers may override this.
    is_xdist_worker = hasattr(config, "workerinput")
    if is_xdist_worker:
        os.environ.setdefault("PYC_VEC_TEST_JOBS", "1")


@pytest.fixture(scope="session")
def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


@pytest.fixture(scope="session")
def pyc_pythonpath(repo_root: Path) -> str:
    parts = [
        repo_root / "python" / "semantic-core" / "src",
        repo_root / "python" / "pycircuit" / "src",
        repo_root / "designs",
        repo_root,
    ]
    return os.pathsep.join(str(p) for p in parts)


@pytest.fixture(scope="session")
def pycc(repo_root: Path) -> Path:
    env = os.environ.get("PYCC")
    candidates = [
        Path(env) if env else None,
        repo_root / ".pycircuit_out" / "toolchain" / "install" / "bin" / "pycc",
        repo_root / "build" / "bin" / "pycc",
        repo_root / ".pycircuit_out" / "layout-final" / "install" / "bin" / "pycc",
        repo_root / ".pycircuit_out" / "toolchain" / "install" / "bin" / "pycc",
        repo_root / ".pycircuit_out" / "layout-root" / "bin" / "pycc",
    ]
    for candidate in candidates:
        if (
            candidate is not None
            and candidate.is_file()
            and os.access(candidate, os.X_OK)
        ):
            return candidate
    pytest.skip("pycc not found; set PYCC or build pycc")


@pytest.fixture(scope="session")
def verilator() -> str | None:
    return shutil.which("verilator")


@pytest.fixture(scope="session")
def vec_test_root(repo_root: Path) -> Path:
    root = repo_root / ".pycircuit_out" / "vec-tests"
    root.mkdir(parents=True, exist_ok=True)
    return root
