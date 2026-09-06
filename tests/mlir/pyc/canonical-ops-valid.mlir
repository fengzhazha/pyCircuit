module {
  func.func @canonical(%cond: i1, %lhs: i8, %rhs: i8, %amount: i4) -> (i8, i1, i1, i1, i8, i8, i8) {
    %selected = pyc.select %cond, %lhs, %rhs : i1, i8, i8 -> i8
    %eq = pyc.cmp %lhs, %rhs {predicate = "eq"} : i8, i8 -> i1
    %ult = pyc.cmp %lhs, %rhs {predicate = "ult"} : i8, i8 -> i1
    %slt = pyc.cmp %lhs, %rhs {predicate = "slt"} : i8, i8 -> i1
    %shl = pyc.shl %lhs, %amount : i8, i4
    %lshr = pyc.lshr %lhs, %amount : i8, i4
    %ashr = pyc.ashr %lhs, %amount : i8, i4
    func.return %selected, %eq, %ult, %slt, %shl, %lshr, %ashr : i8, i1, i1, i1, i8, i8, i8
  }
}
