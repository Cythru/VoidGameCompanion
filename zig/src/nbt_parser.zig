// Zero-copy NBT (Named Binary Tag) parser.
//
// The Minecraft NBT format stores structured data as a tree of typed tags.
// This parser operates on a flat buffer and produces a tree of NbtNode
// structs allocated via the C allocator. "Zero-copy" means string payloads
// point directly into the input buffer rather than duplicating the bytes.
//
// Supported tag types (all 13):
//   0  TAG_End
//   1  TAG_Byte
//   2  TAG_Short
//   3  TAG_Int
//   4  TAG_Long
//   5  TAG_Float
//   6  TAG_Double
//   7  TAG_Byte_Array
//   8  TAG_String
//   9  TAG_List
//  10  TAG_Compound
//  11  TAG_Int_Array
//  12  TAG_Long_Array
//
// All exported symbols use the C calling convention.

const std = @import("std");

// -------------------------------------------------------------------
// Error codes
// -------------------------------------------------------------------

pub const GcNbtError = enum(c_int) {
    ok = 0,
    null_pointer = -1,
    unexpected_end = -2,
    invalid_tag = -3,
    alloc_failed = -4,
    depth_exceeded = -5,
};

const MAX_DEPTH = 512;

// -------------------------------------------------------------------
// Public types exposed to C
// -------------------------------------------------------------------

pub const NbtTagType = enum(u8) {
    end = 0,
    byte = 1,
    short = 2,
    int = 3,
    long = 4,
    float = 5,
    double = 6,
    byte_array = 7,
    string = 8,
    list = 9,
    compound = 10,
    int_array = 11,
    long_array = 12,
};

pub const NbtNode = extern struct {
    tag_type: u8, // NbtTagType
    // Name — zero-copy pointer into source buffer. null for list elements.
    name_ptr: ?[*]const u8,
    name_len: u16,

    // Payload — interpretation depends on tag_type.
    payload: extern union {
        byte_val: i8,
        short_val: i16,
        int_val: i32,
        long_val: i64,
        float_val: f32,
        double_val: f64,
        // For string, byte_array, int_array, long_array:
        array: extern struct {
            ptr: ?[*]const u8, // zero-copy pointer into source
            len: u32, // element count (bytes for byte_array/string, ints for int_array, etc.)
        },
        // For compound and list:
        children: extern struct {
            items: ?[*]*NbtNode,
            count: u32,
            list_tag_type: u8, // only meaningful for TAG_List
            _pad: [3]u8,
        },
    },
};

// -------------------------------------------------------------------
// Internal parser state
// -------------------------------------------------------------------

