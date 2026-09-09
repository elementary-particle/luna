local luna = require("luna")

luna.set_window_size(1480, 920)
luna.set_frame_time(1 / 120)

local function font_path()
  return "assets/ABeeZee-Regular.ttf"
end

local colors = {
  bg_top = 0xFF0A1020,
  bg_bottom = 0xFF151F38,
  panel_top = 0xF61A2340,
  panel_bottom = 0xEE0D1428,
  panel_border = 0x33566D98,
  stage_top = 0xFF101A31,
  stage_bottom = 0xFF0B1122,
  card_fill = 0xFF17213B,
  card_hover = 0xFF1C2948,
  text = 0xFFF3F6FF,
  muted = 0xFF94A6C7,
  faint = 0x26FFFFFF,
  soft = 0x3AFFFFFF,
  shadow = 0x14000000,
  aqua = 0xFF3FD0C7,
  coral = 0xFFFF9363,
  lime = 0xFFA9F279,
  gold = 0xFFFFD166,
  red = 0xFFFF6B7A,
  navy = 0xFF0E1527,
}

local modes = {
  {
    id = "inspect",
    title = "Inspect",
    summary = "Sidebar cards and surface tiles use hit_test_rect.",
    note = "Rect hit testing is enough for classic UI surfaces like cards, chips, and command bars.",
    accent = colors.aqua,
    tone = 0x163FD0C7,
  },
  {
    id = "compose",
    title = "Compose",
    summary = "The animated ring is a path-backed control.",
    note = "Path hit testing makes non-rectangular artwork behave like a real widget instead of a visual only.",
    accent = colors.coral,
    tone = 0x16FF9363,
  },
  {
    id = "ship",
    title = "Ship",
    summary = "Transforms affect drawing and hit testing together.",
    note = "The rotated capsule and scaled ring stay clickable because hit testing uses the current canvas transform.",
    accent = colors.lime,
    tone = 0x16A9F279,
  },
}

local layout = {
  window_w = 1480,
  window_h = 920,
  sidebar = { x = 36, y = 118, w = 322, h = 766 },
  stage = { x = 382, y = 118, w = 700, h = 766 },
  inspector = { x = 1106, y = 118, w = 338, h = 766 },
}

local fonts = nil
local shapes = nil
local paragraph_cache = {}

local state = {
  mode = 1,
  mouse_x = layout.window_w * 0.5,
  mouse_y = layout.window_h * 0.5,
  mouse_down = false,
  click = false,
  hovered_id = "background",
  hovered_kind = "none",
  hovered_label = "background",
  last_action = "Move the pointer across the controls.",
  card_clicks = 0,
  capsule_clicks = 0,
  ring_clicks = 0,
}

local function fill(shader)
  return { shader = shader, style = "fill" }
end

local function stroke(shader, width)
  return { shader = shader, style = { type = "stroke", width = width } }
end

local function linear_shader(x0, y0, x1, y1, shader_colors)
  return {
    type = "linear_gradient",
    x0 = x0,
    y0 = y0,
    x1 = x1,
    y1 = y1,
    colors = shader_colors,
  }
end

local function compile_fonts(canvas)
  return {
    hero = canvas:font({ size = 34, family = "ABeeZee" }),
    title = canvas:font({ size = 24, family = "ABeeZee" }),
    subtitle = canvas:font({ size = 17, family = "ABeeZee" }),
    body = canvas:font({ size = 16, family = "ABeeZee" }),
    label = canvas:font({ size = 14, family = "ABeeZee" }),
    chip = canvas:font({ size = 13, family = "ABeeZee" }),
    metric = canvas:font({ size = 28, family = "ABeeZee" }),
    button = canvas:font({ size = 18, family = "ABeeZee" }),
  }
end

local function add_ngon(path, radius, sides, start_angle)
  for i = 1, sides do
    local angle = start_angle + ((i - 1) / sides) * math.pi * 2
    local x = math.cos(angle) * radius
    local y = math.sin(angle) * radius
    if i == 1 then
      path:move_to(x, y)
    else
      path:line_to(x, y)
    end
  end
  path:close()
