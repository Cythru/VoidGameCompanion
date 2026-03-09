const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // --- Static library: libgc_zig.a ---
    const lib = b.addStaticLibrary(.{
        .name = "gc_zig",
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    // Link libc for C interop and zlib for decompression.
    lib.linkLibC();
    lib.linkSystemLibrary("z");

    b.installArtifact(lib);

    // --- Unit tests ---
    const test_step = b.step("test", "Run unit tests");

    const test_modules = [_][]const u8{
        "src/packet_codec.zig",
        "src/chunk_decoder.zig",
        "src/nbt_parser.zig",
        "src/zlib_stream.zig",
        "src/allocator.zig",
    };

    for (test_modules) |src| {
        const t = b.addTest(.{
            .root_source_file = b.path(src),
            .target = target,
            .optimize = optimize,
        });
        t.linkLibC();
        t.linkSystemLibrary("z");
        const run = b.addRunArtifact(t);
        test_step.dependOn(&run.step);
    }
}
