`include "pyc_add.sv"
`include "pyc_mux.sv"
`include "pyc_and.sv"
`include "pyc_or.sv"
`include "pyc_xor.sv"
`include "pyc_not.sv"
`include "pyc_reg.sv"
`include "pyc_fifo.sv"

`include "pyc_byte_mem.sv"

module JitPipelineVec (
  input logic sys_clk,
  input logic sys_rst,
  input logic [15:0] a,
  input logic [15:0] b,
  input logic sel,
  output logic tag,
  output logic [15:0] data,
  output logic [7:0] lo8
);

logic [24:0] v1;
logic v2;
logic [24:0] v3;
logic v4;
logic en__jit_pipeline_vec__L8;
logic [15:0] a__jit_pipeline_vec__L10;
logic [15:0] b__jit_pipeline_vec__L11;
logic sel__jit_pipeline_vec__L12;
logic [15:0] v5;
logic [15:0] sum___jit_pipeline_vec__L15;
logic [15:0] v6;
logic [15:0] x__jit_pipeline_vec__L16;
logic [15:0] v7;
logic [15:0] data__jit_pipeline_vec__L17;
logic v8;
logic tag__jit_pipeline_vec__L18;
logic [7:0] v9;
logic [7:0] lo8__jit_pipeline_vec__L19;
logic [24:0] v10;
logic [24:0] v11;
logic [24:0] v12;
logic [24:0] v13;
logic [24:0] v14;
logic [24:0] v15;
logic [24:0] v16;
logic [24:0] v17;
logic [24:0] v18;
logic [24:0] bus__jit_pipeline_vec__L22;
logic [24:0] PIPE__bus__next;
logic [24:0] v19;
logic [24:0] PIPE__bus;
logic [24:0] PIPE__bus_r__jit_pipeline_vec__L27;
logic [24:0] v20;
logic [24:0] PIPE__bus__jit_pipeline_vec__L29;
logic [24:0] PIPE__bus__next_2;
logic [24:0] v21;
logic [24:0] PIPE__bus_2;
logic [24:0] PIPE__bus_r__jit_pipeline_vec__L27_2;
logic [24:0] v22;
logic [24:0] PIPE__bus__jit_pipeline_vec__L29_2;
logic [24:0] PIPE__bus__next_3;
logic [24:0] v23;
logic [24:0] PIPE__bus_3;
logic [24:0] PIPE__bus_r__jit_pipeline_vec__L27_3;
logic [24:0] v24;
logic [24:0] PIPE__bus__jit_pipeline_vec__L29_3;
logic [24:0] bus__jit_pipeline_vec__L25;
logic [7:0] v25;
logic [15:0] v26;
logic v27;
logic [7:0] v28;
logic [15:0] v29;
logic v30;

assign v1 = 25'd0;
assign v2 = 1'd1;
assign v3 = v1;
assign v4 = v2;
assign en__jit_pipeline_vec__L8 = v4;
assign a__jit_pipeline_vec__L10 = a;
assign b__jit_pipeline_vec__L11 = b;
assign sel__jit_pipeline_vec__L12 = sel;
pyc_add #(.WIDTH(16)) v5_inst (
  .a(a__jit_pipeline_vec__L10),
  .b(b__jit_pipeline_vec__L11),
  .y(v5)
);
assign sum___jit_pipeline_vec__L15 = v5;
pyc_xor #(.WIDTH(16)) v6_inst (
  .a(a__jit_pipeline_vec__L10),
  .b(b__jit_pipeline_vec__L11),
  .y(v6)
);
assign x__jit_pipeline_vec__L16 = v6;
pyc_mux #(.WIDTH(16)) v7_inst (
  .sel(sel__jit_pipeline_vec__L12),
  .a(sum___jit_pipeline_vec__L15),
  .b(x__jit_pipeline_vec__L16),
  .y(v7)
);
assign data__jit_pipeline_vec__L17 = v7;
assign v8 = (a__jit_pipeline_vec__L10 == b__jit_pipeline_vec__L11);
assign tag__jit_pipeline_vec__L18 = v8;
assign v9 = data__jit_pipeline_vec__L17[7:0];
assign lo8__jit_pipeline_vec__L19 = v9;
assign v10 = {{17{1'b0}}, lo8__jit_pipeline_vec__L19};
assign v11 = (v3 | v10);
assign v12 = {{9{1'b0}}, data__jit_pipeline_vec__L17};
assign v13 = (v12 << 8);
assign v14 = (v11 | v13);
assign v15 = {{24{1'b0}}, tag__jit_pipeline_vec__L18};
assign v16 = (v15 << 24);
assign v17 = (v14 | v16);
assign v18 = v17;
assign bus__jit_pipeline_vec__L22 = v18;
pyc_reg #(.WIDTH(25)) v19_inst (
  .clk(sys_clk),
  .rst(sys_rst),
  .en(v4),
  .d(PIPE__bus__next),
  .init(v3),
  .q(v19)
);
assign PIPE__bus = v19;
assign PIPE__bus_r__jit_pipeline_vec__L27 = PIPE__bus;
pyc_mux #(.WIDTH(25)) v20_inst (
  .sel(en__jit_pipeline_vec__L8),
  .a(bus__jit_pipeline_vec__L22),
  .b(PIPE__bus_r__jit_pipeline_vec__L27),
  .y(v20)
);
assign PIPE__bus__next = v20;
assign PIPE__bus__jit_pipeline_vec__L29 = PIPE__bus_r__jit_pipeline_vec__L27;
pyc_reg #(.WIDTH(25)) v21_inst (
  .clk(sys_clk),
  .rst(sys_rst),
  .en(v4),
  .d(PIPE__bus__next_2),
  .init(v3),
  .q(v21)
);
assign PIPE__bus_2 = v21;
assign PIPE__bus_r__jit_pipeline_vec__L27_2 = PIPE__bus_2;
pyc_mux #(.WIDTH(25)) v22_inst (
  .sel(en__jit_pipeline_vec__L8),
  .a(PIPE__bus__jit_pipeline_vec__L29),
  .b(PIPE__bus_r__jit_pipeline_vec__L27_2),
  .y(v22)
);
assign PIPE__bus__next_2 = v22;
assign PIPE__bus__jit_pipeline_vec__L29_2 = PIPE__bus_r__jit_pipeline_vec__L27_2;
pyc_reg #(.WIDTH(25)) v23_inst (
  .clk(sys_clk),
  .rst(sys_rst),
  .en(v4),
  .d(PIPE__bus__next_3),
  .init(v3),
  .q(v23)
);
assign PIPE__bus_3 = v23;
assign PIPE__bus_r__jit_pipeline_vec__L27_3 = PIPE__bus_3;
pyc_mux #(.WIDTH(25)) v24_inst (
  .sel(en__jit_pipeline_vec__L8),
  .a(PIPE__bus__jit_pipeline_vec__L29_2),
  .b(PIPE__bus_r__jit_pipeline_vec__L27_3),
  .y(v24)
);
assign PIPE__bus__next_3 = v24;
assign PIPE__bus__jit_pipeline_vec__L29_3 = PIPE__bus_r__jit_pipeline_vec__L27_3;
assign bus__jit_pipeline_vec__L25 = PIPE__bus__jit_pipeline_vec__L29_3;
assign v25 = bus__jit_pipeline_vec__L25[7:0];
assign v26 = bus__jit_pipeline_vec__L25[23:8];
assign v27 = bus__jit_pipeline_vec__L25[24];
assign v28 = v25;
assign v29 = v26;
assign v30 = v27;
assign tag = v30;
assign data = v29;
assign lo8 = v28;

endmodule

