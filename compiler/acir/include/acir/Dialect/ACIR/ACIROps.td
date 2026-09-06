#ifndef ACIR_DIALECT_ACIR_ACIROPS_TD
#define ACIR_DIALECT_ACIR_ACIROPS_TD

include "acir/Dialect/ACIR/ACIRInterfaces.td"
include "acir/Dialect/ACIR/ACIRTypes.td"
include "mlir/IR/SymbolInterfaces.td"
include "mlir/IR/RegionKindInterface.td"
include "mlir/Interfaces/DataLayoutInterfaces.td"
include "mlir/Interfaces/ControlFlowInterfaces.td"
include "mlir/Interfaces/FunctionInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

class ACIR_Op<string mnemonic, list<Trait> traits = []>
    : Op<ACIR_Dialect, mnemonic, traits>;

def ACIR_TypeScopeOp : ACIR_Op<"type_scope", [
    Symbol, SymbolTable, SingleBlock, NoTerminator,
    DeclareOpInterfaceMethods<DataLayoutOpInterface>
  ]> {
  let arguments = (ins SymbolNameAttr:$sym_name);
  let regions = (region SizedRegion<1>:$body);
  let builders = [OpBuilder<(ins "::llvm::StringRef":$name), [{
    $_state.addAttribute(::mlir::SymbolTable::getSymbolAttrName(),
                        $_builder.getStringAttr(name));
    $_state.addRegion();
  }]>];
  let assemblyFormat = "$sym_name $body attr-dict";
}

def ACIR_TypeAliasOp : ACIR_Op<"type_alias", [Symbol]> {
  let arguments = (ins SymbolNameAttr:$sym_name, TypeAttr:$target);
  let hasVerifier = 1;
}

class ACIR_RecordDeclarationOp<string mnemonic>
    : ACIR_Op<mnemonic, [Symbol, ACIR_RecordLike]> {
  let arguments = (ins SymbolNameAttr:$sym_name, ArrayAttr:$fields);
  let hasVerifier = 1;
  let assemblyFormat = "$sym_name `fields` $fields attr-dict";
}

def ACIR_StructOp : ACIR_RecordDeclarationOp<"struct">;

def ACIR_BitfieldOp : ACIR_Op<"bitfield", [Symbol]> {
  let arguments = (ins SymbolNameAttr:$sym_name, I64Attr:$width,
                       StrAttr:$fingerprint, ArrayAttr:$fields);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `width` $width `fingerprint` $fingerprint `fields` $fields
    attr-dict
  }];
}

def ACIR_EnumOp : ACIR_Op<"enum", [Symbol]> {
  let arguments = (ins SymbolNameAttr:$sym_name, StrArrayAttr:$enumerants);
  let hasVerifier = 1;
  let assemblyFormat = "$sym_name `enumerants` $enumerants attr-dict";
}

def ACIR_UnionOp : ACIR_Op<"union", [Symbol]> {
  let arguments = (ins SymbolNameAttr:$sym_name, ArrayAttr:$fields,
                       StrAttr:$discriminator);
  let hasVerifier = 1;
}

def ACIR_PacketOp : ACIR_Op<"packet", [Symbol, ACIR_RecordLike]> {
  let arguments = (ins SymbolNameAttr:$sym_name, ArrayAttr:$fields);
  let hasVerifier = 1;
}

def ACIR_TransactionOp : ACIR_RecordDeclarationOp<"transaction">;

def ACIR_RecordCreateOp : ACIR_Op<"record.create", [Pure]> {
  let arguments = (ins Variadic<AnyType>:$values, StrArrayAttr:$field_names);
  let results = (outs AnyType:$result);
  let hasVerifier = 1;
}

def ACIR_RecordGetOp : ACIR_Op<"record.get", [Pure]> {
  let arguments = (ins AnyType:$record, StrAttr:$field);
  let results = (outs AnyType:$result);
  let hasVerifier = 1;
}

def ACIR_RecordWithOp : ACIR_Op<"record.with", [Pure]> {
  let arguments = (ins AnyType:$record, AnyType:$value, StrAttr:$field);
  let results = (outs AnyType:$result);
  let hasVerifier = 1;
}

def ACIR_VarConstantOp : ACIR_Op<"var.constant", [Pure]> {
  let arguments = (ins AnyAttr:$value);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = "$value attr-dict `as` qualified(type($result))";
}

def ACIR_VarEnumOp : ACIR_Op<"var.enum", [Pure]> {
  let arguments = (ins SymbolRefAttr:$declaration, StrAttr:$enumerant);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $declaration $enumerant attr-dict `:` qualified(type($result))
  }];
}

def ACIR_VarTupleOp : ACIR_Op<"var.tuple", [Pure]> {
  let arguments = (ins Variadic<ACIR_VarType>:$values);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $values attr-dict `:` type($values) `->` qualified(type($result))
  }];
}

def ACIR_VarArrayOp : ACIR_Op<"var.array", [Pure]> {
  let arguments = (ins Variadic<ACIR_VarType>:$values);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $values attr-dict `:` type($values) `->` qualified(type($result))
  }];
}

def ACIR_VarElementOp : ACIR_Op<"var.element", [Pure]> {
  let arguments = (ins ACIR_VarType:$aggregate, I64Attr:$index);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $aggregate `at` $index attr-dict `:` qualified(type($aggregate)) `->`
    qualified(type($result))
  }];
}

class ACIR_VarBinaryOp<string mnemonic> : ACIR_Op<mnemonic, [
    Pure, AllTypesMatch<["lhs", "rhs", "result"]>
  ]> {
  let arguments = (ins ACIR_VarType:$lhs, ACIR_VarType:$rhs);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $lhs `,` $rhs attr-dict `:` qualified(type($result))
  }];
}

def ACIR_VarAddOp : ACIR_VarBinaryOp<"var.add">;
def ACIR_VarSubOp : ACIR_VarBinaryOp<"var.sub">;
def ACIR_VarMulOp : ACIR_VarBinaryOp<"var.mul">;
def ACIR_VarAndOp : ACIR_VarBinaryOp<"var.and">;
def ACIR_VarOrOp : ACIR_VarBinaryOp<"var.or">;
def ACIR_VarXorOp : ACIR_VarBinaryOp<"var.xor">;
def ACIR_VarShlOp : ACIR_VarBinaryOp<"var.shl">;
def ACIR_VarShrOp : ACIR_VarBinaryOp<"var.shr">;

def ACIR_VarMatchesOp : ACIR_Op<"var.matches", [Pure]> {
  let arguments = (ins ACIR_VarType:$input, UI64Attr:$mask,
                       UI64Attr:$value);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $input `mask` $mask `value` $value attr-dict `:`
    qualified(type($input)) `->` qualified(type($result))
  }];
}

def ACIR_VarNotOp : ACIR_Op<"var.not", [Pure]> {
  let arguments = (ins ACIR_VarType:$in);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = "$in attr-dict `:` qualified(type($in)) `->` qualified(type($result))";
}

def ACIR_VarPopcountOp : ACIR_Op<"var.popcount", [Pure]> {
  let arguments = (ins ACIR_VarType:$in);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = "$in attr-dict `:` qualified(type($in)) `->` qualified(type($result))";
}

def ACIR_VarCountZerosOp : ACIR_Op<"var.count_zeros", [Pure]> {
  let arguments = (ins ACIR_VarType:$in, StrAttr:$direction);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = "$in `direction` $direction attr-dict `:` qualified(type($in)) `->` qualified(type($result))";
}

def ACIR_VarPriorityEncodeOp : ACIR_Op<"var.priority_encode", [Pure]> {
  let arguments = (ins ACIR_VarType:$in, StrAttr:$order);
  let results = (outs ACIR_VarType:$index, ACIR_VarType:$valid);
  let hasVerifier = 1;
  let assemblyFormat = "$in `order` $order attr-dict `:` qualified(type($in)) `->` qualified(type($index)) `,` qualified(type($valid))";
}

