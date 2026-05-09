local luna = require("luna")

luna.set_window_size(1560, 960)
luna.set_frame_time(1 / 60)

local function font_path()
  return "assets/ABeeZee-Regular.ttf"
end

local colors = {
  background = 0xFFF5F1E8,
  panel_fill = 0xFFFCFAF4,
  panel_border = 0xFFD7C7AE,
  ink = 0xFF1E2A33,
  muted = 0xFF607181,
  accent = 0xFF007A78,
  warm = 0xFFC96A3D,
  blue = 0xFF3E78C5,
  red = 0xFFC54A52,
  green = 0xFF4E8B5E,
  faint = 0x1A1E2A33,
  guide = 0x332D3A43,
  box_fill = 0x1AC96A3D,
  block_fill = 0x12007A78,
  block_blue = 0x123E78C5,
}

local fonts = nil
local scene = nil

local function compile_fonts(canvas)
  return {
    hero = canvas:font({ size = 30, family = "ABeeZee" }),
    title = canvas:font({ size = 22, family = "ABeeZee" }),
    body = canvas:font({ size = 16, family = "ABeeZee" }),
    small = canvas:font({ size = 15, family = "ABeeZee" }),
    tiny = canvas:font({ size = 14, family = "ABeeZee" }),
    text = canvas:font({ size = 24, family = "ABeeZee" }),
    text_big = canvas:font({ size = 30, family = "ABeeZee" }),
    text_huge = canvas:font({ size = 48, family = "ABeeZee" }),
    para = canvas:font({ size = 22, family = "ABeeZee" }),
    para_big = canvas:font({ size = 30, family = "ABeeZee" }),
    trunc = canvas:font({ size = 23, family = "ABeeZee" }),
    trunc_big = canvas:font({ size = 33, family = "ABeeZee" }),
  }
end

local function fill(color)
  return { shader = color, style = "fill" }
end

local function stroke(color, width)
  return { shader = color, style = { type = "stroke", width = width } }
end

local function draw_hline(canvas, x, y, w, color, thickness)
  canvas:draw_rect(x, y - thickness * 0.5, w, thickness, fill(color))
end

local function draw_vline(canvas, x, y, h, color, thickness)
  canvas:draw_rect(x - thickness * 0.5, y, thickness, h, fill(color))
end

local function draw_panel(canvas, rect)
  canvas:draw_rrect(rect.x, rect.y, rect.w, rect.h, 26, 26, fill(colors.panel_fill))
  canvas:draw_rrect(rect.x, rect.y, rect.w, rect.h, 26, 26, stroke(colors.panel_border, 2))
end

local function draw_chip(canvas, x, y, w, label, bg, fg)
  canvas:draw_rrect(x, y - 15, w, 24, 12, 12, fill(bg))
  canvas:draw_text(label, x + 10, y + 2, fonts.tiny, fill(fg or colors.ink))
end

local function text_baseline_for_center(canvas, center_y, font)
  local metrics = canvas:measure_text("Ag", font)
  return center_y - ((metrics.ascent + metrics.descent) * 0.5)
end

local function draw_legend_item(canvas, x, y, kind, color, label)
  local icon_size = 18
  local center_y = y
  local text_y = text_baseline_for_center(canvas, center_y, fonts.tiny)

  if kind == "line" then
    draw_hline(canvas, x, center_y, icon_size, color, 2)
  elseif kind == "vline" then
    draw_vline(canvas, x + icon_size * 0.5, center_y - icon_size * 0.5, icon_size, color, 2)
  elseif kind == "box" then
    canvas:draw_rect(x, center_y - icon_size * 0.5, icon_size, icon_size, fill(colors.box_fill))
    canvas:draw_rect(x, center_y - icon_size * 0.5, icon_size, icon_size, stroke(color, 2))
  elseif kind == "pill" then
    canvas:draw_rrect(x, center_y - 6, icon_size, 12, 6, 6, fill(color))
  end
  canvas:draw_text(label, x + 28, text_y, fonts.tiny, fill(colors.ink))
