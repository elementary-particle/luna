# Luna

Luna is a C++20-based Lua visual novel engine built around a simple idea: keep the scripting surface small, predictable, and easy to learn, while still giving developers enough control to assemble rich cross-platform narrative experiences.

The engine uses Lua for game logic, SDL3 for windowing and rendering, SDL3_image for image loading, SDL3_ttf for text rendering, SDL3_mixer for audio playback, and LuaJIT as the embedded runtime.

## Design goals

- **Simple by default**: the core Lua API is intentionally compact and direct.
- **Flexible in practice**: scripts can coordinate rendering, events, timers, async resource loading, and save data without fighting the engine.
- **Portable foundation**: Luna is designed on widely supported libraries so the same project structure can travel across platforms more easily.
- **Coroutine-driven flow**: visual novel logic maps cleanly to Lua coroutines, timers, and explicit waits.

## Current feature set

- Window creation and SDL-driven main loop
- Coroutine task scheduler for Lua scripts
- Frame waits, custom events, and timer events
- Immediate-mode `luna.gfx` canvas with transforms, clips, layers, and text
- Asynchronous texture and font loading through promises
- Audio track creation, playback, stopping, and gain control
- Save-data helpers with sandboxed JSON read/write support
- Sandboxed virtual file access for save files

## Project layout

- `src/main.cpp` — application entry point; runs `main.lua` by default or a user-supplied Lua file
- `src/engine.cpp` — engine lifecycle, task scheduler, Lua bindings, audio integration
- `src/renderer.cpp` — texture/font loading plus `luna.gfx` canvas, draw lists, transforms, clips, and layers
- `src/vfs.cpp` — save-system sandbox and JSON serialization helpers
- `docs/example/` — example assets and a sample script

## Building

Luna uses CMake and requires a C++20 compiler.

### Dependencies

- `fmt`
- `SDL3`

---
### Input and presentation

#### `luna.poll_events()`

Returns a Lua array of SDL-derived events collected since the previous poll. Each event table contains:

- `type`
- `x`
- `y`
- `button` for mouse button events when available

Currently exposed event types are:

- `quit`
- `mouse_move`
- `mouse_down`
- `mouse_up`

### Rendering

Rendering happens through the immediate-mode `luna.gfx` canvas userdata. The canvas records draw commands until `gfx:present()` and supports style tables, transform stack operations, clip stack operations, and scoped layers.

#### `gfx:set_canvas_size(w, h)`

Sets the logical canvas size used for rendering.

#### `gfx:clear(abgr)`

Sets the clear color for the next `gfx:present()`.

#### `gfx:present()`

Flushes the recorded canvas commands and presents the frame.

#### `gfx:rect(x, y, w, h[, style])`

Queues a filled rectangle.

#### `gfx:rect_outline(x, y, w, h[, style])`

Queues an outlined rectangle.

#### `gfx:image(texture, x, y, w, h[, style])`

Queues an image draw using a previously loaded texture.

#### `gfx:text(font, text, x, y[, style])`

Queues a text draw using a loaded font. Text anchors at `(x, y)` and can be aligned with `style.align_x` / `style.align_y`.

#### `font:measure(text[, wrap_w])`

Returns the rendered text width and height in pixels, using the same wrapping behavior as `gfx:text`.

#### `gfx:style(style_table)`

Compiles a reusable style userdata for efficient repeated draw calls.

#### `gfx:push_transform(transform)` / `gfx:pop_transform()`

Pushes or pops the transform stack. `transform` currently supports `x`, `y`, `rotation`, `origin_x`, `origin_y`, `scale_x`, and `scale_y`.

#### `gfx:translate(x, y)`, `gfx:scale(x[, y])`, `gfx:rotate(radians)`

Mutate the current top-of-stack transform.

#### `gfx:push_clip(x, y, w, h)` / `gfx:pop_clip()`

Pushes or pops the clip stack. Clips are intersected with the parent clip.

#### `gfx:begin_layer([style])` / `gfx:end_layer()`

Captures nested draw commands into an offscreen layer, then composites the layer back into its parent using the provided style.

#### Style tables

Style tables may include:

- `color`
- `opacity`
- `blend` as a preset string (`"alpha"`, `"add"`, `"multiply"`, `"screen"`, `"none"`) or a custom blend-state table
- `rotation`, `origin_x`, `origin_y`, `scale_x`, `scale_y`
- `align_x` as `0..1` or `"left"`, `"center"`, `"right"`
- `align_y` as `0..1` or `"top"`, `"middle"`, `"bottom"`
- `clip = { x, y, w, h }`
- `wrap_w` for text

Custom blend-state tables may include:

- `enabled`
- `color_op`, `alpha_op`
- `src_color`, `dst_color`, `src_alpha`, `dst_alpha`

### Text and assets

#### `luna.load_texture(path)`


---
assert(state, err)
```

#### `luna.vfs`

Sandboxed raw file access in the same save area.

- `luna.vfs.open(path[, mode])`

Returned file objects provide:

- `file:read_all()`
- `file:write(data)`
- `file:close()`

## Example script

A working sample script is provided at `docs/example/main.lua`. It demonstrates:

- asynchronous texture and font loading
- coroutine startup
