// Minecraft VarInt / VarLong codec and packet framing.
//
// The Minecraft protocol encodes integers as variable-length sequences of
// 7-bit groups with a continuation bit (MSB).  This module provides:
//
//   - Single VarInt / VarLong decode & encode
//   - SIMD-accelerated batch VarInt decode (processes 16 bytes at once to
//     locate continuation boundaries, then scalar extraction)
//   - Packet framing: prepends a VarInt length header to a payload
//
// All exported symbols use the C calling convention.

const std = @import("std");

// -------------------------------------------------------------------
// Error codes returned to C callers
// -------------------------------------------------------------------

pub const GcCodecError = enum(c_int) {
    ok = 0,
    buffer_too_small = -1,
    varint_too_long = -2,
    unexpected_end = -3,
    null_pointer = -4,
};

// -------------------------------------------------------------------
// VarInt (32-bit)
// -------------------------------------------------------------------

/// Decode a VarInt from `data[0..len]`.
/// On success writes the decoded value to `out_value`, the number of bytes
/// consumed to `out_bytes_read`, and returns 0.
export fn gc_decode_varint(
    data: ?[*]const u8,
    len: usize,
    out_value: ?*i32,
    out_bytes_read: ?*usize,
) callconv(.C) c_int {
    const d = data orelse return @intFromEnum(GcCodecError.null_pointer);
    const ov = out_value orelse return @intFromEnum(GcCodecError.null_pointer);
    const ob = out_bytes_read orelse return @intFromEnum(GcCodecError.null_pointer);

    var result: u32 = 0;
    var shift: u5 = 0;
    var i: usize = 0;

    while (i < len) {
        const byte = d[i];
        result |= @as(u32, byte & 0x7F) << shift;
        i += 1;

        if (byte & 0x80 == 0) {
            ov.* = @bitCast(result);
            ob.* = i;
            return @intFromEnum(GcCodecError.ok);
        }

        if (shift >= 28) {
            return @intFromEnum(GcCodecError.varint_too_long);
        }
        shift += 7;
    }

    return @intFromEnum(GcCodecError.unexpected_end);
}

/// Encode a VarInt into `buf[0..buf_len]`.
/// Returns the number of bytes written (1..5), or a negative error code.
export fn gc_encode_varint(
    value: i32,
    buf: ?[*]u8,
    buf_len: usize,
) callconv(.C) c_int {
    const b = buf orelse return @intFromEnum(GcCodecError.null_pointer);

    var v: u32 = @bitCast(value);
    var i: usize = 0;

    while (true) {
        if (i >= buf_len) return @intFromEnum(GcCodecError.buffer_too_small);

        if (v & ~@as(u32, 0x7F) == 0) {
            b[i] = @truncate(v);
            return @intCast(i + 1);
        }

        b[i] = @as(u8, @truncate(v & 0x7F)) | 0x80;
        v >>= 7;
        i += 1;
    }
}

// -------------------------------------------------------------------
// VarLong (64-bit)
// -------------------------------------------------------------------

/// Decode a VarLong from `data[0..len]`.
export fn gc_decode_varlong(
    data: ?[*]const u8,
    len: usize,
    out_value: ?*i64,
    out_bytes_read: ?*usize,
) callconv(.C) c_int {
    const d = data orelse return @intFromEnum(GcCodecError.null_pointer);
    const ov = out_value orelse return @intFromEnum(GcCodecError.null_pointer);
    const ob = out_bytes_read orelse return @intFromEnum(GcCodecError.null_pointer);

    var result: u64 = 0;
    var shift: u7 = 0;
    var i: usize = 0;

    while (i < len) {
        const byte = d[i];
        result |= @as(u64, byte & 0x7F) << @intCast(shift);
        i += 1;

        if (byte & 0x80 == 0) {
            ov.* = @bitCast(result);
            ob.* = i;
            return @intFromEnum(GcCodecError.ok);
        }

        if (shift >= 63) {
            return @intFromEnum(GcCodecError.varint_too_long);
        }
        shift += 7;
    }

    return @intFromEnum(GcCodecError.unexpected_end);
}

// -------------------------------------------------------------------
// Packet framing
// -------------------------------------------------------------------

/// Frame a packet: writes `[VarInt length][payload]` into `out_buf`.
/// `payload_len` is the size of the payload.
/// On success returns total bytes written; on failure a negative error code.
export fn gc_frame_packet(
    payload: ?[*]const u8,
    payload_len: usize,
    out_buf: ?[*]u8,
    out_buf_len: usize,
    out_total: ?*usize,
) callconv(.C) c_int {
    const p = payload orelse return @intFromEnum(GcCodecError.null_pointer);
    const ob = out_buf orelse return @intFromEnum(GcCodecError.null_pointer);
    const ot = out_total orelse return @intFromEnum(GcCodecError.null_pointer);

    // Encode the length header into a small stack buffer.
    var hdr_buf: [5]u8 = undefined;
    const hdr_rc = gc_encode_varint(@intCast(payload_len), &hdr_buf, 5);
    if (hdr_rc < 0) return hdr_rc;
    const hdr_len: usize = @intCast(hdr_rc);

    const total = hdr_len + payload_len;
    if (total > out_buf_len) return @intFromEnum(GcCodecError.buffer_too_small);

    @memcpy(ob[0..hdr_len], hdr_buf[0..hdr_len]);
    @memcpy(ob[hdr_len .. hdr_len + payload_len], p[0..payload_len]);
    ot.* = total;

    return @intFromEnum(GcCodecError.ok);
}

// -------------------------------------------------------------------
// SIMD-accelerated batch VarInt decode
// -------------------------------------------------------------------

