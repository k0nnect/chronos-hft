// pipelined radix-2 fractional divider.
//
// computes quotient = floor(numerator * 2^FRAC / denominator) as an unsigned
// Q0.FRAC value, for the case numerator <= denominator (so the result lies in
// [0, 1]). this is the exact primitive both the micro-price weight and the
// volume-imbalance ratio need.
//
// it is a classic non-restoring-style shift/subtract, but unrolled into one
// pipeline stage per quotient bit: every stage shifts the running remainder up
// by one, compares against the divisor, conditionally subtracts, and shifts the
// resulting bit into the quotient. because every stage is a register, the unit
// is fully pipelined -- it accepts a new dividend every clock and produces a
// result FRAC+1 clocks later. the valid bit rides through the same registers so
// outputs stay aligned with their inputs. no `/` or `%` is ever inferred.
//
// strictly synchronous, asynchronous active-low reset, non-blocking assignments
// for all sequential state. verilator-lint clean.
`default_nettype none

module frac_divider #(
    parameter int unsigned WIDTH = 33,  // operand width (numerator / denominator)
    parameter int unsigned FRAC  = 16   // fractional quotient bits == pipeline depth
) (
    input  wire                   clk,
    input  wire                   rst_n,
    input  wire                   in_valid,
    input  wire  [WIDTH-1:0]      numerator,    // must satisfy numerator <= denominator
    input  wire  [WIDTH-1:0]      denominator,  // must be non-zero
    output wire                   out_valid,
    output wire  [FRAC-1:0]       quotient      // Q0.FRAC, in [0, 1]
);
    // requires FRAC >= 2 so the {q[FRAC-2:0], bit} concatenation below is always
    // well-formed (FRAC == 1 would index q[-1:0]); the project uses FRAC = 16.

    // pipeline state, one extra slot (index 0) for the input-capture stage.
    // rem is one bit wider than the operands to hold the pre-subtract left shift.
    logic            valid_r [0:FRAC];
    logic [WIDTH:0]  rem_r   [0:FRAC];  // running remainder
    logic [WIDTH-1:0] den_r  [0:FRAC];  // divisor carried alongside the data
    logic [FRAC-1:0] quot_r  [0:FRAC];  // quotient accumulated msb-first

    // stage 0: capture the operands. the remainder starts as the numerator.
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            valid_r[0] <= 1'b0;
            rem_r[0]   <= '0;
            den_r[0]   <= '0;
            quot_r[0]  <= '0;
        end else begin
            valid_r[0] <= in_valid;
            rem_r[0]   <= {1'b0, numerator};
            den_r[0]   <= denominator;
            quot_r[0]  <= '0;
        end
    end

    // stages 1..FRAC: one quotient bit each.
    genvar i;
    generate
        for (i = 0; i < int'(FRAC); i++) begin : g_stage
            // combinational shift/compare for this stage's inputs.
            wire [WIDTH:0] shifted = {rem_r[i][WIDTH-1:0], 1'b0};
            wire [WIDTH:0] divisor = {1'b0, den_r[i]};
            wire           take    = (shifted >= divisor);

            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    valid_r[i+1] <= 1'b0;
                    rem_r[i+1]   <= '0;
                    den_r[i+1]   <= '0;
                    quot_r[i+1]  <= '0;
                end else begin
                    valid_r[i+1] <= valid_r[i];
                    den_r[i+1]   <= den_r[i];
                    rem_r[i+1]   <= take ? (shifted - divisor) : shifted;
                    quot_r[i+1]  <= {quot_r[i][FRAC-2:0], take};
                end
            end
        end
    endgenerate

    assign out_valid = valid_r[FRAC];
    assign quotient  = quot_r[FRAC];

endmodule

`default_nettype wire