def ACIR_VarCmpOp : ACIR_Op<"var.cmp", [
    Pure, AllTypesMatch<["lhs", "rhs"]>
  ]> {
  let arguments = (ins StrAttr:$predicate, ACIR_VarType:$lhs,
                       ACIR_VarType:$rhs);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $predicate $lhs `,` $rhs attr-dict `:` qualified(type($lhs)) `->`
    qualified(type($result))
  }];
}

def ACIR_VarSelectOp : ACIR_Op<"var.select", [
    Pure, AllTypesMatch<["true_value", "false_value", "result"]>
  ]> {
  let arguments = (ins ACIR_VarType:$condition, ACIR_VarType:$true_value,
                       ACIR_VarType:$false_value);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $condition `,` $true_value `,` $false_value attr-dict `:`
    qualified(type($condition)) `,` qualified(type($true_value)) `->`
    qualified(type($result))
  }];
}

def ACIR_VarExtractOp : ACIR_Op<"var.extract", [Pure]> {
  let arguments = (ins ACIR_VarType:$input, I64Attr:$lsb, I64Attr:$width);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $input `from` $lsb `width` $width attr-dict `:` qualified(type($input))
    `->` qualified(type($result))
  }];
}

def ACIR_VarConcatOp : ACIR_Op<"var.concat", [Pure]> {
  let arguments = (ins Variadic<ACIR_VarType>:$inputs);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $inputs attr-dict `:` type($inputs) `->` qualified(type($result))
  }];
}

def ACIR_VarInsertOp : ACIR_Op<"var.insert", [Pure]> {
  let arguments = (ins ACIR_VarType:$base, ACIR_VarType:$value, I64Attr:$lsb);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $base `,` $value `at` $lsb attr-dict `:` qualified(type($base)) `,`
    qualified(type($value)) `->` qualified(type($result))
  }];
}

def ACIR_VarGetOp : ACIR_Op<"var.get", [Pure]> {
  let arguments = (ins ACIR_VarType:$record, StrAttr:$field);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $record `field` $field attr-dict `:` qualified(type($record)) `->`
    qualified(type($result))
  }];
}

def ACIR_VarWithOp : ACIR_Op<"var.with", [Pure]> {
  let arguments = (ins ACIR_VarType:$record, ACIR_VarType:$value,
                       StrAttr:$field);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $record `,` $value `field` $field attr-dict `:`
    qualified(type($record)) `,` qualified(type($value)) `->`
    qualified(type($result))
  }];
}

// A lexical, persistent ac.var identity. Python module/class fields lower to
// this generic variable form before storage selection chooses a concrete
// committed-state implementation.
def ACIR_VarDeclOp : ACIR_Op<"var.decl"> {
  let arguments = (ins SymbolNameAttr:$sym_name, TypeAttr:$value_type,
                       AnyAttr:$init, StrAttr:$owner, StrAttr:$stable_id,
                       OptionalAttr<DenseI64ArrayAttr>:$shape);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `type` $value_type `init` $init `owner` $owner
    `stable_id` $stable_id (`shape` $shape^)? attr-dict
  }];
}

def ACIR_VarReadOp : ACIR_Op<"var.read", [Pure]> {
  let arguments = (ins FlatSymbolRefAttr:$variable);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = "$variable attr-dict `:` qualified(type($result))";
}

def ACIR_VarReadElementOp : ACIR_Op<"var.read_element", [Pure]> {
  let arguments = (ins FlatSymbolRefAttr:$variable, ACIR_VarType:$index);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $variable `[` $index `]` attr-dict `:` qualified(type($index)) `->`
    qualified(type($result))
  }];
}

// A firing-local next-state assignment. It never commits independently.
def ACIR_VarAssignOp : ACIR_Op<"var.assign"> {
  let arguments = (ins FlatSymbolRefAttr:$variable, ACIR_VarType:$value,
                       Optional<ACIR_VarType>:$when);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $variable `=` $value (`when` $when^ `:` qualified(type($when)))?
    attr-dict `:` qualified(type($value))
  }];
}

def ACIR_VarAssignElementOp : ACIR_Op<"var.assign_element"> {
  let arguments = (ins FlatSymbolRefAttr:$variable, ACIR_VarType:$index,
                       ACIR_VarType:$value, Optional<ACIR_VarType>:$when);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $variable `[` $index `]` `=` $value
    (`when` $when^ `:` qualified(type($when)))? attr-dict `:`
    qualified(type($index)) `,` qualified(type($value))
  }];
}

// Collection queries over a lexical shaped ac.var. These remain independent
// of the concrete committed-state storage selected by later passes.
def ACIR_VarMatchOp : ACIR_Op<"var.match", [SingleBlock]> {
  let arguments = (ins FlatSymbolRefAttr:$variable);
  let results = (outs ACIR_VarType:$mask);
  let regions = (region SizedRegion<1>:$predicate);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $variable `predicate` $predicate attr-dict `->` qualified(type($mask))
  }];
}

def ACIR_VarMatchYieldOp : ACIR_Op<"var.match.yield", [
    Pure, Terminator, HasParent<"VarMatchOp">
  ]> {
  let arguments = (ins ACIR_VarType:$value);
  let assemblyFormat = "$value attr-dict `:` qualified(type($value))";
}

def ACIR_VarChooseOp : ACIR_Op<"var.choose"> {
  let arguments = (ins FlatSymbolRefAttr:$variable, ACIR_VarType:$mask,
                       I64Attr:$count, StrAttr:$policy);
  let results = (outs ACIR_VarType:$index, ACIR_VarType:$valid);
  let regions = (region AnyRegion:$key);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $variable $mask `:` qualified(type($mask)) `count` $count
    `policy` $policy `key` $key attr-dict `->`
    qualified(type($index)) `,` qualified(type($valid))
  }];
}

def ACIR_VarChooseYieldOp : ACIR_Op<"var.choose.yield", [
    Pure, Terminator, HasParent<"VarChooseOp">
  ]> {
  let arguments = (ins ACIR_VarType:$value);
  let assemblyFormat = "$value attr-dict `:` qualified(type($value))";
}

def ACIR_PacketSerializeOp : ACIR_Op<"packet.serialize", [Pure]> {
  let arguments = (ins AnyType:$packet_value, SymbolRefAttr:$packet);
  let results = (outs AnyType:$bytes);
  let hasVerifier = 1;
}

def ACIR_PacketDeserializeOp : ACIR_Op<"packet.deserialize", [Pure]> {
  let arguments = (ins AnyType:$bytes, SymbolRefAttr:$packet);
  let results = (outs AnyType:$packet_value);
  let hasVerifier = 1;
}

def ACIR_TransformOp : ACIR_Op<"transform", [SingleBlock]> {
  let arguments = (ins Variadic<ACIR_QueueType>:$inputs,
                       DenseI64ArrayAttr:$output_depths,
                       DenseI64ArrayAttr:$output_latencies);
  let results = (outs Variadic<ACIR_QueueType>:$outputs);
  let regions = (region SizedRegion<1>:$body);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $inputs `depths` $output_depths `latencies` $output_latencies
    $body attr-dict `:` functional-type($inputs, $outputs)
  }];
}

def ACIR_TransformYieldOp : ACIR_Op<"transform.yield", [
    Pure, Terminator, HasParent<"TransformOp">
  ]> {
  let arguments = (ins Variadic<ACIR_VarType>:$values);
  let assemblyFormat = "attr-dict ($values^ `:` type($values))?";
}

