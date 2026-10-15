//! cbm_zig.zig — root of libcbm_zig, the static library holding every Zig
//! module linked into the C build. Each module exports the functions its C
//! header declares; importing it here pulls those exports into the library.

comptime {
    _ = @import("cli_zig/cbm_cli_zig.zig");
    _ = @import("git/git_context.zig");
    _ = @import("simhash/minhash.zig");
    _ = @import("watcher/watcher.zig");
}