end

local function build_shapes(canvas)
  local ring = canvas:path({ fill_type = "even_odd" })
  add_ngon(ring, 124, 6, -math.pi * 0.5)
  add_ngon(ring, 66, 6, -math.pi * 0.5)

  local spark = canvas:path()
  spark:move_to(0, -42)
  spark:line_to(16, -12)
  spark:line_to(44, 0)
  spark:line_to(16, 12)
  spark:line_to(0, 42)
  spark:line_to(-16, 12)
  spark:line_to(-44, 0)
  spark:line_to(-16, -12)
  spark:close()

  return {
    ring = ring,
    spark = spark,
  }
end

local function text_metrics(canvas, text, font)
  return canvas:measure_text(text, font)
end

local function text_width(canvas, text, font)
  return math.ceil(text_metrics(canvas, text, font).advance_width or 0)
end

local function baseline_for_center(canvas, center_y, font)
  local metrics = text_metrics(canvas, "Ag", font)
  return center_y - ((metrics.ascent + metrics.descent) * 0.5)
end

local function baseline_for_top(canvas, top, font)
  return top + text_metrics(canvas, "Ag", font).ascent
end

local function draw_panel(canvas, rect)
  canvas:draw_rrect(rect.x, rect.y + 14, rect.w, rect.h, 28, 28, fill(colors.shadow))
  canvas:draw_rrect(
    rect.x,
    rect.y,
    rect.w,
    rect.h,
    28,
    28,
    fill(linear_shader(rect.x, rect.y, rect.x + rect.w, rect.y + rect.h, {
      colors.panel_top,
      colors.panel_bottom,
    }))
  )
  canvas:draw_rrect(rect.x, rect.y, rect.w, rect.h, 28, 28, stroke(colors.panel_border, 2))
end

local function draw_chip(canvas, x, y, label, bg, fg)
  local w = math.max(54, text_width(canvas, label, fonts.chip) + 20)
  canvas:draw_rrect(x, y, w, 24, 12, 12, fill(bg))
  local baseline = baseline_for_center(canvas, y + 12, fonts.chip)
  local text_x = x + (w - text_width(canvas, label, fonts.chip)) * 0.5
  canvas:draw_text(label, text_x, baseline, fonts.chip, fill(fg))
  return w
end

local function cached_paragraph(canvas, key, opts)
  local cached = paragraph_cache[key]
  if cached ~= nil then
    return cached
  end

  local paragraph = canvas:paragraph(opts)
  local metrics = paragraph:measure()
  cached = {
    paragraph = paragraph,
    metrics = metrics,
    height = math.ceil(math.max(metrics.height or 0, 1)),
  }
  paragraph_cache[key] = cached
  return cached
end

local function draw_copy(canvas, key, x, y, width, text, font, color)
  local cached = cached_paragraph(canvas, key, {
    width = width,
    font = font,
    color = color,
    segments = {
      { text = text },
    },
  })
  canvas:draw_paragraph(cached.paragraph, x, y)
  return cached
end

local function draw_header(canvas)
  local mode = modes[state.mode]

  canvas:draw_text("Hit Testing UI Demo", 40, 72, fonts.hero, fill(colors.text))
  draw_copy(
    canvas,
    "header.subtitle",
    40,
    88,
    940,
    "Rect cards, a transformed capsule, and an even-odd path target all share the same pointer state.",
    fonts.subtitle,
    colors.muted
  )

  local chip_x = layout.window_w - 270
  local chip_w = draw_chip(canvas, chip_x, 48, "MODE", mode.tone, mode.accent)
  draw_chip(canvas, chip_x + chip_w + 10, 48, mode.title, mode.accent, colors.navy)
end

local function set_hover(id, kind, label)
  state.hovered_id = id
  state.hovered_kind = kind
  state.hovered_label = label
