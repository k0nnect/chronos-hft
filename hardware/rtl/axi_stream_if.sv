// axi4-stream interface + the packed beat layouts carried over it.
//
// the package defines the two beat formats: the inbound book update (four 32-bit
// fixed-point fields) & the outbound feature beat (micro-price + imbalance).
// using packed structs makes the bit layout unambiguous & shared between the
// rtl, the testbench & the c++ driver (which packs identical words).
//
// the interface itself is a thin, standard axi-stream bundle with master/slave
// modports. note: the synthesizable top (feature_extractor) exposes *flat* axi
// signals rather than an interface port, because verilator's support for
// interface ports on the verilated top is limited; the interface is used to wire
// things together cleanly inside the testbench.
`default_nettype none

package hft_axi_pkg;

    // ---- inbound: a top-of-book update (128 bits) -------------------------
    // packed struct fields are msb-first, so best_bid_price occupies the low
    // 32 bits [31:0] & best_ask_qty the high 32 bits [127:96]. the c++ driver
    // packs the four little-endian 32-bit words in the same order.
    typedef struct packed {
        logic [31:0] best_ask_qty;    // [127:96]
        logic [31:0] best_ask_price;  // [95:64]
        logic [31:0] best_bid_qty;    // [63:32]
        logic [31:0] best_bid_price;  // [31:0]
    } book_update_beat_t;

    localparam int unsigned BOOK_BEAT_W = $bits(book_update_beat_t);  // 128

    // ---- outbound: the computed features (64 bits) ------------------------
    // micro_price occupies the low 32 bits [31:0]; imbalance the high 32 bits.
    typedef struct packed {
        logic signed [31:0] imbalance;    // [63:32] Q.16 signed, in [-1, 1]
        logic        [31:0] micro_price;  // [31:0]  Q16.16 unsigned
    } feature_beat_t;

    localparam int unsigned FEATURE_BEAT_W = $bits(feature_beat_t);  // 64

endpackage : hft_axi_pkg

// generic axi4-stream channel.
interface axi_stream_if #(
    parameter int unsigned DATA_W = 32
) (
    input wire aclk,
    input wire aresetn
);
    logic [DATA_W-1:0] tdata;
    logic              tvalid;
    logic              tready;
    logic              tlast;

    // source of the stream.
    modport master(
        output tdata,
        output tvalid,
        input  tready,
        output tlast,
        input  aclk,
        input  aresetn
    );

    // sink of the stream.
    modport slave(
        input  tdata,
        input  tvalid,
        output tready,
        input  tlast,
        input  aclk,
        input  aresetn
    );
endinterface : axi_stream_if

`default_nettype wire
