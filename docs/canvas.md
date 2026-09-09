# Canvas API

See the [Lua runtime overview](lua-api.md) for setup and
[asset loading](fs-assets.md#assets) for images and fonts.

The canvas API supports operations such as:

- clearing, transforms, save/restore
- rectangle drawing and clipping
- geometry hit testing
- path construction from commands
- path drawing and clipping
- image drawing and snapshots
- text measurement and drawing
- paragraph creation, layout, measurement, and drawing

## Canvas interface

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
- solid colors use `shader`, for example `canvas:paint({ shader = 0xFF7CC6FF, style = "fill" })`
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
- draw images using an image shader inside paint

Path construction:

- `canvas:path()` creates an empty path
- `path:move_to(x, y)`, `path:line_to(x, y)`, `path:quad_to(x1, y1, x2, y2)`, `path:cubic_to(x1, y1, x2, y2, x3, y3)`, `path:arc_to(rx, ry, x_axis_rotation, large_arc_flag, sweep_flag, x1, y1)`
- `path:close()`, `path:reset()`, `path:set_fill_type(fill_type)`
- portable `fill_type` values are `"winding"` and `"even_odd"`
- Both backends accept SVG path data: `canvas:path("M0 0 L10 10 Z")` and `canvas:path({ svg = "..." })`. All standard path commands, relative coordinates, and repeated arguments are supported; this does not load SVG documents. Arc rotation is in degrees.
- SVG serialization remains Skia-only: `path:to_svg_string(relative)`.

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
