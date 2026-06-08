// fully pipelined fixed-point micro-price calculator.
//
// the micro-price (bid*ask_qty + ask*bid_qty) / (bid_qty + ask_qty) is algebraic
// ally identical to
//
//     micro = bid + (ask - bid) * weight,   weight = bid_qty / (bid_qty + ask_qty)
//
// which is far friendlier to hardware: `weight` is a proper fraction in [0, 1],
// so it is produced by the radix-2 fractional divider directly, & the rest is
// one signed multiply & one add. the result is emitted as Q16.16 fixed point
// (16 integer bits of price, 16 fractional bits). prices are expected to be band
// relative (small), which keeps the Q16.16 result inside 32 bits.
//
// latency is FRAC + 3 cycles: FRAC + 1 for the divider, then a multiply stage
// & an accumulate stage. operand/`valid` alignment is held by a shift-register
// delay line that matches the divider depth exactly. fully pipelined (one result
// per clock). synchronous, async active-low reset, non-blocking sequential.
`default_nettype none

module micro_price #(
    parameter int unsigned PRICE_W = 32,
    parameter int unsigned QTY_W   = 32,
    parameter int unsigned FRAC    = 16,
    parameter int unsigned OUT_W   = 32   // Q(OUT_W-FRAC).FRAC
) (
    input  wire                 clk,
    input  wire                 rst_n,
    input  wire                 in_valid,
    input  wire [PRICE_W-1:0]   bid_price,  // band-relative
    input  wire [PRICE_W-1:0]   ask_price,  // band-relative
    input  wire [QTY_W-1:0]     bid_qty,
    input  wire [QTY_W-1:0]     ask_qty,
    output wire                 out_valid,
    output wire [OUT_W-1:0]     micro_price  // Q16.16, band-relative
);
    localparam int unsigned DEN_W = QTY_W + 1;  // bid_qty + ask_qty
    localparam int unsigned DLY   = FRAC + 1;   // divider latency (capture + FRAC)

    // combinational denominator / numerator for the weight = bid_qty / (bq + aq).
    wire [DEN_W-1:0] denom     = {1'b0, bid_qty} + {1'b0, ask_qty};
    wire [DEN_W-1:0] numerator = {1'b0, bid_qty};

    // signed price spread (ask - bid); kept signed so a locked/crossed book is
    // handled gracefully even though the normal book is uncrossed.
    wire signed [PRICE_W:0] spread = $signed({1'b0, ask_price}) - $signed({1'b0, bid_price});

    // weight in Q0.FRAC, available DLY cycles after in_valid.
    wire             weight_valid;
    wire [FRAC-1:0]  weight;
    frac_divider #(
        .WIDTH(DEN_W),
        .FRAC (FRAC)
    ) u_div (
        .clk        (clk),
        .rst_n      (rst_n),
        .in_valid   (in_valid),
        .numerator  (numerator),
        .denominator(denom),
        .out_valid  (weight_valid),
        .quotient   (weight)
    );

    // delay line carrying bid_price & spread alongside the divider so they meet
    // the weight at the multiply stage. index 0 is one cycle deep, index DLY-1 is
    // DLY cycles deep -- the same depth as the divider output.
    logic [PRICE_W-1:0]      bid_dly    [0:DLY-1];
    logic signed [PRICE_W:0] spread_dly [0:DLY-1];

    integer k;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (k = 0; k < int'(DLY); k++) begin
                bid_dly[k]    <= '0;
                spread_dly[k] <= '0;
            end
        end else begin
            bid_dly[0]    <= bid_price;
            spread_dly[0] <= spread;
            for (k = 1; k < int'(DLY); k++) begin
                bid_dly[k]    <= bid_dly[k-1];
                spread_dly[k] <= spread_dly[k-1];
            end
        end
    end

    // stage M (multiply): offset = spread * weight  (Q(.).FRAC), & pre-shift the
    // aligned bid into Q.FRAC so the next stage is a plain add. the product width
    // is the sum of the operand widths: spread is (PRICE_W+1) bits signed & the
    // zero-extended weight is (FRAC+1) bits signed.
    localparam int unsigned PROD_W = (PRICE_W + 1) + (FRAC + 1);  // spread * weight
    localparam int unsigned BSH_W  = PRICE_W + FRAC;              // bid << FRAC

    logic                    mul_valid;
    logic signed [PROD_W-1:0] offset_q;
    logic        [BSH_W-1:0]  bid_shifted_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mul_valid     <= 1'b0;
            offset_q      <= '0;
            bid_shifted_q <= '0;
        end else begin
            mul_valid     <= weight_valid;
            offset_q      <= spread_dly[DLY-1] * $signed({1'b0, weight});
            bid_shifted_q <= {bid_dly[DLY-1], {FRAC{1'b0}}};
        end
    end

    // stage A (accumulate): micro = (bid << FRAC) + offset, truncated to OUT_W.
    // one guard bit above the wider operand covers the signed addition exactly.
    localparam int unsigned ACC_W = (PROD_W > BSH_W ? PROD_W : BSH_W) + 1;

    logic                 out_valid_q;
    logic [OUT_W-1:0]     micro_q;

    wire signed [ACC_W-1:0] micro_full =
        $signed({1'b0, bid_shifted_q}) + $signed(offset_q);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            out_valid_q <= 1'b0;
            micro_q     <= '0;
        end else begin
            out_valid_q <= mul_valid;
            micro_q     <= micro_full[OUT_W-1:0];
        end
    end

    assign out_valid   = out_valid_q;
    assign micro_price = micro_q;

endmodule

`default_nettype wire
