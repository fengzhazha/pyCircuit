// pyCircuit C++ emission (prototype)
#include <pyc/cpp/pyc_sim.hpp>

namespace pyc::gen {

struct WireOps {
  pyc::cpp::Wire<1> sys_clk{};
  pyc::cpp::Wire<1> sys_rst{};
  pyc::cpp::Wire<8> a{};
  pyc::cpp::Wire<8> b{};
  pyc::cpp::Wire<1> sel{};
  pyc::cpp::Wire<8> y{};

  pyc::cpp::Wire<8> v1{};
  pyc::cpp::Wire<1> v2{};
  pyc::cpp::Wire<8> v3{};
  pyc::cpp::Wire<1> v4{};
  pyc::cpp::Wire<8> a__wire_ops__L9{};
  pyc::cpp::Wire<8> b__wire_ops__L10{};
  pyc::cpp::Wire<1> sel__wire_ops__L11{};
  pyc::cpp::Wire<8> v5{};
  pyc::cpp::Wire<8> v6{};
  pyc::cpp::Wire<8> v7{};
  pyc::cpp::Wire<8> v8{};
  pyc::cpp::Wire<8> COMB__y__wire_ops__L14{};
  pyc::cpp::Wire<1> en__wire_ops__L16{};
  pyc::cpp::Wire<8> y_reg__next{};
  pyc::cpp::Wire<8> v9{};
  pyc::cpp::Wire<8> y_reg{};
  pyc::cpp::Wire<8> r__wire_ops__L17{};
  pyc::cpp::Wire<8> v10{};

  pyc::cpp::pyc_reg<8> v9_inst;

  WireOps() :
      v9_inst(sys_clk, sys_rst, v4, y_reg__next, v3, v9) {
    eval();
  }

  inline void eval_comb_0() {
    v1 = pyc::cpp::Wire<8>(0ull);
    v2 = pyc::cpp::Wire<1>(1ull);
    v3 = v1;
    v4 = v2;
  }

  inline void eval_comb_1() {
    v5 = (a__wire_ops__L9 & b__wire_ops__L10);
    v6 = (a__wire_ops__L9 ^ b__wire_ops__L10);
    v7 = (sel__wire_ops__L11.toBool() ? v5 : v6);
    v8 = v7;
  }

  inline void eval_comb_pass() {
    eval_comb_0();
    a__wire_ops__L9 = a;
    b__wire_ops__L10 = b;
    sel__wire_ops__L11 = sel;
    eval_comb_1();
    COMB__y__wire_ops__L14 = v8;
    en__wire_ops__L16 = v4;
    y_reg = v9;
    r__wire_ops__L17 = y_reg;
    v10 = (en__wire_ops__L16.toBool() ? COMB__y__wire_ops__L14 : r__wire_ops__L17);
    y_reg__next = v10;
  }

  void eval() {
    eval_comb_pass();
    y = r__wire_ops__L17;
  }

  void tick() {
    // Two-phase update: compute next state for all sequential elements,
    // then commit together. This avoids ordering artifacts between regs.
    // Phase 1: compute.
    v9_inst.tick_compute();
    // Phase 2: commit.
    v9_inst.tick_commit();
  }
};

} // namespace pyc::gen
