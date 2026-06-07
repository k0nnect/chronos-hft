// fully pipelined fixed-point volume-imbalance calculator.
//
// imbalance = (bid_qty - ask_qty) / (bid_qty + ask_qty), a signed ratio in
// [-1, 1]. the magnitude |bid_qty - ask_qty| / (bid_qty + ask_qty) is a proper
// fraction, so the radix-2 fractional divider produces it directly as Q0.FRAC;
// the sign is computed up front and re-applied after the divide. the result is
// emitted as a signed Q.FRAC value (value == imbalance * 2^FRAC).
//
// latency is FRAC + 3 cycles, matched to micro_price.sv so the feature_extractor
// can emit both fields in one aligned beat: FRAC + 1 for the divider, one stage
// to apply the sign, and one stage to register the sign-extended output. fully
// pipelined, synchronous, async active-low reset, non-blocking sequential.
`default_nettype none

module volume_imbalance #(
    parameter int unsigned QTY_W = 32,
    parameter int unsigned FRAC  = 16,
    parameter int unsigned OUT_W = 32   // signed Q.FRAC
) (
    input  wire                     clk,
    input  wire                     rst_n,
    input  wire                     in_valid,
    input  wire [QTY_W-1:0]         bid_qty,
    input  wire [QTY_W-1:0]         ask_qty,
    output wire                     out_valid,
    output wire signed [OUT_W-1:0]  imbalance   // Q.FRAC, in [-1, 1]
);
    localparam int unsigned DEN_W = QTY_W + 1;
    localparam int unsigned DLY   = FRAC + 1;  // divider latency

    wire [DEN_W-1:0] denom    = {1'b0, bid_qty} + {1'b0, ask_qty};
    wire             bid_ge   = (bid_qty >= ask_qty);
    wire [QTY_W-1:0] abs_diff = bid_ge ? (bid_qty - ask_qty) : (ask_qty - bid_qty);
    wire             neg      = ~bid_ge;  // imbalance is negative when ask_qty > bid_qty

    // ratio = |bid_qty - ask_qty| / (bid_qty + ask_qty) in Q0.FRAC.
    wire             ratio_valid;
    wire [FRAC-1:0]  ratio;
    frac_divider #(
        .WIDTH(DEN_W),
        .FRAC (FRAC)
    ) u_div (
        .clk        (clk),
        .rst_n      (rst_n),
        .in_valid   (in_valid),
        .numerator  ({1'b0, abs_diff}),
        .denominator(denom),
        .out_valid  (ratio_valid),
        .quotient   (ratio)
    );

    // delay the sign bit to meet the ratio at the sign-apply stage.
    logic [DLY-1:0] neg_dly;
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            neg_dly <= '0;
        end else begin
            neg_dly <= {neg_dly[DLY-2:0], neg};
        end
    end

    // stage S (apply sign): produce a signed Q.FRAC value.
    localparam int unsigned SGN_W = FRAC + 1;  // ratio plus sign room

    logic                   sign_valid;
    logic signed [SGN_W-1:0] imb_signed_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            sign_valid   <= 1'b0;
            imb_signed_q <= '0;
        end else begin
            sign_valid <= ratio_valid;
            imb_signed_q <= neg_dly[DLY-1] ? -$signed({1'b0, ratio})
                                           :  $signed({1'b0, ratio});
        end
    end

    // stage O (output register): sign-extend to OUT_W. this extra stage equalises
    // the latency with micro_price.sv (FRAC + 3 total).
    logic                  out_valid_q;
    logic signed [OUT_W-1:0] imb_q;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            out_valid_q <= 1'b0;
            imb_q       <= '0;
        end else begin
            out_valid_q <= sign_valid;
            imb_q       <= OUT_W'($signed(imb_signed_q));
        end
    end

    assign out_valid = out_valid_q;
    assign imbalance = imb_q;

endmodule

`default_nettype wire
