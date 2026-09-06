module {
  func.func @invalid(%lhs: i8, %rhs: i8) -> i1 {
    %bad = pyc.cmp %lhs, %rhs {predicate = "ule"} : i8, i8 -> i1
    func.return %bad : i1
  }
}
