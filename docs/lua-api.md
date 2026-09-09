# Lua runtime and tasks

The `luna` module currently exposes helpers in a few main areas:

- Engine: `start`, `next_frame`, `await`, `make_event`, `wait`, `now`, `after`, `set_frame_time`, `set_window_size`
- Rendering: `make_canvas`, `window`
- Input: `input.poll_events`, keyboard state/text input helpers, mouse state/capture helpers, touch device/finger queries, and SDL3 gamepad state/rumble/LED/sensor helpers
- Assets: `assets:image`, `assets:font`, `assets:audio`; futures with `await`, `poll`, `result`, `event`
- Audio: `audio.track_create`, `audio.track_destroy`, `audio.track_set`, `audio.track_play`, `audio.track_stop`, `audio.track_playing`, `audio.track_stop_event`, `audio.track_set_gain`, `audio.set_mixer_gain`
- Files: `fs.game`, `fs.user`, `fs.cache`; independent `json.encode` / `json.decode`
- Profiling: `tracy.begin_zone`, `tracy.end_zone` when Tracy is compiled in

Poll events with `luna.input.poll_events()`. The top-level `luna.poll_events`
API has been removed. Mouse events use `mouse_motion`, `mouse_button_down`,
and `mouse_button_up`; coordinates and button names remain available.

## File VFS and save data

The C++ VFS API uses file-oriented names: `luna::file::Vfs`,
`FileStatus`, `FileInfo`, `MappedFile`, `FileStream`, and `FileSource`.
Fallible operations return `FileStatus`; successful outputs are written to
caller-provided output arguments.

Game content is still described as assets in author-facing text, but the
runtime file layer is not asset-specific. Image, font, audio, Lua entry, and
package-backed reads all use the same `MappedFile` or `FileStream` primitives.

Lua storage uses rooted filesystem handles with asynchronous reads, seekable
streams, metadata, atomic writes, and non-recursive removal. JSON is independent
of storage. Assets accept file references or immutable blobs. Module loading
uses the game VFS, not host `package.path`.

See [Filesystem and asset API](fs-assets.md) for the complete contract,
manifest format, examples, and current limits. See [Canvas](canvas.md) for
rendering methods and backend differences.

## Save Data and Files

Save paths are relative to `luna.fs.user`, isolated by the `id` in `game.json`
(or `--game-id`). Writes atomically replace a whole file; JSON encoding is a
separate operation. Absolute paths, traversal, symlinks and Windows reparse
points below the root are rejected. The supplied root itself is trusted.

```lua
local luna = require("luna")
luna.start(function()
  local encoded = assert(luna.json.encode({ chapter = 3 }))
  local ok, err = luna.await(luna.fs.user:write("slots/1.json", encoded))
  assert(ok, err and err.message)
  local blob, read_error = luna.await(luna.fs.user:read("slots/1.json"))
  assert(blob, read_error and read_error.message)
  local state = assert(luna.json.decode(blob))
  assert(state.chapter == 3)
end)
```

Future results are reusable: multiple consumers receive the same successful
objects. Operational failures return `nil, Error`; invalid API usage throws.
Waiting on the same event more than once resumes the coroutine once when that
event is signaled. An uncaught coroutine error stops the runtime and produces a
nonzero process exit status.

## Runtime notes

- Graphics initialization is deferred until the first frame, so Vulkan-related failures may appear when the main loop begins rather than at process startup.
- The engine uses a small worker pool to complete background asset-loading jobs.