const Parser = struct {
    data: [*]const u8,
    len: usize,
    pos: usize,
    alloc: std.mem.Allocator,

    fn remaining(self: *Parser) usize {
        return if (self.pos < self.len) self.len - self.pos else 0;
    }

    fn readByte(self: *Parser) ?u8 {
        if (self.pos >= self.len) return null;
        const b = self.data[self.pos];
        self.pos += 1;
        return b;
    }

    fn readBytes(self: *Parser, n: usize) ?[*]const u8 {
        if (self.remaining() < n) return null;
        const ptr = self.data + self.pos;
        self.pos += n;
        return ptr;
    }

    fn readU16BE(self: *Parser) ?u16 {
        const b = self.readBytes(2) orelse return null;
        return @as(u16, b[0]) << 8 | @as(u16, b[1]);
    }

    fn readI16BE(self: *Parser) ?i16 {
        return @bitCast(self.readU16BE() orelse return null);
    }

    fn readI32BE(self: *Parser) ?i32 {
        const b = self.readBytes(4) orelse return null;
        return @bitCast(@as(u32, b[0]) << 24 | @as(u32, b[1]) << 16 | @as(u32, b[2]) << 8 | @as(u32, b[3]));
    }

    fn readI64BE(self: *Parser) ?i64 {
        const b = self.readBytes(8) orelse return null;
        var v: u64 = 0;
        inline for (0..8) |i| {
            v |= @as(u64, b[i]) << @intCast((7 - i) * 8);
        }
        return @bitCast(v);
    }

    fn readF32BE(self: *Parser) ?f32 {
        const bits = self.readI32BE() orelse return null;
        return @bitCast(@as(u32, @bitCast(bits)));
    }

    fn readF64BE(self: *Parser) ?f64 {
        const bits = self.readI64BE() orelse return null;
        return @bitCast(@as(u64, @bitCast(bits)));
    }

    fn readName(self: *Parser) ?struct { ptr: ?[*]const u8, len: u16 } {
        const name_len = self.readU16BE() orelse return null;
        if (name_len == 0) return .{ .ptr = null, .len = 0 };
        const ptr = self.readBytes(name_len) orelse return null;
        return .{ .ptr = ptr, .len = name_len };
    }

    // ---------------------------------------------------------------
    // Recursive tag parser with comptime dispatch
    // ---------------------------------------------------------------

    fn parsePayload(self: *Parser, tag_type: u8, depth: u32) ?*NbtNode {
        if (depth > MAX_DEPTH) return null;

        const node = self.alloc.create(NbtNode) catch return null;
        node.tag_type = tag_type;
        node.name_ptr = null;
        node.name_len = 0;
        // Zero-initialise payload.
        node.payload = std.mem.zeroes(@TypeOf(node.payload));

        switch (tag_type) {
            @intFromEnum(NbtTagType.byte) => {
                node.payload.byte_val = @bitCast(self.readByte() orelse return null);
            },
            @intFromEnum(NbtTagType.short) => {
                node.payload.short_val = self.readI16BE() orelse return null;
            },
            @intFromEnum(NbtTagType.int) => {
                node.payload.int_val = self.readI32BE() orelse return null;
            },
            @intFromEnum(NbtTagType.long) => {
                node.payload.long_val = self.readI64BE() orelse return null;
            },
            @intFromEnum(NbtTagType.float) => {
                node.payload.float_val = self.readF32BE() orelse return null;
            },
            @intFromEnum(NbtTagType.double) => {
                node.payload.double_val = self.readF64BE() orelse return null;
            },
            @intFromEnum(NbtTagType.byte_array) => {
                const count = self.readI32BE() orelse return null;
                if (count < 0) return null;
                const uc: u32 = @intCast(count);
                const ptr = self.readBytes(uc) orelse return null;
                node.payload.array.ptr = ptr;
                node.payload.array.len = uc;
            },
            @intFromEnum(NbtTagType.string) => {
                const slen = self.readU16BE() orelse return null;
                const ptr = if (slen > 0) (self.readBytes(slen) orelse return null) else null;
                node.payload.array.ptr = ptr;
                node.payload.array.len = slen;
            },
            @intFromEnum(NbtTagType.list) => {
                const elem_type = self.readByte() orelse return null;
                const count = self.readI32BE() orelse return null;
                if (count < 0) return null;
                const uc: u32 = @intCast(count);

                node.payload.children.list_tag_type = elem_type;
                node.payload.children.count = uc;
                node.payload.children._pad = .{ 0, 0, 0 };

                if (uc == 0) {
                    node.payload.children.items = null;
                } else {
                    const items = self.alloc.alloc(*NbtNode, uc) catch return null;
                    for (0..uc) |i| {
                        items[i] = self.parsePayload(elem_type, depth + 1) orelse return null;
                    }
                    node.payload.children.items = items.ptr;
                }
            },
            @intFromEnum(NbtTagType.compound) => {
                // Compounds have a dynamic number of children terminated by TAG_End.
                var list = std.ArrayList(*NbtNode).init(self.alloc);
                defer list.deinit();

                while (true) {
                    const child_type = self.readByte() orelse return null;
                    if (child_type == @intFromEnum(NbtTagType.end)) break;

                    const name = self.readName() orelse return null;
                    const child = self.parsePayload(child_type, depth + 1) orelse return null;
                    child.name_ptr = name.ptr;
                    child.name_len = name.len;
                    list.append(child) catch return null;
                }

                node.payload.children.count = @intCast(list.items.len);
                node.payload.children.list_tag_type = 0;
                node.payload.children._pad = .{ 0, 0, 0 };
                if (list.items.len == 0) {
                    node.payload.children.items = null;
                } else {
                    // Copy the slice into a stable allocation (ArrayList may realloc).
                    const items = self.alloc.alloc(*NbtNode, list.items.len) catch return null;
                    @memcpy(items, list.items);
                    node.payload.children.items = items.ptr;
                }
            },
            @intFromEnum(NbtTagType.int_array) => {
                const count = self.readI32BE() orelse return null;
                if (count < 0) return null;
                const uc: u32 = @intCast(count);
                const byte_count = @as(usize, uc) * 4;
                const ptr = self.readBytes(byte_count) orelse return null;
                node.payload.array.ptr = ptr;
                node.payload.array.len = uc;
            },
            @intFromEnum(NbtTagType.long_array) => {
                const count = self.readI32BE() orelse return null;
                if (count < 0) return null;
                const uc: u32 = @intCast(count);
                const byte_count = @as(usize, uc) * 8;
                const ptr = self.readBytes(byte_count) orelse return null;
                node.payload.array.ptr = ptr;
                node.payload.array.len = uc;
            },
            else => return null,
        }

        return node;
    }
};