// Transient rule IR.  It is legal only before the rule materialization
// pipeline and must not survive topology freeze.
def ACIR_RuleOp : ACIR_Op<"rule", [SingleBlock, RecursiveMemoryEffects]> {
  let arguments = (ins Variadic<ACIR_QueueType>:$inputs,
                       DenseI64ArrayAttr:$output_depths,
                       DenseI64ArrayAttr:$output_latencies,
                       StrAttr:$name, StrAttr:$stable_id,
                       StrAttr:$time_domain,
                       ACIR_TypeConstraintStateAttr:$type_state,
                       ACIR_ValueFactKindAttr:$input_fact);
  let results = (outs Variadic<ACIR_QueueType>:$outputs);
  let regions = (region SizedRegion<1>:$body);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $inputs `depths` $output_depths `latencies` $output_latencies
    `name` $name `stable_id` $stable_id `domain` $time_domain
    `type` $type_state `input_fact` $input_fact $body attr-dict `:`
    functional-type($inputs, $outputs)
  }];
}

def ACIR_RuleReturnOp : ACIR_Op<"rule.return", [
    Pure, Terminator, HasParent<"RuleOp">
  ]> {
  let arguments = (ins Variadic<ACIR_VarType>:$values);
  let assemblyFormat = "attr-dict ($values^ `:` type($values))?";
}

def ACIR_RuleConditionOp : ACIR_Op<"rule.condition", [HasParent<"RuleOp">]> {
  let arguments = (ins ACIR_VarType:$condition);
  let hasVerifier = 1;
  let assemblyFormat = "$condition attr-dict `:` qualified(type($condition))";
}

def ACIR_RuleOutputOp : ACIR_Op<"rule.output", [HasParent<"RuleOp">]> {
  let arguments = (ins ACIR_VarType:$value, ACIR_VarType:$when,
                       I64Attr:$ordinal);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $value `when` $when `ordinal` $ordinal attr-dict `:`
    qualified(type($value)) `,` qualified(type($when))
  }];
}

// Compiler-owned proof that a functional predicate depends on a committed
// state snapshot. It is not a write and never commits independently.
def ACIR_StateSnapshotOp : ACIR_Op<"state.snapshot"> {
  let arguments = (ins FlatSymbolRefAttr:$table, Optional<ACIR_VarType>:$index,
                       ACIR_VarType:$predicate,
                       ACIR_RuleIndexKindAttr:$index_kind,
                       ArrayAttr:$read_fields);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $table (`[` $index^ `:` qualified(type($index)) `]`)?
    `for` $predicate `:` qualified(type($predicate))
    `kind` $index_kind `read_fields` $read_fields attr-dict
  }];
}

// A dynamic snapshot set produced by the region-local TableGet indices that
// are actually evaluated by one table.match predicate or table.choose key.
def ACIR_StateSnapshotSetOp : ACIR_Op<"state.snapshot_set"> {
  let arguments = (ins FlatSymbolRefAttr:$table, ACIR_VarType:$source,
                       ACIR_VarType:$predicate, ArrayAttr:$read_fields);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $table `from` $source `:` qualified(type($source))
    `for` $predicate `:` qualified(type($predicate))
    `read_fields` $read_fields attr-dict
  }];
}

// The three marker classes are deliberately distinct operations.  They are
// not an attribute bag: each class has its own state lattice and resolver.
class ACIR_PureRuleMarkerOp<string mnemonic> : ACIR_Op<mnemonic, [
    AllTypesMatch<["input", "result"]>, HasParent<"RuleOp">
  ]> {
  let arguments = (ins ACIR_VarType:$input);
  let results = (outs ACIR_VarType:$result);
}

def ACIR_TypeConstraintMarkerOp : ACIR_PureRuleMarkerOp<"marker.type"> {
  let arguments = (ins ACIR_VarType:$input,
                       ACIR_TypeConstraintStateAttr:$state,
                       ACIR_TypeConstraintKindAttr:$constraint);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $input `state` $state `constraint` $constraint attr-dict `:`
    qualified(type($result))
  }];
}

def ACIR_ValueFactMarkerOp : ACIR_PureRuleMarkerOp<"marker.value"> {
  let arguments = (ins ACIR_VarType:$input, ACIR_ValueFactKindAttr:$fact,
                       StrAttr:$identity, StrAttr:$path_predicate);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $input `fact` $fact `identity` $identity `path` $path_predicate
    attr-dict `:` qualified(type($result))
  }];
}

def ACIR_PendingObligationMarkerOp : ACIR_Op<"marker.obligation", [
    AllTypesMatch<["input", "result"]>, HasParent<"RuleOp">
  ]> {
  let arguments = (ins ACIR_VarType:$input, ACIR_ObligationStateAttr:$state,
                       ACIR_ObligationResolverAttr:$resolver,
                       StrAttr:$origin, StrAttr:$path_predicate);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $input `state` $state `resolver` $resolver `origin` $origin
    `path` $path_predicate attr-dict `:`
    qualified(type($result))
  }];
}

def ACIR_SourceOp : ACIR_Op<"source", [
    DeclareOpInterfaceMethods<MemoryEffectsOpInterface>
  ]> {
  let arguments = (ins I64Attr:$depth, I64Attr:$latency);
  let results = (outs ACIR_QueueType:$output);
  let hasVerifier = 1;
  let assemblyFormat = [{
    `depth` $depth `latency` $latency attr-dict `:`
    qualified(type($output))
  }];
}

def ACIR_SinkOp : ACIR_Op<"sink", [
    DeclareOpInterfaceMethods<MemoryEffectsOpInterface>
  ]> {
  let arguments = (ins ACIR_QueueType:$input);
  let assemblyFormat = "$input attr-dict `:` qualified(type($input))";
}

def ACIR_ObserveOp : ACIR_Op<"observe"> {
  let arguments = (ins ACIR_QueueType:$input, StrAttr:$name);
  let hasVerifier = 1;
  let assemblyFormat = "$input `name` $name attr-dict `:` qualified(type($input))";
}

def ACIR_ExpectOp : ACIR_Op<"expect", [SingleBlock]> {
  let arguments = (ins ACIR_QueueType:$input, StrAttr:$message);
  let regions = (region SizedRegion<1>:$predicate);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $input `message` $message $predicate attr-dict `:` qualified(type($input))
  }];
}

def ACIR_ExpectYieldOp : ACIR_Op<"expect.yield", [
    Pure, Terminator, HasParent<"ExpectOp">
  ]> {
  let arguments = (ins ACIR_VarType:$condition);
  let assemblyFormat = "$condition attr-dict `:` qualified(type($condition))";
}

def ACIR_BroadcastOp : ACIR_Op<"broadcast"> {
  let arguments = (ins ACIR_QueueType:$input,
                       DenseI64ArrayAttr:$output_depths,
                       DenseI64ArrayAttr:$output_latencies);
  let results = (outs Variadic<ACIR_QueueType>:$outputs);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $input `depths` $output_depths `latencies` $output_latencies attr-dict
    `:` qualified(type($input)) `->` `(` type($outputs) `)`
  }];
}

def ACIR_ForkOp : ACIR_Op<"fork"> {
  let arguments = (ins ACIR_QueueType:$input,
                       DenseI64ArrayAttr:$output_depths,
                       DenseI64ArrayAttr:$output_latencies);
  let results = (outs Variadic<ACIR_QueueType>:$outputs);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $input `depths` $output_depths `latencies` $output_latencies attr-dict
    `:` qualified(type($input)) `->` `(` type($outputs) `)`
  }];
}

def ACIR_RouteOp : ACIR_Op<"route", [SingleBlock]> {
  let arguments = (ins ACIR_QueueType:$input,
                       DenseI64ArrayAttr:$output_depths,
                       DenseI64ArrayAttr:$output_latencies);
  let results = (outs Variadic<ACIR_QueueType>:$outputs);
  let regions = (region SizedRegion<1>:$selector);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $input `depths` $output_depths `latencies` $output_latencies $selector
    attr-dict `:` qualified(type($input)) `->` `(` type($outputs) `)`
  }];
}

def ACIR_RouteYieldOp : ACIR_Op<"route.yield", [
    Pure, Terminator, HasParent<"RouteOp">
  ]> {
  let arguments = (ins ACIR_VarType:$selector);
  let assemblyFormat = "$selector attr-dict `:` qualified(type($selector))";
}

