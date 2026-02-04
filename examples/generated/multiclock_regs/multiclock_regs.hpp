// pyCircuit C++ emission (prototype)
#include <pyc/cpp/pyc_sim.hpp>

namespace pyc::gen {

struct MulticlockRegs {
  pyc::cpp::Wire<1> clk_a{};
  pyc::cpp::Wire<1> rst_a{};
  pyc::cpp::Wire<1> clk_b{};
  pyc::cpp::Wire<1> rst_b{};
  pyc::cpp::Wire<8> a_count{};
  pyc::cpp::Wire<8> b_count{};

  pyc::cpp::Wire<8> v1{};
  pyc::cpp::Wire<8> v2{};
  pyc::cpp::Wire<1> v3{};
  pyc::cpp::Wire<8> v4{};
  pyc::cpp::Wire<8> v5{};
  pyc::cpp::Wire<1> v6{};
  pyc::cpp::Wire<1> en__multiclock_regs__L12{};
  pyc::cpp::Wire<8> a__next{};
  pyc::cpp::Wire<8> v7{};
  pyc::cpp::Wire<8> a{};
  pyc::cpp::Wire<8> a__multiclock_regs__L14{};
  pyc::cpp::Wire<8> v8{};
  pyc::cpp::Wire<8> b__next{};
  pyc::cpp::Wire<8> v9{};
  pyc::cpp::Wire<8> b{};
  pyc::cpp::Wire<8> b__multiclock_regs__L18{};
  pyc::cpp::Wire<8> v10{};

  pyc::cpp::pyc_reg<8> v7_inst;
  pyc::cpp::pyc_reg<8> v9_inst;

  MulticlockRegs() :
      v7_inst(clk_a, rst_a, en__multiclock_regs__L12, a__next, v5, v7),
      v9_inst(clk_b, rst_b, en__multiclock_regs__L12, b__next, v5, v9) {
    eval();
  }

  inline void eval_comb_0() {
    v1 = pyc::cpp::Wire<8>(1ull);
    v2 = pyc::cpp::Wire<8>(0ull);
    v3 = pyc::cpp::Wire<1>(1ull);
    v4 = v1;
    v5 = v2;
    v6 = v3;
  }

  inline void eval_comb_pass() {
    eval_comb_0();
    en__multiclock_regs__L12 = v6;
    a = v7;
    a__multiclock_regs__L14 = a;
    v8 = (a__multiclock_regs__L14 + v4);
    a__next = v8;
    b = v9;
    b__multiclock_regs__L18 = b;
    v10 = (b__multiclock_regs__L18 + v4);
    b__next = v10;
  }

  void eval() {
    eval_comb_pass();
    a_count = a__multiclock_regs__L14;
    b_count = b__multiclock_regs__L18;
  }

  void tick() {
    // Two-phase update: compute next state for all sequential elements,
    // then commit together. This avoids ordering artifacts between regs.
    // Phase 1: compute.
    v7_inst.tick_compute();
    v9_inst.tick_compute();
    // Phase 2: commit.
    v7_inst.tick_commit();
    v9_inst.tick_commit();
  }
};

} // namespace pyc::gen
