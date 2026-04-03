# Luna

Luna is a small native runtime for Lua-driven 2D applications. It combines a coroutine-based Lua host with Skia rendering on top of Vulkan, SDL3 windowing/input, and SDL3_mixer audio playback.

The executable loads a Lua entry file at startup, defaulting to `main.lua`, and exposes engine, rendering, asset-loading, audio, and save-file helpers through `require("luna")`.

## What It Includes

- LuaJIT embedded directly in the project
- SDL3 windowing, input, and event pumping
- Vulkan-backed rendering with Skia Graphite
- Canvas-style drawing APIs for rectangles, images, text, and paragraphs
- Asynchronous asset loading for images, font faces, and audio
- A small event/promise model for coordinating background work from Lua
- Sandboxed save-data helpers with JSON read/write support

## Project Layout

- `src/`: engine, renderer, canvas, audio mixer, logging, and VFS implementation
- `vendor/`: vendored LuaJIT build integration
- `vcpkg-ports/`: overlay ports for Skia and SDL3-related packages
- `vcpkg-triplets/`: custom triplets used by the provided CMake presets
- `docs/example/main.lua`: a minimal Lua script showing the runtime shape

## Requirements

- CMake 3.20 or newer
- Ninja
- A working `vcpkg` installation exposed through `VCPKG_ROOT`
- A C++20-capable toolchain
- A Vulkan-capable system and graphics driver

The repository is set up around `vcpkg` manifests and CMake presets. Dependencies such as `fmt`, `SDL3`, `SDL3_mixer`, `Skia`, and Vulkan Memory Allocator are resolved through `vcpkg`.

## Building

### Linux

The repo includes a preset for the custom `x64-linux-clang-lto` triplet:

```bash
cmake --preset vcpkg-x64-linux
cmake --build build
```

### Windows

The repo also includes a preset for the custom `x64-windows-clangcl-lto` triplet:

```powershell
cmake --preset vcpkg-x64-windows
cmake --build build
```

If `VCPKG_ROOT` is not set, CMake will not be able to find the toolchain file used by the presets.

## Running

By default, Luna looks for `main.lua` in the working directory:

```bash
./build/luna
```

You can also provide an explicit Lua entry path:

```bash
./build/luna path/to/main.lua
```

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
      color = 0xFFFFCC66,
      anti_alias = true,
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
- Audio: `audio.track_create`, `audio.track_destroy`, `audio.track_set`, `audio.track_play`, `audio.track_stop`, `audio.track_set_gain`, `audio.set_mixer_gain`
- Filesystem/save data: `vfs.open`, `save.write_json`, `save.read_json`, `save.root`

The canvas API supports operations such as:

- clearing, transforms, save/restore
- rectangle drawing and clipping
- path construction from commands or SVG path strings
- path drawing and clipping
- image drawing and snapshots
- text measurement and drawing
- paragraph creation, layout, measurement, and drawing

### Canvas Interface

`luna.window` is the window-backed canvas, and `luna.make_canvas(width, height)` creates an offscreen canvas with the same drawing API.

Core canvas methods:

- `canvas:clear(color)`
- `canvas:save()` / `canvas:restore()`
- `canvas:translate(dx, dy)`
- `canvas:scale(sx, sy)`
- `canvas:rotate(degrees)`
- `canvas:snapshot()` returns an image for offscreen canvases

Paint and font compilation:

- `canvas:paint(opts)` accepts `{ color, anti_alias, style, stroke_width }`
- `paint.style` may be `"fill"`, `"stroke"`, or `"stroke_and_fill"`
- `canvas:font(opts)` accepts `{ size, family, style, weight, width, slant }`

Drawing and clipping:

- `canvas:draw_rect(x, y, w, h, paint)`
- `canvas:draw_path(path, paint)`
- `canvas:draw_image_rect(image, x, y, w, h, sx, sy, sw, sh, paint)`
- `canvas:draw_text(text, x, y, font, paint)`
- `canvas:draw_paragraph(paragraph, x, y)`
- `canvas:clip_rect(x, y, w, h, op, anti_alias)`
- `canvas:clip_path(path, op, anti_alias)`
- `op` may be `"intersect"` or `"difference"`

Path construction:

- `canvas:path()` creates an empty path
- `canvas:path("M0 0 L10 10 Z")` parses an SVG path string
- `canvas:path({ svg = "...", fill_type = "even_odd" })` also supports table-based construction
- `path:move_to(x, y)`, `path:line_to(x, y)`, `path:quad_to(x1, y1, x2, y2)`, `path:cubic_to(x1, y1, x2, y2, x3, y3)`, `path:conic_to(x1, y1, x2, y2, weight)`
- `path:add_rect(x, y, w, h, direction)` and `path:add_oval(x, y, w, h, direction)`
- `path:close()`, `path:reset()`, `path:set_fill_type(fill_type)`, `path:to_svg_string(relative)`
- `fill_type` may be `"winding"`, `"even_odd"`, `"inverse_winding"`, or `"inverse_even_odd"`
- `direction` may be `"cw"` or `"ccw"`

Text and paragraphs:

- `canvas:measure_text(text, font, paint)` returns metrics including bounds, ascent, descent, and line height
- `canvas:paragraph(opts)` accepts `{ text, width, font, color, align, max_lines, ellipsis }`
- `paragraph:measure()` returns layout metrics such as width, height, intrinsic widths, and line count
- `align` may be `"left"`, `"center"`, or `"right"`

Example:

```lua
local fill = luna.window:paint({
  color = 0xFF7CC6FF,
  style = "fill",
  anti_alias = true,
})

local ring = luna.window:path({ fill_type = "even_odd" })
ring:add_rect(80, 80, 220, 220)
ring:add_oval(130, 130, 120, 120, "ccw")

luna.window:save()
luna.window:clip_rect(60, 60, 260, 260, "intersect", true)
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
- There is not currently a top-level test suite or packaging flow checked into the repository.

## Example

See `docs/example/main.lua` for a small runnable script.