end

local function draw_sidebar(canvas, rect)
  draw_panel(canvas, rect)

  canvas:draw_text("Mode Switcher", rect.x + 24, rect.y + 40, fonts.title, fill(colors.text))
  draw_copy(
    canvas,
    "sidebar.intro",
    rect.x + 24,
    rect.y + 62,
    rect.w - 48,
    "These cards use hit_test_rect and change the stage emphasis.",
    fonts.body,
    colors.muted
  )

  local card_x = rect.x + 18
  local card_w = rect.w - 36
  local card_y = rect.y + 138

  for i, mode in ipairs(modes) do
    local card_rect = { x = card_x, y = card_y + (i - 1) * 128, w = card_w, h = 110 }
    local hovered = canvas:hit_test_rect(
      card_rect.x,
      card_rect.y,
      card_rect.w,
      card_rect.h,
      state.mouse_x,
      state.mouse_y
    )
    local active = state.mode == i

    if hovered then
      set_hover(mode.id .. "_card", "rect", mode.title .. " card")
    end
    if hovered and state.click then
      state.mode = i
      state.card_clicks = state.card_clicks + 1
      state.last_action = mode.title .. " selected via hit_test_rect."
      active = true
    end

    local fill_shader = linear_shader(card_rect.x, card_rect.y, card_rect.x + card_rect.w, card_rect.y + card_rect.h, active and {
      0xFF223255,
      0xFF18233D,
    } or hovered and {
      colors.card_hover,
      colors.card_fill,
    } or {
      colors.card_fill,
      0xFF121A2F,
    })

    canvas:draw_rrect(card_rect.x, card_rect.y, card_rect.w, card_rect.h, 22, 22, fill(fill_shader))
    canvas:draw_rrect(
      card_rect.x,
      card_rect.y,
      card_rect.w,
      card_rect.h,
      22,
      22,
      stroke(active and mode.accent or hovered and colors.soft or colors.panel_border, active and 2.5 or 1.5)
    )
    canvas:draw_rrect(card_rect.x + 16, card_rect.y + 18, 8, card_rect.h - 36, 4, 4, fill(mode.accent))
    canvas:draw_text(mode.title, card_rect.x + 38, card_rect.y + 36, fonts.title, fill(colors.text))
    draw_copy(
      canvas,
      "sidebar.card." .. mode.id .. ".summary",
      card_rect.x + 38,
      card_rect.y + 56,
      card_rect.w - 86,
      mode.summary,
      fonts.body,
      colors.muted
    )

    if active then
      draw_chip(canvas, card_rect.x + card_rect.w - 88, card_rect.y + 16, "ACTIVE", mode.accent, colors.navy)
    elseif hovered then
      draw_chip(canvas, card_rect.x + card_rect.w - 70, card_rect.y + 16, "HOT", colors.soft, colors.text)
    end
  end

  local note = cached_paragraph(canvas, "sidebar.note", {
    width = rect.w - 72,
    font = fonts.body,
    color = colors.muted,
    segments = {
      { text = "Click a card here or the command capsule on the stage. Both rely on " },
      { text = "hit_test_rect", color = colors.gold },
      { text = " even when the element is rotated." },
    },
  })
  local note_h = math.max(122, 52 + note.height + 20)
  local note_y = rect.y + rect.h - 42 - note_h
  canvas:draw_rrect(rect.x + 18, note_y, rect.w - 36, note_h, 22, 22, fill(0xFF12192D))
  canvas:draw_rrect(rect.x + 18, note_y, rect.w - 36, note_h, 22, 22, stroke(colors.panel_border, 1.5))
  canvas:draw_text("Rect Targets", rect.x + 36, note_y + 34, fonts.subtitle, fill(colors.gold))
  canvas:draw_paragraph(note.paragraph, rect.x + 36, note_y + 52)
end

