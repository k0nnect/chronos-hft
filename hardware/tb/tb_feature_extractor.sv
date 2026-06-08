// self-checking systemverilog testbench for feature_extractor.
//
// streams a batch of directed/pseudo-random book-update beats through the dut &
// checks each emitted feature beat against an in-testbench golden model that uses
// the same integer arithmetic as the rtl (here a plain `/` is fine -- the tb is
// not synthesized). this is the pure-rtl sanity check; the broader cross-check
// against the phase-2 market generator & the floating-point order_book lives in
// the c++ harness (hardware/tb/cosim_check.cpp).
//
// runnable under verilator (`--binary --timing`) or any event simulator. the few
// timing controls are confined to the tb & fenced from the rtl lint rules.
`default_nettype none

module tb_feature_extractor;
    import hft_axi_pkg::*;

    localparam int unsigned FRAC    = 16;
    localparam int unsigned LATENCY = FRAC + 3;
    localparam int unsigned NVEC    = 256;

    logic clk;
    logic rst_n;

    // clock / reset generation (tb-only timing).
    /* verilator lint_off STMTDLY */
    initial clk = 1'b0;
    always #5 clk = ~clk;
    /* verilator lint_on STMTDLY */

    // dut connection via the axi-stream interface.
    axi_stream_if #(.DATA_W(128)) s_axi (.aclk(clk), .aresetn(rst_n));
    axi_stream_if #(.DATA_W(64))  m_axi (.aclk(clk), .aresetn(rst_n));

    feature_extractor #(
        .PRICE_W(32),
        .QTY_W  (32),
        .FRAC   (FRAC)
    ) dut (
        .clk     (clk),
        .rst_n   (rst_n),
        .s_tdata (s_axi.tdata),
        .s_tvalid(s_axi.tvalid),
        .s_tready(s_axi.tready),
        .s_tlast (s_axi.tlast),
        .m_tdata (m_axi.tdata),
        .m_tvalid(m_axi.tvalid),
        .m_tready(m_axi.tready),
        .m_tlast (m_axi.tlast)
    );

    assign m_axi.tready = 1'b1;

    // ---- golden model (integer, matches the rtl datapath) -----------------
    function automatic logic [31:0] gold_micro(input logic [31:0] bid,
                                               input logic [31:0] ask,
                                               input logic [31:0] bq,
                                               input logic [31:0] aq);
        logic [32:0]        denom;
        logic [63:0]        weight;
        logic signed [63:0] sdiff;
        logic signed [63:0] micro;
        denom  = {1'b0, bq} + {1'b0, aq};
        weight = ({32'b0, bq} << FRAC) / {31'b0, denom};
        sdiff  = $signed({33'b0, ask}) - $signed({33'b0, bid});
        micro  = ($signed({33'b0, bid}) <<< FRAC) + sdiff * $signed(weight);
        return micro[31:0];
    endfunction

    function automatic logic signed [31:0] gold_imb(input logic [31:0] bq,
                                                    input logic [31:0] aq);
        logic [32:0]        denom;
        logic [31:0]        absd;
        logic               neg;
        logic [63:0]        ratio;
        denom = {1'b0, bq} + {1'b0, aq};
        neg   = aq > bq;
        absd  = neg ? (aq - bq) : (bq - aq);
        ratio = ({32'b0, absd} << FRAC) / {31'b0, denom};
        return neg ? -$signed(ratio[31:0]) : $signed(ratio[31:0]);
    endfunction

    // ---- stimulus & expected vectors ------------------------------------
    logic [31:0] vbid  [0:NVEC-1];
    logic [31:0] vask  [0:NVEC-1];
    logic [31:0] vbq   [0:NVEC-1];
    logic [31:0] vaq   [0:NVEC-1];
    logic [31:0] e_mp  [0:NVEC-1];
    logic [31:0] e_imb [0:NVEC-1];

    integer in_idx;
    integer out_idx;
    integer errors;

    book_update_beat_t beat;

    initial begin
        // deterministic pseudo-random vectors with a sane, uncrossed book.
        for (int n = 0; n < NVEC; n++) begin
            automatic logic [31:0] base_rel = 32'($urandom_range(0, 2000));
            automatic logic [31:0] spr      = 32'($urandom_range(1, 4));
            vbid[n] = base_rel;
            vask[n] = base_rel + spr;
            vbq[n]  = 32'($urandom_range(1, 1000));
            vaq[n]  = 32'($urandom_range(1, 1000));
            e_mp[n]  = gold_micro(vbid[n], vask[n], vbq[n], vaq[n]);
            e_imb[n] = gold_imb(vbq[n], vaq[n]);
        end

        in_idx  = 0;
        out_idx = 0;
        errors  = 0;
        rst_n   = 1'b0;
        s_axi.tvalid = 1'b0;
        s_axi.tlast  = 1'b0;
        s_axi.tdata  = '0;

        repeat (5) @(posedge clk);
        rst_n <= 1'b1;
    end

    // drive one beat per cycle until all vectors are sent.
    always_ff @(posedge clk) begin
        if (!rst_n) begin
            s_axi.tvalid <= 1'b0;
        end else if (in_idx < NVEC) begin
            beat.best_bid_price <= vbid[in_idx];
            beat.best_bid_qty   <= vbq[in_idx];
            beat.best_ask_price <= vask[in_idx];
            beat.best_ask_qty   <= vaq[in_idx];
            s_axi.tdata         <= {vaq[in_idx], vask[in_idx], vbq[in_idx], vbid[in_idx]};
            s_axi.tvalid        <= 1'b1;
            s_axi.tlast         <= (in_idx == NVEC - 1);
            in_idx              <= in_idx + 1;
        end else begin
            s_axi.tvalid <= 1'b0;
            s_axi.tlast  <= 1'b0;
        end
    end

    // check outputs in order as they appear.
    always_ff @(posedge clk) begin
        if (rst_n && m_axi.tvalid) begin
            if (m_axi.tdata[31:0] !== e_mp[out_idx]) begin
                $error("vec %0d: micro mismatch rtl=%08x exp=%08x", out_idx,
                       m_axi.tdata[31:0], e_mp[out_idx]);
                errors <= errors + 1;
            end
            if (m_axi.tdata[63:32] !== e_imb[out_idx]) begin
                $error("vec %0d: imbalance mismatch rtl=%08x exp=%08x", out_idx,
                       m_axi.tdata[63:32], e_imb[out_idx]);
                errors <= errors + 1;
            end
            out_idx <= out_idx + 1;
            if (out_idx == NVEC - 1) begin
                if (errors == 0) begin
                    $display("tb_feature_extractor: PASS (%0d vectors)", NVEC);
                end else begin
                    $display("tb_feature_extractor: FAIL (%0d errors)", errors);
                end
                $finish;
            end
        end
    end

    // safety timeout.
    initial begin
        repeat (NVEC + LATENCY + 100) @(posedge clk);
        $error("tb_feature_extractor: timeout, only %0d/%0d outputs seen", out_idx, NVEC);
        $finish;
    end

endmodule

`default_nettype wire
