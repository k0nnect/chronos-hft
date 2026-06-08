// top-level feature-extraction accelerator.
//
// consumes a 128-bit book-update beat on a slave axi-stream, runs the micro-price
// & volume-imbalance datapaths concurrently (they share the same FRAC + 3 cycle
// latency by construction), & emits a 64-bit feature beat on a master stream.
//
// the two datapaths are independent & equal-latency, so a single valid/last
// delay line of LATENCY stages reproduces the pipeline delay for the stream
// handshake; the per-datapath out_valid outputs are left for standalone unit
// testing. the pipeline never stalls, so s_tready is held asserted whenever the
// block is out of reset (an always-ready downstream, or a downstream skid buffer,
// is assumed). prices in the beat are band-relative so the Q16.16 micro-price
// fits in 32 bits.
//
// flat axi ports (not an interface) so the verilated top is trivially drivable
// from c++. synchronous, async active-low reset, non-blocking sequential.
`default_nettype none

module feature_extractor #(
    parameter int unsigned PRICE_W = 32,
    parameter int unsigned QTY_W   = 32,
    parameter int unsigned FRAC    = 16
) (
    input  wire         clk,
    input  wire         rst_n,

    // slave stream: inbound book-update beats.
    input  wire [127:0] s_tdata,
    input  wire         s_tvalid,
    output wire         s_tready,
    input  wire         s_tlast,

    // master stream: outbound feature beats.
    output wire [63:0]  m_tdata,
    output wire         m_tvalid,
    input  wire         m_tready,
    output wire         m_tlast
);
    // total datapath latency shared by both feature blocks.
    localparam int unsigned LATENCY = FRAC + 3;

    // unpack the inbound beat (see hft_axi_pkg::book_update_beat_t for the layout).
    wire [PRICE_W-1:0] best_bid_price = s_tdata[31:0];
    wire [QTY_W-1:0]   best_bid_qty   = s_tdata[63:32];
    wire [PRICE_W-1:0] best_ask_price = s_tdata[95:64];
    wire [QTY_W-1:0]   best_ask_qty   = s_tdata[127:96];

    // fully pipelined; we always accept input when out of reset.
    assign s_tready = rst_n;

    wire accept = s_tvalid & s_tready;

    // ---- concurrent feature datapaths ------------------------------------
    wire               mp_valid;
    wire [31:0]        mp_micro;
    micro_price #(
        .PRICE_W(PRICE_W),
        .QTY_W  (QTY_W),
        .FRAC   (FRAC),
        .OUT_W  (32)
    ) u_micro (
        .clk        (clk),
        .rst_n      (rst_n),
        .in_valid   (accept),
        .bid_price  (best_bid_price),
        .ask_price  (best_ask_price),
        .bid_qty    (best_bid_qty),
        .ask_qty    (best_ask_qty),
        .out_valid  (mp_valid),
        .micro_price(mp_micro)
    );

    wire                vi_valid;
    wire signed [31:0]  vi_imb;
    volume_imbalance #(
        .QTY_W(QTY_W),
        .FRAC (FRAC),
        .OUT_W(32)
    ) u_imb (
        .clk      (clk),
        .rst_n    (rst_n),
        .in_valid (accept),
        .bid_qty  (best_bid_qty),
        .ask_qty  (best_ask_qty),
        .out_valid(vi_valid),
        .imbalance(vi_imb)
    );

    // ---- stream handshake delay line -------------------------------------
    // reproduce the LATENCY-cycle pipeline delay for valid / last so the output
    // beat is asserted exactly when its data is ready.
    logic [LATENCY-1:0] valid_sr;
    logic [LATENCY-1:0] last_sr;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_sr <= '0;
            last_sr  <= '0;
        end else begin
            valid_sr <= {valid_sr[LATENCY-2:0], accept};
            last_sr  <= {last_sr[LATENCY-2:0], (accept & s_tlast)};
        end
    end

    // ---- pack the outbound feature beat ----------------------------------
    // [63:32] = imbalance (signed Q.16), [31:0] = micro_price (Q16.16).
    assign m_tdata  = {vi_imb, mp_micro};
    assign m_tvalid = valid_sr[LATENCY-1];
    assign m_tlast  = last_sr[LATENCY-1];

    // the per-datapath valids are aligned with the handshake delay line; keep
    // them observable for unit testing but they do not drive the stream.
    wire _unused_ok = &{1'b0, mp_valid, vi_valid, m_tready};

endmodule

`default_nettype wire