local function draw_grid(canvas, rect)
  local cols = 9
  local rows = 6
  for i = 0, cols do
    local x = rect.x + (rect.w / cols) * i
    canvas:draw_rect(x, rect.y, 1, rect.h, fill(0x132C3B61))
  end
  for i = 0, rows do
    local y = rect.y + (rect.h / rows) * i
    canvas:draw_rect(rect.x, y, rect.w, 1, fill(0x132C3B61))
  end
end

local function draw_centered_text(canvas, cx, cy, text, font, color)
  local w = text_width(canvas, text, font)
  canvas:draw_text(text, cx - w * 0.5, baseline_for_center(canvas, cy, font), font, fill(color))
end

local function draw_capsule_target(canvas, cx, cy, accent)
  canvas:save()
  canvas:translate(cx, cy)
  canvas:rotate(-0.18)

  local hovered = canvas:hit_test_rect(-122, -34, 244, 68, state.mouse_x, state.mouse_y)
  if hovered then
    set_hover("launch_capsule", "rect", "launch capsule")
  end
  if hovered and state.click then
    state.capsule_clicks = state.capsule_clicks + 1
    state.last_action = "Launch capsule pressed via transformed hit_test_rect."
  end

  canvas:draw_rrect(
    -122,
    -34,
    244,
    68,
    34,
    34,
    fill(linear_shader(-122, -34, 122, 34, hovered and {
      accent,
      0xFF14203A,
    } or {
      0xFF263656,
      0xFF131B30,
    }))
  )
  canvas:draw_rrect(
    -122,
    -34,
    244,
    68,
    34,
    34,
    stroke(hovered and colors.text or accent, hovered and 3 or 2)
  )
  draw_centered_text(canvas, 0, -6, "ROTATED RECT TARGET", fonts.button, colors.text)
  draw_centered_text(canvas, 0, 18, "click to increment", fonts.label, hovered and colors.text or colors.muted)

  canvas:restore()
end

local function draw_ring_target(canvas, cx, cy, accent)
  local now = luna.now()
  local pulse = 1.0 + 0.045 * math.sin(now * 2.1)

  canvas:save()
  canvas:translate(cx, cy)
  canvas:rotate(now * 0.55)
  canvas:scale(pulse, pulse)

  local hovered = canvas:hit_test_path(shapes.ring, state.mouse_x, state.mouse_y)
  if hovered then
    set_hover("sync_ring", "path", "sync ring")
  end
  if hovered and state.click then
    state.ring_clicks = state.ring_clicks + 1
    state.last_action = "Sync ring charged via hit_test_path; the center hole stays inactive."
  end

  canvas:draw_path(
    shapes.ring,
    fill(linear_shader(-124, -124, 124, 124, hovered and {
      accent,
      0xFF13203D,
    } or {
      0xFF223250,
      0xFF10192C,
    }))
  )
  canvas:draw_path(shapes.ring, stroke(hovered and colors.text or accent, hovered and 4 or 2))
  canvas:draw_path(shapes.spark, fill(hovered and colors.text or accent))

  canvas:restore()

  draw_centered_text(canvas, cx, cy - 8, "PATH TARGET", fonts.button, colors.text)
  draw_centered_text(canvas, cx, cy + 20, "even-odd hole is not clickable", fonts.label, colors.muted)
end