def ACIR_SelectOp : ACIR_Op<"select", [SingleBlock]> {
  let arguments = (ins Variadic<ACIR_QueueType>:$inputs,
                       I64Attr:$depth, I64Attr:$latency);
  let results = (outs ACIR_QueueType:$output);
  let regions = (region AnyRegion:$key);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $inputs `depth` $depth `latency` $latency `key` $key attr-dict `:`
    functional-type($inputs, $output)
  }];
}

def ACIR_SelectYieldOp : ACIR_Op<"select.yield", [
    Pure, Terminator, HasParent<"SelectOp">
  ]> {
  let arguments = (ins ACIR_VarType:$selector);
  let assemblyFormat = "$selector attr-dict `:` qualified(type($selector))";
}

def ACIR_MergeOp : ACIR_Op<"merge"> {
  let arguments = (ins Variadic<ACIR_QueueType>:$inputs, StrAttr:$policy,
                       I64Attr:$depth, I64Attr:$latency);
  let results = (outs ACIR_QueueType:$output);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $inputs `policy` $policy `depth` $depth `latency` $latency attr-dict
    `:` functional-type($inputs, $output)
  }];
}

def ACIR_BarrierOp : ACIR_Op<"barrier"> {
  let arguments = (ins Variadic<ACIR_QueueType>:$inputs,
                       DenseI64ArrayAttr:$output_depths,
                       DenseI64ArrayAttr:$output_latencies);
  let results = (outs Variadic<ACIR_QueueType>:$outputs);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $inputs `depths` $output_depths `latencies` $output_latencies attr-dict
    `:` functional-type($inputs, $outputs)
  }];
}

def ACIR_ReorderOp : ACIR_Op<"reorder", [SingleBlock]> {
  let arguments = (ins ACIR_QueueType:$input, I64Attr:$capacity,
                       I64Attr:$start, I64Attr:$depth, I64Attr:$latency);
  let results = (outs ACIR_QueueType:$output);
  let regions = (region SizedRegion<1>:$key);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $input `capacity` $capacity `start` $start `depth` $depth
    `latency` $latency $key attr-dict `:` qualified(type($input)) `->`
    qualified(type($output))
  }];
}

def ACIR_ReorderYieldOp : ACIR_Op<"reorder.yield", [
    Pure, Terminator, HasParent<"ReorderOp">
  ]> {
  let arguments = (ins ACIR_VarType:$key);
  let assemblyFormat = "$key attr-dict `:` qualified(type($key))";
}

def ACIR_DependencyOp : ACIR_Op<"dependency", [SingleBlock]> {
  let arguments = (ins ACIR_QueueType:$input, I64Attr:$capacity,
                       I64Attr:$resources, I64Attr:$no_dependency,
                       I64Attr:$depth, I64Attr:$latency);
  let results = (outs ACIR_QueueType:$output);
  let regions = (region SizedRegion<1>:$key,
                        SizedRegion<1>:$waits_for,
                        SizedRegion<1>:$resource,
                        SizedRegion<1>:$cost);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $input `capacity` $capacity `resources` $resources
    `no_dependency` $no_dependency
    `depth` $depth `latency` $latency `key` $key
    `waits_for` $waits_for `resource` $resource `cost` $cost attr-dict `:`
    qualified(type($input)) `->` qualified(type($output))
  }];
}

def ACIR_DependencyYieldOp : ACIR_Op<"dependency.yield", [
    Pure, Terminator, HasParent<"DependencyOp">
  ]> {
  let arguments = (ins ACIR_VarType:$value);
  let assemblyFormat = "$value attr-dict `:` qualified(type($value))";
}

def ACIR_CreditOp : ACIR_Op<"credit", [SingleBlock]> {
  let arguments = (ins ACIR_QueueType:$input, I64Attr:$credits,
                       I64Attr:$depth, I64Attr:$latency);
  let results = (outs ACIR_QueueType:$output);
  let regions = (region SizedRegion<1>:$cost);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $input `credits` $credits `depth` $depth `latency` $latency `cost` $cost
    attr-dict `:` qualified(type($input)) `->` qualified(type($output))
  }];
}

def ACIR_CreditYieldOp : ACIR_Op<"credit.yield", [
    Pure, Terminator, HasParent<"CreditOp">
  ]> {
  let arguments = (ins ACIR_VarType:$cost);
  let assemblyFormat = "$cost attr-dict `:` qualified(type($cost))";
}

def ACIR_MemoryInstanceOp : ACIR_Op<"memory.instance", [Symbol]> {
  let arguments = (ins SymbolNameAttr:$sym_name, TypeAttr:$data_type,
                       I64Attr:$entries, I64Attr:$init, I64Attr:$latency,
                       StrAttr:$owner, StrAttr:$stable_id);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `data` $data_type `entries` $entries `init` $init
    `latency` $latency `owner` $owner `stable_id` $stable_id attr-dict
  }];
}

def ACIR_MemoryRequestOp : ACIR_Op<"memory.request", [SingleBlock]> {
  let arguments = (ins FlatSymbolRefAttr:$instance, ACIR_QueueType:$input,
                       I64Attr:$ordinal, StrAttr:$result_field,
                       I64Attr:$depth);
  let results = (outs ACIR_QueueType:$output);
  let regions = (region SizedRegion<1>:$address,
                        SizedRegion<1>:$write,
                        SizedRegion<1>:$data);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $instance `,` $input `ordinal` $ordinal `result_field` $result_field
    `depth` $depth `address` $address
    `write` $write `data` $data attr-dict `:` qualified(type($input)) `->`
    qualified(type($output))
  }];
}

def ACIR_MemoryYieldOp : ACIR_Op<"memory.yield", [
    Pure, Terminator, HasParent<"MemoryRequestOp">
  ]> {
  let arguments = (ins ACIR_VarType:$value);
  let assemblyFormat = "$value attr-dict `:` qualified(type($value))";
}

def ACIR_TableOp : ACIR_Op<"table", [
    DeclareOpInterfaceMethods<MemoryEffectsOpInterface>
  ]> {
  let arguments = (ins SymbolNameAttr:$sym_name, TypeAttr:$entry_type,
                       I64Attr:$entries, I64Attr:$init, StrAttr:$owner,
                       StrAttr:$stable_id);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `entry` $entry_type `entries` $entries `init` $init
    `owner` $owner `stable_id` $stable_id attr-dict
  }];
}

def ACIR_TableGetOp : ACIR_Op<"table.get", [
    DeclareOpInterfaceMethods<MemoryEffectsOpInterface>
  ]> {
  let arguments = (ins FlatSymbolRefAttr:$table, ACIR_VarType:$index);
  let results = (outs ACIR_VarType:$result);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $table `[` $index `]` attr-dict `:` qualified(type($index)) `->`
    qualified(type($result))
  }];
}

// A firing-local Table next-state proposal. It is legal only inside transient
// ac.rule or closed ac.firing and does not commit independently.
def ACIR_TableProposeOp : ACIR_Op<"table.propose"> {
  let arguments = (ins FlatSymbolRefAttr:$table, ACIR_VarType:$index,
                       ACIR_VarType:$value, Optional<ACIR_VarType>:$when,
                       StrAttr:$mode,
                       StrArrayAttr:$write_fields);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $table `[` $index `]` `=` $value
    (`when` $when^ `:` qualified(type($when)))? `mode` $mode
    `write_fields` $write_fields attr-dict `:` qualified(type($index)) `,`
    qualified(type($value))
  }];
}

def ACIR_TableReadOp : ACIR_Op<"table.read", [
    SingleBlock, DeclareOpInterfaceMethods<MemoryEffectsOpInterface>
  ]> {
  let arguments = (ins FlatSymbolRefAttr:$table,
                       Optional<ACIR_QueueType>:$input,
                       I64Attr:$depth, I64Attr:$latency);
  let results = (outs ACIR_QueueType:$output);
  let regions = (region SizedRegion<1>:$address,
                        SizedRegion<1>:$when);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $table (`,` $input^ `:` qualified(type($input)))?
    `depth` $depth `latency` $latency `address` $address `when` $when
    attr-dict `->` qualified(type($output))
  }];
}

