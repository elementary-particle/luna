return function(canvas_mt, path_mt)
  local raw_paint = assert(canvas_mt._paint)
  local raw_font = assert(canvas_mt._font)
  local raw_draw_rect = assert(canvas_mt._draw_rect)
  local raw_draw_round_rect = assert(canvas_mt._draw_round_rect)
  local raw_draw_text = assert(canvas_mt._draw_text)
  local raw_measure_text = assert(canvas_mt._measure_text)
  local raw_hit_test_rect = assert(canvas_mt._hit_test_rect)
  local raw_hit_test_path = assert(canvas_mt._hit_test_path)
  local raw_clip_rect = assert(canvas_mt._clip_rect)
  local raw_clip_round_rect = assert(canvas_mt._clip_round_rect)
  local raw_clip_path = assert(canvas_mt._clip_path)
  local raw_save_layer = assert(canvas_mt._save_layer)
  local raw_path = assert(canvas_mt._path)
  local raw_path_svg = canvas_mt._path_svg
  local raw_draw_path = assert(canvas_mt._draw_path)
  local raw_path_set_fill_type = assert(path_mt._set_fill_type)
  local raw_shader_solid_color = assert(canvas_mt._shader_solid_color)
  local raw_shader_linear_gradient = assert(canvas_mt._shader_linear_gradient)
  local raw_shader_radial_gradient = assert(canvas_mt._shader_radial_gradient)
  local raw_shader_image = assert(canvas_mt._shader_image)
  local raw_paragraph = assert(canvas_mt._paragraph)
  local raw_draw_paragraph = assert(canvas_mt._draw_paragraph)

  local constants = assert(canvas_mt._constants)

  local paint_style_ = {
    fill = constants.paint_style.fill,
    stroke = constants.paint_style.stroke,
  }

  local blend_mode_ = {
    clear = constants.blend_mode.clear,
    src = constants.blend_mode.src,
    dst = constants.blend_mode.dst,
    src_over = constants.blend_mode.src_over,
    dst_over = constants.blend_mode.dst_over,
    src_in = constants.blend_mode.src_in,
    dst_in = constants.blend_mode.dst_in,
    src_out = constants.blend_mode.src_out,
    dst_out = constants.blend_mode.dst_out,
    src_atop = constants.blend_mode.src_atop,
    dst_atop = constants.blend_mode.dst_atop,
    xor = constants.blend_mode.xor,
    plus = constants.blend_mode.plus,
    modulate = constants.blend_mode.modulate,
    screen = constants.blend_mode.screen,
    overlay = constants.blend_mode.overlay,
    darken = constants.blend_mode.darken,
    lighten = constants.blend_mode.lighten,
    color_dodge = constants.blend_mode.color_dodge,
    color_burn = constants.blend_mode.color_burn,
    hard_light = constants.blend_mode.hard_light,
    soft_light = constants.blend_mode.soft_light,
    difference = constants.blend_mode.difference,
    exclusion = constants.blend_mode.exclusion,
    multiply = constants.blend_mode.multiply,
  }

  local image_sampling_ = {
    nearest = constants.sampling.nearest,
    linear = constants.sampling.linear,
    cubic = constants.sampling.cubic,
  }

  local tile_mode_ = {
    clamp = constants.tile_mode.clamp,
    ["repeat"] = constants.tile_mode["repeat"],
    mirror = constants.tile_mode.mirror,
    decal = constants.tile_mode.decal,
  }

  local path_fill_type_ = {
    winding = constants.path_fill_type.winding,
    even_odd = constants.path_fill_type.even_odd,
  }

  local text_align_ = {
    left = constants.text_align.left,
    center = constants.text_align.center,
    right = constants.text_align.right,
  }

  local allowed_paint_keys = {
    shader = true,
    alpha = true,
    blend_mode = true,
    style = true,
  }

  local allowed_paint_style_keys = {
    type = true,
    width = true,
    stroke_width = true,
  }

  local allowed_font_keys = {
    size = true,
    family = true,
    style = true,
    weight = true,
    width = true,
    slant = true,
  }

  local allowed_font_style_keys = {
    weight = true,
    width = true,
    slant = true,
  }

  local allowed_path_keys = {
    svg = true,
    fill_type = true,
  }

  local allowed_shader_keys = {
    type = true,
    color = true,
    colors = true,
    positions = true,
    tile_mode = true,
    sampling = true,
    image = true,
    x0 = true,
    y0 = true,
    x1 = true,
    y1 = true,
    cx = true,
    cy = true,
    radius = true,
    x = true,
    y = true,
    w = true,
    h = true,
    sx = true,
    sy = true,
    sw = true,
    sh = true,
  }

  local allowed_paragraph_keys = {
    width = true,
    align = true,
    max_lines = true,
    ellipsis = true,
    font = true,
    color = true,
    segments = true,
  }

  local allowed_segment_keys = {
    text = true,
    font = true,
    color = true,
  }

  local function normalize_enum(name, value, map, level)
    if value == nil then
      return nil
    end
    if type(value) == "number" then
      return value
    end
    if type(value) ~= "string" then
      error(string.format("%s must be a number or string", name), level or 3)
    end

    local mapped = map[value]
    if mapped == nil then
      error(string.format("invalid %s '%s'", name, value), level or 3)
    end
    return mapped
  end

  local function normalize_blend_mode(mode, level)
    return normalize_enum("paint.blend_mode", mode, blend_mode_, level)
  end

  local function normalize_image_sampling(sampling, level)
    return normalize_enum("shader.sampling", sampling, image_sampling_, level)
  end

  local function normalize_tile_mode(mode, level)
    return normalize_enum("shader.tile_mode", mode, tile_mode_, level)
  end

  local function normalize_path_fill_type(fill_type, level)
    return normalize_enum("path.fill_type", fill_type, path_fill_type_, level)
  end

  local function normalize_text_align(align, level)
    return normalize_enum("paragraph.align", align, text_align_, level)
  end

  local function normalize_font_style_value(kind, value, map, level)
    return normalize_enum("font." .. kind, value, map, level)
  end

  local function compile_font_style(font, level)
    local style = font.style
    if style ~= nil then
      if type(style) ~= "table" then
        error("font.style must be a table", level or 3)
      end
      if font.weight ~= nil or font.width ~= nil or font.slant ~= nil then
        error("font.style cannot be combined with font.weight/font.width/font.slant", level or 3)
      end
      for key in pairs(style) do
        if not allowed_font_style_keys[key] then
          error(string.format("unknown font.style field '%s'", tostring(key)), level or 3)
        end
      end
      font = style
    end

    return {
      weight = normalize_font_style_value("weight", font.weight, constants.font_style.weight, (level or 3) + 1),
      width = normalize_font_style_value("width", font.width, constants.font_style.width, (level or 3) + 1),
      slant = normalize_font_style_value("slant", font.slant, constants.font_style.slant, (level or 3) + 1),
    }
  end

  local function compile_font(self, font, level)
    if font == nil or type(font) == "userdata" then
      return font
    end
    if type(font) ~= "table" then
      error("font must be a table or compiled font", level or 3)
    end

    for key in pairs(font) do
      if not allowed_font_keys[key] then
        error(string.format("unknown font field '%s'", tostring(key)), level or 3)
      end
    end

    if font.size == nil then
      error("font.size is required", level or 3)
    end

    local style = compile_font_style(font, level)
    if font.family == nil and (style.weight ~= nil or style.width ~= nil or style.slant ~= nil) then
      error("font.family is required when specifying font style", level or 3)
    end

    return raw_font(self, font.size, font.family, style.weight, style.width, style.slant)
  end

  local function compile_paint_style(style, level)
    if style == nil then
      return nil, nil
    end
    if type(style) == "string" or type(style) == "number" then
      local normalized = normalize_enum("paint.style", style, paint_style_, level)
      if normalized == paint_style_.stroke then
        error("paint.style.width is required for stroke paints", level or 3)
      end
      return normalized, nil
    end
    if type(style) ~= "table" then
      error("paint.style must be a number, string, or table", level or 3)
    end

    for key in pairs(style) do
      if not allowed_paint_style_keys[key] then
        error(string.format("unknown paint.style field '%s'", tostring(key)), level or 3)
      end
    end

    local stroke_width = style.width
    if stroke_width == nil then
      stroke_width = style.stroke_width
    end

    local style_type = style.type
    if style_type == nil then
      if stroke_width ~= nil then
        style_type = "stroke"
      else
        error("paint.style.type is required", level or 3)
      end
    end

    local normalized = normalize_enum("paint.style", style_type, paint_style_, (level or 3) + 1)
    if normalized == paint_style_.stroke then
      if stroke_width == nil then
        error("paint.style.width is required for stroke paints", level or 3)
      end
      return normalized, stroke_width
    end

    if stroke_width ~= nil then
      error("paint.style.width is only valid for stroke paints", level or 3)
    end
    return normalized, nil
  end

  local function compile_path(self, path, level)
    if path == nil then
      return raw_path(self, nil)
    end
    if type(path) == "userdata" then
      return path
    end
    if type(path) == "string" then
      if raw_path_svg == nil then
        error("SVG path strings are not supported by this renderer backend", level or 3)
      end
      return raw_path_svg(self, path, nil)
    end
    if type(path) ~= "table" then
      error("path must be a table, SVG path string, or compiled path", level or 3)
    end

    for key in pairs(path) do
      if not allowed_path_keys[key] then
        error(string.format("unknown path field '%s'", tostring(key)), level or 3)
      end
    end

    local fill_type = normalize_path_fill_type(path.fill_type, (level or 3) + 1)
    if path.svg ~= nil then
      if raw_path_svg == nil then
        error("SVG path strings are not supported by this renderer backend", level or 3)
      end
      return raw_path_svg(self, path.svg, fill_type)
    end
    return raw_path(self, fill_type)
  end

  function canvas_mt:shader(shader)
    if shader == nil or type(shader) == "userdata" then
      return shader
    end
    if type(shader) == "number" then
      return raw_shader_solid_color(self, shader)
    end
    if type(shader) ~= "table" then
      error("shader must be a number, table, or compiled shader", 2)
    end

    for key in pairs(shader) do
      if not allowed_shader_keys[key] then
        error(string.format("unknown shader field '%s'", tostring(key)), 2)
      end
    end

    local shader_type = shader.type
    if shader_type == nil and shader.color ~= nil then
      shader_type = "solid_color"
    end

    if shader_type == "solid_color" or shader_type == "solid" then
      if shader.color == nil then
        error("shader.color is required for solid_color shaders", 2)
      end
      return raw_shader_solid_color(self, shader.color)
    end

    if shader_type == "linear_gradient" then
      return raw_shader_linear_gradient(
        self,
        shader.x0,
        shader.y0,
        shader.x1,
        shader.y1,
        shader.colors,
        shader.positions,
        normalize_tile_mode(shader.tile_mode, 3)
      )
    end

    if shader_type == "radial_gradient" then
      return raw_shader_radial_gradient(
        self,
        shader.cx,
        shader.cy,
        shader.radius,
        shader.colors,
        shader.positions,
        normalize_tile_mode(shader.tile_mode, 3)
      )
    end

    if shader_type == "image" then
      if shader.image == nil then
        error("shader.image is required for image shaders", 2)
      end

      return raw_shader_image(
        self,
        shader.image,
        normalize_image_sampling(shader.sampling, 3),
        normalize_tile_mode(shader.tile_mode, 3)
      )
    end

    error(string.format("invalid shader.type '%s'", tostring(shader_type)), 2)
  end

  local function compile_paint(self, paint, level)
    if paint == nil or type(paint) == "userdata" then
      return paint
    end
    if type(paint) ~= "table" then
      error("paint must be a table or compiled paint", level or 3)
    end

    for key in pairs(paint) do
      if not allowed_paint_keys[key] then
        error(string.format("unknown paint field '%s'", tostring(key)), level or 3)
      end
    end

    local style, stroke_width = compile_paint_style(paint.style, (level or 3) + 1)
    return raw_paint(
      self,
      self:shader(paint.shader),
      paint.alpha,
      normalize_blend_mode(paint.blend_mode, (level or 3) + 1),
      style,
      stroke_width
    )
  end

  local function compile_segments(self, opts, level)
    local segments = opts.segments
    if segments == nil then
      error("paragraph.segments is required", level or 3)
    end

    if type(segments) ~= "table" then
      error("paragraph.segments must be a table", level or 3)
    end

    local out = {}
    for i, segment in ipairs(segments) do
      if type(segment) ~= "table" then
        error(string.format("paragraph.segments[%d] must be a table", i), level or 3)
      end

      for key in pairs(segment) do
        if not allowed_segment_keys[key] then
          error(string.format("unknown paragraph.segments[%d] field '%s'", i, tostring(key)), level or 3)
        end
      end

      if segment.text == nil then
        error(string.format("paragraph.segments[%d].text is required", i), level or 3)
      end

      out[i] = {
        text = segment.text,
        font = compile_font(self, segment.font or opts.font, (level or 3) + 1),
        color = segment.color or opts.color,
      }
    end

    if next(out) == nil then
      error("paragraph.segments must contain at least 1 segment", level or 3)
    end

    return out
  end

  function canvas_mt:paint(paint)
    return compile_paint(self, paint, 3)
  end

  function canvas_mt:font(font)
    return compile_font(self, font, 3)
  end

  function canvas_mt:path(path)
    return compile_path(self, path, 3)
  end

  function canvas_mt:draw_rect(x, y, w, h, paint)
    return raw_draw_rect(self, x, y, w, h, compile_paint(self, paint, 3))
  end

  function canvas_mt:draw_rrect(x, y, w, h, rx, ry, paint)
    return raw_draw_round_rect(self, x, y, w, h, rx, ry, compile_paint(self, paint, 3))
  end

  function canvas_mt:draw_path(path, paint)
    return raw_draw_path(self, compile_path(self, path, 3), compile_paint(self, paint, 3))
  end

  function canvas_mt:draw_text(text, x, y, font, paint)
    return raw_draw_text(
      self,
      text,
      x,
      y,
      compile_font(self, font, 3),
      compile_paint(self, paint, 3)
    )
  end

  function canvas_mt:measure_text(text, font, paint)
    return raw_measure_text(
      self,
      text,
      compile_font(self, font, 3),
      compile_paint(self, paint, 3)
    )
  end

  function canvas_mt:hit_test_rect(x, y, w, h, hit_x, hit_y)
    return raw_hit_test_rect(self, x, y, w, h, hit_x, hit_y)
  end

  function canvas_mt:hit_test_path(path, x, y)
    return raw_hit_test_path(self, compile_path(self, path, 3), x, y)
  end

  function canvas_mt:clip_rect(x, y, w, h)
    return raw_clip_rect(self, x, y, w, h)
  end

  function canvas_mt:clip_rrect(x, y, w, h, rx, ry)
    return raw_clip_round_rect(self, x, y, w, h, rx, ry)
  end

  function canvas_mt:clip_path(path)
    return raw_clip_path(self, compile_path(self, path, 3))
  end

  function canvas_mt:save_layer(paint)
    return raw_save_layer(self, compile_paint(self, paint, 3))
  end

  function path_mt:set_fill_type(fill_type)
    return raw_path_set_fill_type(self, normalize_path_fill_type(fill_type, 3))
  end

  function canvas_mt:paragraph(opts)
    if type(opts) ~= "table" then
      error("paragraph options must be a table", 2)
    end

    for key in pairs(opts) do
      if not allowed_paragraph_keys[key] then
        error(string.format("unknown paragraph field '%s'", tostring(key)), 2)
      end
    end

    if opts.width == nil then
      error("paragraph.width is required", 2)
    end

    return raw_paragraph(
      self,
      opts.width,
      compile_segments(self, opts, 3),
      normalize_text_align(opts.align, 3),
      opts.max_lines,
      opts.ellipsis
    )
  end

  function canvas_mt:draw_paragraph(paragraph, x, y)
    return raw_draw_paragraph(self, paragraph, x, y)
  end
end