local function draw_stage(canvas, rect)
  local mode = modes[state.mode]

  draw_panel(canvas, rect)
  canvas:draw_text("Interaction Stage", rect.x + 24, rect.y + 40, fonts.title, fill(colors.text))
  draw_copy(
    canvas,
    "stage.intro",
    rect.x + 24,
    rect.y + 62,
    rect.w - 48,
    "Both targets are tested in window coordinates after the same transform stack used for drawing.",
    fonts.body,
    colors.muted
  )

  local stage_box = { x = rect.x + 20, y = rect.y + 132, w = rect.w - 40, h = 408 }
  canvas:draw_rrect(
    stage_box.x,
    stage_box.y,
    stage_box.w,
    stage_box.h,
    24,
    24,
    fill(linear_shader(stage_box.x, stage_box.y, stage_box.x, stage_box.y + stage_box.h, {
      colors.stage_top,
      colors.stage_bottom,
    }))
  )
  canvas:draw_rrect(stage_box.x, stage_box.y, stage_box.w, stage_box.h, 24, 24, stroke(colors.panel_border, 1.5))

  canvas:save()
  canvas:clip_rrect(stage_box.x, stage_box.y, stage_box.w, stage_box.h, 24, 24)
  draw_grid(canvas, stage_box)
  canvas:draw_rrect(stage_box.x + 70, stage_box.y + 54, 240, 240, 120, 120, fill(mode.tone))
  canvas:draw_rrect(stage_box.x + 350, stage_box.y + 220, 260, 180, 90, 90, fill(0x12FFFFFF))
  draw_capsule_target(canvas, stage_box.x + 492, stage_box.y + 122, mode.accent)
  draw_ring_target(canvas, stage_box.x + 232, stage_box.y + 246, mode.accent)
  canvas:restore()

  local metric_y = stage_box.y + stage_box.h + 24
  local metric_w = (rect.w - 56) * 0.5
  local metric_h = 84

  local function draw_metric_box(x, label, value, accent)
    canvas:draw_rrect(x, metric_y, metric_w, metric_h, 20, 20, fill(0xFF11192E))
    canvas:draw_rrect(x, metric_y, metric_w, metric_h, 20, 20, stroke(colors.panel_border, 1.5))
    canvas:draw_text(label, x + 18, metric_y + 30, fonts.label, fill(colors.muted))
    canvas:draw_text(value, x + 18, metric_y + 64, fonts.metric, fill(accent))
  end

  draw_metric_box(rect.x + 20, "Rect activations", tostring(state.card_clicks + state.capsule_clicks), mode.accent)
  draw_metric_box(rect.x + rect.w - metric_w - 20, "Path activations", tostring(state.ring_clicks), colors.gold)

  local note = cached_paragraph(canvas, "stage.note." .. mode.id, {
    width = rect.w - 48,
    font = fonts.body,
    color = colors.muted,
    segments = {
      { text = mode.note .. " " },
      { text = "The ring uses hit_test_path", color = colors.gold },
      { text = " while the capsule uses " },
      { text = "hit_test_rect", color = mode.accent },
      { text = "." },
    },
  })
  canvas:draw_paragraph(note.paragraph, rect.x + 24, metric_y + metric_h + 28)
end

local function draw_info_row(canvas, rect, label, value, accent)
  canvas:draw_rrect(rect.x, rect.y, rect.w, rect.h, 18, 18, fill(0xFF121A2F))
  canvas:draw_rrect(rect.x, rect.y, rect.w, rect.h, 18, 18, stroke(colors.panel_border, 1.5))
  canvas:draw_text(label, rect.x + 18, rect.y + 28, fonts.label, fill(colors.muted))
  canvas:draw_text(value, rect.x + 18, rect.y + 60, fonts.subtitle, fill(accent or colors.text))
end

