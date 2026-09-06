cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(AgenticCircuit VERSION 0.1.0 LANGUAGES C CXX)
endif()

include(GNUInstallDirs)

set(ACIR_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(ACIR_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(ACIR_VERSION "0.1.0")
get_filename_component(ACIR_REPO_ROOT "${ACIR_SOURCE_DIR}/../.." ABSOLUTE)
set(ACIR_GFSIM_SOURCE_DIR "${ACIR_REPO_ROOT}/simulator/gfsim")

list(APPEND CMAKE_MODULE_PATH "${ACIR_SOURCE_DIR}/cmake/modules")
include(cmake/LLVMToolchain.cmake)
acir_configure_llvm()

function(acir_append_llvm_link_flag flag)
  set_property(GLOBAL APPEND PROPERTY ACIR_LLVM_LINK_FLAGS_PROPERTY "${flag}")
endfunction()

function(acir_collect_llvm_link_target target)
  get_property(_visited GLOBAL PROPERTY ACIR_LLVM_LINK_TARGETS_PROPERTY)
  if("${target}" IN_LIST _visited)
    return()
  endif()
  set_property(GLOBAL APPEND PROPERTY ACIR_LLVM_LINK_TARGETS_PROPERTY "${target}")

  get_target_property(_location "${target}" IMPORTED_LOCATION)
  if(NOT _location OR _location MATCHES "-NOTFOUND$")
    get_target_property(_configurations "${target}" IMPORTED_CONFIGURATIONS)
    foreach(_configuration IN LISTS _configurations)
      string(TOUPPER "${_configuration}" _configuration_upper)
      get_target_property(
        _candidate "${target}" "IMPORTED_LOCATION_${_configuration_upper}"
      )
      if(_candidate AND NOT _candidate MATCHES "-NOTFOUND$")
        set(_location "${_candidate}")
        break()
      endif()
    endforeach()
  endif()
  if(_location AND NOT _location MATCHES "-NOTFOUND$")
    acir_append_llvm_link_flag("${_location}")
  endif()

  get_target_property(_dependencies "${target}" INTERFACE_LINK_LIBRARIES)
  if(NOT _dependencies OR _dependencies MATCHES "-NOTFOUND$")
    return()
  endif()
  foreach(_dependency IN LISTS _dependencies)
    if(_dependency MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
      set(_dependency "${CMAKE_MATCH_1}")
    elseif(_dependency MATCHES "^\\$<")
      message(FATAL_ERROR
        "unsupported LLVM link dependency expression: ${_dependency}"
      )
    endif()
    if(TARGET "${_dependency}")
      acir_collect_llvm_link_target("${_dependency}")
    elseif(IS_ABSOLUTE "${_dependency}" OR _dependency MATCHES "^-")
      acir_append_llvm_link_flag("${_dependency}")
    else()
      acir_append_llvm_link_flag("-l${_dependency}")
    endif()
  endforeach()
endfunction()

set_property(GLOBAL PROPERTY ACIR_LLVM_LINK_FLAGS_PROPERTY "")
set_property(GLOBAL PROPERTY ACIR_LLVM_LINK_TARGETS_PROPERTY "")
if(TARGET LLVM)
  acir_collect_llvm_link_target(LLVM)
else()
  acir_collect_llvm_link_target(LLVMSupport)
endif()
get_property(ACIR_LLVM_LINK_FLAGS GLOBAL PROPERTY ACIR_LLVM_LINK_FLAGS_PROPERTY)

list(APPEND CMAKE_MODULE_PATH "${LLVM_DIR}" "${MLIR_DIR}")
include(TableGen)
include(AddLLVM)
include(AddMLIR)
include(AddACIR)

set(_ACIR_BUILD_TESTING_DEFAULT "${PROJECT_IS_TOP_LEVEL}")
if(DEFINED BUILD_TESTING)
  set(_ACIR_BUILD_TESTING_DEFAULT "${BUILD_TESTING}")
endif()
option(ACIR_BUILD_TESTING "Build Agentic Circuit tests"
  "${_ACIR_BUILD_TESTING_DEFAULT}")
unset(_ACIR_BUILD_TESTING_DEFAULT)
if(ACIR_BUILD_TESTING)
  include(CTest)
  enable_testing()
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  find_program(ACIR_LIT_EXECUTABLE NAMES llvm-lit lit
    HINTS "${ACIR_REPO_ROOT}/.venv/bin"
    REQUIRED
  )
  get_filename_component(ACIR_LIT_BIN_DIR "${ACIR_LIT_EXECUTABLE}" DIRECTORY)
  find_program(ACIR_TEST_PYTHON NAMES python python3
    HINTS "${ACIR_LIT_BIN_DIR}"
    REQUIRED
  )
  set(LLVM_EXTERNAL_LIT "${ACIR_LIT_EXECUTABLE}" CACHE FILEPATH
    "lit executable used by Agentic Circuit tests" FORCE
  )
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS} ${MLIR_INCLUDE_DIRS})
include_directories(
  "${ACIR_SOURCE_DIR}/include"
  "${ACIR_BINARY_DIR}/include"
)
add_definitions(${LLVM_DEFINITIONS})

option(ACIR_ENABLE_ASSERTIONS "Keep C/C++ assertions enabled" OFF)
add_library(acir_project_options INTERFACE)
add_library(AgenticCircuit::ProjectOptions ALIAS acir_project_options)
set_target_properties(acir_project_options PROPERTIES EXPORT_NAME ProjectOptions)
target_compile_features(acir_project_options INTERFACE cxx_std_20)
if(ACIR_ENABLE_ASSERTIONS)
  if(MSVC)
    target_compile_options(acir_project_options INTERFACE /UNDEBUG)
  else()
    target_compile_options(acir_project_options INTERFACE -UNDEBUG)
  endif()
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(
  "${ACIR_SOURCE_DIR}/cmake/AgenticCircuitConfig.cmake.in"
  "${ACIR_BINARY_DIR}/AgenticCircuitConfig.cmake"
  INSTALL_DESTINATION lib/cmake/AgenticCircuit
)
write_basic_package_version_file(
  "${ACIR_BINARY_DIR}/AgenticCircuitConfigVersion.cmake"
  VERSION "${ACIR_VERSION}"
  COMPATIBILITY ExactVersion
)

install(
  FILES
    "${ACIR_BINARY_DIR}/AgenticCircuitConfig.cmake"
    "${ACIR_BINARY_DIR}/AgenticCircuitConfigVersion.cmake"
  DESTINATION lib/cmake/AgenticCircuit
)
install(TARGETS acir_project_options EXPORT AgenticCircuitTargets)
install(
  EXPORT AgenticCircuitTargets
  FILE AgenticCircuitTargets.cmake
  NAMESPACE AgenticCircuit::
  DESTINATION lib/cmake/AgenticCircuit
)
install(FILES "${ACIR_SOURCE_DIR}/cmake/modules/AddACIR.cmake"
  DESTINATION lib/cmake/AgenticCircuit/modules)
install(DIRECTORY "${ACIR_SOURCE_DIR}/include/acir" DESTINATION include
  PATTERN "CMakeLists.txt" EXCLUDE)
install(DIRECTORY "${ACIR_GFSIM_SOURCE_DIR}/include/gfsim" DESTINATION include)
install(
  DIRECTORY "${ACIR_BINARY_DIR}/include/acir"
  DESTINATION include
  FILES_MATCHING
  PATTERN "*.h.inc"
  PATTERN "CMakeFiles" EXCLUDE
)

add_subdirectory(include)
add_subdirectory(lib)
add_subdirectory("${ACIR_GFSIM_SOURCE_DIR}" "${ACIR_BINARY_DIR}/gfsim")
add_subdirectory(bindings/python)
add_subdirectory(tools)

if(ACIR_BUILD_TESTING)
  # LLVM's lit target always prefixes LLVM_EXTERNAL_LIT with
  # Python3_EXECUTABLE. Use the interpreter adjacent to the discovered lit
  # entrypoint so venv-provided lit packages remain importable.
  set(Python3_EXECUTABLE "${ACIR_TEST_PYTHON}")
  add_subdirectory(
    "${ACIR_REPO_ROOT}/tests/mlir/agentic-circuit"
    "${ACIR_BINARY_DIR}/tests/mlir"
  )
  add_subdirectory(
    "${ACIR_REPO_ROOT}/tests/cpp/agentic-circuit"
    "${ACIR_BINARY_DIR}/tests/cpp"
  )

  set(_ACIR_PYTHON_TEST_ENVIRONMENT
    "AGENTIC_CIRCUIT_TEST_BUILD_DIR=${ACIR_BINARY_DIR}"
    "PYTHONPATH=${ACIR_REPO_ROOT}/python/semantic-core/src:${ACIR_REPO_ROOT}/python/agentic-circuit/src:${ACIR_BINARY_DIR}/python"
  )
  add_test(
    NAME WorkspaceE2ETests
    COMMAND "${Python3_EXECUTABLE}"
      "${ACIR_REPO_ROOT}/tests/integration/agentic-circuit/e2e/test_workspace_examples.py"
      -v
  )
  set_tests_properties(WorkspaceE2ETests PROPERTIES
    WORKING_DIRECTORY "${ACIR_REPO_ROOT}"
    TIMEOUT 300
    LABELS "e2e;workspace"
    ENVIRONMENT "${_ACIR_PYTHON_TEST_ENVIRONMENT}"
  )
  add_test(
    NAME DavinciOOTraceTests
    COMMAND "${Python3_EXECUTABLE}"
      "${ACIR_REPO_ROOT}/tests/integration/agentic-circuit/e2e/test_davincioo_trace.py"
      -v
  )
  set_tests_properties(DavinciOOTraceTests PROPERTIES
    WORKING_DIRECTORY "${ACIR_REPO_ROOT}"
    TIMEOUT 60
    LABELS "e2e;davincioo"
    ENVIRONMENT "${_ACIR_PYTHON_TEST_ENVIRONMENT}"
  )
  add_test(
    NAME RuleRetirementE2ETests
    COMMAND "${Python3_EXECUTABLE}"
      "${ACIR_REPO_ROOT}/tests/integration/agentic-circuit/e2e/test_queue_codegen.py"
      "QueueCodegenTest.test_rule_retirement_demo_runs_through_mlir_passes_and_gfsim"
      -v
  )
  set_tests_properties(RuleRetirementE2ETests PROPERTIES
    WORKING_DIRECTORY "${ACIR_REPO_ROOT}"
    TIMEOUT 60
    LABELS "e2e;rule;gfsim"
    ENVIRONMENT "${_ACIR_PYTHON_TEST_ENVIRONMENT}"
  )
  add_test(
    NAME BitfieldE2ETests
    COMMAND "${Python3_EXECUTABLE}"
      "${ACIR_REPO_ROOT}/tests/integration/agentic-circuit/e2e/test_queue_codegen.py"
      "QueueCodegenTest.test_bitfield_decode_runs_through_frozen_queuegraph_and_gfsim"
      -v
  )
  set_tests_properties(BitfieldE2ETests PROPERTIES
    WORKING_DIRECTORY "${ACIR_REPO_ROOT}"
    TIMEOUT 60
    LABELS "e2e;gfsim;bitfield"
    ENVIRONMENT "${_ACIR_PYTHON_TEST_ENVIRONMENT}"
  )
  add_test(
    NAME NestedPayloadE2ETests
    COMMAND "${Python3_EXECUTABLE}"
      "${ACIR_REPO_ROOT}/tests/integration/agentic-circuit/e2e/test_queue_codegen.py"
      "QueueCodegenTest.test_nested_payload_runs_with_dependency_ordered_cpp_types"
      -v
  )
  set_tests_properties(NestedPayloadE2ETests PROPERTIES
    WORKING_DIRECTORY "${ACIR_REPO_ROOT}"
    TIMEOUT 60
    LABELS "e2e;gfsim;nested-payload"
    ENVIRONMENT "${_ACIR_PYTHON_TEST_ENVIRONMENT}"
  )
  add_test(
    NAME EnumPayloadE2ETests
    COMMAND "${Python3_EXECUTABLE}"
      "${ACIR_REPO_ROOT}/tests/integration/agentic-circuit/e2e/test_queue_codegen.py"
      "QueueCodegenTest.test_nominal_enum_nested_payload_runs_in_gfsim"
      -v
  )
  set_tests_properties(EnumPayloadE2ETests PROPERTIES
    WORKING_DIRECTORY "${ACIR_REPO_ROOT}"
    TIMEOUT 60
    LABELS "e2e;gfsim;enum-payload"
    ENVIRONMENT "${_ACIR_PYTHON_TEST_ENVIRONMENT}"
  )
  add_test(
    NAME AggregatePayloadE2ETests
    COMMAND "${Python3_EXECUTABLE}"
      "${ACIR_REPO_ROOT}/tests/integration/agentic-circuit/e2e/test_queue_codegen.py"
      "QueueCodegenTest.test_tuple_and_value_array_payload_runs_packed_in_gfsim"
      -v
  )
  set_tests_properties(AggregatePayloadE2ETests PROPERTIES
    WORKING_DIRECTORY "${ACIR_REPO_ROOT}"
    TIMEOUT 60
    LABELS "e2e;gfsim;aggregate-payload"
    ENVIRONMENT "${_ACIR_PYTHON_TEST_ENVIRONMENT}"
  )
  add_test(
    NAME RecursiveAggregatePayloadE2ETests
    COMMAND "${Python3_EXECUTABLE}"
      "${ACIR_REPO_ROOT}/tests/integration/agentic-circuit/e2e/test_queue_codegen.py"
      "QueueCodegenTest.test_recursive_nominal_aggregate_payload_runs_packed_in_gfsim"
      -v
  )
  set_tests_properties(RecursiveAggregatePayloadE2ETests PROPERTIES
    WORKING_DIRECTORY "${ACIR_REPO_ROOT}"
    TIMEOUT 60
    LABELS "e2e;gfsim;aggregate-payload;recursive-payload"
    ENVIRONMENT "${_ACIR_PYTHON_TEST_ENVIRONMENT}"
  )
  add_test(
    NAME PycVerilogBackendTests
    COMMAND "${Python3_EXECUTABLE}"
      "${ACIR_REPO_ROOT}/tests/python/agentic-circuit/tools/test_pyc_verilog_backend.py"
      -v
  )
  set_tests_properties(PycVerilogBackendTests PROPERTIES
    WORKING_DIRECTORY "${ACIR_REPO_ROOT}"
    TIMEOUT 30
    LABELS "pyc;verilog"
    ENVIRONMENT "${_ACIR_PYTHON_TEST_ENVIRONMENT}"
  )
  unset(_ACIR_PYTHON_TEST_ENVIRONMENT)
endif()
