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

  pyc::cpp::Wire<8> a{};
  pyc::cpp::Wire<8> a__multiclock_regs__L12{};
  pyc::cpp::Wire<8> a__next{};
  pyc::cpp::Wire<8> b{};
  pyc::cpp::Wire<8> b__multiclock_regs__L16{};
  pyc::cpp::Wire<8> b__next{};
  pyc::cpp::Wire<8> pyc_add_12{};
  pyc::cpp::Wire<8> pyc_add_8{};
  pyc::cpp::Wire<8> pyc_comb_10{};
  pyc::cpp::Wire<8> pyc_comb_13{};
  pyc::cpp::Wire<8> pyc_comb_14{};
  pyc::cpp::Wire<8> pyc_comb_4{};
  pyc::cpp::Wire<8> pyc_comb_5{};
  pyc::cpp::Wire<1> pyc_comb_6{};
  pyc::cpp::Wire<8> pyc_comb_9{};
  pyc::cpp::Wire<8> pyc_constant_1{};
  pyc::cpp::Wire<8> pyc_constant_2{};
  pyc::cpp::Wire<1> pyc_constant_3{};
  pyc::cpp::Wire<8> pyc_reg_11{};
  pyc::cpp::Wire<8> pyc_reg_7{};

  pyc::cpp::pyc_reg<8> pyc_reg_11_inst;
  pyc::cpp::pyc_reg<8> pyc_reg_7_inst;

  MulticlockRegs() :
      pyc_reg_11_inst(clk_b, rst_b, pyc_comb_6, b__next, pyc_comb_5, pyc_reg_11),
      pyc_reg_7_inst(clk_a, rst_a, pyc_comb_6, a__next, pyc_comb_5, pyc_reg_7) {
    eval();
  }

  inline void eval_comb_0() {
    b = pyc_reg_11;
    b__multiclock_regs__L16 = b;
    pyc_add_12 = (b__multiclock_regs__L16 + pyc_comb_4);
    pyc_comb_13 = b__multiclock_regs__L16;
    pyc_comb_14 = pyc_add_12;
  }

  inline void eval_comb_1() {
    pyc_constant_1 = pyc::cpp::Wire<8>({0x1ull});
    pyc_constant_2 = pyc::cpp::Wire<8>({0x0ull});
    pyc_constant_3 = pyc::cpp::Wire<1>({0x1ull});
    pyc_comb_4 = pyc_constant_1;
    pyc_comb_5 = pyc_constant_2;
    pyc_comb_6 = pyc_constant_3;
  }

  inline void eval_comb_2() {
    a = pyc_reg_7;
    a__multiclock_regs__L12 = a;
    pyc_add_8 = (a__multiclock_regs__L12 + pyc_comb_4);
    pyc_comb_9 = a__multiclock_regs__L12;
    pyc_comb_10 = pyc_add_8;
  }

  inline void eval_comb_pass() {
    eval_comb_1();
    eval_comb_0();
    b__next = pyc_comb_14;
    eval_comb_2();
    a__next = pyc_comb_10;
  }

  void eval() {
    eval_comb_1();
    eval_comb_0();
    b__next = pyc_comb_14;
    eval_comb_2();
    a__next = pyc_comb_10;
    a_count = pyc_comb_9;
    b_count = pyc_comb_13;
  }

  void tick() {
    // Two-phase update: compute next state for all sequential elements,
    // then commit together. This avoids ordering artifacts between regs.
    // Phase 1: compute.
    pyc_reg_11_inst.tick_compute();
    pyc_reg_7_inst.tick_compute();
    // Phase 2: commit.
    pyc_reg_11_inst.tick_commit();
    pyc_reg_7_inst.tick_commit();
  }
};

} // namespace pyc::gen