def ACIR_TableWriteOp : ACIR_Op<"table.write", [
    SingleBlock, DeclareOpInterfaceMethods<MemoryEffectsOpInterface>
  ]> {
  let arguments = (ins FlatSymbolRefAttr:$table,
                       Optional<ACIR_QueueType>:$input,
                       StrAttr:$mode,
                       StrArrayAttr:$write_fields);
  let regions = (region SizedRegion<1>:$address,
                        SizedRegion<1>:$enable,
                        SizedRegion<1>:$value);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $table (`,` $input^ `:` qualified(type($input)))?
    `mode` $mode `write_fields` $write_fields `address` $address `enable` $enable
    `value` $value attr-dict
  }];
}

def ACIR_TableMaskedWriteOp : ACIR_Op<"table.masked_write", [
    SingleBlock, DeclareOpInterfaceMethods<MemoryEffectsOpInterface>
  ]> {
  let arguments = (ins FlatSymbolRefAttr:$table, ACIR_VarType:$mask,
                       StrAttr:$mode,
                       StrArrayAttr:$write_fields);
  let regions = (region SizedRegion<1>:$enable,
                        SizedRegion<1>:$value);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $table $mask `:` qualified(type($mask))
    `mode` $mode `write_fields` $write_fields `enable` $enable `value` $value attr-dict
  }];
}

def ACIR_TableYieldOp : ACIR_Op<"table.yield", [Pure, Terminator]> {
  let arguments = (ins ACIR_VarType:$value);
  let assemblyFormat = "$value attr-dict `:` qualified(type($value))";
}

def ACIR_TableMatchOp : ACIR_Op<"table.match", [
    SingleBlock, DeclareOpInterfaceMethods<MemoryEffectsOpInterface>
  ]> {
  let arguments = (ins FlatSymbolRefAttr:$table);
  let results = (outs ACIR_VarType:$mask);
  let regions = (region SizedRegion<1>:$predicate);
  let hasVerifier = 1;
  let assemblyFormat = "$table `predicate` $predicate attr-dict `->` qualified(type($mask))";
}

def ACIR_TableMatchYieldOp : ACIR_Op<"table.match.yield", [Pure, Terminator]> {
  let arguments = (ins ACIR_VarType:$value);
  let assemblyFormat = "$value attr-dict `:` qualified(type($value))";
}

def ACIR_TableChooseOp : ACIR_Op<"table.choose", [
    DeclareOpInterfaceMethods<MemoryEffectsOpInterface>
  ]> {
  let arguments = (ins FlatSymbolRefAttr:$table, ACIR_VarType:$mask,
                       I64Attr:$count, StrAttr:$policy);
  let results = (outs ACIR_VarType:$index, ACIR_VarType:$valid);
  let regions = (region AnyRegion:$key);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $table $mask `:` qualified(type($mask)) `count` $count `policy` $policy
    `key` $key attr-dict `->`
    qualified(type($index)) `,` qualified(type($valid))
  }];
}

def ACIR_TableChooseYieldOp : ACIR_Op<"table.choose.yield", [Pure, Terminator]> {
  let arguments = (ins ACIR_VarType:$value);
  let assemblyFormat = "$value attr-dict `:` qualified(type($value))";
}

def ACIR_SlotOp : ACIR_Op<"slot", [
    Symbol, DeclareOpInterfaceMethods<MemoryEffectsOpInterface>
  ]> {
  let arguments = (ins SymbolNameAttr:$sym_name, ACIR_QueueType:$input,
                       StrAttr:$owner, StrAttr:$stable_id);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `,` $input `owner` $owner `stable_id` $stable_id attr-dict
    `:` qualified(type($input))
  }];
}

def ACIR_SlotGetOp : ACIR_Op<"slot.get", [
    DeclareOpInterfaceMethods<MemoryEffectsOpInterface>
  ]> {
  let arguments = (ins FlatSymbolRefAttr:$slot);
  let results = (outs ACIR_VarType:$valid, ACIR_VarType:$value);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $slot attr-dict `:` qualified(type($valid)) `,` qualified(type($value))
  }];
}

def ACIR_SlotReleaseOp : ACIR_Op<"slot.release", [
    SingleBlock, DeclareOpInterfaceMethods<MemoryEffectsOpInterface>
  ]> {
  let arguments = (ins FlatSymbolRefAttr:$slot);
  let regions = (region SizedRegion<1>:$when);
  let hasVerifier = 1;
  let assemblyFormat = "$slot `when` $when attr-dict";
}

def ACIR_SlotYieldOp : ACIR_Op<"slot.yield", [Pure, Terminator]> {
  let arguments = (ins ACIR_VarType:$value);
  let assemblyFormat = "$value attr-dict `:` qualified(type($value))";
}

def ACIR_FeedbackOp : ACIR_Op<"feedback", [SingleBlock]> {
  let arguments = (ins ACIR_QueueType:$input, I64Attr:$depth,
                       I64Attr:$latency, I64Attr:$max_iterations);
  let results = (outs ACIR_QueueType:$output);
  let regions = (region SizedRegion<1>:$body);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $input `depth` $depth `latency` $latency
    `max_iterations` $max_iterations $body attr-dict `:`
    qualified(type($input)) `->` qualified(type($output))
  }];
}

def ACIR_FeedbackYieldOp : ACIR_Op<"feedback.yield", [
    Pure, Terminator, HasParent<"FeedbackOp">
  ]> {
  let arguments = (ins ACIR_VarType:$value, ACIR_VarType:$continue_value);
  let assemblyFormat = [{
    $value `continue` $continue_value attr-dict `:`
    qualified(type($value)) `,` qualified(type($continue_value))
  }];
}

def ACIR_ScopeOp : ACIR_Op<"scope", [SingleBlock, RecursiveMemoryEffects]> {
  let arguments = (ins SymbolNameAttr:$sym_name,
                       Variadic<ACIR_QueueType>:$inputs);
  let results = (outs Variadic<ACIR_QueueType>:$outputs);
  let regions = (region SizedRegion<1>:$body);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `(` $inputs `)` $body attr-dict `:`
    functional-type($inputs, $outputs)
  }];
}

def ACIR_ScopeYieldOp : ACIR_Op<"scope.yield", [
    Pure, Terminator, HasParent<"ScopeOp">
  ]> {
  let arguments = (ins Variadic<ACIR_QueueType>:$queues);
  let assemblyFormat = "attr-dict ($queues^ `:` type($queues))?";
}

def ACIR_FiringOp : ACIR_Op<"firing", [
    SingleBlock, RecursiveMemoryEffects
  ]> {
  let arguments = (ins Variadic<ACIR_QueueType>:$inputs,
                       DenseI64ArrayAttr:$output_depths,
                       DenseI64ArrayAttr:$output_latencies,
                       StrAttr:$stable_id, StrAttr:$time_domain,
                       StrAttr:$functional_guard, ArrayAttr:$checks,
                       StrAttr:$handshake, StrAttr:$schedule,
                       StrArrayAttr:$effects);
  let results = (outs Variadic<ACIR_QueueType>:$outputs);
  let regions = (region SizedRegion<1>:$body);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $inputs `depths` $output_depths `latencies` $output_latencies
    `stable_id` $stable_id `domain` $time_domain `guard` $functional_guard
    `checks` $checks `handshake` $handshake `schedule` $schedule
    `effects` $effects $body attr-dict `:` functional-type($inputs, $outputs)
  }];
}

def ACIR_FiringYieldOp : ACIR_Op<"firing.yield", [
    Pure, Terminator, HasParent<"FiringOp">
  ]> {
  let arguments = (ins Variadic<ACIR_VarType>:$values);
  let assemblyFormat = "attr-dict ($values^ `:` type($values))?";
}

