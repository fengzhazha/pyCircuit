module {
  func.func @invalid_result(%cond: i1, %lhs: i8, %rhs: i8) -> i16 {
    %bad = pyc.select %cond, %lhs, %rhs : i1, i8, i8 -> i16
    func.return %bad : i16
  }
}
