// RUN: %split_file %s %t
// RUN: %not %acir_opt %t/event-token-result.mlir 2>&1 | %FileCheck %s --check-prefix=EVENT
// RUN: %not %acir_opt %t/event-resource-ref.mlir 2>&1 | %FileCheck %s --check-prefix=RESOURCE-REF

// EVENT: topology type '!ac.resource_token<@Resource>' cannot be nested inside '!ac.event<!ac.resource_token<@Resource>>'
// RESOURCE-REF: topology type '!ac.resource_ref<@Resource, @role>' cannot be nested inside '!ac.event<!ac.resource_ref<@Resource, @role>>'

//--- event-token-result.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %x = "builtin.unrealized_conversion_cast"() : () -> !ac.event<!ac.resource_token<@Resource>>
}

//--- event-resource-ref.mlir
builtin.module attributes {ac.contract_epoch = "0.5"} {
  %x = "builtin.unrealized_conversion_cast"() : () -> !ac.event<!ac.resource_ref<@Resource, @role>>
}
