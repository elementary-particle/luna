# Filesystem and asset API

The Lua API separates bytes, resource decoding, and game state. There are no
host paths, save-slot conventions, or implicit JSON serialization in the FS API.

See the [Lua runtime overview](lua-api.md) for task usage and a save-file example,
and [game archives](game-archive.md) for distribution and the package format.

## Filesystems

- `luna.fs.game`: read-only mounted content.
- `luna.fs.user`: persistent game-specific data.
- `luna.fs.cache`: disposable game-specific data. Do not put saves here.

All paths are relative to their handle. Absolute paths, parent traversal,
embedded NULs, and symlinks/reparse points below native roots are rejected.
`list("")` and `stat("")` address the root. Listing is non-recursive and returns
root-relative paths in deterministic order.

| Operation | Successful future result |
| --- | --- |
| `fs:read(path)` | Immutable `Blob` |
| `fs:open(path)` | Seekable `ReadStream` |
| `fs:stat(path)` | `{path, kind="file" or "directory", size}` |
| `fs:list(path)` | Array of the same metadata records |
| `fs:write(path, string_or_blob)` | `true` |
| `fs:remove(path)` | `true` |

`write` creates parent directories and atomically replaces the destination
using a same-directory temporary. On failure before replacement, the previous
file remains intact. Live readers retain the old file. Atomicity is not a
power-loss durability guarantee: there is no fsync/durable option yet.
Concurrent writes have no submission-order guarantee; await one before starting
another when order matters. Removal accepts regular files only; missing files
return `not_found`. Neither operation is allowed on `game`.

`fs:ref(path)` is synchronous and performs no I/O. It returns an immutable
filesystem/path reference, not a file snapshot; each load resolves the current
file. Invalid reference paths are argument errors. Referenced files need not
exist yet.

## Futures, blobs, and streams

`luna.await(future)` waits inside a Luna task and returns the result. Outside a
task, use `future:poll()` to check readiness and `future:result()` when settled.
The successful result objects are retained by the future and can be read more
than once, including by multiple tasks. Discard the future to release its
additional ownership. Discarding a pending future does not cancel its job.

- `future:poll()`: `false` while pending; otherwise `true, succeeded`.
- `future:result()`: successful values, or `nil, Error`; throws while pending.
- `future:event()`: completion event for custom schedulers. Check readiness
  before waiting: events are edge-triggered, not retained notifications.
- `blob:size()`: byte count.
- `blob:string()`: a Lua string copy, preserving embedded NULs.
- `stream:read(count)`: future returning a blob of up to count bytes.
- `stream:seek("set"|"cur"|"end", offset)`: future returning the new position.
- `stream:close()`: future returning true; idempotent.

An empty read at end-of-file is successful. Individual stream reads are limited
to 64 MiB. Wait for an outstanding stream operation to settle before starting the
next operation; concurrent operations on one cursor are argument errors. The
stream closes automatically when its last owner is released.

Filesystem access and decoding run on workers; result publication and renderer
resource finalization run on the Lua thread. Lua module loading remains
synchronous because `require` is synchronous.

## Errors and JSON

Operational failures return `nil, {code, operation, path, message, native_code}`.
Use `code` for control flow and `message` for diagnostics. Codes include
`not_found`, `invalid_path`, `not_a_file`, `is_directory`, `not_a_directory`,
`permission_denied`, `io_error`, `format_error`, `unsupported`, and
`budget_exceeded`. Wrong argument types, invalid options, and premature
`result()` calls throw. Worker exceptions become `io_error` results.

`luna.json.encode(value)` returns a string; `luna.json.decode(string_or_blob)`
returns a Lua value. Both are synchronous, storage-independent and return
`nil, Error` on serialization/parse failure.

## Assets

```lua
local image, width, height =
  luna.await(luna.assets:image(luna.fs.game:ref("images/title.png")))
local registered, err =
  luna.await(luna.assets:font(luna.fs.game:ref("fonts/dialogue.ttf")))
local music, audio_error =
  luna.await(luna.assets:audio(luna.fs.game:ref("music/title.ogg"),
    { mode = "stream" }))
```

Each loader accepts a file reference or a blob; strings are deliberately not
ambiguous file-or-data inputs. Image results remain compatible with canvas
paints. Font loading registers the font with the renderer and returns true;
canvas font styles still select family/weight, rather than an opaque file handle.
Audio results remain compatible with `luna.audio.track_set`.

Audio modes:

- `memory` (default): retain compressed audio in memory; decode during playback.
- `decode`: retain decoded PCM.
- `stream`: retain an opened source and read on demand, without a whole-file
  buffer. Tracks use independent cursors on the same pinned source. Replacing
  its path after loading does not change the loaded audio.

The SDL mixer performs track decoder setup when a stream is attached; track
attachment itself is still synchronous. Format validation happens in the load
job. Decoder-owned data, open sources and mappings survive releasing the
future. Tracks retain their audio while attached.

The game VFS has a 64 MiB raw-cache budget. Native mappings do not allocate a
second full-file byte buffer. User/cache reads bypass the raw cache. Decoded
resource caching remains caller-owned; there is no global decoded-cache or
in-flight load deduplication.

## Startup and modules

An optional `game.json` in `--root` configures the game:

```json
{
  "id": "my-game",
  "entry": "main.lua",
  "mounts": [
    {"type": "directory", "path": "content", "prefix": "/", "priority": 0}
  ]
}
```

The loose root is mounted at priority -100. Additional mount paths are relative
to that root. The longest mount prefix wins; within that prefix, higher
priorities win and equal-priority mounts retain declaration order.
Directory listing uses the first resolving source (not an overlay union).
`id` is required when a manifest exists
and permits ASCII letters, digits, dots, underscores and hyphens (not "." or
".."). It is independent of installation location. `--game-id` overrides it.
Without either, development runs share `luna-development`; ship a manifest to
avoid that shared identity. SDL selects the per-user preference directory;
`user` and `cache` are separate subdirectories there. Cache eviction/cleanup is
not automatic.

`--script` overrides the manifest entry. The engine does not change the process
working directory. Its `require` searcher preserves preload modules (including
`luna`) and searches the game VFS for `module/name.lua` and
`module/name/init.lua`. Host Lua/C searchers are removed. This is portability,
not a hostile-script sandbox: the ordinary Lua libraries are still available.

Directory and package mounts are implemented. `--root` also accepts a single
`.luna` archive containing the manifest and game content. See
[the archive specification and packaging commands](game-archive.md). Durable
saves, centralized decoded caching, and game-specific checkpoint serialization
remain separate work.
