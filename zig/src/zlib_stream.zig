// Streaming zlib decompression for Minecraft compressed packets.
//
// Minecraft uses zlib (RFC 1950) to compress packet payloads above a
// threshold.  This module wraps the system zlib through Zig's C interop
// and provides:
//
//   - One-shot decompression (`gc_zlib_decompress`)
//   - Streaming decompression via a reusable stream object
//     (`gc_zlib_stream_new`, feed data, `gc_zlib_stream_free`)
//
// Memory is managed via an internal arena that can be bulk-freed.
//
// All exported symbols use the C calling convention.

const std = @import("std");
const c = @cImport({
    @cInclude("zlib.h");
});

// -------------------------------------------------------------------
// Error codes
// -------------------------------------------------------------------

pub const GcZlibError = enum(c_int) {
    ok = 0,
    null_pointer = -1,
    buffer_too_small = -2,
    zlib_error = -3,
    alloc_failed = -4,
    stream_finished = -5,
};

// -------------------------------------------------------------------
// One-shot decompression
// -------------------------------------------------------------------

/// Decompress `src[0..src_len]` (zlib format) into `dst[0..dst_cap]`.
/// On success, stores the number of decompressed bytes in `*out_len`
/// and returns 0.
export fn gc_zlib_decompress(
    src: ?[*]const u8,
    src_len: usize,
    dst: ?[*]u8,
    dst_cap: usize,
    out_len: ?*usize,
) callconv(.C) c_int {
    const s = src orelse return @intFromEnum(GcZlibError.null_pointer);
    const d = dst orelse return @intFromEnum(GcZlibError.null_pointer);
    const ol = out_len orelse return @intFromEnum(GcZlibError.null_pointer);

    var stream: c.z_stream = std.mem.zeroes(c.z_stream);
    stream.next_in = @constCast(s);
    stream.avail_in = @intCast(@min(src_len, std.math.maxInt(c_uint)));
    stream.next_out = d;
    stream.avail_out = @intCast(@min(dst_cap, std.math.maxInt(c_uint)));

    var ret = c.inflateInit(&stream);
    if (ret != c.Z_OK) return @intFromEnum(GcZlibError.zlib_error);

    ret = c.inflate(&stream, c.Z_FINISH);
    const total = stream.total_out;
    _ = c.inflateEnd(&stream);

    if (ret != c.Z_STREAM_END and ret != c.Z_OK) {
        if (ret == c.Z_BUF_ERROR)
            return @intFromEnum(GcZlibError.buffer_too_small);
        return @intFromEnum(GcZlibError.zlib_error);
    }

    ol.* = total;
    return @intFromEnum(GcZlibError.ok);
}

// -------------------------------------------------------------------
// Streaming decompression
// -------------------------------------------------------------------

const CHUNK_SIZE: usize = 64 * 1024;

pub const ZlibStream = struct {
    zstream: c.z_stream,
    finished: bool,
    // Internal output buffer — caller copies from here.
    out_buf: [*]u8,
    out_cap: usize,

    pub fn feed(self: *ZlibStream, input: [*]const u8, input_len: usize, out_written: *usize) c_int {
        if (self.finished) return @intFromEnum(GcZlibError.stream_finished);

        self.zstream.next_in = @constCast(input);
        self.zstream.avail_in = @intCast(@min(input_len, std.math.maxInt(c_uint)));
        self.zstream.next_out = self.out_buf;
        self.zstream.avail_out = @intCast(@min(self.out_cap, std.math.maxInt(c_uint)));

        const ret = c.inflate(&self.zstream, c.Z_SYNC_FLUSH);

        if (ret == c.Z_STREAM_END) {
            self.finished = true;
        } else if (ret != c.Z_OK and ret != c.Z_BUF_ERROR) {
            return @intFromEnum(GcZlibError.zlib_error);
        }

        const produced = self.out_cap - @as(usize, self.zstream.avail_out);
        out_written.* = produced;
        return @intFromEnum(GcZlibError.ok);
    }
};

