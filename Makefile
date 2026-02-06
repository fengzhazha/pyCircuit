.PHONY: help configure build tools regen test install package clean

BUILD_DIR ?= build

help:
	@echo "Targets:"
	@echo "  configure  Configure CMake (needs LLVM_DIR/MLIR_DIR)"
	@echo "  tools      Build pyc-compile + pyc-opt"
	@echo "  regen      Regenerate checked-in outputs (examples/ + janus/)"
	@echo "  test       Run C++ regressions (linx_cpu + janus)"
	@echo "  install    Install into dist/pycircuit/"
	@echo "  package    Build a TGZ via CPack"
	@echo "  clean      Remove build/ and dist/"

configure:
	@if [ -z "$$LLVM_DIR" ] || [ -z "$$MLIR_DIR" ]; then \
	  echo "error: set LLVM_DIR and MLIR_DIR (see README.md)"; \
	  exit 2; \
	fi
	cmake -G Ninja -S . -B "$(BUILD_DIR)" \
	  -DCMAKE_BUILD_TYPE=Release \
	  -DLLVM_DIR="$$LLVM_DIR" \
	  -DMLIR_DIR="$$MLIR_DIR" \
	  -DPYC_BUILD_CPP_EXAMPLES=OFF

tools: configure
	ninja -C "$(BUILD_DIR)" pyc-compile pyc-opt

regen: tools
	PYC_COMPILE="$(BUILD_DIR)/bin/pyc-compile" bash examples/update_generated.sh
	PYC_COMPILE="$(BUILD_DIR)/bin/pyc-compile" bash janus/update_generated.sh

test: tools
	PYC_COMPILE="$(BUILD_DIR)/bin/pyc-compile" bash tools/run_linx_cpu_pyc_cpp.sh
	PYC_COMPILE="$(BUILD_DIR)/bin/pyc-compile" bash tools/run_fastfwd_pyc_cpp.sh
	PYC_COMPILE="$(BUILD_DIR)/bin/pyc-compile" bash janus/tools/run_janus_bcc_pyc_cpp.sh
	PYC_COMPILE="$(BUILD_DIR)/bin/pyc-compile" bash janus/tools/run_janus_bcc_ooo_pyc_cpp.sh

install: tools
	cmake --install "$(BUILD_DIR)" --prefix dist/pycircuit

package: tools
	(cd "$(BUILD_DIR)" && cpack -G TGZ)

clean:
	rm -rf "$(BUILD_DIR)" dist
