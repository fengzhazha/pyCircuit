// pyCircuit C++ emission (prototype)
#include <pyc/cpp/pyc_sim.hpp>

namespace pyc::gen {

struct Counter {
  pyc::cpp::Wire<1> clk{};
  pyc::cpp::Wire<1> rst{};
  pyc::cpp::Wire<1> en{};
  pyc::cpp::Wire<8> count{};

  pyc::cpp::Wire<8> v1{};
  pyc::cpp::Wire<8> v2{};
  pyc::cpp::Wire<1> v3{};
  pyc::cpp::Wire<8> v4{};
  pyc::cpp::Wire<8> v5{};
  pyc::cpp::Wire<1> v6{};
  pyc::cpp::Wire<1> do__counter__L9{};
  pyc::cpp::Wire<8> count__next{};
  pyc::cpp::Wire<8> v7{};
  pyc::cpp::Wire<8> count{};
  pyc::cpp::Wire<8> count__counter__L11{};
  pyc::cpp::Wire<8> COUNT__c__counter__L15{};
  pyc::cpp::Wire<8> v8{};
  pyc::cpp::Wire<8> v9{};
  pyc::cpp::Wire<8> v10{};

  pyc::cpp::pyc_reg<8> v7_inst;

  Counter() :
      v7_inst(clk, rst, v6, count__next, v5, v7) {
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

  inline void eval_comb_1() {
    v8 = (COUNT__c__counter__L15 + v4);
    v9 = (do__counter__L9.toBool() ? v8 : count__counter__L11);
    v10 = v9;
  }

  inline void eval_comb_pass() {
    eval_comb_0();
    do__counter__L9 = en;
    count = v7;
    count__counter__L11 = count;
    COUNT__c__counter__L15 = count__counter__L11;
    eval_comb_1();
    count__next = v10;
  }

  void eval() {
    eval_comb_pass();
    count = count__counter__L11;
  }

  void tick() {
    // Two-phase update: compute next state for all sequential elements,
    // then commit together. This avoids ordering artifacts between regs.
    // Phase 1: compute.
    v7_inst.tick_compute();
    // Phase 2: commit.
    v7_inst.tick_commit();
  }
};

} // namespace pyc::gen
