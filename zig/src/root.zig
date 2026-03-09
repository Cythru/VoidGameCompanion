// Root module — re-exports all C-callable symbols so the static library
// contains every exported function from every sub-module.

pub const packet_codec = @import("packet_codec.zig");
pub const chunk_decoder = @import("chunk_decoder.zig");
pub const nbt_parser = @import("nbt_parser.zig");
pub const zlib_stream = @import("zlib_stream.zig");
pub const allocator = @import("allocator.zig");

// Force the compiler to analyse (and therefore emit) the exported symbols
// from each module even though nothing in *this* file calls them directly.
comptime {
    _ = packet_codec;
    _ = chunk_decoder;
    _ = nbt_parser;
    _ = zlib_stream;
    _ = allocator;
}