def ACIR_FiringConditionOp
    : ACIR_Op<"firing.condition", [HasParent<"FiringOp">]> {
  let arguments = (ins ACIR_VarType:$condition);
  let hasVerifier = 1;
  let assemblyFormat = "$condition attr-dict `:` qualified(type($condition))";
}

def ACIR_FiringOutputOp
    : ACIR_Op<"firing.output", [HasParent<"FiringOp">]> {
  let arguments = (ins ACIR_VarType:$value, ACIR_VarType:$when,
                       I64Attr:$ordinal);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $value `when` $when `ordinal` $ordinal attr-dict `:`
    qualified(type($value)) `,` qualified(type($when))
  }];
}

def ACIR_InterfaceOp : ACIR_Op<"interface", [
    Symbol, SymbolTable, SingleBlock, NoTerminator, IsolatedFromAbove,
    RegionKindInterface, HasOnlyGraphRegion, ACIR_InterfaceContainer
  ]> {
  let arguments = (ins SymbolNameAttr:$sym_name);
  let regions = (region SizedRegion<1>:$body);
  let builders = [OpBuilder<(ins "::llvm::StringRef":$name), [{
    $_state.addAttribute(::mlir::SymbolTable::getSymbolAttrName(),
                        $_builder.getStringAttr(name));
    $_state.addRegion();
  }]>];
  let hasVerifier = 1;
  let assemblyFormat = "$sym_name $body attr-dict";
}

def ACIR_ProtocolOp : ACIR_Op<"protocol", [
    Symbol, SymbolTable, SingleBlock, NoTerminator, IsolatedFromAbove,
    RegionKindInterface, HasOnlyGraphRegion, ACIR_ProtocolContainer
  ]> {
  let arguments = (ins SymbolNameAttr:$sym_name);
  let regions = (region SizedRegion<1>:$body);
  let builders = [OpBuilder<(ins "::llvm::StringRef":$name), [{
    $_state.addAttribute(::mlir::SymbolTable::getSymbolAttrName(),
                        $_builder.getStringAttr(name));
    $_state.addRegion();
  }]>];
  let hasVerifier = 1;
  let assemblyFormat = "$sym_name $body attr-dict";
}

def ACIR_RoleOp : ACIR_Op<"role", [Symbol, Pure]> {
  let arguments = (ins SymbolNameAttr:$sym_name, FlatSymbolRefAttr:$dual,
                       StrAttr:$cardinality);
  let hasVerifier = 1;
  let assemblyFormat = "$sym_name `dual` $dual `cardinality` $cardinality attr-dict";
}

def ACIR_StateOp : ACIR_Op<"state", [Symbol, Pure]> {
  let arguments = (ins SymbolNameAttr:$sym_name, BoolAttr:$initial,
                       BoolAttr:$terminal);
  let hasVerifier = 1;
  let assemblyFormat = "$sym_name `initial` $initial `terminal` $terminal attr-dict";
}

def ACIR_EventOp : ACIR_Op<"event", [Symbol, Pure]> {
  let arguments = (ins SymbolNameAttr:$sym_name, FlatSymbolRefAttr:$from,
                       FlatSymbolRefAttr:$to, TypeAttr:$payload,
                       StrAttr:$action);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `from` $from `to` $to `payload` $payload `action` $action
    attr-dict
  }];
}

def ACIR_TransitionOp : ACIR_Op<"transition", [SingleBlock, NoTerminator]> {
  let arguments = (ins FlatSymbolRefAttr:$source, FlatSymbolRefAttr:$target,
                       FlatSymbolRefAttr:$event,
                       OptionalAttr<I64Attr>:$priority,
                       DefaultValuedAttr<BoolAttr, "false">:$transfer,
                       DefaultValuedAttr<BoolAttr, "false">:$retain);
  let regions = (region AnyRegion:$guard);
  let hasVerifier = 1;
  let assemblyFormat = [{
    `from` $source `to` $target `on` $event (`priority` $priority^)?
    `transfer` $transfer `retain` $retain `guard` $guard attr-dict
  }];
}

def ACIR_GuaranteeOp : ACIR_Op<"guarantee", [Pure]> {
  let arguments = (ins StrAttr:$kind, AnyAttr:$value);
  let hasVerifier = 1;
  let assemblyFormat = "$kind `=` $value attr-dict";
}

def ACIR_PortOp : ACIR_Op<"port", [Symbol, Pure]> {
  let arguments = (ins SymbolNameAttr:$sym_name, TypeAttr:$type,
                       FlatSymbolRefAttr:$from, FlatSymbolRefAttr:$to,
                       FlatSymbolRefAttr:$protocol_from,
                       FlatSymbolRefAttr:$protocol_to);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `:` $type `from` $from `to` $to
    `protocol_roles` $protocol_from `to` $protocol_to attr-dict
  }];
}

def ACIR_SystemOp : ACIR_Op<"system", [Symbol]> {
  let arguments = (ins SymbolNameAttr:$sym_name, FlatSymbolRefAttr:$root,
                       StrAttr:$root_name, I64Attr:$tick_epoch,
                       StrAttr:$tick_unit,
                       OptionalAttr<SymbolRefAttr>:$primary_workload,
                       DictionaryAttr:$seed_policy,
                       ArrayAttr:$instrumentation,
                       DictionaryAttr:$result_schema,
                       DefaultValuedAttr<BoolAttr, "false">:$selected);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `root` $root `as` $root_name `tick` $tick_epoch $tick_unit
    (`workload` $primary_workload^)? `seed` $seed_policy
    `instrumentation` $instrumentation `results` $result_schema
    (`selected` $selected^)? attr-dict
  }];
}

def ACIR_ModuleOp : ACIR_Op<"module", [
    Symbol, FunctionOpInterface, IsolatedFromAbove, SingleBlock,
    RegionKindInterface, HasOnlyGraphRegion
  ]> {
  let arguments = (ins SymbolNameAttr:$sym_name,
                       TypeAttrOf<FunctionType>:$function_type,
                       OptionalAttr<StrAttr>:$sym_visibility,
                       OptionalAttr<DictArrayAttr>:$arg_attrs,
                       OptionalAttr<DictArrayAttr>:$res_attrs,
                       DictionaryAttr:$static_params);
  let regions = (region AnyRegion:$body);
  let builders = [OpBuilder<(ins "::llvm::StringRef":$name,
                                 "::mlir::FunctionType":$type,
                                 CArg<"::mlir::DictionaryAttr", "{}">:$staticParams), [{
    $_state.addAttribute(::mlir::SymbolTable::getSymbolAttrName(),
                        $_builder.getStringAttr(name));
    $_state.addAttribute(getFunctionTypeAttrName($_state.name),
                        ::mlir::TypeAttr::get(type));
    $_state.addAttribute("static_params",
                        staticParams ? staticParams : $_builder.getDictionaryAttr({}));
    $_state.addRegion();
  }]>];
  let extraClassDeclaration = [{
    ::mlir::ArrayRef<::mlir::Type> getArgumentTypes() {
      return getFunctionType().getInputs();
    }
    ::mlir::ArrayRef<::mlir::Type> getResultTypes() {
      return getFunctionType().getResults();
    }
    ::mlir::Region *getCallableRegion() { return &getBody(); }
    bool isDeclaration() { return false; }
  }];
  let hasCustomAssemblyFormat = 1;
  let hasVerifier = 1;
}

class ACIR_ModuleDeclarationOp<string mnemonic>
    : ACIR_Op<mnemonic, [Symbol]> {
  let arguments = (ins SymbolNameAttr:$sym_name,
                       TypeAttrOf<FunctionType>:$function_type,
                       OptionalAttr<StrAttr>:$sym_visibility,
                       DictionaryAttr:$static_params,
                       DictionaryAttr:$binding);
  let hasVerifier = 1;
}

def ACIR_ModuleExternOp : ACIR_ModuleDeclarationOp<"module.extern"> {
  let arguments = (ins SymbolNameAttr:$sym_name,
                       TypeAttrOf<FunctionType>:$function_type,
                       OptionalAttr<StrAttr>:$sym_visibility,
                       DictionaryAttr:$static_params,
                       DictionaryAttr:$implementation);
  let assemblyFormat = [{
    $sym_name `:` $function_type `parameters` $static_params
    `implementation` $implementation attr-dict
  }];
}

