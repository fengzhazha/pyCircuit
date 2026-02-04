`include "pyc_add.sv"
`include "pyc_mux.sv"
`include "pyc_and.sv"
`include "pyc_or.sv"
`include "pyc_xor.sv"
`include "pyc_not.sv"
`include "pyc_reg.sv"
`include "pyc_fifo.sv"

`include "pyc_byte_mem.sv"

module Counter (
  input logic clk,
  input logic rst,
  input logic en,
  output logic [7:0] count
);

logic [7:0] v1;
logic [7:0] v2;
logic v3;
logic [7:0] v4;
logic [7:0] v5;
logic v6;
logic do__counter__L9;
logic [7:0] count__next;
logic [7:0] v7;
logic [7:0] count;
logic [7:0] count__counter__L11;
logic [7:0] COUNT__c__counter__L15;
logic [7:0] v8;
logic [7:0] v9;
logic [7:0] v10;

assign v1 = 8'd1;
assign v2 = 8'd0;
assign v3 = 1'd1;
assign v4 = v1;
assign v5 = v2;
assign v6 = v3;
assign do__counter__L9 = en;
pyc_reg #(.WIDTH(8)) v7_inst (
  .clk(clk),
  .rst(rst),
  .en(v6),
  .d(count__next),
  .init(v5),
  .q(v7)
);
assign count = v7;
assign count__counter__L11 = count;
assign COUNT__c__counter__L15 = count__counter__L11;
assign v8 = (COUNT__c__counter__L15 + v4);
assign v9 = (do__counter__L9 ? v8 : count__counter__L11);
assign v10 = v9;
assign count__next = v10;
assign count = count__counter__L11;

endmodule

