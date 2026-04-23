# Luna

Luna is a small native runtime for Lua-driven 2D applications. It combines a coroutine-based Lua host with a build-time-selectable rendering backend, SDL3 windowing/input, and SDL3_mixer audio playback.

The executable loads a Lua entry file at startup, defaulting to `main.lua`, and exposes engine, rendering, asset-loading, audio, and save-file helpers through `require("luna")`.

## What It Includes

- LuaJIT embedded directly in the project
- SDL3 windowing, input, and event pumping
- GPU-accelerated Skia Graphite backend
- Simple CPU based Blend2D backend
- Canvas-style drawing APIs for rectangles, images, text, and paragraphs
- Asynchronous asset loading for images, font faces, and audio
- A small event/promise model for coordinating background work from Lua
- Sandboxed save-data helpers with JSON read/write support

## Project Layout

- `src/`: engine, renderer, canvas, audio mixer, logging, and VFS implementation
- `vendor/`: vendored LuaJIT build integration and renderer submodules
- `vcpkg-ports/`: overlay ports for Skia and SDL3-related packages
- `vcpkg-triplets/`: custom triplets used by the provided CMake presets
- `docs/example/main.lua`: a minimal Lua script showing the runtime shape

## Requirements

- CMake 3.20 or newer
- Ninja
- A working `vcpkg` installation exposed through `VCPKG_ROOT`
- A C++20-capable toolchain
- A Vulkan-capable system and graphics driver when the Skia backend is enabled

The repository is set up around `vcpkg` manifests and CMake presets. Dependencies such as `fmt`, `SDL3`, `SDL3_mixer`, `Skia`, and Vulkan Memory Allocator are resolved through `vcpkg`. LuaJIT is vendored as `vendor/luajit` and must be initialized for every build. The Blend2D backend additionally expects `vendor/asmjit` and `vendor/blend2d` to be initialized as git submodules.

## Building

### Linux

The repo includes a preset for the custom `x64-linux-clang-lto` triplet:

```bash
git submodule update --init --recursive
cmake --preset vcpkg-x64-linux
cmake --build build
```

### Windows

The repo also includes a preset for the custom `x64-windows-clangcl-lto` triplet:

```powershell
git submodule update --init --recursive
cmake --preset vcpkg-x64-windows
cmake --build build
```

If `VCPKG_ROOT` is not set, CMake will not be able to find the toolchain file used by the presets.

Backend selection is controlled at configure time:

```bash
cmake --preset vcpkg-x64-linux -DLUNA_BACKEND_SKIA=ON -DLUNA_BACKEND_BLEND2D=ON
cmake --build build
```

If you want Blend2D in the build, initialize the renderer submodules first:

```bash
git submodule update --init --recursive
cmake --preset vcpkg-x64-linux -DLUNA_BACKEND_BLEND2D=ON
cmake --build build
```

To build Skia only and skip the Blend2D submodules:

```bash
git submodule update --init vendor/luajit
cmake --preset vcpkg-x64-linux -DLUNA_BACKEND_BLEND2D=OFF
cmake --build build
```

To build Blend2D only and skip Vulkan at runtime:

```bash
git submodule update --init --recursive
cmake --preset vcpkg-x64-linux -DLUNA_BACKEND_SKIA=OFF -DLUNA_BACKEND_BLEND2D=ON
cmake --build build
```

## Running

By default, Luna looks for `main.lua` in the working directory:

```bash
./build/luna
```

You can also provide an explicit Lua entry path:

```bash
./build/luna path/to/main.lua
```

If the build includes more than one renderer backend, you can select one at
startup with `LUNA_BACKEND`:

```bash
LUNA_BACKEND=skia ./build/luna
LUNA_BACKEND=blend2d ./build/luna path/to/main.lua
```

Accepted values are `skia` and `blend2d` (`blend2d_cpu` is also accepted as an
alias). If `LUNA_BACKEND` is unset, Luna keeps its default backend selection.
If you request a backend that was not compiled into the current build, startup
fails with a clear error.

Blend2D also accepts an optional worker-count parameter through
`LUNA_BLEND2D_THREAD_COUNT`:

```bash
LUNA_BACKEND=blend2d LUNA_BLEND2D_THREAD_COUNT=4 ./build/luna
```

Set `LUNA_BLEND2D_THREAD_COUNT=0` to keep Blend2D in synchronous mode.

On Windows, run the generated `luna.exe` from the build output directory.

## Minimal Lua Example

```lua
local luna = require("luna")

local function main()
  while true do
    local events = luna.poll_events()
    for _, event in ipairs(events) do
      if event.type == "quit" then
        return
      end
    end

    luna.window:clear(0xFF1D2026)
    luna.window:draw_rect(100, 100, 320, 180, {
      shader = 0xFFFFCC66,
      style = "fill",
    })

    luna.next_frame()
  end
end

luna.start(main)
```

## Lua Runtime Surface

The `luna` module currently exposes helpers in a few main areas:

- Engine: `start`, `next_frame`, `make_event`, `wait`, `now`, `after`, `set_frame_time`, `set_window_size`
- Rendering: `poll_events`, `make_canvas`, `window`
- Async asset loading: `load_image`, `load_fontface`, `load_audio`
- Audio: `audio.track_create`, `audio.track_destroy`, `audio.track_set`, `audio.track_play`, `audio.track_stop`, `audio.track_playing`, `audio.track_stop_event`, `audio.track_set_gain`, `audio.set_mixer_gain`
- Filesystem/save data: `vfs.open`, `save.write_json`, `save.read_json`, `save.root`
- Profiling: `tracy.begin_zone`, `tracy.end_zone` when Tracy is compiled in

The canvas API supports operations such as:

