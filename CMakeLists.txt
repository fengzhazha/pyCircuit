cmake_minimum_required(VERSION 3.20)
project(pycircuit VERSION 0.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)

option(PYC_BUILD_MLIR_TOOLS "Build MLIR-based pyc tools" ON)
option(PYC_BUILD_CPP_EXAMPLES "Build C++ simulation examples" ON)
option(PYC_INSTALL_TEMPLATES "Install include/pyc template libraries" ON)
option(PYC_INSTALL_PYTHON "Install Python frontend sources (for binary packages)" ON)

if(PYC_BUILD_MLIR_TOOLS)
  add_subdirectory(pyc/mlir)
endif()

if(PYC_BUILD_CPP_EXAMPLES)
  add_subdirectory(examples/cpp)
endif()

if(PYC_INSTALL_TEMPLATES)
  install(DIRECTORY include/pyc DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()

if(PYC_INSTALL_PYTHON)
  install(
    DIRECTORY python/pycircuit
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pycircuit/python"
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
  )
  install(FILES pyproject.toml DESTINATION "${CMAKE_INSTALL_DATADIR}/pycircuit")
endif()

install(FILES LICENSE README.md DESTINATION "${CMAKE_INSTALL_DATADIR}/pycircuit")

# Packaging (binary tarball via CPack).
set(CPACK_PACKAGE_NAME "pycircuit" CACHE STRING "CPack package name")
set(CPACK_PACKAGE_VENDOR "pyCircuit" CACHE STRING "CPack package vendor")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "pyCircuit: Python -> PYC (MLIR) -> Verilog/C++" CACHE STRING "CPack summary")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING "CPack package version")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ" CACHE STRING "CPack generator")
include(CPack)