/// Create a new streaming decompressor.
/// `out_buf_size` is the size of the internal output buffer (0 = default 64K).
/// Returns null on failure.
export fn gc_zlib_stream_new(out_buf_size: usize) callconv(.C) ?*ZlibStream {
    const alloc = std.heap.c_allocator;
    const cap = if (out_buf_size == 0) CHUNK_SIZE else out_buf_size;

    const s = alloc.create(ZlibStream) catch return null;
    const buf = alloc.alloc(u8, cap) catch {
        alloc.destroy(s);
        return null;
    };

    s.* = .{
        .zstream = std.mem.zeroes(c.z_stream),
        .finished = false,
        .out_buf = buf.ptr,
        .out_cap = cap,
    };

    const ret = c.inflateInit(&s.zstream);
    if (ret != c.Z_OK) {
        alloc.free(buf);
        alloc.destroy(s);
        return null;
    }

    return s;
}

/// Feed compressed data into the stream.
/// Decompressed output is written to the stream's internal buffer.
/// `*out_ptr` is set to point at the output, `*out_len` to the byte count.
/// Returns 0 on success.
export fn gc_zlib_stream_feed(
    stream: ?*ZlibStream,
    input: ?[*]const u8,
    input_len: usize,
    out_ptr: ?*[*]const u8,
    out_len: ?*usize,
) callconv(.C) c_int {
    const s = stream orelse return @intFromEnum(GcZlibError.null_pointer);
    const inp = input orelse return @intFromEnum(GcZlibError.null_pointer);
    const op = out_ptr orelse return @intFromEnum(GcZlibError.null_pointer);
    const ol = out_len orelse return @intFromEnum(GcZlibError.null_pointer);

    var written: usize = 0;
    const rc = s.feed(inp, input_len, &written);
    op.* = s.out_buf;
    ol.* = written;
    return rc;
}

/// Free a streaming decompressor and its internal buffer.
export fn gc_zlib_stream_free(stream: ?*ZlibStream) callconv(.C) void {
    const s = stream orelse return;
    _ = c.inflateEnd(&s.zstream);
    std.heap.c_allocator.free(s.out_buf[0..s.out_cap]);
    std.heap.c_allocator.destroy(s);
}

// -------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------

test "one-shot round-trip via deflate/inflate" {
    // Compress some data with deflate, then decompress with our function.
    const input = "Hello, Minecraft world! This is a test of zlib compression for packet data.";

    // Compress.
    var deflate_stream: c.z_stream = std.mem.zeroes(c.z_stream);
    var compressed: [512]u8 = undefined;
    deflate_stream.next_in = @constCast(input.ptr);
    deflate_stream.avail_in = input.len;
    deflate_stream.next_out = &compressed;
    deflate_stream.avail_out = 512;

    try std.testing.expectEqual(c.Z_OK, c.deflateInit(&deflate_stream, c.Z_DEFAULT_COMPRESSION));
    try std.testing.expectEqual(c.Z_STREAM_END, c.deflate(&deflate_stream, c.Z_FINISH));
    const compressed_len = deflate_stream.total_out;
    _ = c.deflateEnd(&deflate_stream);

    // Decompress with our function.
    var output: [512]u8 = undefined;
    var out_len: usize = 0;
    const rc = gc_zlib_decompress(&compressed, compressed_len, &output, 512, &out_len);
    try std.testing.expectEqual(@as(c_int, 0), rc);
    try std.testing.expectEqualSlices(u8, input, output[0..out_len]);
}

test "streaming decompression" {
    const input = "Streaming test data for Minecraft compressed packets. " ** 10;

    // Compress.
    var deflate_stream: c.z_stream = std.mem.zeroes(c.z_stream);
    var compressed: [4096]u8 = undefined;
    deflate_stream.next_in = @constCast(input.ptr);
    deflate_stream.avail_in = input.len;
    deflate_stream.next_out = &compressed;
    deflate_stream.avail_out = 4096;

    try std.testing.expectEqual(c.Z_OK, c.deflateInit(&deflate_stream, c.Z_DEFAULT_COMPRESSION));
    try std.testing.expectEqual(c.Z_STREAM_END, c.deflate(&deflate_stream, c.Z_FINISH));
    const compressed_len = deflate_stream.total_out;
    _ = c.deflateEnd(&deflate_stream);

    // Decompress via stream.
    const s = gc_zlib_stream_new(0) orelse return error.AllocFailed;
    defer gc_zlib_stream_free(s);

    var out_ptr: [*]const u8 = undefined;
    var out_len: usize = 0;
    const rc = gc_zlib_stream_feed(s, &compressed, compressed_len, &out_ptr, &out_len);
    try std.testing.expectEqual(@as(c_int, 0), rc);
    try std.testing.expectEqualSlices(u8, input, out_ptr[0..out_len]);
}
