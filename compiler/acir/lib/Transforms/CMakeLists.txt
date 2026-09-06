add_acir_library(ACIRTransforms SOURCES
  VerifyModel.cpp
  VerifyValueConstraints.cpp
  FreezeTopology.cpp
  CanonicalizeModel.cpp
  ResolveBindings.cpp
  NormalizeACIRFile.cpp
  VerifyACIRFile.cpp
  LowerProcessState.cpp
  LowerRules.cpp
  LowerVariables.cpp
)
add_dependencies(ACIRTransforms ACIRTransformsIncGen)
target_link_libraries(ACIRTransforms PUBLIC
  ACIRAnalysis
  ACIRBindings
  ACIRDialect
  MLIRPass
)
target_include_directories(ACIRTransforms PUBLIC
  "$<BUILD_INTERFACE:${ACIR_SOURCE_DIR}/include>"
  "$<BUILD_INTERFACE:${ACIR_BINARY_DIR}/include>"
  "$<INSTALL_INTERFACE:include>"
)
target_include_directories(ACIRTransforms PRIVATE
  "${ACIR_SOURCE_DIR}/lib"
)
add_library(AgenticCircuit::ACIRTransforms ALIAS ACIRTransforms)
