add_acir_library(ACIRAnalysis SOURCES
  ModelAnalysis.cpp
  ProcessStateIdentity.cpp
  ProcessStateExpansion.cpp
  ProcessStateContinuation.cpp
  ProcessStateWake.cpp
  ProcessStateLiveness.cpp
  ProcessStateCost.cpp
  ProcessStatePlan.cpp
  ProcessStateReport.cpp
  VariableAnalysis.cpp
)
target_link_libraries(ACIRAnalysis PUBLIC
  ACIRDialect
  ACIRBindings
  MLIRFuncDialect
  MLIRSideEffectInterfaces
  MLIRAnalysis
  LLVMSupport
)
target_include_directories(ACIRAnalysis PUBLIC
  "$<BUILD_INTERFACE:${ACIR_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${ACIR_BINARY_DIR}/include>"
  "$<INSTALL_INTERFACE:include>"
)
target_include_directories(ACIRAnalysis PRIVATE
  "${ACIR_SOURCE_DIR}/lib"
)
add_library(AgenticCircuit::ACIRAnalysis ALIAS ACIRAnalysis)
