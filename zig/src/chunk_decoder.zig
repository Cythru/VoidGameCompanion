// Minecraft chunk section palette decoder.
//
// Each chunk section stores block IDs in a compacted long array where N bits
// per block are packed into 64-bit longs.  This module decodes those arrays
// into flat u16 block-ID arrays.
//
// Performance:
//   - Comptime-specialised inner loops for the most common bits-per-block
//     values (4, 5, 6, 8, 15) so the compiler can unroll / vectorise.
//   - Runtime fallback for arbitrary bits-per-block.
//
// All exported symbols use the C calling convention.

const std = @import("std");

// -------------------------------------------------------------------
// Error codes
// -------------------------------------------------------------------

pub const GcChunkError = enum(c_int) {
    ok = 0,
    null_pointer = -1,
    invalid_bits = -2,
    buffer_too_small = -3,
    data_too_short = -4,
};

// -------------------------------------------------------------------
// Comptime-specialised decoder
// -------------------------------------------------------------------

/// Decode a compacted long array with a compile-time-known bits_per_block.
fn decodeComptime(comptime bpb: u5, longs: [*]const u64, num_longs: usize, out: [*]u16, out_cap: usize) c_int {
    const mask: u64 = (@as(u64, 1) << bpb) - 1;
    const entries_per_long = @divFloor(@as(u6, 64), @as(u6, bpb));

    var out_idx: usize = 0;
    for (0..num_longs) |li| {
        var val = longs[li];
        for (0..entries_per_long) |_| {
            if (out_idx >= out_cap) return @intFromEnum(GcChunkError.ok);
            out[out_idx] = @truncate(val & mask);
            out_idx += 1;
            val >>= bpb;
        }
    }

    return @intFromEnum(GcChunkError.ok);
}

/// Runtime decoder for arbitrary bits_per_block.
fn decodeRuntime(bpb: u6, longs: [*]const u64, num_longs: usize, out: [*]u16, out_cap: usize) c_int {
    if (bpb == 0 or bpb > 64) return @intFromEnum(GcChunkError.invalid_bits);

    const mask: u64 = if (bpb == 64) ~@as(u64, 0) else (@as(u64, 1) << bpb) - 1;
    const entries_per_long = @divFloor(@as(u7, 64), @as(u7, bpb));

    var out_idx: usize = 0;
    for (0..num_longs) |li| {
        var val = longs[li];
        for (0..entries_per_long) |_| {
            if (out_idx >= out_cap) return @intFromEnum(GcChunkError.ok);
            out[out_idx] = @truncate(val & mask);
            out_idx += 1;
            val >>= @intCast(bpb);
        }
    }

    return @intFromEnum(GcChunkError.ok);
}

// -------------------------------------------------------------------
// C exports
// -------------------------------------------------------------------

/// Decode a chunk section from compacted longs.
///
/// `longs`      — pointer to the packed u64 array (host byte order).
/// `num_longs`  — number of u64s in the array.
/// `bits_per_block` — bits used per block entry (typically 4..15).
/// `out_blocks` — output buffer for decoded block IDs (u16).
/// `out_cap`    — capacity of `out_blocks` (should be >= 4096 for a full section).
///
/// Returns 0 on success, negative on error.
export fn gc_decode_chunk_section(
    longs: ?[*]const u64,
    num_longs: usize,
    bits_per_block: u8,
    out_blocks: ?[*]u16,
    out_cap: usize,
) callconv(.C) c_int {
    const l = longs orelse return @intFromEnum(GcChunkError.null_pointer);
    const o = out_blocks orelse return @intFromEnum(GcChunkError.null_pointer);

    if (bits_per_block == 0 or bits_per_block > 64)
        return @intFromEnum(GcChunkError.invalid_bits);

    // Dispatch to comptime-specialised versions for common values.
    return switch (bits_per_block) {
        4 => decodeComptime(4, l, num_longs, o, out_cap),
        5 => decodeComptime(5, l, num_longs, o, out_cap),
        6 => decodeComptime(6, l, num_longs, o, out_cap),
        8 => decodeComptime(8, l, num_longs, o, out_cap),
        15 => decodeComptime(15, l, num_longs, o, out_cap),
        else => decodeRuntime(@intCast(bits_per_block), l, num_longs, o, out_cap),
    };
}

/// Decode a single palette entry given a long array and a block index.
/// This avoids decoding the entire section when you only need one block.
///
/// Returns the block ID, or a negative error code.
export fn gc_decode_palette_entry(
    longs: ?[*]const u64,
    num_longs: usize,
    bits_per_block: u8,
    block_index: usize,
) callconv(.C) c_int {
    const l = longs orelse return @intFromEnum(GcChunkError.null_pointer);

    if (bits_per_block == 0 or bits_per_block > 64)
        return @intFromEnum(GcChunkError.invalid_bits);

    const bpb: u6 = @intCast(bits_per_block);
    const entries_per_long: usize = @divFloor(@as(usize, 64), @as(usize, bpb));
    const long_idx = block_index / entries_per_long;
    const bit_offset = (block_index % entries_per_long) * @as(usize, bpb);

    if (long_idx >= num_longs)
        return @intFromEnum(GcChunkError.data_too_short);

    const mask: u64 = if (bpb == 64) ~@as(u64, 0) else (@as(u64, 1) << bpb) - 1;
    const val = (l[long_idx] >> @intCast(bit_offset)) & mask;

    return @intCast(val);
}

// -------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------

test "decode 4-bit section" {
    // Pack 16 entries per u64, values 0..15 repeating.
    const num_longs: usize = 256; // 4096 / 16
    var longs: [256]u64 = undefined;

    for (0..num_longs) |li| {
        var packed: u64 = 0;
        for (0..16) |ei| {
            const block_id: u64 = (li * 16 + ei) & 0xF;
            packed |= block_id << @intCast(ei * 4);
        }
        longs[li] = packed;
    }

    var out: [4096]u16 = undefined;
    const rc = gc_decode_chunk_section(&longs, num_longs, 4, &out, 4096);
    try std.testing.expectEqual(@as(c_int, 0), rc);

    // Verify.
    for (0..4096) |i| {
        try std.testing.expectEqual(@as(u16, @truncate(i & 0xF)), out[i]);
    }
}

test "decode palette entry" {
    var longs = [_]u64{ 0x0807060504030201, 0x100F0E0D0C0B0A09 };
    // bits_per_block = 8 → 8 entries per long.
    const val = gc_decode_palette_entry(&longs, 2, 8, 3);
    try std.testing.expectEqual(@as(c_int, 4), val);

    const val2 = gc_decode_palette_entry(&longs, 2, 8, 8);
    try std.testing.expectEqual(@as(c_int, 9), val2);
}

test "null pointer returns error" {
    const rc = gc_decode_chunk_section(null, 0, 4, null, 0);
    try std.testing.expectEqual(@intFromEnum(GcChunkError.null_pointer), rc);
}