// -------------------------------------------------------------------
// Recursive free
// -------------------------------------------------------------------

fn freeNode(alloc: std.mem.Allocator, node: *NbtNode) void {
    switch (node.tag_type) {
        @intFromEnum(NbtTagType.list), @intFromEnum(NbtTagType.compound) => {
            const count = node.payload.children.count;
            if (node.payload.children.items) |items| {
                for (0..count) |i| {
                    freeNode(alloc, items[i]);
                }
                alloc.free(items[0..count]);
            }
        },
        else => {},
    }
    alloc.destroy(node);
}

// -------------------------------------------------------------------
// C exports
// -------------------------------------------------------------------

/// Parse an NBT blob from `data[0..len]`.
/// On success, sets `*out_root` to the root NbtNode and `*out_consumed` to
/// the number of bytes consumed.  Returns 0 on success.
///
/// The caller MUST call `gc_nbt_free` on the returned root when done.
export fn gc_nbt_parse(
    data: ?[*]const u8,
    len: usize,
    out_root: ?*?*NbtNode,
    out_consumed: ?*usize,
) callconv(.C) c_int {
    const d = data orelse return @intFromEnum(GcNbtError.null_pointer);
    const or_ = out_root orelse return @intFromEnum(GcNbtError.null_pointer);
    const oc = out_consumed orelse return @intFromEnum(GcNbtError.null_pointer);

    var parser = Parser{
        .data = d,
        .len = len,
        .pos = 0,
        .alloc = std.heap.c_allocator,
    };

    // Root tag: type byte + name.
    const root_type = parser.readByte() orelse return @intFromEnum(GcNbtError.unexpected_end);
    if (root_type == @intFromEnum(NbtTagType.end)) {
        or_.* = null;
        oc.* = parser.pos;
        return @intFromEnum(GcNbtError.ok);
    }

    const name = parser.readName() orelse return @intFromEnum(GcNbtError.unexpected_end);
    const root = parser.parsePayload(root_type, 0) orelse return @intFromEnum(GcNbtError.alloc_failed);
    root.name_ptr = name.ptr;
    root.name_len = name.len;

    or_.* = root;
    oc.* = parser.pos;
    return @intFromEnum(GcNbtError.ok);
}

/// Free an NbtNode tree previously returned by `gc_nbt_parse`.
export fn gc_nbt_free(root: ?*NbtNode) callconv(.C) void {
    const r = root orelse return;
    freeNode(std.heap.c_allocator, r);
}

// -------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------

test "parse simple compound" {
    // Hand-craft a minimal NBT: TAG_Compound("") { TAG_Int("val") = 42 } TAG_End
    var buf: [256]u8 = undefined;
    var pos: usize = 0;

    // Root: TAG_Compound, name ""
    buf[pos] = 10;
    pos += 1; // type
    buf[pos] = 0;
    pos += 1;
    buf[pos] = 0;
    pos += 1; // name length = 0

    // Child: TAG_Int, name "val"
    buf[pos] = 3;
    pos += 1; // type
    buf[pos] = 0;
    pos += 1;
    buf[pos] = 3;
    pos += 1; // name length = 3
    buf[pos] = 'v';
    pos += 1;
    buf[pos] = 'a';
    pos += 1;
    buf[pos] = 'l';
    pos += 1;
    // Value: 42 big-endian
    buf[pos] = 0;
    pos += 1;
    buf[pos] = 0;
    pos += 1;
    buf[pos] = 0;
    pos += 1;
    buf[pos] = 42;
    pos += 1;

    // TAG_End
    buf[pos] = 0;
    pos += 1;

    var root: ?*NbtNode = null;
    var consumed: usize = 0;
    const rc = gc_nbt_parse(&buf, pos, &root, &consumed);
    try std.testing.expectEqual(@as(c_int, 0), rc);
    try std.testing.expectEqual(pos, consumed);
    try std.testing.expect(root != null);

    const r = root.?;
    defer gc_nbt_free(r);

    try std.testing.expectEqual(@as(u8, 10), r.tag_type); // compound
    try std.testing.expectEqual(@as(u32, 1), r.payload.children.count);

    const child = r.payload.children.items.?[0];
    try std.testing.expectEqual(@as(u8, 3), child.tag_type); // int
    try std.testing.expectEqual(@as(i32, 42), child.payload.int_val);
}