/// Batch-decode up to `max_count` VarInts from `data[0..len]`.
/// Writes decoded values into `out_values[0..max_count]` and stores the
/// number actually decoded in `out_count`.
/// Returns 0 on success, negative on error.
export fn gc_batch_decode_varint(
    data: ?[*]const u8,
    len: usize,
    out_values: ?[*]i32,
    max_count: usize,
    out_count: ?*usize,
) callconv(.C) c_int {
    const d = data orelse return @intFromEnum(GcCodecError.null_pointer);
    const vals = out_values orelse return @intFromEnum(GcCodecError.null_pointer);
    const oc = out_count orelse return @intFromEnum(GcCodecError.null_pointer);

    var offset: usize = 0;
    var count: usize = 0;

    while (count < max_count and offset < len) {
        // Use SIMD to quickly find the terminating byte (MSB clear).
        // Load up to 16 bytes (or whatever remains) and check MSB.
        const remaining = len - offset;
        const varint_end = simdFindVarIntEnd(d + offset, remaining);

        if (varint_end == 0) {
            // Could not find a terminator in the available bytes.
            oc.* = count;
            return @intFromEnum(GcCodecError.unexpected_end);
        }

        if (varint_end > 5) {
            oc.* = count;
            return @intFromEnum(GcCodecError.varint_too_long);
        }

        // Scalar extract — the varint is at most 5 bytes.
        var result: u32 = 0;
        var shift: u5 = 0;
        for (0..varint_end) |j| {
            result |= @as(u32, d[offset + j] & 0x7F) << shift;
            shift +|= 7;
        }
        vals[count] = @bitCast(result);
        count += 1;
        offset += varint_end;
    }

    oc.* = count;
    return @intFromEnum(GcCodecError.ok);
}

/// Use SIMD (128-bit) to locate the first byte with MSB == 0 in the buffer,
/// returning its 1-based position (i.e. byte count of the varint).
/// Returns 0 if no terminator found within `len` bytes (max 16 scanned).
fn simdFindVarIntEnd(ptr: [*]const u8, len: usize) usize {
    const scan_len = @min(len, 16);
    if (scan_len == 0) return 0;

    // If we have enough bytes for a vector approach, use it.
    if (scan_len >= 16) {
        const vec: @Vector(16, u8) = ptr[0..16].*;
        const msb_mask: @Vector(16, u8) = @splat(0x80);
        const and_result = vec & msb_mask;
        const zero_vec: @Vector(16, u8) = @splat(0);
        const cmp = and_result == zero_vec; // true where MSB is clear
        const bitmask = @as(u16, @bitCast(cmp));
        if (bitmask != 0) {
            return @ctz(bitmask) + 1;
        }
        return 0;
    }

    // Scalar fallback for short tails.
    for (0..scan_len) |i| {
        if (ptr[i] & 0x80 == 0) return i + 1;
    }
    return 0;
}

// -------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------

test "varint round-trip" {
    const cases = [_]i32{ 0, 1, -1, 127, 128, 255, 25565, 2097151, 2147483647, -2147483648 };
    for (cases) |val| {
        var buf: [5]u8 = undefined;
        const n = gc_encode_varint(val, &buf, 5);
        try std.testing.expect(n > 0);

        var decoded: i32 = undefined;
        var consumed: usize = undefined;
        const rc = gc_decode_varint(&buf, @intCast(n), &decoded, &consumed);
        try std.testing.expectEqual(@as(c_int, 0), rc);
        try std.testing.expectEqual(val, decoded);
        try std.testing.expectEqual(@as(usize, @intCast(n)), consumed);
    }
}

test "varlong round-trip" {
    const cases = [_]i64{ 0, 1, -1, 2147483647, -2147483648, 9223372036854775807, -9223372036854775808 };
    for (cases) |val| {
        // Encode manually.
        var buf: [10]u8 = undefined;
        var v: u64 = @bitCast(val);
        var i: usize = 0;
        while (true) {
            if (v & ~@as(u64, 0x7F) == 0) {
                buf[i] = @truncate(v);
                i += 1;
                break;
            }
            buf[i] = @as(u8, @truncate(v & 0x7F)) | 0x80;
            v >>= 7;
            i += 1;
        }

        var decoded: i64 = undefined;
        var consumed: usize = undefined;
        const rc = gc_decode_varlong(&buf, i, &decoded, &consumed);
        try std.testing.expectEqual(@as(c_int, 0), rc);
        try std.testing.expectEqual(val, decoded);
    }
}

test "frame_packet" {
    const payload = "Hello";
    var out: [64]u8 = undefined;
    var total: usize = undefined;
    const rc = gc_frame_packet(payload.ptr, payload.len, &out, 64, &total);
    try std.testing.expectEqual(@as(c_int, 0), rc);
    // First byte should be the VarInt-encoded length (5).
    try std.testing.expectEqual(@as(u8, 5), out[0]);
    try std.testing.expectEqual(@as(usize, 6), total);
    try std.testing.expectEqualSlices(u8, "Hello", out[1..6]);
}

test "batch decode varint" {
    // Encode several varints back to back.
    var buf: [32]u8 = undefined;
    var pos: usize = 0;
    const values = [_]i32{ 0, 1, 300, -1 };
    for (values) |v| {
        const n = gc_encode_varint(v, buf[pos..].ptr, 32 - pos);
        try std.testing.expect(n > 0);
        pos += @intCast(n);
    }

    var out: [16]i32 = undefined;
    var count: usize = undefined;
    const rc = gc_batch_decode_varint(&buf, pos, &out, 16, &count);
    try std.testing.expectEqual(@as(c_int, 0), rc);
    try std.testing.expectEqual(@as(usize, 4), count);
    for (values, 0..) |v, i| {
        try std.testing.expectEqual(v, out[i]);
    }
}
