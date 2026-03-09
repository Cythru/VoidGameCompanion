// Arena (bump) allocator for per-tick transient state.
//
// Design:
//   - Allocates in large pages (64 KiB by default).
//   - Individual allocations are simple pointer bumps — O(1).
//   - `reset` rewinds to the beginning of the first page, keeping all
//     pages allocated so the next tick avoids syscalls.
//   - `free` releases every page back to the OS.
//
// All exported functions use the C calling convention for FFI.

const std = @import("std");
const Allocator = std.mem.Allocator;

const PAGE_SIZE: usize = 64 * 1024; // 64 KiB

const Page = struct {
    data: [*]u8,
    capacity: usize,
    used: usize,
    next: ?*Page,
};

pub const Arena = struct {
    head: ?*Page,
    current: ?*Page,
    page_alloc: std.mem.Allocator, // backing allocator for pages

    // ---------------------------------------------------------------
    // Internal helpers
    // ---------------------------------------------------------------

    fn newPage(self: *Arena, min_size: usize) ?*Page {
        const cap = @max(PAGE_SIZE, min_size);
        const page_mem = self.page_alloc.create(Page) catch return null;
        const data = self.page_alloc.alignedAlloc(u8, 16, cap) catch {
            self.page_alloc.destroy(page_mem);
            return null;
        };
        page_mem.* = .{
            .data = data.ptr,
            .capacity = cap,
            .used = 0,
            .next = null,
        };
        return page_mem;
    }

    fn freePage(self: *Arena, page: *Page) void {
        self.page_alloc.free(page.data[0..page.capacity]);
        self.page_alloc.destroy(page);
    }

    // ---------------------------------------------------------------
    // Public API (called from C wrappers below)
    // ---------------------------------------------------------------

    pub fn alloc(self: *Arena, size: usize, alignment: u8) ?[*]u8 {
        if (size == 0) return null;

        const align: usize = if (alignment == 0) 8 else @as(usize, alignment);

        // Try current page first.
        if (self.current) |cur| {
            const aligned_off = std.mem.alignForward(usize, cur.used, align);
            if (aligned_off + size <= cur.capacity) {
                cur.used = aligned_off + size;
                return cur.data + aligned_off;
            }
            // Try the page after current (may exist from a previous tick).
            if (cur.next) |nxt| {
                nxt.used = 0;
                const a2 = std.mem.alignForward(usize, 0, align);
                if (a2 + size <= nxt.capacity) {
                    nxt.used = a2 + size;
                    self.current = nxt;
                    return nxt.data + a2;
                }
            }
        }

        // Need a new page.
        const page = self.newPage(size + 16) orelse return null;

        // Link it in.
        if (self.current) |cur| {
            page.next = cur.next;
            cur.next = page;
        } else {
            self.head = page;
        }
        self.current = page;

        const a3 = std.mem.alignForward(usize, 0, align);
        page.used = a3 + size;
        return page.data + a3;
    }

    pub fn reset(self: *Arena) void {
        var it = self.head;
        while (it) |page| {
            page.used = 0;
            it = page.next;
        }
        self.current = self.head;
    }

    pub fn deinit(self: *Arena) void {
        var it = self.head;
        while (it) |page| {
            const nxt = page.next;
            self.freePage(page);
            it = nxt;
        }
        self.head = null;
        self.current = null;
    }
};

// -------------------------------------------------------------------
// C-callable exports
// -------------------------------------------------------------------

/// Create a new arena. Returns null on allocation failure.
export fn gc_arena_new() callconv(.C) ?*Arena {
    const backing = std.heap.c_allocator;
    const arena = backing.create(Arena) catch return null;
    arena.* = .{
        .head = null,
        .current = null,
        .page_alloc = backing,
    };
    return arena;
}

/// Bump-allocate `size` bytes with the given alignment (0 = default 8).
/// Returns null on failure.
export fn gc_arena_alloc(arena: ?*Arena, size: usize, alignment: u8) callconv(.C) ?[*]u8 {
    const a = arena orelse return null;
    return a.alloc(size, alignment);
}

/// Reset the arena — keeps pages allocated, rewinds all cursors.
export fn gc_arena_reset(arena: ?*Arena) callconv(.C) void {
    const a = arena orelse return;
    a.reset();
}

/// Free the arena and all its pages.
export fn gc_arena_free(arena: ?*Arena) callconv(.C) void {
    const a = arena orelse return;
    a.deinit();
    std.heap.c_allocator.destroy(a);
}

// -------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------

test "arena basic alloc and reset" {
    const a = gc_arena_new() orelse return error.OutOfMemory;
    defer gc_arena_free(a);

    const p1 = gc_arena_alloc(a, 128, 0) orelse return error.OutOfMemory;
    p1[0] = 0xAB;

    const p2 = gc_arena_alloc(a, 256, 16) orelse return error.OutOfMemory;
    // Ensure alignment.
    try std.testing.expect(@intFromPtr(p2) % 16 == 0);

    gc_arena_reset(a);

    // After reset we can allocate again — should reuse the same page.
    const p3 = gc_arena_alloc(a, 64, 0) orelse return error.OutOfMemory;
    _ = p3;
}

test "arena large allocation" {
    const a = gc_arena_new() orelse return error.OutOfMemory;
    defer gc_arena_free(a);

    // Larger than PAGE_SIZE — should still work.
    const big = gc_arena_alloc(a, 128 * 1024, 0) orelse return error.OutOfMemory;
    big[0] = 0xFF;
    big[128 * 1024 - 1] = 0x01;
}