end

local function draw_panel_title(canvas, rect, title, subtitle)
  canvas:draw_text(title, rect.x + 26, rect.y + 42, fonts.title, fill(colors.ink))
  canvas:draw_text(subtitle, rect.x + 26, rect.y + 68, fonts.body, fill(colors.muted))
end

local function inset_rect(rect, left, top, right, bottom)
  right = right or left
  bottom = bottom or top
  return {
    x = rect.x + left,
    y = rect.y + top,
    w = rect.w - left - right,
    h = rect.h - top - bottom,
  }
end

local function panel_body_rect(rect)
  return inset_rect(rect, 26, 94, 26, 24)
end

local function panel_rects()
  return {
    measured = { x = 40, y = 136, w = 732, h = 316 },
    wrap = { x = 788, y = 136, w = 732, h = 316 },
    align = { x = 40, y = 480, w = 732, h = 440 },
    truncation = { x = 788, y = 480, w = 732, h = 440 },
  }
end

local function split_columns(rect, count, gap)
  local total_gap = gap * (count - 1)
  local column_w = math.floor((rect.w - total_gap) / count)
  local columns = {}
  local cursor_x = rect.x
  for i = 1, count do
    local w = (i == count) and (rect.x + rect.w - cursor_x) or column_w
    columns[i] = { x = cursor_x, y = rect.y, w = w, h = rect.h }
    cursor_x = cursor_x + w + gap
  end
  return columns
end

local function alignment_card_rects(rect)
  local body = panel_body_rect(rect)
  local cards_area = inset_rect(body, 4, 18, 4, 72)
  return split_columns(cards_area, 3, 28)
end

local function text_width(canvas, text, font)
  return math.ceil(canvas:measure_text(text, font).advance_width or 0)
end

local function draw_chip_auto(canvas, x, y, label, bg, fg)
  local w = math.max(52, text_width(canvas, label, fonts.tiny) + 20)
  draw_chip(canvas, x, y, w, label, bg, fg)
  return w
end

local function draw_chip_row(canvas, x, y, gap, items)
  local cursor_x = x
  for _, item in ipairs(items) do
    cursor_x = cursor_x + draw_chip_auto(canvas, cursor_x, y, item.label, item.bg, item.fg) + gap
  end
end

local function draw_legend_row(canvas, x, y, gap, items)
  local cursor_x = x
  for _, item in ipairs(items) do
    draw_legend_item(canvas, cursor_x, y, item.kind, item.color, item.label)
    cursor_x = cursor_x + 28 + text_width(canvas, item.label, fonts.tiny) + gap
  end
end