- clearing, transforms, save/restore
- rectangle drawing and clipping
- geometry hit testing
- path construction from commands
- path drawing and clipping
- image drawing and snapshots
- text measurement and drawing
- paragraph creation, layout, measurement, and drawing

### Canvas Interface

`luna.window` is the window-backed canvas, and `luna.make_canvas(width, height)` creates an offscreen canvas with the same drawing API.

Core canvas methods:

- `canvas:clear(color)`
- `canvas:save()` / `canvas:restore()`
- `canvas:save_layer(paint)` saves into an isolated layer, optionally with a compositing paint
- `canvas:translate(dx, dy)`
- `canvas:scale(sx, sy)`
- `canvas:skew(sx, sy)`
- `canvas:rotate(radians)`
- `canvas:snapshot()` returns an image for offscreen canvases

Paint and font compilation:

- `canvas:paint(opts)` accepts `{ shader, alpha, blend_mode, style }`
- `paint.shader` accepts a packed color, a compiled shader, or a shader table
- solid color moved into shaders, so `canvas:paint({ shader = 0xFF7CC6FF, style = "fill" })` replaces the old `color = ...` paint form
- `paint.style` may be `"fill"` or `{ type = "stroke", width = number }`
- `paint.blend_mode` may be values such as `"src_over"`, `"multiply"`, `"screen"`, `"overlay"`, or `"difference"`
- `canvas:font(opts)` accepts `{ size, family, style, weight, width, slant }`
- `font.style = { weight, width, slant }` is the structured alternative to top-level `weight` / `width` / `slant`

Drawing and clipping:

- `canvas:draw_rect(x, y, w, h, paint)`
- `canvas:draw_rrect(x, y, w, h, rx, ry, paint)`
- `canvas:draw_path(path, paint)`
- `canvas:hit_test_rect(x, y, w, h, hit_x, hit_y)` returns whether the point falls inside the transformed rectangle
- `canvas:hit_test_path(path, hit_x, hit_y)` returns whether the point falls inside the transformed path; `path` accepts the same compiled path and path table forms as `draw_path`
- `canvas:draw_text(text, x, y, font, paint)`
- `canvas:draw_paragraph(paragraph, x, y)`
- `canvas:clip_rect(x, y, w, h)`
- `canvas:clip_rrect(x, y, w, h, rx, ry)`
- `canvas:clip_path(path)`
- `draw_image_rect` was removed; use an image shader inside paint instead

Path construction:

- `canvas:path()` creates an empty path
- `path:move_to(x, y)`, `path:line_to(x, y)`, `path:quad_to(x1, y1, x2, y2)`, `path:cubic_to(x1, y1, x2, y2, x3, y3)`, `path:arc_to(rx, ry, x_axis_rotation, large_arc_flag, sweep_flag, x1, y1)`
- `path:close()`, `path:reset()`, `path:set_fill_type(fill_type)`
- portable `fill_type` values are `"winding"` and `"even_odd"`
- Skia-only path helpers: `canvas:path("M0 0 L10 10 Z")`, `canvas:path({ svg = "..." })`, and `path:to_svg_string(relative)`
- geometry-adding helpers such as `add_rect`, `add_rrect`, and `add_oval` are no longer part of the Lua helper API

Shaders:

- `canvas:shader(0xFFAABBCC)` creates a solid-color shader
- `canvas:shader({ type = "solid_color", color })` also creates a solid-color shader
- `canvas:shader({ type = "linear_gradient", x0, y0, x1, y1, colors, positions, tile_mode })`
- `canvas:shader({ type = "radial_gradient", cx, cy, radius, colors, positions, tile_mode })`
- `canvas:shader({ type = "image", image, sampling, tile_mode })`
- `colors` must contain at least 2 packed colors
- `positions` is optional and must match `colors` length when provided
- `sampling` may be `"nearest"`, `"linear"`, or `"cubic"`
- Blend2D currently treats image `sampling = "cubic"` the same as `"linear"`
- `tile_mode` may be `"clamp"`, `"repeat"`, `"mirror"`, or `"decal"`

Text and paragraphs:

- `canvas:measure_text(text, font, paint)` returns metrics including bounds, ascent, descent, and line height
- `canvas:paragraph(opts)` accepts `{ width, align, max_lines, ellipsis, font, color, segments = { ... } }`
- each paragraph segment accepts `{ text, font, color }`
- `paragraph:measure()` returns layout metrics such as width, height, intrinsic widths, and line count
- `align` may be `"left"`, `"center"`, or `"right"`

Example:

```lua
local fill = luna.window:paint({
  shader = 0xFF7CC6FF,
  style = "fill",
})

local ring = luna.window:path({ fill_type = "even_odd" })
ring:move_to(80, 80)
ring:line_to(300, 80)
ring:line_to(300, 300)
ring:line_to(80, 300)
ring:close()
ring:move_to(130, 130)
ring:line_to(130, 250)
ring:line_to(250, 250)
ring:line_to(250, 130)
ring:close()

luna.window:save()
luna.window:clip_rect(60, 60, 260, 260)
luna.window:draw_path(ring, fill)
luna.window:restore()
```

## Save Data and Files

Luna separates regular file reads from save-data writes:

- `vfs.open(path)` opens a file for reading
- `save.write_json(path, value)` writes JSON into the app save directory
- `save.read_json(path)` reads JSON back from the app save directory
- `save.root` exposes the resolved save directory path

Save paths are sandboxed and reject absolute paths or `..` traversal.

## Notes

- Graphics initialization is deferred until the first frame, so Vulkan-related failures may appear when the main loop begins rather than at process startup.
- The engine uses a small worker pool to complete background asset-loading jobs.
- Focused backend tests live under `test/`; the render parity fuzz binary is available for manual investigation when both backends are built.

## Example

See `docs/example/main.lua` for a small runnable script.
