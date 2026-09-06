set(ACIR_TEST_LLVM_LINKER_FLAGS "")
foreach(ACIR_LLVM_LINK_FLAG IN LISTS ACIR_LLVM_LINK_FLAGS)
  string(APPEND ACIR_TEST_LLVM_LINKER_FLAGS
    " --linker-flag=${ACIR_LLVM_LINK_FLAG}"
  )
endforeach()
string(STRIP "${ACIR_TEST_LLVM_LINKER_FLAGS}" ACIR_TEST_LLVM_LINKER_FLAGS)
get_filename_component(ACIR_LIT_BIN_DIR "${ACIR_LIT_EXECUTABLE}" DIRECTORY)
find_program(ACIR_TEST_PYTHON NAMES python python3
  HINTS "${ACIR_LIT_BIN_DIR}"
  REQUIRED
)

configure_lit_site_cfg(
  "${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in"
  "${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py"
  MAIN_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py"
)

add_lit_testsuite(check-acir "Running Agentic Circuit lit tests"
  "${CMAKE_CURRENT_BINARY_DIR}"
  DEPENDS
    acir-build
    acir-cxxgen
    acir-opcode-catalog
    acir-opt
    acir-opt-internal
    acir-queue-cxxgen
    acir-queue-plan
    acir-queue-pycgen
    agentic_circuit_native
)