local function concat_segments(segments)
  local parts = {}
  for _, segment in ipairs(segments) do
    parts[#parts + 1] = segment.text
  end
  return table.concat(parts)
end

local function find_explicit_break_line(metrics, text)
  if not metrics.lines or not text then
    return nil
  end

  local newline_at = text:find("\n", 1, true)
  if not newline_at then
    return nil
  end

  local line_start = newline_at
  for i, line in ipairs(metrics.lines) do
    if line.start_index == line_start then
      return i
    end
  end

  return nil
end

local function paragraph_baseline_shift(metrics)
  return (metrics.ideographic_baseline or 0) - (metrics.alphabetic_baseline or 0)
end

local function draw_paragraph_guides(canvas, x, y, metrics, opts)
  opts = opts or {}
  local layout_stroke = opts.layout_stroke or colors.accent
  local line_fill = opts.line_fill or 0x144E8B5E
  local line_stroke = opts.line_stroke or colors.green
  local baseline_color = opts.baseline_color or colors.red
  local ideographic_color = opts.ideographic_color or colors.warm
  local left_tick_color = opts.left_tick_color or colors.blue
  local show_layout_box = opts.show_layout_box ~= false
  local show_line_boxes = opts.show_line_boxes ~= false
  local show_baselines = opts.show_baselines ~= false
  local show_ideographic = opts.show_ideographic ~= false
  local show_left_ticks = opts.show_left_ticks ~= false
  local layout_height = math.max(metrics.height, 1)
  local ideographic_shift = paragraph_baseline_shift(metrics)

  if show_layout_box then
    canvas:draw_rect(x, y, metrics.width, layout_height, stroke(layout_stroke, 2))
  end
  for _, line in ipairs(metrics.lines or {}) do
    local line_x = x + line.left
    local line_top = y + line.baseline - line.ascent
    local line_w = math.max(line.width, 1)
    local line_h = math.max(line.height, 1)

    if show_line_boxes then
      canvas:draw_rrect(line_x, line_top, line_w, line_h, 10, 10, fill(line_fill))
      canvas:draw_rrect(line_x, line_top, line_w, line_h, 10, 10, stroke(line_stroke, 1.5))
    end
    if show_baselines then
      draw_hline(canvas, line_x, y + line.baseline, line_w, baseline_color, 2)
      if show_ideographic then
        draw_hline(canvas, line_x, y + line.baseline + ideographic_shift, line_w, ideographic_color, 1.5)
      end
    end
    if show_left_ticks then
      draw_vline(canvas, line_x, line_top, line_h, left_tick_color, 1.5)
    end
  end
end

local function visible_text_fraction(metrics, total_chars)
  if total_chars <= 0 or not metrics.lines or #metrics.lines == 0 then
    return 0
  end

  return math.max(0, math.min(1, metrics.lines[#metrics.lines].end_index / total_chars))
end

local function draw_visibility_strip(canvas, x, y, w, metrics, total_chars)
  local visible = visible_text_fraction(metrics, total_chars)
  canvas:draw_rrect(x, y, w, 18, 9, 9, fill(0x0E1E2A33))
  canvas:draw_rrect(x, y, w, 18, 9, 9, stroke(colors.guide, 1))

  if visible > 0 then
    canvas:draw_rrect(x + 2, y + 2, math.max(6, (w - 4) * visible), 14, 7, 7, fill(colors.blue))
  end
  if visible < 1 then
    local hidden_x = x + 2 + (w - 4) * visible
    canvas:draw_rrect(hidden_x, y + 2, math.max(4, (w - 4) * (1 - visible)), 14, 7, 7, fill(0x18C54A52))
  end
  if metrics.did_exceed_max_lines then
    draw_vline(canvas, x + w * visible, y - 4, 26, colors.warm, 2)
  end
end

local function build_scene(canvas)
  local panels = panel_rects()
  local wrap_body = panel_body_rect(panels.wrap)
  local truncation_body = panel_body_rect(panels.truncation)
  local align_cards = alignment_card_rects(panels.align)
  local measured = {}
  measured.sample = "Sphinx of black quartz"
  measured.metrics = canvas:measure_text(measured.sample, fonts.text_huge)

  local wrap = {}
  wrap.width = wrap_body.w
  wrap.segments = {
    { text = "One paragraph keeps a shared width while " },
    { text = "highlighted words", font = fonts.text_big, color = colors.blue },
    { text = " and an intentional\nline break create distinct rows." },
  }
  wrap.text = concat_segments(wrap.segments)
  wrap.paragraph = canvas:paragraph({
    width = wrap.width,
    font = fonts.text,
    color = colors.ink,
    segments = wrap.segments,
  })
  wrap.metrics = wrap.paragraph:measure()
  wrap.explicit_break_line = find_explicit_break_line(wrap.metrics, wrap.text)

  local align = {}
  align.columns = {
    {
      label = "Left",
      paragraph = canvas:paragraph({
        width = align_cards[1].w - 28,
        align = "left",
        font = fonts.para,
        color = colors.ink,
        segments = {
          { text = "Alignment shifts the same wrapped copy without changing the box." },
        },
      }),
    },
    {
      label = "Center",
      paragraph = canvas:paragraph({
        width = align_cards[2].w - 28,
        align = "center",
        font = fonts.para,
        color = colors.ink,
        segments = {
          { text = "Alignment shifts the same wrapped copy without changing the box." },
        },
      }),
    },
    {
      label = "Right",
      paragraph = canvas:paragraph({
        width = align_cards[3].w - 28,
        align = "right",
        font = fonts.para,
        color = colors.ink,
        segments = {
          { text = "Alignment shifts the same wrapped copy without changing the box." },
        },
      }),
    },
  }
  for _, column in ipairs(align.columns) do
    column.metrics = column.paragraph:measure()
  end

  local truncation = {}
  truncation.width = truncation_body.w
  truncation.segments = {
    { text = "Overflow ", font = fonts.trunc_big, color = colors.warm },
    {
      text = "is clipped after three visible rows, so the last line ends with an ellipsis while the hidden ending continues beyond the card for a while longer to make the cutoff obvious.\n",
    },
    { text = "This line of text is hidden" },
  }
  truncation.text = concat_segments(truncation.segments)
  truncation.paragraph = canvas:paragraph({
    width = truncation.width,
    font = fonts.trunc,
    color = colors.ink,
    max_lines = 3,
    ellipsis = "...",
    segments = truncation.segments,
  })
  truncation.metrics = truncation.paragraph:measure()

  return {
    measured = measured,
    wrap = wrap,
    align = align,
    truncation = truncation,
  }
end

local function draw_background(canvas)
  canvas:clear(colors.background)
  for x = 24, 1560, 48 do
    draw_vline(canvas, x, 0, 1080, colors.faint, 1)
  end
  for y = 24, 1080, 48 do
    draw_hline(canvas, 0, y, 1560, colors.faint, 1)
  end
end

local function draw_header(canvas)
  canvas:draw_text("Text Rendering Demo", 42, 58, fonts.hero, fill(colors.ink))
  canvas:draw_text(
    "See how one line sits, how paragraphs form rows, and how text is clipped when space runs out.",
    42,
    84,
    fonts.body,
    fill(colors.muted)
  )
  draw_hline(canvas, 42, 102, 240, colors.accent, 3)
  draw_hline(canvas, 290, 102, 84, colors.warm, 3)
  draw_hline(canvas, 382, 102, 58, colors.blue, 3)
end

local function draw_measured_text_panel(canvas, rect)
  draw_panel(canvas, rect)
  draw_panel_title(
    canvas,
    rect,
    "Measured Text",
    "See where one line sits, extends, and draws."
  )

  local sample = scene.measured
  local m = sample.metrics
  local body = panel_body_rect(rect)
  local sidebar_w = 146
  local sample_area = inset_rect(body, 0, 22, sidebar_w, 0)
  local sidebar_x = body.x + body.w - sidebar_w + 4
  local origin_x = sample_area.x + 10
  local baseline_y = sample_area.y + 112
  local guide_w = sample_area.w - 18
  local ascent_y = baseline_y + m.ascent
  local descent_y = baseline_y + m.descent

  canvas:draw_rect(
    origin_x + m.bounds_x,
    baseline_y + m.bounds_y,
    m.bounds_w,
    m.bounds_h,
    fill(colors.box_fill)
  )
  canvas:draw_rect(
    origin_x + m.bounds_x,
    baseline_y + m.bounds_y,
    m.bounds_w,
    m.bounds_h,
    stroke(colors.warm, 2)
  )

  draw_hline(canvas, origin_x - 4, baseline_y, guide_w, colors.red, 2)
  draw_hline(canvas, origin_x - 4, ascent_y, guide_w, colors.blue, 2)
  draw_hline(canvas, origin_x - 4, descent_y, guide_w, colors.green, 2)
  draw_vline(canvas, origin_x + m.advance_width, baseline_y - 72, 96, colors.accent, 2)

  canvas:draw_text(sample.sample, origin_x, baseline_y, fonts.text_huge, fill(colors.ink))

  draw_legend_item(canvas, sidebar_x + 4, body.y + 18, "line", colors.red, "baseline")
  draw_legend_item(canvas, sidebar_x + 4, body.y + 42, "line", colors.blue, "top guide")
  draw_legend_item(canvas, sidebar_x + 4, body.y + 66, "line", colors.green, "bottom guide")
  draw_legend_item(canvas, sidebar_x + 4, body.y + 90, "vline", colors.accent, "advance")
  draw_legend_item(canvas, sidebar_x + 4, body.y + 114, "box", colors.warm, "drawn bounds")
  draw_hline(canvas, sidebar_x + 4, body.y + 146, 18, colors.blue, 2)
  canvas:draw_text("above the line", sidebar_x + 32, body.y + 150, fonts.tiny, fill(colors.blue))
  draw_hline(canvas, sidebar_x + 4, body.y + 170, 18, colors.green, 2)
  canvas:draw_text("below the line", sidebar_x + 32, body.y + 174, fonts.tiny, fill(colors.green))
end

local function draw_wrap_panel(canvas, rect)
  draw_panel(canvas, rect)
  draw_panel_title(
    canvas,
    rect,
    "Paragraph Layout",
    "A paragraph can flow into several rows while still sharing one width."
  )

  local wrap = scene.wrap
  local m = wrap.metrics
  local body = panel_body_rect(rect)
  local paragraph_x = body.x
  local paragraph_y = body.y + 18
  local legend_y = rect.y + rect.h - 28

  canvas:draw_rect(paragraph_x, paragraph_y, wrap.width, math.max(m.height, 1), fill(colors.block_fill))
  canvas:draw_paragraph(wrap.paragraph, paragraph_x, paragraph_y)
  draw_paragraph_guides(canvas, paragraph_x, paragraph_y, m, {
    layout_stroke = colors.accent,
    line_fill = 0x124E8B5E,
    line_stroke = colors.green,
    left_tick_color = colors.blue,
    show_baselines = false,
    show_ideographic = false,
  })

  local summary = tostring(m.line_count) .. " rows inside one paragraph"
  if wrap.explicit_break_line then
    summary = summary .. ", including one intentional line break"
  end
  canvas:draw_text(summary, body.x + 2, legend_y - 30, fonts.tiny, fill(colors.muted))
  draw_legend_row(canvas, body.x + 2, legend_y, 18, {
    { kind = "box", color = colors.accent, label = "paragraph width" },
    { kind = "vline", color = colors.blue, label = "row start" },
    { kind = "box", color = colors.green, label = "row block" },
  })
end

local function draw_alignment_panel(canvas, rect)
  draw_panel(canvas, rect)
  draw_panel_title(
    canvas,
    rect,
    "Alignment",
    "The same paragraph can sit left, center, or right without changing its width."
  )

  local cards = alignment_card_rects(rect)
  for i, column in ipairs(scene.align.columns) do
    local card = cards[i]
    local paragraph_x = card.x + 14
    local paragraph_y = card.y + 52
    canvas:draw_rrect(card.x, card.y, card.w, card.h, 18, 18, fill(0x0D3E78C5))
    canvas:draw_rrect(card.x, card.y, card.w, card.h, 18, 18, stroke(colors.panel_border, 2))
    draw_vline(canvas, card.x + card.w * 0.5, card.y + 24, card.h - 36, colors.guide, 1.5)
    canvas:draw_text(column.label, card.x + 14, card.y + 36, fonts.tiny, fill(colors.ink))
    canvas:draw_paragraph(column.paragraph, paragraph_x, paragraph_y)
    draw_paragraph_guides(canvas, paragraph_x, paragraph_y, column.metrics, {
      layout_stroke = colors.guide,
      line_fill = 0x0E3E78C5,
      line_stroke = colors.blue,
      left_tick_color = colors.accent,
      show_baselines = false,
      show_ideographic = false,
    })
  end

  local body = panel_body_rect(rect)
  draw_legend_row(canvas, body.x + 2, rect.y + rect.h - 40, 22, {
    { kind = "vline", color = colors.guide, label = "card center" },
    { kind = "vline", color = colors.accent, label = "row start" },
    { kind = "box", color = colors.blue, label = "row block" },
  })
end

local function draw_truncation_panel(canvas, rect)
  draw_panel(canvas, rect)
  draw_panel_title(
    canvas,
    rect,
    "Max Lines And Ellipsis",
    "When a paragraph runs past three rows, the last visible row ends with an ellipsis."
  )

  local truncation = scene.truncation
  local m = truncation.metrics
  local body = panel_body_rect(rect)
  local paragraph_x = body.x
  local paragraph_y = body.y + 26
  local visible_height = math.max(m.height + 20, 1)
  local content_bottom = paragraph_y + visible_height

  draw_chip_row(canvas, body.x, body.y + 4, 8, {
    { label = "3-row limit", bg = 0x143E78C5, fg = colors.blue },
    {
      label = m.did_exceed_max_lines and "truncated" or "fits",
      bg = m.did_exceed_max_lines and 0x18C96A3D or 0x184E8B5E,
      fg = m.did_exceed_max_lines and colors.warm or colors.green,
    },
  })

  canvas:draw_rect(paragraph_x, paragraph_y, truncation.width, visible_height, fill(colors.block_blue))
  canvas:draw_paragraph(truncation.paragraph, paragraph_x, paragraph_y + 10)
  canvas:draw_rect(paragraph_x, paragraph_y, truncation.width, visible_height, stroke(colors.blue, 2))

  for _, line in ipairs(m.lines) do
    local line_x = paragraph_x + line.left
    local line_y = paragraph_y + 10
    local line_top = line_y + line.baseline - line.ascent
    canvas:draw_rrect(line_x, line_top, math.max(line.width, 1), math.max(line.height, 1), 8, 8, fill(0x143E78C5))
  end

  local legend_items = {
    { kind = "box", color = colors.blue, label = "shown rows" },
  }
  if m.did_exceed_max_lines then
    local strip_y = content_bottom + 26
    local label_y = strip_y + 28
    draw_visibility_strip(canvas, paragraph_x, strip_y, truncation.width, m, #truncation.text)
    canvas:draw_text("shown text", paragraph_x, label_y, fonts.tiny, fill(colors.blue))
    canvas:draw_text("hidden text", paragraph_x + truncation.width - 74, label_y, fonts.tiny, fill(colors.red))
    legend_items[#legend_items + 1] = { kind = "pill", color = colors.red, label = "hidden text" }
    draw_legend_row(canvas, body.x + 2, label_y + 44, 22, legend_items)
  else
    canvas:draw_text(
      "Everything fits inside the three-row limit.",
      paragraph_x,
      content_bottom + 34,
      fonts.tiny,
      fill(colors.muted)
    )
    draw_legend_row(canvas, body.x + 2, content_bottom + 84, 22, legend_items)
  end
end

local function draw_scene(canvas)
  local panels = panel_rects()
  draw_background(canvas)
  draw_header(canvas)
  draw_measured_text_panel(canvas, panels.measured)
  draw_wrap_panel(canvas, panels.wrap)
  draw_alignment_panel(canvas, panels.align)
  draw_truncation_panel(canvas, panels.truncation)
end

local function await_promise(promise)
  if not promise:poll() then
    luna.wait(promise:event())
  end
  return promise:take()
end

local function main()
  await_promise(luna.load_fontface(font_path()))
  fonts = compile_fonts(luna.window)
  scene = build_scene(luna.window)

  while true do
    local events = luna.poll_events()
    for _, event in ipairs(events) do
      if event.type == "quit" then
        return
      end
    end

    draw_scene(luna.window)
    luna.next_frame()
  end
end

luna.start(main)
