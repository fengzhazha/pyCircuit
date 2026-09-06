from __future__ import annotations

import os
from pathlib import Path

from setuptools import Distribution, find_namespace_packages, setup
from wheel.bdist_wheel import bdist_wheel as _bdist_wheel


def package_files(root: Path, package_root: Path) -> list[str]:
    files: list[str] = []
    if not root.is_dir():
        return files
    for path in root.rglob("*"):
        if path.is_file():
            files.append(str(path.relative_to(package_root)))
    return files


class BinaryDistribution(Distribution):
    def has_ext_modules(self) -> bool:
        return True


class PlatformBinaryWheel(_bdist_wheel):
    def finalize_options(self) -> None:
        super().finalize_options()
        self.root_is_pure = False

    def get_tag(self) -> tuple[str, str, str]:
        _python, _abi, plat = super().get_tag()
        return ("py3", "none", plat)


ROOT = Path(__file__).resolve().parent
PACKAGE_ROOT = ROOT / "pycircuit"

setup(
    name="pycircuit-hisi",
    version=os.environ["PYC_WHEEL_VERSION"],
    description="A Python-based hardware description framework that compiles Python to RTL through MLIR",
    long_description=(ROOT / "README.md").read_text(encoding="utf-8"),
    long_description_content_type="text/markdown",
    license="BSD-3-Clause",
    author="PTO-ISA Contributors",
    url="https://github.com/PTO-ISA/pyCircuit",
    project_urls={
        "Repository": "https://github.com/PTO-ISA/pyCircuit",
        "Issues": "https://github.com/PTO-ISA/pyCircuit/issues",
        "Releases": "https://github.com/PTO-ISA/pyCircuit/releases",
    },
    python_requires=">=3.10",
    install_requires=[
        "click>=8.0.0",
        "pyyaml>=6.0",
        f"pycircuit-semantic-core=={os.environ['PYC_WHEEL_VERSION']}",
    ],
    packages=find_namespace_packages(
        include=["pycircuit", "pycircuit.*"], exclude=["pycircuit._toolchain*"]
    ),
    package_data={
        "pycircuit": package_files(PACKAGE_ROOT / "_toolchain", PACKAGE_ROOT)
        + package_files(PACKAGE_ROOT / "_tools", PACKAGE_ROOT),
    },
    # Toolchain files are enumerated explicitly in package_data above. Disabling
    # implicit discovery avoids treating toolchain directories as Python packages.
    include_package_data=False,
    zip_safe=False,
    entry_points={
        "console_scripts": [
            "pycircuit=pycircuit.cli:main",
            "pycc=pycircuit.packaged_toolchain:main",
            "pyc-opt=pycircuit.packaged_toolchain:pyc_opt_main",
        ]
    },
    cmdclass={"bdist_wheel": PlatformBinaryWheel},
    distclass=BinaryDistribution,
)