local function draw_inspector(canvas, rect)
  local mode = modes[state.mode]
  draw_panel(canvas, rect)

  canvas:draw_text("Inspector", rect.x + 24, rect.y + 40, fonts.title, fill(colors.text))
  canvas:draw_text("Current pointer and action state.", rect.x + 24, rect.y + 70, fonts.body, fill(colors.muted))

  local row_x = rect.x + 18
  local row_w = rect.w - 36
  local row_h = 76
  local row_y = rect.y + 106

  draw_info_row(canvas, { x = row_x, y = row_y, w = row_w, h = row_h }, "Hovered target", state.hovered_label, mode.accent)
  draw_info_row(
    canvas,
    { x = row_x, y = row_y + 90, w = row_w, h = row_h },
    "Hit kind",
    state.hovered_kind,
    state.hovered_kind == "path" and colors.gold or mode.accent
  )
  draw_info_row(
    canvas,
    { x = row_x, y = row_y + 180, w = row_w, h = row_h },
    "Pointer",
    string.format("%d, %d", math.floor(state.mouse_x + 0.5), math.floor(state.mouse_y + 0.5)),
    colors.text
  )
  draw_info_row(
    canvas,
    { x = row_x, y = row_y + 270, w = row_w, h = row_h },
    "Mouse button",
    state.mouse_down and "down" or "up",
    state.mouse_down and mode.accent or colors.muted
  )

  local action = cached_paragraph(canvas, "inspector.action." .. state.last_action, {
    width = row_w - 36,
    font = fonts.body,
    color = colors.text,
    segments = {
      { text = state.last_action },
    },
  })
  local action_h = math.max(132, 48 + action.height + 20)
  local action_y = rect.y + rect.h - 18 - action_h

  canvas:draw_rrect(row_x, action_y, row_w, action_h, 22, 22, fill(0xFF11192D))
  canvas:draw_rrect(row_x, action_y, row_w, action_h, 22, 22, stroke(colors.panel_border, 1.5))
  canvas:draw_text("Last action", row_x + 18, action_y + 30, fonts.subtitle, fill(mode.accent))
  canvas:draw_paragraph(action.paragraph, row_x + 18, action_y + 48)
end

local function draw_pointer(canvas)
  local accent = state.hovered_kind == "path" and colors.gold or modes[state.mode].accent
  local x = state.mouse_x
  local y = state.mouse_y
  canvas:draw_rect(x - 1, y - 10, 2, 20, fill(accent))
  canvas:draw_rect(x - 10, y - 1, 20, 2, fill(accent))
  canvas:draw_rrect(x - 4, y - 4, 8, 8, 4, 4, fill(colors.text))
end

local function draw_background(canvas)
  canvas:clear(colors.bg_top)
  canvas:draw_rect(
    0,
    0,
    layout.window_w,
    layout.window_h,
    fill(linear_shader(0, 0, 0, layout.window_h, {
      colors.bg_top,
      colors.bg_bottom,
    }))
  )
  canvas:draw_rrect(-120, 640, 300, 220, 110, 110, fill(0x10FFFFFF))
  canvas:draw_rrect(1190, -70, 260, 220, 110, 110, fill(0x123FD0C7))
  canvas:draw_rrect(980, 710, 360, 240, 120, 120, fill(0x10FF9363))
end

local function draw_scene(canvas)
  state.hovered_id = "background"
  state.hovered_kind = "none"
  state.hovered_label = "background"

  draw_background(canvas)
  draw_header(canvas)
  draw_sidebar(canvas, layout.sidebar)
  draw_stage(canvas, layout.stage)
  draw_inspector(canvas, layout.inspector)
  draw_pointer(canvas)
end

local function await_promise(promise)
  if not promise:poll() then
    luna.wait(promise:event())
  end
  return promise:result()
end

local function pump_events()
  state.click = false

  for _, event in ipairs(luna.input.poll_events()) do
    if event.type == "quit" then
      return false
    end

    if event.type == "mouse_motion" then
      state.mouse_x = event.x
      state.mouse_y = event.y
    elseif event.type == "mouse_button_down" and event.button == "left" then
      state.mouse_x = event.x
      state.mouse_y = event.y
      state.mouse_down = true
      state.click = true
    elseif event.type == "mouse_button_up" and event.button == "left" then
      state.mouse_x = event.x
      state.mouse_y = event.y
      state.mouse_down = false
    end
  end

  return true
end

local function main()
  await_promise(luna.assets:font(luna.fs.game:ref(font_path())))
  fonts = compile_fonts(luna.window)
  shapes = build_shapes(luna.window)
  paragraph_cache = {}

  while true do
    if not pump_events() then
      return
    end

    draw_scene(luna.window)
    luna.next_frame()
  end
end

luna.start(main)
