`include "pyc_add.sv"
`include "pyc_mux.sv"
`include "pyc_and.sv"
`include "pyc_or.sv"
`include "pyc_xor.sv"
`include "pyc_not.sv"
`include "pyc_reg.sv"
`include "pyc_fifo.sv"

`include "pyc_byte_mem.sv"

module MulticlockRegs (
  input logic clk_a,
  input logic rst_a,
  input logic clk_b,
  input logic rst_b,
  output logic [7:0] a_count,
  output logic [7:0] b_count
);

logic [7:0] v1;
logic [7:0] v2;
logic v3;
logic [7:0] v4;
logic [7:0] v5;
logic v6;
logic en__multiclock_regs__L12;
logic [7:0] a__next;
logic [7:0] v7;
logic [7:0] a;
logic [7:0] a__multiclock_regs__L14;
logic [7:0] v8;
logic [7:0] b__next;
logic [7:0] v9;
logic [7:0] b;
logic [7:0] b__multiclock_regs__L18;
logic [7:0] v10;

assign v1 = 8'd1;
assign v2 = 8'd0;
assign v3 = 1'd1;
assign v4 = v1;
assign v5 = v2;
assign v6 = v3;
assign en__multiclock_regs__L12 = v6;
pyc_reg #(.WIDTH(8)) v7_inst (
  .clk(clk_a),
  .rst(rst_a),
  .en(en__multiclock_regs__L12),
  .d(a__next),
  .init(v5),
  .q(v7)
);
assign a = v7;
assign a__multiclock_regs__L14 = a;
pyc_add #(.WIDTH(8)) v8_inst (
  .a(a__multiclock_regs__L14),
  .b(v4),
  .y(v8)
);
assign a__next = v8;
pyc_reg #(.WIDTH(8)) v9_inst (
  .clk(clk_b),
  .rst(rst_b),
  .en(en__multiclock_regs__L12),
  .d(b__next),
  .init(v5),
  .q(v9)
);
assign b = v9;
assign b__multiclock_regs__L18 = b;
pyc_add #(.WIDTH(8)) v10_inst (
  .a(b__multiclock_regs__L18),
  .b(v4),
  .y(v10)
);
assign b__next = v10;
assign a_count = a__multiclock_regs__L14;
assign b_count = b__multiclock_regs__L18;

endmodule