def ACIR_ModuleGeneratedOp : ACIR_ModuleDeclarationOp<"module.generated"> {
  let arguments = (ins SymbolNameAttr:$sym_name,
                       TypeAttrOf<FunctionType>:$function_type,
                       OptionalAttr<StrAttr>:$sym_visibility,
                       DictionaryAttr:$static_params,
                       DictionaryAttr:$generator);
  let assemblyFormat = [{
    $sym_name `:` $function_type `parameters` $static_params
    `generator` $generator attr-dict
  }];
}

def ACIR_InstanceOp : ACIR_Op<"instance", [Pure]> {
  let arguments = (ins Variadic<AnyType>:$inputs,
                       FlatSymbolRefAttr:$definition, SymbolNameAttr:$sym_name,
                       StrAttr:$stable_id, StrAttr:$path,
                       DictionaryAttr:$static_args);
  let results = (outs Variadic<AnyType>:$outputs);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `of` $definition `(` $inputs `)` `static` $static_args
    `id` $stable_id `path` $path attr-dict `:` functional-type($inputs, $outputs)
  }];
}

def ACIR_ArrayOp : ACIR_Op<"array", [Pure]> {
  let arguments = (ins Variadic<AnyType>:$inputs,
                       FlatSymbolRefAttr:$definition, SymbolNameAttr:$sym_name,
                       StrAttr:$stable_id, StrAttr:$path,
                       DenseI64ArrayAttr:$shape, ArrayAttr:$static_args);
  let results = (outs Variadic<AnyType>:$outputs);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `of` $definition `shape` $shape `(` $inputs `)`
    `static` $static_args `id` $stable_id `path` $path attr-dict
    `:` functional-type($inputs, $outputs)
  }];
}

def ACIR_InstancesOp : ACIR_Op<"instances", [Pure]> {
  let arguments = (ins Variadic<AnyType>:$inputs,
                       SymbolNameAttr:$sym_name, StrAttr:$stable_id,
                       StrAttr:$path, ArrayAttr:$definitions,
                       StrArrayAttr:$names, StrArrayAttr:$stable_ids,
                       StrArrayAttr:$paths,
                       TypeAttrOf<FunctionType>:$interface,
                       ArrayAttr:$static_args);
  let results = (outs Variadic<AnyType>:$outputs);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `definitions` $definitions `names` $names `ids` $stable_ids
    `paths` $paths `interface` $interface `(` $inputs `)`
    `static` $static_args `id` $stable_id `path` $path attr-dict
    `:` functional-type($inputs, $outputs)
  }];
}

def ACIR_ViewOp : ACIR_Op<"view", [Pure]> {
  let arguments = (ins Variadic<AnyType>:$inputs,
                       SymbolNameAttr:$sym_name, StrAttr:$kind,
                       FlatSymbolRefArrayAttr:$source_producers,
                       ArrayAttr:$source_shapes,
                       OptionalAttr<I64Attr>:$axis,
                       DenseI64ArrayAttr:$indices,
                       DenseI64ArrayAttr:$shape);
  let results = (outs Variadic<AnyType>:$outputs);
  let extraClassDeclaration = [{
    ::mlir::LogicalResult verifyWithProducerIndex(
        const ::llvm::StringMap<::mlir::Operation *> &producerIndex);
  }];
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name $kind `(` $inputs `)` `producers` $source_producers
    `sources` $source_shapes (`axis` $axis^)?
    `indices` $indices `shape` $shape attr-dict
    `:` functional-type($inputs, $outputs)
  }];
}

class ACIR_OwnedStateOp<string mnemonic, list<Trait> traits = []>
    : ACIR_Op<mnemonic, !listconcat([
        DeclareOpInterfaceMethods<MemoryEffectsOpInterface>
      ], traits)>;

def ACIR_QueueOp : ACIR_OwnedStateOp<"queue"> {
  let arguments = (ins SymbolNameAttr:$sym_name, StrAttr:$stable_id,
                       StrAttr:$path, TypeAttr:$payload,
                       I64Attr:$entry_capacity,
                       OptionalAttr<I64Attr>:$byte_capacity,
                       StrAttr:$ordering, FlatSymbolRefAttr:$protocol,
                       StrAttr:$ownership,
                       OptionalAttr<DictionaryAttr>:$watermarks,
                       DefaultValuedAttr<I64Attr, "1">:$delay_ticks);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `payload` $payload `entries` $entry_capacity
    (`bytes` $byte_capacity^)? `ordering` $ordering `protocol` $protocol
    `ownership` $ownership `id` $stable_id `path` $path
    (`watermarks` $watermarks^)? attr-dict
  }];
}

def ACIR_EventQueueOp : ACIR_OwnedStateOp<"event_queue"> {
  let arguments = (ins SymbolNameAttr:$sym_name, StrAttr:$stable_id,
                       StrAttr:$path, TypeAttr:$payload, I64Attr:$capacity,
                       StrAttr:$ordering, FlatSymbolRefAttr:$time_domain,
                       DefaultValuedAttr<I64Attr, "1">:$delay_ticks);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `payload` $payload `capacity` $capacity `ordering` $ordering
    `domain` $time_domain `id` $stable_id `path` $path attr-dict
  }];
}

def ACIR_ResourceOp : ACIR_OwnedStateOp<"resource"> {
  let arguments = (ins SymbolNameAttr:$sym_name, StrAttr:$stable_id,
                       StrAttr:$path, I64Attr:$capacity,
                       I64Attr:$issue_width, I64Attr:$initiation_interval,
                       DictionaryAttr:$latency_model,
                       DictionaryAttr:$lifecycle, StrAttr:$ownership,
                       OptionalAttr<FlatSymbolRefAttr>:$arbitration_owner,
                       ArrayAttr:$transaction_classes,
                       DefaultValuedAttr<I64Attr, "1">:$delay_ticks);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `capacity` $capacity `issue_width` $issue_width
    `ii` $initiation_interval `latency` $latency_model
    `lifecycle` $lifecycle `ownership` $ownership
    (`arbiter` $arbitration_owner^)? `classes` $transaction_classes
    `id` $stable_id `path` $path attr-dict
  }];
}

def ACIR_AddressSpaceOp : ACIR_Op<"address_space"> {
  let arguments = (ins SymbolNameAttr:$sym_name, StrAttr:$stable_id,
                       StrAttr:$path, I64Attr:$address_width,
                       StrAttr:$address_unit,
                       OptionalAttr<AnyAttr>:$data_layout,
                       OptionalAttr<FlatSymbolRefAttr>:$parent,
                       OptionalAttr<DictionaryAttr>:$translation);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `width` $address_width `unit` $address_unit
    `id` $stable_id `path` $path (`layout` $data_layout^)?
    (`parent` $parent^ `translate` $translation)? attr-dict
  }];
}

def ACIR_AddressMapOp : ACIR_Op<"address_map"> {
  let arguments = (ins SymbolNameAttr:$sym_name,
                       FlatSymbolRefAttr:$source, ArrayAttr:$entries,
                       DictionaryAttr:$default_behavior);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `source` $source `entries` $entries
    `default` $default_behavior attr-dict
  }];
}

def ACIR_TimeDomainOp : ACIR_Op<"time_domain"> {
  let arguments = (ins SymbolNameAttr:$sym_name, I64Attr:$period,
                       I64Attr:$phase, I64Attr:$tick_scale,
                       OptionalAttr<FlatSymbolRefAttr>:$parent,
                       OptionalAttr<DictionaryAttr>:$bridge);
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `period` $period `phase` $phase `scale` $tick_scale
    (`parent` $parent^ `bridge` $bridge)? attr-dict
  }];
}

