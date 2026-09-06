#include "acir/Dialect/ACIR/ACIRDialect.h"
#include "acir/Dialect/ACIR/GraphRegion.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/TypeSwitch.h"

using namespace mlir;

namespace acir::ac {
namespace {

bool isTimeUnit(Unit unit) {
  switch (unit) {
  case Unit::Ticks:
  case Unit::Cycles:
  case Unit::Seconds:
  case Unit::Milliseconds:
  case Unit::Microseconds:
  case Unit::Nanoseconds:
  case Unit::Picoseconds:
    return true;
  case Unit::Bytes:
  case Unit::Bits:
  case Unit::Entries:
  case Unit::Packets:
  case Unit::Transactions:
    return false;
  }
  llvm_unreachable("unknown ACIR unit");
}

LogicalResult verifyValueElement(function_ref<InFlightDiagnostic()> emitError,
                                 Type elementType) {
  if (containsChannelType(elementType))
    return emitError() << "channel types cannot be nested inside value types";
  if (containsQueueOrVarType(elementType))
    return emitError()
           << "queue and var types cannot be nested inside value types";
  return success();
}

DictionaryAttr findLayoutEntry(Type type, DataLayoutEntryListRef entries) {
  for (DataLayoutEntryInterface entry : entries)
    if (entry.getKey() == DataLayoutEntryKey(type))
      return dyn_cast<DictionaryAttr>(entry.getValue());
  return {};
}

int64_t getPositiveLayoutInteger(Type type, DataLayoutEntryListRef entries,
                                 StringRef name) {
  DictionaryAttr entry = findLayoutEntry(type, entries);
  auto value = entry ? entry.getAs<IntegerAttr>(name) : IntegerAttr();
  return value && value.getInt() > 0 ? value.getInt() : 0;
}

llvm::TypeSize getNamedTypeSizeInBits(Type type,
                                      DataLayoutEntryListRef entries) {
  return llvm::TypeSize::getFixed(
      static_cast<uint64_t>(getPositiveLayoutInteger(type, entries, "size")) *
      8);
}

uint64_t getNamedTypeAlignment(Type type, DataLayoutEntryListRef entries,
                               StringRef name) {
  return static_cast<uint64_t>(getPositiveLayoutInteger(type, entries, name));
}

LogicalResult verifyNamedLayoutEntries(DataLayoutEntryListRef entries,
                                       Location location, bool packet) {
  for (DataLayoutEntryInterface entry : entries) {
    auto dictionary = dyn_cast<DictionaryAttr>(entry.getValue());
    auto size =
        dictionary ? dictionary.getAs<IntegerAttr>("size") : IntegerAttr();
    auto abi = dictionary ? dictionary.getAs<IntegerAttr>("abi_alignment")
                          : IntegerAttr();
    auto preferred = dictionary
                         ? dictionary.getAs<IntegerAttr>("preferred_alignment")
                         : IntegerAttr();
    auto endian =
        dictionary ? dictionary.getAs<StringAttr>("endianness") : StringAttr();
    if (!size || !abi || !preferred || !endian || size.getInt() <= 0 ||
        abi.getInt() <= 0 || preferred.getInt() <= 0 ||
        (endian.getValue() != "little" && endian.getValue() != "big"))
      return emitError(location)
             << "layout entry requires positive size/alignment and explicit "
                "endianness";
    if (packet) {
      auto width = dictionary.getAs<IntegerAttr>("serialization_width");
      if (!width || width.getInt() <= 0)
        return emitError(location)
               << "packet layout entry requires positive serialization_width";
    }
  }
  return success();
}

bool isStaticCollectionElementType(Type type) {
  return isa<QueueType, VarType, ArrayType, MapType, SetType>(type);
}

LogicalResult
verifyStaticCollectionElement(function_ref<InFlightDiagnostic()> emitError,
                              Type elementType, StringRef collection) {
  if (isStaticCollectionElementType(elementType))
    return success();
  return emitError() << collection
                     << " element must be a queue, var, or static collection";
}

} // namespace

bool containsChannelType(Type type) {
  return type.walk([](ChannelType) { return WalkResult::interrupt(); })
      .wasInterrupted();
}

bool containsQueueOrVarType(Type type) {
  return type
      .walk([](Type nested) {
        return isa<QueueType, VarType>(nested) ? WalkResult::interrupt()
                                               : WalkResult::advance();
      })
      .wasInterrupted();
}

bool isImmutablePayloadType(Type type) {
  if (isa<IntegerType, FloatType, IndexType, StructType, PacketType,
          TransactionType, EnumType, UnionType>(type))
    return true;
  if (auto optional = dyn_cast<OptionalType>(type))
    return isImmutablePayloadType(optional.getElementType());
  if (auto vector = dyn_cast<VectorType>(type))
    return isImmutablePayloadType(vector.getElementType());
  if (auto array = dyn_cast<ValueArrayType>(type))
    return isImmutablePayloadType(array.getElementType());
  if (auto vector = dyn_cast<mlir::VectorType>(type))
    return isImmutablePayloadType(vector.getElementType());
  if (auto tuple = dyn_cast<mlir::TupleType>(type))
    return llvm::all_of(tuple.getTypes(), isImmutablePayloadType);
  return false;
}

bool isNormativePayloadType(Type type) {
  if (isImmutablePayloadType(type))
    return true;
  if (auto list = dyn_cast<ListType>(type))
    return isNormativePayloadType(list.getElementType());
  return false;
}

LogicalResult
verifyQualifiedDataName(function_ref<InFlightDiagnostic()> emitError,
                        SymbolRefAttr name) {
  if (name.getNestedReferences().size() == 1)
    return success();
  return emitError()
         << "named data references require a qualified symbol such as "
            "'@types::@S'";
}

#define ACIR_DEFINE_DATA_NAME_VERIFY(TYPE)                                     \
  LogicalResult TYPE::verify(function_ref<InFlightDiagnostic()> emitError,     \
                             SymbolRefAttr name) {                             \
    return verifyQualifiedDataName(emitError, name);                           \
  }

ACIR_DEFINE_DATA_NAME_VERIFY(StructType)
ACIR_DEFINE_DATA_NAME_VERIFY(PacketType)
ACIR_DEFINE_DATA_NAME_VERIFY(TransactionType)
ACIR_DEFINE_DATA_NAME_VERIFY(EnumType)
ACIR_DEFINE_DATA_NAME_VERIFY(UnionType)

#undef ACIR_DEFINE_DATA_NAME_VERIFY

LogicalResult OptionalType::verify(function_ref<InFlightDiagnostic()> emitError,
                                   Type elementType) {
  return verifyValueElement(emitError, elementType);
}

LogicalResult ListType::verify(function_ref<InFlightDiagnostic()> emitError,
                               Type elementType) {
  return verifyValueElement(emitError, elementType);
}

LogicalResult VectorType::verify(function_ref<InFlightDiagnostic()> emitError,
                                 int64_t length, Type elementType) {
  if (length <= 0)
    return emitError() << "vector length must be positive";
  return verifyValueElement(emitError, elementType);
}

LogicalResult
ValueArrayType::verify(function_ref<InFlightDiagnostic()> emitError,
                       int64_t length, Type elementType) {
  if (length <= 0)
    return emitError() << "value_array length must be positive";
  if (!isImmutablePayloadType(elementType))
    return emitError()
           << "value_array element must be an immutable ACIR value type";
  return success();
}

LogicalResult VarType::verify(function_ref<InFlightDiagnostic()> emitError,
                              Type elementType) {
  if (isImmutablePayloadType(elementType))
    return success();
  return emitError() << "var payload must be an immutable ACIR value type";
}

LogicalResult QueueType::verify(function_ref<InFlightDiagnostic()> emitError,
                                Type elementType) {
  if (isImmutablePayloadType(elementType))
    return success();
  return emitError() << "queue payload must be an immutable ACIR value type";
}

LogicalResult ArrayType::verify(function_ref<InFlightDiagnostic()> emitError,
                                int64_t length, Type elementType) {
  if (length <= 0)
    return emitError() << "array length must be positive";
  return verifyStaticCollectionElement(emitError, elementType, "array");
}

LogicalResult MapType::verify(function_ref<InFlightDiagnostic()> emitError,
                              ArrayAttr keys, Type elementType) {
  if (keys.empty())
    return emitError()
           << "map keys must be non-empty and strictly lexicographic";
  StringRef previous;
  for (Attribute rawKey : keys) {
    auto key = dyn_cast<StringAttr>(rawKey);
    if (!key || key.getValue().empty() ||
        (!previous.empty() && previous >= key.getValue()))
      return emitError()
             << "map keys must be non-empty and strictly lexicographic";
    previous = key.getValue();
  }
  return verifyStaticCollectionElement(emitError, elementType, "map");
}

LogicalResult SetType::verify(function_ref<InFlightDiagnostic()> emitError,
                              int64_t length, Type elementType) {
  if (length <= 0)
    return emitError() << "set length must be positive";
  return verifyStaticCollectionElement(emitError, elementType, "set");
}

LogicalResult FlowType::verify(function_ref<InFlightDiagnostic()> emitError,
                               Type elementType, FlatSymbolRefAttr) {
  return verifyValueElement(emitError, elementType);
}

LogicalResult ChannelType::verify(function_ref<InFlightDiagnostic()> emitError,
                                  Type elementType, FlatSymbolRefAttr) {
  if (!containsChannelType(elementType))
    return success();
  return emitError() << "channel types cannot carry channel types";
}

LogicalResult DurationType::verify(function_ref<InFlightDiagnostic()> emitError,
                                   Unit unit) {
  if (isTimeUnit(unit))
    return success();
  return emitError() << "duration requires a time unit";
}

LogicalResult RateType::verify(function_ref<InFlightDiagnostic()> emitError,
                               Unit numerator, Unit denominator) {
  if (isTimeUnit(numerator))
    return emitError() << "rate numerator must be a data unit";
  if (!isTimeUnit(denominator))
    return emitError() << "rate denominator must be a time unit";
  return success();
}

LogicalResult EventType::verify(function_ref<InFlightDiagnostic()> emitError,
                                Type elementType) {
  return verifyValueElement(emitError, elementType);
}

#define ACIR_DEFINE_NAMED_LAYOUT(TYPE, IS_PACKET)                              \
  llvm::TypeSize TYPE::getTypeSizeInBits(                                      \
      const DataLayout &, DataLayoutEntryListRef entries) const {              \
    return getNamedTypeSizeInBits(*this, entries);                             \
  }                                                                            \
  uint64_t TYPE::getABIAlignment(const DataLayout &,                           \
                                 DataLayoutEntryListRef entries) const {       \
    return getNamedTypeAlignment(*this, entries, "abi_alignment");             \
  }                                                                            \
  uint64_t TYPE::getPreferredAlignment(const DataLayout &,                     \
                                       DataLayoutEntryListRef entries) const { \
    return getNamedTypeAlignment(*this, entries, "preferred_alignment");       \
  }                                                                            \
  LogicalResult TYPE::verifyEntries(DataLayoutEntryListRef entries,            \
                                    Location location) const {                 \
    return verifyNamedLayoutEntries(entries, location, IS_PACKET);             \
  }

ACIR_DEFINE_NAMED_LAYOUT(StructType, false)
ACIR_DEFINE_NAMED_LAYOUT(PacketType, true)
ACIR_DEFINE_NAMED_LAYOUT(EnumType, false)
ACIR_DEFINE_NAMED_LAYOUT(UnionType, false)

#undef ACIR_DEFINE_NAMED_LAYOUT

} // namespace acir::ac

#include "acir/Dialect/ACIR/ACIREnums.cpp.inc"

#define GET_ATTRDEF_CLASSES
#include "acir/Dialect/ACIR/ACIRAttributes.cpp.inc"

#define GET_TYPEDEF_CLASSES
#include "acir/Dialect/ACIR/ACIRTypes.cpp.inc"

void acir::ac::ACIRDialect::initialize() {
  addInterfaces<StructuralProviderDialectInterface>();
  addAttributes<
#define GET_ATTRDEF_LIST
#include "acir/Dialect/ACIR/ACIRAttributes.cpp.inc"
      >();
  addTypes<
#define GET_TYPEDEF_LIST
#include "acir/Dialect/ACIR/ACIRTypes.cpp.inc"
      >();
  addOperations<
#define GET_OP_LIST
#include "acir/Dialect/ACIR/ACIROps.cpp.inc"
      >();
}