class ACIR_RuntimeEffectOp<string mnemonic, list<Trait> traits = []>
    : ACIR_Op<mnemonic, !listconcat([
        DeclareOpInterfaceMethods<MemoryEffectsOpInterface>
      ], traits)>;

def ACIR_ProcessOp : ACIR_Op<"process", [
    IsolatedFromAbove, SingleBlock, RecursiveMemoryEffects, ACIR_ProcessRegion
  ]> {
  let arguments = (ins SymbolNameAttr:$sym_name, StrAttr:$kind,
                       Variadic<AnyType>:$captures);
  let regions = (region AnyRegion:$body);
  let builders = [OpBuilder<(ins "::llvm::StringRef":$name,
                                 "::llvm::StringRef":$kind,
                                 "::mlir::ValueRange":$captures), [{
    $_state.addAttribute(::mlir::SymbolTable::getSymbolAttrName(),
                        $_builder.getStringAttr(name));
    $_state.addAttribute("kind", $_builder.getStringAttr(kind));
    $_state.addOperands(captures);
    $_state.addRegion();
  }]>];
  let hasVerifier = 1;
  let assemblyFormat = [{
    $sym_name `kind` $kind (`captures` `(` $captures^ `:` type($captures) `)`)?
    $body attr-dict
  }];
}

def ACIR_TrySendOp : ACIR_RuntimeEffectOp<"try_send"> {
  let arguments = (ins AnyType:$value, SymbolRefAttr:$queue);
  let results = (outs I1:$accepted);
  let builders = [
    OpBuilder<(ins "mlir::Type":$accepted, "mlir::Value":$value,
                   "llvm::StringRef":$queue), [{
      $_state.addTypes(accepted);
      $_state.addOperands(value);
      $_state.addAttribute(
          "queue", mlir::FlatSymbolRefAttr::get($_builder.getContext(), queue));
    }]>
  ];
  let hasVerifier = 1;
  let assemblyFormat = "$queue $value attr-dict `:` type($value)";
}

def ACIR_TryRecvOp : ACIR_RuntimeEffectOp<"try_recv"> {
  let arguments = (ins SymbolRefAttr:$queue);
  let results = (outs AnyType:$value, I1:$received);
  let builders = [
    OpBuilder<(ins "mlir::Type":$value, "mlir::Type":$received,
                   "llvm::StringRef":$queue), [{
      $_state.addTypes({value, received});
      $_state.addAttribute(
          "queue", mlir::FlatSymbolRefAttr::get($_builder.getContext(), queue));
    }]>
  ];
  let hasVerifier = 1;
  let assemblyFormat = "$queue attr-dict `:` type($value)";
}

def ACIR_ScheduleOp : ACIR_RuntimeEffectOp<"schedule"> {
  let arguments = (ins AnyType:$value, I64:$delay,
                       FlatSymbolRefAttr:$target);
  let hasVerifier = 1;
  let assemblyFormat = "$target $value `after` $delay attr-dict `:` type($value)";
}

def ACIR_WaitUntilOp : ACIR_RuntimeEffectOp<"wait_until"> {
  let arguments = (ins I1:$condition);
  let hasVerifier = 1;
  let assemblyFormat = "$condition attr-dict";
}

def ACIR_WaitForOp : ACIR_RuntimeEffectOp<"wait_for"> {
  let arguments = (ins FlatSymbolRefAttr:$resource);
  let hasVerifier = 1;
  let assemblyFormat = "$resource attr-dict";
}

def ACIR_AwaitEventOp : ACIR_RuntimeEffectOp<"await_event"> {
  let arguments = (ins FlatSymbolRefAttr:$event_queue);
  let hasVerifier = 1;
  let assemblyFormat = "$event_queue attr-dict";
}

def ACIR_YieldSimOp : ACIR_RuntimeEffectOp<"yield_sim", [Terminator]> {
  let hasVerifier = 1;
  let assemblyFormat = "attr-dict";
}

def ACIR_TraceOpenOp : ACIR_RuntimeEffectOp<"trace.open"> {
  let arguments = (ins StrAttr:$source);
  let results = (outs Index:$cursor);
  let hasVerifier = 1;
  let assemblyFormat = "`source` $source attr-dict";
}

def ACIR_TraceNextOp : ACIR_RuntimeEffectOp<"trace.next"> {
  let arguments = (ins Index:$input_cursor, StrAttr:$source);
  let results = (outs Index:$cursor, AnyType:$entry, I1:$advanced);
  let hasVerifier = 1;
  let assemblyFormat = "$input_cursor `from` `source` $source attr-dict `:` type($entry)";
}

def ACIR_TraceDecodeOp : ACIR_Op<"trace.decode", [Pure]> {
  let arguments = (ins AnyType:$entry);
  let results = (outs AnyType:$result);
  let hasVerifier = 1;
  let assemblyFormat = "$entry attr-dict `:` type($entry) `to` type($result)";
}

def ACIR_TraceEofOp : ACIR_RuntimeEffectOp<"trace.eof"> {
  let arguments = (ins Index:$input_cursor, StrAttr:$source);
  let results = (outs I1:$eof);
  let hasVerifier = 1;
  let assemblyFormat = "$input_cursor `from` `source` $source attr-dict";
}

def ACIR_TracePositionOp : ACIR_RuntimeEffectOp<"trace.position"> {
  let arguments = (ins Index:$input_cursor, StrAttr:$source);
  let results = (outs Index:$position);
  let hasVerifier = 1;
  let assemblyFormat = "$input_cursor `from` `source` $source attr-dict";
}

class ACIR_ContractOp<string mnemonic>
    : ACIR_RuntimeEffectOp<mnemonic, [ACIR_Observation]> {
  let arguments = (ins I1:$condition, StrAttr:$message);
  let hasVerifier = 1;
  let assemblyFormat = "$condition `,` $message attr-dict";
}

def ACIR_RequireOp : ACIR_ContractOp<"require">;
def ACIR_EnsureOp : ACIR_ContractOp<"ensure">;
def ACIR_AssertOp : ACIR_ContractOp<"assert">;

def ACIR_ProbeOp : ACIR_RuntimeEffectOp<"probe", [ACIR_Observation]> {
  let arguments = (ins FlatSymbolRefAttr:$target, StrAttr:$kind);
  let results = (outs AnyType:$value);
  let hasVerifier = 1;
  let assemblyFormat = "$target `kind` $kind attr-dict `:` type($value)";
}

def ACIR_StatOp : ACIR_RuntimeEffectOp<"stat", [ACIR_Observation]> {
  let arguments = (ins SymbolNameAttr:$sym_name, StrAttr:$kind);
  let hasVerifier = 1;
  let assemblyFormat = "$sym_name `kind` $kind attr-dict";
}

def ACIR_StatAddOp : ACIR_RuntimeEffectOp<"stat.add", [ACIR_Observation]> {
  let arguments = (ins AnyType:$value, FlatSymbolRefAttr:$stat);
  let hasVerifier = 1;
  let assemblyFormat = "$stat $value attr-dict `:` type($value)";
}

def ACIR_InstrumentationOp : ACIR_Op<"instrumentation", [
    SingleBlock, NoTerminator, RecursiveMemoryEffects,
    ACIR_Observation
  ]> {
  let arguments = (ins SymbolNameAttr:$sym_name);
  let regions = (region AnyRegion:$body);
  let builders = [OpBuilder<(ins "::llvm::StringRef":$name), [{
    $_state.addAttribute(::mlir::SymbolTable::getSymbolAttrName(),
                        $_builder.getStringAttr(name));
    $_state.addRegion();
  }]>];
  let hasVerifier = 1;
  let assemblyFormat = "$sym_name $body attr-dict";
}

def ACIR_ReturnOp : ACIR_Op<"return", [Pure, Terminator, ReturnLike,
                                        HasParent<"ModuleOp">]> {
  let arguments = (ins Variadic<AnyType>:$operands);
  let assemblyFormat = "attr-dict ($operands^ `:` type($operands))?";
  let hasVerifier = 1;
}

#endif
