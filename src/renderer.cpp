#include "renderer.h"

#include <SDL3/SDL_blendmode.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_surface.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>

#include <fmt/format.h>

#include "lua.h"
#include "vfs.h"

namespace luna {

static constexpr const char *LUA_TEXTURE_MT = "luna.Texture";
static constexpr const char *LUA_FONT_MT = "luna.Font";
static constexpr const char *LUA_CANVAS_MT = "luna.Canvas";
static constexpr const char *LUA_STYLE_MT = "luna.Style";
static constexpr const char *RENDERER_PTR_KEY = "luna.renderer_ptr";
static constexpr const char *GFX_HELPERS_CHUNK = "luna.renderer_helpers";

static constexpr const char *GFX_HELPERS_LUA = R"LUA(
return function(canvas_mt)
  local raw_style = assert(canvas_mt._style)
  local raw_rect = assert(canvas_mt._rect)
  local raw_rect_outline = assert(canvas_mt._rect_outline)
  local raw_image = assert(canvas_mt._image)
  local raw_text = assert(canvas_mt._text)
  local raw_begin_layer = assert(canvas_mt._begin_layer)

  local function normalize_align(value, axis)
    if value == nil then
      return nil
    end
    if type(value) == "number" then
      return value
    end
    if type(value) ~= "string" then
      error(string.format("align_%s must be a number or string", axis), 3)
    end

    if axis == "x" then
      if value == "left" then return 0 end
      if value == "center" then return 0.5 end
      if value == "right" then return 1 end
    else
      if value == "top" then return 0 end
      if value == "middle" then return 0.5 end
      if value == "bottom" then return 1 end
    end

    error(string.format("invalid align_%s '%s'", axis, value), 3)
  end
  local raw_push_transform = assert(canvas_mt._push_transform)
  local constants = assert(canvas_mt._constants)
  local blend_factor = assert(constants.blend_factor)
  local blend_op = assert(constants.blend_op)

  local blend_presets = {
    alpha = {
      enabled = true,
      color_op = blend_op.add,
      src_color = blend_factor.src_alpha,
      dst_color = blend_factor.one_minus_src_alpha,
      alpha_op = blend_op.add,
      src_alpha = blend_factor.one,
      dst_alpha = blend_factor.one_minus_src_alpha,
    },
    add = {
      enabled = true,
      color_op = blend_op.add,
      src_color = blend_factor.src_alpha,
      dst_color = blend_factor.one,
      alpha_op = blend_op.add,
      src_alpha = blend_factor.one,
      dst_alpha = blend_factor.one,
    },
    multiply = {
      enabled = true,
      color_op = blend_op.add,
      src_color = blend_factor.dst_color,
      dst_color = blend_factor.one_minus_src_alpha,
      alpha_op = blend_op.add,
      src_alpha = blend_factor.one,
      dst_alpha = blend_factor.one_minus_src_alpha,
    },
    screen = {
      enabled = true,
      color_op = blend_op.add,
      src_color = blend_factor.one,
      dst_color = blend_factor.one_minus_src_color,
      alpha_op = blend_op.add,
      src_alpha = blend_factor.one,
      dst_alpha = blend_factor.one_minus_src_alpha,
    },
    none = {
      enabled = false,
      color_op = blend_op.add,
      src_color = blend_factor.src_alpha,
      dst_color = blend_factor.one_minus_src_alpha,
      alpha_op = blend_op.add,
      src_alpha = blend_factor.one,
      dst_alpha = blend_factor.one_minus_src_alpha,
    },
  }

  local function resolve_blend(blend)
    if blend == nil then
      return nil, nil, nil, nil, nil, nil, nil
    end

    if type(blend) == "string" then
      local preset = blend_presets[blend]
      if not preset then
        error(string.format("invalid blend preset '%s'", blend), 3)
      end
      blend = preset
    elseif type(blend) ~= "table" then
      error("blend must be a preset name or table", 3)
    end

    local function factor(name)
      local value = blend[name]
      if value == nil then
        return nil
      end
      if type(value) == "string" then
        local mapped = blend_factor[value]
        if mapped == nil then
          error(string.format("invalid blend factor '%s' for %s", value, name), 4)
        end
        return mapped
      end
      if type(value) ~= "number" then
        error(string.format("blend factor %s must be a string or number", name), 4)
      end
      return value
    end

    local function op(name)
      local value = blend[name]
      if value == nil then
        return nil
      end
      if type(value) == "string" then
        local mapped = blend_op[value]
        if mapped == nil then
          error(string.format("invalid blend operation '%s' for %s", value, name), 4)
        end
        return mapped
      end
      if type(value) ~= "number" then
        error(string.format("blend operation %s must be a string or number", name), 4)
      end
      return value
    end

    local enabled = blend.enabled
    if enabled ~= nil and type(enabled) ~= "boolean" then
      error("blend.enabled must be a boolean", 3)
    end

    return enabled,
      op("color_op"),
      factor("src_color"),
      factor("dst_color"),
      op("alpha_op"),
      factor("src_alpha"),
      factor("dst_alpha")
  end

  local function compile_style(self, style, level)
    if style == nil or type(style) == "userdata" then
      return style
    end
    if type(style) ~= "table" then
      error("style must be a table or compiled style", level or 3)
    end

    local clip = style.clip
    local clip_enabled = clip ~= nil
    local clip_x, clip_y, clip_w, clip_h
    if clip_enabled then
      if type(clip) ~= "table" then
        error("style.clip must be a table", level or 3)
      end
      clip_x = assert(clip.x, "style.clip.x is required")
      clip_y = assert(clip.y, "style.clip.y is required")
      clip_w = assert(clip.w, "style.clip.w is required")
      clip_h = assert(clip.h, "style.clip.h is required")
    end

    local blend_enabled, color_op, src_color, dst_color, alpha_op, src_alpha, dst_alpha =
      resolve_blend(style.blend)

    return raw_style(
      self,
      style.color,
      style.opacity,
      blend_enabled,
      color_op,
      src_color,
      dst_color,
      alpha_op,
      src_alpha,
      dst_alpha,
      style.rotation,
      style.origin_x,
      style.origin_y,
      style.scale_x,
      style.scale_y,
      normalize_align(style.align_x, "x"),
      normalize_align(style.align_y, "y"),
      clip_enabled,
      clip_x,
      clip_y,
      clip_w,
      clip_h,
      style.wrap_w
    )
  end

  function canvas_mt:style(style)
    return compile_style(self, style, 3)
  end

  function canvas_mt:rect(x, y, w, h, style)
    return raw_rect(self, x, y, w, h, compile_style(self, style, 3))
  end

  function canvas_mt:rect_outline(x, y, w, h, style)
    return raw_rect_outline(self, x, y, w, h, compile_style(self, style, 3))
  end

  function canvas_mt:image(texture, x, y, w, h, style)
    return raw_image(self, texture, x, y, w, h, compile_style(self, style, 3))
  end

  function canvas_mt:text(font, text, x, y, style)
    return raw_text(self, font, text, x, y, compile_style(self, style, 3))
  end

  function canvas_mt:begin_layer(style)
    return raw_begin_layer(self, compile_style(self, style, 3))
  end

  function canvas_mt:push_transform(transform)
    if transform == nil then
      return raw_push_transform(self, 0, 0, 0, 0, 0, 1, 1)
    end
    if type(transform) ~= "table" then
      error("transform must be a table", 2)
    end
    return raw_push_transform(
      self,
      transform.x or 0,
      transform.y or 0,
      transform.rotation or 0,
      transform.origin_x or 0,
      transform.origin_y or 0,
      transform.scale_x or 1,
      transform.scale_y or 1
    )
  end

  function canvas_mt:with_transform(transform, fn)
    if type(fn) ~= "function" then
      error("fn must be a function", 2)
    end

    self:push_transform(transform)
    local ok, result = xpcall(fn, debug.traceback)
    self:pop_transform()
    if not ok then
      error(result, 0)
    end
    return result
  end

  function canvas_mt:layer(style, fn)
    if type(fn) ~= "function" then
      error("fn must be a function", 2)
    end

    self:begin_layer(style)
    local ok, result = xpcall(fn, debug.traceback)
    self:end_layer()
    if not ok then
      error(result, 0)
    end
    return result
  end
end
)LUA";

namespace {

Mat2D Multiply(const Mat2D &lhs, const Mat2D &rhs) {
  return Mat2D{
      lhs.a * rhs.a + lhs.c * rhs.b,
      lhs.b * rhs.a + lhs.d * rhs.b,
      lhs.a * rhs.c + lhs.c * rhs.d,
      lhs.b * rhs.c + lhs.d * rhs.d,
      lhs.a * rhs.tx + lhs.c * rhs.ty + lhs.tx,
      lhs.b * rhs.tx + lhs.d * rhs.ty + lhs.ty,
  };
}

Mat2D Translation(float x, float y) { return Mat2D{1, 0, 0, 1, x, y}; }

Mat2D Scale(float x, float y) { return Mat2D{x, 0, 0, y, 0, 0}; }

Mat2D Rotation(float radians) {
  const float s = std::sin(radians);
  const float c = std::cos(radians);
  return Mat2D{c, s, -s, c, 0, 0};
}

SDL_FPoint TransformPoint(const Mat2D &m, float x, float y) {
  return SDL_FPoint{m.a * x + m.c * y + m.tx, m.b * x + m.d * y + m.ty};
}

Rect IntersectRects(const Rect &a, const Rect &b) {
  const float x1 = std::max(a.x, b.x);
  const float y1 = std::max(a.y, b.y);
  const float x2 = std::min(a.x + a.w, b.x + b.w);
  const float y2 = std::min(a.y + a.h, b.y + b.h);
  if (x2 <= x1 || y2 <= y1) {
    return Rect{x1, y1, 0, 0};
  }
  return Rect{x1, y1, x2 - x1, y2 - y1};
}

BlendState AlphaBlend() { return BlendState{}; }

BlendState AddBlend() {
  BlendState blend{};
  blend.src_color = SDL_BLENDFACTOR_SRC_ALPHA;
  blend.dst_color = SDL_BLENDFACTOR_ONE;
  blend.src_alpha = SDL_BLENDFACTOR_ONE;
  blend.dst_alpha = SDL_BLENDFACTOR_ONE;
  return blend;
}

BlendState MultiplyBlend() {
  BlendState blend{};
  blend.src_color = SDL_BLENDFACTOR_DST_COLOR;
  blend.dst_color = SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  blend.src_alpha = SDL_BLENDFACTOR_ONE;
  blend.dst_alpha = SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  return blend;
}

BlendState ScreenBlend() {
  BlendState blend{};
  blend.src_color = SDL_BLENDFACTOR_ONE;
  blend.dst_color = SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
  blend.src_alpha = SDL_BLENDFACTOR_ONE;
  blend.dst_alpha = SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  return blend;
}

BlendState NoneBlend() {
  BlendState blend{};
  blend.enabled = false;
  return blend;
}

bool IsIdentity(const Mat2D &m) {
  return m.a == 1.0f && m.b == 0.0f && m.c == 0.0f && m.d == 1.0f &&
         m.tx == 0.0f && m.ty == 0.0f;
}

} // namespace

Renderer::Renderer() {
  transform_stack_.push_back(Mat2D{});
  clip_stack_.push_back(ClipRect{});
}

Renderer::LuaTexture *Renderer::CheckLuaTexture(lua_State *L, int idx) {
  return static_cast<LuaTexture *>(luaL_checkudata(L, idx, LUA_TEXTURE_MT));
}

Renderer::LuaFont *Renderer::CheckLuaFont(lua_State *L, int idx) {
  return static_cast<LuaFont *>(luaL_checkudata(L, idx, LUA_FONT_MT));
}

Renderer::LuaCanvas *Renderer::CheckLuaCanvas(lua_State *L, int idx) {
  return static_cast<LuaCanvas *>(luaL_checkudata(L, idx, LUA_CANVAS_MT));
}

Renderer::LuaStyle *Renderer::CheckLuaStyle(lua_State *L, int idx) {
  return static_cast<LuaStyle *>(luaL_checkudata(L, idx, LUA_STYLE_MT));
}

SDL_Color Renderer::ToSdlColor(uint32_t abgr) {
  SDL_Color c;
  c.a = (abgr >> 24) & 0xFF;
  c.b = (abgr >> 16) & 0xFF;
  c.g = (abgr >> 8) & 0xFF;
  c.r = (abgr >> 0) & 0xFF;
  return c;
}

void Renderer::SetLuaGlobals(lua_State *L) {
  lua_pushlightuserdata(L, this);
  lua_setfield(L, LUA_REGISTRYINDEX, RENDERER_PTR_KEY);
}

Renderer *Renderer::GetInstance(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, RENDERER_PTR_KEY);
  auto *renderer = static_cast<Renderer *>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return renderer;
}

bool Renderer::Init() {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
    SDL_Log("SDL_Init failed: %s", SDL_GetError());
    return false;
  }

  if (!TTF_Init()) {
    SDL_Log("TTF_Init failed: %s", SDL_GetError());
    return false;
  }

  window_ = SDL_CreateWindow("Luna", canvas_width_, canvas_height_,
                             SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window_) {
    SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
    return false;
  }

  sdl_ = SDL_CreateRenderer(window_, nullptr);
  if (!sdl_) {
    SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
    return false;
  }

  SDL_SetRenderLogicalPresentation(sdl_, canvas_width_, canvas_height_,
                                   SDL_LOGICAL_PRESENTATION_LETTERBOX);
  return true;
}

void Renderer::Fini() {
  if (sdl_)
    SDL_DestroyRenderer(sdl_);
  if (window_)
    SDL_DestroyWindow(window_);

  TTF_Quit();
  SDL_Quit();
}

void Renderer::PumpSdlEvents() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    SDL_ConvertEventToRenderCoordinates(sdl_, &e);
    switch (e.type) {
    case SDL_EVENT_QUIT:
      polled_events_.push_back(PolledEvent{"quit", std::nullopt, 0, 0});
      break;
    case SDL_EVENT_MOUSE_MOTION:
      polled_events_.push_back(PolledEvent{"mouse_move", std::nullopt,
                                           (int)e.motion.x, (int)e.motion.y});
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      std::optional<std::string> button;
      if (e.button.button == SDL_BUTTON_LEFT) {
        button = "left";
      } else if (e.button.button == SDL_BUTTON_RIGHT) {
        button = "right";
      } else if (e.button.button == SDL_BUTTON_MIDDLE) {
        button = "middle";
      }
      polled_events_.push_back(PolledEvent{
          e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "mouse_down" : "mouse_up",
          button, (int)e.button.x, (int)e.button.y});
      break;
    }
    default:
      break;
    }
  }
}

static CompiledStyle CheckCompiledStyle(lua_State *L, int idx,
                                        const char *func_name) {
  if (idx == 0 || lua_isnoneornil(L, idx)) {
    return CompiledStyle{};
  }
  if (luaL_testudata(L, idx, LUA_STYLE_MT)) {
    struct RawLuaStyle {
      CompiledStyle style;
    };
    auto *style =
        static_cast<RawLuaStyle *>(luaL_checkudata(L, idx, LUA_STYLE_MT));
    return style->style;
  }
  luaL_error(L, "%s: expected compiled style userdata", func_name);
  return CompiledStyle{};
}

static BlendState ParseBlendStateArgs(lua_State *L, int idx) {
  BlendState blend = AlphaBlend();

  if (!lua_isnoneornil(L, idx)) {
    blend.enabled = lua_toboolean(L, idx) != 0;
  }
  if (!lua_isnoneornil(L, idx + 1)) {
    blend.color_op = (SDL_BlendOperation)luaL_checkinteger(L, idx + 1);
  }
  if (!lua_isnoneornil(L, idx + 2)) {
    blend.src_color = (SDL_BlendFactor)luaL_checkinteger(L, idx + 2);
  }
  if (!lua_isnoneornil(L, idx + 3)) {
    blend.dst_color = (SDL_BlendFactor)luaL_checkinteger(L, idx + 3);
  }
  if (!lua_isnoneornil(L, idx + 4)) {
    blend.alpha_op = (SDL_BlendOperation)luaL_checkinteger(L, idx + 4);
  }
  if (!lua_isnoneornil(L, idx + 5)) {
    blend.src_alpha = (SDL_BlendFactor)luaL_checkinteger(L, idx + 5);
  }
  if (!lua_isnoneornil(L, idx + 6)) {
    blend.dst_alpha = (SDL_BlendFactor)luaL_checkinteger(L, idx + 6);
  }

  return blend;
}

static CompiledStyle ParseStyleArgs(lua_State *L, int idx) {
  CompiledStyle style{};

  if (!lua_isnoneornil(L, idx)) {
    style.draw_state.paint.color_abgr = (uint32_t)luaL_checkinteger(L, idx);
  }
  if (!lua_isnoneornil(L, idx + 1)) {
    style.draw_state.paint.opacity = (float)luaL_checknumber(L, idx + 1);
  }

  style.draw_state.paint.blend = ParseBlendStateArgs(L, idx + 2);

  if (!lua_isnoneornil(L, idx + 9)) {
    style.rotation = (float)luaL_checknumber(L, idx + 9);
  }
  if (!lua_isnoneornil(L, idx + 10)) {
    style.origin_x = (float)luaL_checknumber(L, idx + 10);
  }
  if (!lua_isnoneornil(L, idx + 11)) {
    style.origin_y = (float)luaL_checknumber(L, idx + 11);
  }
  if (!lua_isnoneornil(L, idx + 12)) {
    style.scale_x = (float)luaL_checknumber(L, idx + 12);
  }
  if (!lua_isnoneornil(L, idx + 13)) {
    style.scale_y = (float)luaL_checknumber(L, idx + 13);
  }
  if (!lua_isnoneornil(L, idx + 14)) {
    style.align_x = (int)luaL_checkinteger(L, idx + 14);
  }
  if (!lua_isnoneornil(L, idx + 15)) {
    style.align_y = (int)luaL_checkinteger(L, idx + 15);
  }

  if (lua_toboolean(L, idx + 16)) {
    style.draw_state.clip.enabled = true;
    style.draw_state.clip.rect.x = (float)luaL_checknumber(L, idx + 17);
    style.draw_state.clip.rect.y = (float)luaL_checknumber(L, idx + 18);
    style.draw_state.clip.rect.w = (float)luaL_checknumber(L, idx + 19);
    style.draw_state.clip.rect.h = (float)luaL_checknumber(L, idx + 20);
  }

  if (!lua_isnoneornil(L, idx + 21)) {
    style.wrap_width = (int)luaL_checkinteger(L, idx + 21);
  }

  return style;
}

static DrawState ResolveDrawState(const ClipRect &base,
                                  const CompiledStyle &style) {
  DrawState out{};
  out.clip = base;
  out.paint = style.draw_state.paint;
  if (base.enabled && style.draw_state.clip.enabled) {
    out.clip.enabled = true;
    out.clip.rect = IntersectRects(base.rect, style.draw_state.clip.rect);
  } else if (style.draw_state.clip.enabled) {
    out.clip = style.draw_state.clip;
  }
  return out;
}

static Mat2D MakeLocalRectTransform(const Rect &rect,
                                    const CompiledStyle &style) {
  Mat2D out = Translation(rect.x, rect.y);
  if (style.origin_x != 0.0f || style.origin_y != 0.0f) {
    out = Multiply(out, Translation(style.origin_x, style.origin_y));
  }
  if (style.rotation != 0.0f) {
    out = Multiply(out, Rotation(style.rotation));
  }
  if (style.scale_x != 1.0f || style.scale_y != 1.0f) {
    out = Multiply(out, Scale(style.scale_x, style.scale_y));
  }
  if (style.origin_x != 0.0f || style.origin_y != 0.0f) {
    out = Multiply(out, Translation(-style.origin_x, -style.origin_y));
  }
  return out;
}

DrawList &Renderer::ActiveDrawList() {
  if (!layer_stack_.empty()) {
    return *layer_stack_.back().list;
  }
  return draw_list_;
}

const ClipRect &Renderer::CurrentClip() const { return clip_stack_.back(); }

const Mat2D &Renderer::CurrentTransform() const {
  return transform_stack_.back();
}

void Renderer::ClearDrawList(lua_State *L, DrawList &list) {
  for (DrawCmd &cmd : list.cmds) {
    if (std::holds_alternative<DrawCmdImage>(cmd)) {
      auto &image = std::get<DrawCmdImage>(cmd);
      if (image.texture_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, image.texture_ref);
        image.texture_ref = LUA_NOREF;
      }
    } else if (std::holds_alternative<DrawCmdText>(cmd)) {
      auto &text = std::get<DrawCmdText>(cmd);
      if (text.font_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, text.font_ref);
        text.font_ref = LUA_NOREF;
      }
    } else if (std::holds_alternative<DrawCmdLayer>(cmd)) {
      auto &layer = std::get<DrawCmdLayer>(cmd);
      if (layer.list) {
        ClearDrawList(L, *layer.list);
        layer.list.reset();
      }
    }
  }
  list.cmds.clear();
}

static void ApplyClip(SDL_Renderer *renderer, const ClipRect &clip) {
  if (!clip.enabled || clip.rect.w <= 0 || clip.rect.h <= 0) {
    SDL_SetRenderClipRect(renderer, nullptr);
    return;
  }

  SDL_Rect r{(int)std::round(clip.rect.x), (int)std::round(clip.rect.y),
             (int)std::round(clip.rect.w), (int)std::round(clip.rect.h)};
  SDL_SetRenderClipRect(renderer, &r);
}

static SDL_BlendMode ComposeBlendMode(const BlendState &blend) {
  if (!blend.enabled) {
    return SDL_BLENDMODE_NONE;
  }
  return SDL_ComposeCustomBlendMode(blend.src_color, blend.dst_color,
                                    blend.color_op, blend.src_alpha,
                                    blend.dst_alpha, blend.alpha_op);
}

static void RenderGeometryQuad(SDL_Renderer *renderer, SDL_Texture *texture,
                               const Rect &rect, const Mat2D &transform,
                               const DrawState &state) {
  SDL_FColor color;
  SDL_Color c;
  c.a = (state.paint.color_abgr >> 24) & 0xFF;
  c.b = (state.paint.color_abgr >> 16) & 0xFF;
  c.g = (state.paint.color_abgr >> 8) & 0xFF;
  c.r = (state.paint.color_abgr >> 0) & 0xFF;
  color.r = c.r / 255.0f;
  color.g = c.g / 255.0f;
  color.b = c.b / 255.0f;
  color.a = (c.a / 255.0f) * state.paint.opacity;

  std::array<SDL_Vertex, 4> verts{};
  verts[0].position = TransformPoint(transform, rect.x, rect.y);
  verts[1].position = TransformPoint(transform, rect.x + rect.w, rect.y);
  verts[2].position =
      TransformPoint(transform, rect.x + rect.w, rect.y + rect.h);
  verts[3].position = TransformPoint(transform, rect.x, rect.y + rect.h);
  for (auto &v : verts) {
    v.color = color;
  }
  verts[0].tex_coord = SDL_FPoint{0.0f, 0.0f};
  verts[1].tex_coord = SDL_FPoint{1.0f, 0.0f};
  verts[2].tex_coord = SDL_FPoint{1.0f, 1.0f};
  verts[3].tex_coord = SDL_FPoint{0.0f, 1.0f};

  constexpr std::array<int, 6> indices{0, 1, 2, 0, 2, 3};
  SDL_RenderGeometry(renderer, texture, verts.data(), (int)verts.size(),
                     indices.data(), (int)indices.size());
}

void Renderer::RenderDrawList(lua_State *L, const DrawList &list) {
  for (const DrawCmd &cmd : list.cmds) {
    if (std::holds_alternative<DrawCmdRectFilled>(cmd)) {
      const auto &c = std::get<DrawCmdRectFilled>(cmd);
      ApplyClip(sdl_, c.state.clip);
      SDL_SetRenderDrawBlendMode(sdl_, ComposeBlendMode(c.state.paint.blend));
      RenderGeometryQuad(sdl_, nullptr, c.rect, c.transform, c.state);
    } else if (std::holds_alternative<DrawCmdRectOutline>(cmd)) {
      const auto &c = std::get<DrawCmdRectOutline>(cmd);
      ApplyClip(sdl_, c.state.clip);
      SDL_SetRenderDrawBlendMode(sdl_, ComposeBlendMode(c.state.paint.blend));

      const Rect top{c.rect.x, c.rect.y, c.rect.w, 1};
      const Rect bottom{c.rect.x, c.rect.y + c.rect.h - 1, c.rect.w, 1};
      const Rect left{c.rect.x, c.rect.y, 1, c.rect.h};
      const Rect right{c.rect.x + c.rect.w - 1, c.rect.y, 1, c.rect.h};
      RenderGeometryQuad(sdl_, nullptr, top, c.transform, c.state);
      RenderGeometryQuad(sdl_, nullptr, bottom, c.transform, c.state);
      RenderGeometryQuad(sdl_, nullptr, left, c.transform, c.state);
      RenderGeometryQuad(sdl_, nullptr, right, c.transform, c.state);
    } else if (std::holds_alternative<DrawCmdImage>(cmd)) {
      const auto &c = std::get<DrawCmdImage>(cmd);
      ApplyClip(sdl_, c.state.clip);
      lua_rawgeti(L, LUA_REGISTRYINDEX, c.texture_ref);
      auto *t = CheckLuaTexture(L, -1);
      SDL_Texture *tex = t ? t->tex : nullptr;
      lua_pop(L, 1);
      if (!tex) {
        continue;
      }
      SDL_SetTextureBlendMode(tex, ComposeBlendMode(c.state.paint.blend));
      SDL_SetTextureColorMod(
          tex, (Uint8)Renderer::ToSdlColor(c.state.paint.color_abgr).r,
          (Uint8)Renderer::ToSdlColor(c.state.paint.color_abgr).g,
          (Uint8)Renderer::ToSdlColor(c.state.paint.color_abgr).b);
      SDL_SetTextureAlphaMod(
          tex, (Uint8)(Renderer::ToSdlColor(c.state.paint.color_abgr).a *
                       c.state.paint.opacity));
      RenderGeometryQuad(sdl_, tex, c.rect, c.transform, c.state);
    } else if (std::holds_alternative<DrawCmdText>(cmd)) {
      const auto &c = std::get<DrawCmdText>(cmd);
      ApplyClip(sdl_, c.state.clip);
      lua_rawgeti(L, LUA_REGISTRYINDEX, c.font_ref);
      auto *f = CheckLuaFont(L, -1);
      TTF_Font *font = f ? f->font : nullptr;
      lua_pop(L, 1);
      if (!font) {
        continue;
      }

      SDL_Color color = ToSdlColor(c.state.paint.color_abgr);
      SDL_Surface *surface = nullptr;
      if (c.wrap_width > 0) {
        surface = TTF_RenderText_Blended_Wrapped(
            font, c.text.c_str(), c.text.size(), color, c.wrap_width);
      } else {
        surface =
            TTF_RenderText_Blended(font, c.text.c_str(), c.text.size(), color);
      }
      if (!surface) {
        SDL_Log("TTF_RenderText_Blended failed: %s", SDL_GetError());
        continue;
      }

      SDL_Texture *tex = SDL_CreateTextureFromSurface(sdl_, surface);
      SDL_DestroySurface(surface);
      if (!tex) {
        SDL_Log("SDL_CreateTextureFromSurface failed: %s", SDL_GetError());
        continue;
      }

      SDL_SetTextureBlendMode(tex, ComposeBlendMode(c.state.paint.blend));
      SDL_SetTextureAlphaMod(tex, (Uint8)(255.0f * c.state.paint.opacity));
      float w = 0.0f;
      float h = 0.0f;
      SDL_GetTextureSize(tex, &w, &h);
      Mat2D text_transform = c.transform;

      text_transform.tx -= w * c.align_x;
      text_transform.ty -= h * c.align_y;
      RenderGeometryQuad(sdl_, tex, Rect{0, 0, w, h}, text_transform, c.state);
      SDL_DestroyTexture(tex);
    } else if (std::holds_alternative<DrawCmdLayer>(cmd)) {
      const auto &c = std::get<DrawCmdLayer>(cmd);
      SDL_Texture *layer_tex = RenderLayerToTexture(L, *c.list);
      if (!layer_tex) {
        continue;
      }

      float w = 0.0f;
      float h = 0.0f;
      SDL_GetTextureSize(layer_tex, &w, &h);
      DrawState state = c.state;
      ApplyClip(sdl_, state.clip);
      SDL_SetTextureBlendMode(layer_tex, ComposeBlendMode(state.paint.blend));
      RenderGeometryQuad(sdl_, layer_tex, Rect{0, 0, w, h}, Mat2D{}, state);
      SDL_DestroyTexture(layer_tex);
    }
  }

  SDL_SetRenderClipRect(sdl_, nullptr);
}

SDL_Texture *Renderer::RenderLayerToTexture(lua_State *L,
                                            const DrawList &list) {
  int width = canvas_width_;
  int height = canvas_height_;
  SDL_Texture *target = SDL_CreateTexture(
      sdl_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, width, height);
  if (!target) {
    SDL_Log("SDL_CreateTexture failed for layer: %s", SDL_GetError());
    return nullptr;
  }

  SDL_SetTextureBlendMode(target, SDL_BLENDMODE_BLEND);
  SDL_Texture *prev = SDL_GetRenderTarget(sdl_);
  SDL_SetRenderTarget(sdl_, target);
  SDL_SetRenderDrawBlendMode(sdl_, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(sdl_, 0, 0, 0, 0);
  SDL_RenderClear(sdl_);
  RenderDrawList(L, list);
  SDL_SetRenderTarget(sdl_, prev);
  return target;
}

void Renderer::RegisterBindings(lua_State *L) {
  SetLuaGlobals(L);

  if (luaL_newmetatable(L, LUA_TEXTURE_MT)) {
    lua_pushcfunction(L, &L_TextureDestroy);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, &L_TextureDestroy);
    lua_setfield(L, -2, "destroy");

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
  }
  lua_pop(L, 1);

  if (luaL_newmetatable(L, LUA_FONT_MT)) {
    lua_pushcfunction(L, &L_FontDestroy);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, &L_FontDestroy);
    lua_setfield(L, -2, "destroy");

    lua_pushcfunction(L, &L_FontMeasure);
    lua_setfield(L, -2, "measure");

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
  }
  lua_pop(L, 1);

  if (luaL_newmetatable(L, LUA_STYLE_MT)) {
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
  }
  lua_pop(L, 1);

  if (luaL_newmetatable(L, LUA_CANVAS_MT)) {
    lua_pushcfunction(L, &L_GfxSetCanvasSize);
    lua_setfield(L, -2, "set_canvas_size");

    lua_pushcfunction(L, &L_GfxClear);
    lua_setfield(L, -2, "clear");

    lua_pushcfunction(L, &L_GfxPresent);
    lua_setfield(L, -2, "present");

    lua_pushcfunction(L, &L_GfxRect);
    lua_setfield(L, -2, "_rect");

    lua_pushcfunction(L, &L_GfxRectOutline);
    lua_setfield(L, -2, "_rect_outline");

    lua_pushcfunction(L, &L_GfxImage);
    lua_setfield(L, -2, "_image");

    lua_pushcfunction(L, &L_GfxText);
    lua_setfield(L, -2, "_text");

    lua_pushcfunction(L, &L_GfxStyle);
    lua_setfield(L, -2, "_style");

    lua_pushcfunction(L, &L_GfxPushTransform);
    lua_setfield(L, -2, "_push_transform");

    lua_pushcfunction(L, &L_GfxPopTransform);
    lua_setfield(L, -2, "pop_transform");

    lua_pushcfunction(L, &L_GfxTranslate);
    lua_setfield(L, -2, "translate");

    lua_pushcfunction(L, &L_GfxScale);
    lua_setfield(L, -2, "scale");

    lua_pushcfunction(L, &L_GfxRotate);
    lua_setfield(L, -2, "rotate");

    lua_pushcfunction(L, &L_GfxPushClip);
    lua_setfield(L, -2, "push_clip");

    lua_pushcfunction(L, &L_GfxPopClip);
    lua_setfield(L, -2, "pop_clip");

    lua_pushcfunction(L, &L_GfxBeginLayer);
    lua_setfield(L, -2, "_begin_layer");

    lua_pushcfunction(L, &L_GfxEndLayer);
    lua_setfield(L, -2, "end_layer");

    lua_newtable(L);

    lua_newtable(L);
    lua_pushinteger(L, SDL_BLENDFACTOR_ZERO);
    lua_setfield(L, -2, "zero");
    lua_pushinteger(L, SDL_BLENDFACTOR_ONE);
    lua_setfield(L, -2, "one");
    lua_pushinteger(L, SDL_BLENDFACTOR_SRC_COLOR);
    lua_setfield(L, -2, "src_color");
    lua_pushinteger(L, SDL_BLENDFACTOR_ONE_MINUS_SRC_COLOR);
    lua_setfield(L, -2, "one_minus_src_color");
    lua_pushinteger(L, SDL_BLENDFACTOR_SRC_ALPHA);
    lua_setfield(L, -2, "src_alpha");
    lua_pushinteger(L, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA);
    lua_setfield(L, -2, "one_minus_src_alpha");
    lua_pushinteger(L, SDL_BLENDFACTOR_DST_COLOR);
    lua_setfield(L, -2, "dst_color");
    lua_pushinteger(L, SDL_BLENDFACTOR_ONE_MINUS_DST_COLOR);
    lua_setfield(L, -2, "one_minus_dst_color");
    lua_pushinteger(L, SDL_BLENDFACTOR_DST_ALPHA);
    lua_setfield(L, -2, "dst_alpha");
    lua_pushinteger(L, SDL_BLENDFACTOR_ONE_MINUS_DST_ALPHA);
    lua_setfield(L, -2, "one_minus_dst_alpha");
    lua_setfield(L, -2, "blend_factor");

    lua_newtable(L);
    lua_pushinteger(L, SDL_BLENDOPERATION_ADD);
    lua_setfield(L, -2, "add");
    lua_pushinteger(L, SDL_BLENDOPERATION_SUBTRACT);
    lua_setfield(L, -2, "subtract");
    lua_pushinteger(L, SDL_BLENDOPERATION_REV_SUBTRACT);
    lua_setfield(L, -2, "reverse_subtract");
    lua_pushinteger(L, SDL_BLENDOPERATION_MINIMUM);
    lua_setfield(L, -2, "min");
    lua_pushinteger(L, SDL_BLENDOPERATION_MAXIMUM);
    lua_setfield(L, -2, "max");
    lua_setfield(L, -2, "blend_op");

    lua_setfield(L, -2, "_constants");

    if (luaL_loadbuffer(L, GFX_HELPERS_LUA, std::strlen(GFX_HELPERS_LUA),
                        GFX_HELPERS_CHUNK) != LUA_OK) {
      lua_error(L);
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
      lua_error(L);
    }
    lua_pushvalue(L, -2);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
      lua_error(L);
    }

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
  }
  lua_pop(L, 1);

  lua_pushcfunction(L, &L_PollSdlEvents);
  lua_setfield(L, -2, "poll_events");

  auto *canvas =
      static_cast<LuaCanvas *>(lua_newuserdata(L, sizeof(LuaCanvas)));
  *canvas = LuaCanvas{this};
  luaL_getmetatable(L, LUA_CANVAS_MT);
  lua_setmetatable(L, -2);
  lua_setfield(L, -2, "gfx");
}

void Renderer::LoadImageJob::Invoke(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!path || !*path) {
    luaL_error(L, "load_texture: path is empty");
  }
  path_ = path;

  auto *vfs = VFS::GetInstance(L);
  io_ = vfs->OpenFile(path_);
  if (!io_) {
    luaL_error(L, "Failed to open %s", path);
  }
}

void Renderer::LoadImageJob::Run() {
  surface_ = IMG_Load_IO(io_, 1);
  if (!surface_) {
    error_ =
        fmt::format("IMG_Load_IO failed for {}: {}", path_, SDL_GetError());
  }
  io_ = nullptr;
}

int Renderer::LoadImageJob::Finish(lua_State *L) {
  auto *renderer = Renderer::GetInstance(L)->sdl_;

  SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surface_);
  float w = 0.0f;
  float h = 0.0f;
  if (tex) {
    SDL_GetTextureSize(tex, &w, &h);
  }

  SDL_DestroySurface(surface_);
  surface_ = nullptr;
  if (!tex) {
    error_ =
        fmt::format("SDL_CreateTextureFromSurface failed: {}", SDL_GetError());
    return 0;
  }

  auto *ud = static_cast<LuaTexture *>(lua_newuserdata(L, sizeof(LuaTexture)));
  *ud = LuaTexture{tex};
  luaL_getmetatable(L, LUA_TEXTURE_MT);
  lua_setmetatable(L, -2);

  lua_pushnumber(L, w);
  lua_pushnumber(L, h);

  return 3;
}

Renderer::LoadImageJob::~LoadImageJob() {
  if (surface_) {
    SDL_DestroySurface(surface_);
  }
  if (io_) {
    SDL_CloseIO(io_);
  }
}

void Renderer::LoadFontJob::Invoke(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  const float size_pt = (float)luaL_checknumber(L, 2);

  if (!path || !*path)
    luaL_error(L, "load_font: path is empty");
  if (size_pt <= 0)
    luaL_error(L, "load_font: size must be > 0");

  path_ = path;
  size_pt_ = size_pt;
}

void Renderer::LoadFontJob::Run() {}

int Renderer::LoadFontJob::Finish(lua_State *L) {
  auto *vfs = VFS::GetInstance(L);
  SDL_IOStream *io = vfs->OpenFile(path_);
  if (!io) {
    return luaL_error(L, "Failed to open font: %s", path_.c_str());
  }

  TTF_Font *font = TTF_OpenFontIO(io, 1, size_pt_);
  if (!font) {
    return luaL_error(L, "TTF_OpenFontIO failed: %s", SDL_GetError());
  }

  auto *ud = static_cast<LuaFont *>(lua_newuserdata(L, sizeof(LuaFont)));
  *ud = LuaFont{font};
  luaL_getmetatable(L, LUA_FONT_MT);
  lua_setmetatable(L, -2);
  return 1;
}

int Renderer::L_PollSdlEvents(lua_State *L) {
  Renderer *r = GetInstance(L);
  lua_newtable(L);
  int out_i = 1;
  for (const PolledEvent &ev : r->polled_events_) {
    lua_newtable(L);
    lua_pushstring(L, ev.type.c_str());
    lua_setfield(L, -2, "type");
    if (ev.button.has_value()) {
      lua_pushstring(L, ev.button->c_str());
      lua_setfield(L, -2, "button");
    }
    lua_pushinteger(L, ev.x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, ev.y);
    lua_setfield(L, -2, "y");
    lua_rawseti(L, -2, out_i++);
  }
  r->polled_events_.clear();
  return 1;
}

int Renderer::L_TextureDestroy(lua_State *L) {
  auto *t = CheckLuaTexture(L, 1);
  if (t->tex) {
    SDL_DestroyTexture(t->tex);
    t->tex = nullptr;
  }
  return 0;
}

int Renderer::L_FontDestroy(lua_State *L) {
  auto *f = CheckLuaFont(L, 1);
  if (f->font) {
    TTF_CloseFont(f->font);
    f->font = nullptr;
  }
  return 0;
}

int Renderer::L_FontMeasure(lua_State *L) {
  auto *font = CheckLuaFont(L, 1);
  const char *text = luaL_optstring(L, 2, "");
  const int wrap_width = (int)luaL_optinteger(L, 3, 0);

  if (!font->font) {
    return luaL_error(L, "font:measure: invalid font handle");
  }

  int w = 0;
  int h = 0;
  const size_t len = text ? std::strlen(text) : 0;
  const bool ok =
      wrap_width > 0
          ? TTF_GetStringSizeWrapped(font->font, text ? text : "", len,
                                     wrap_width, &w, &h)
          : TTF_GetStringSize(font->font, text ? text : "", len, &w, &h);
  if (!ok) {
    return luaL_error(L, "font:measure failed: %s", SDL_GetError());
  }

  lua_pushinteger(L, w);
  lua_pushinteger(L, h);
  return 2;
}

int Renderer::L_GfxSetCanvasSize(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  const int w = (int)luaL_checkinteger(L, 2);
  const int h = (int)luaL_checkinteger(L, 3);
  if (w <= 0 || h <= 0) {
    return luaL_error(L, "gfx:set_canvas_size: dimensions must be > 0");
  }
  r->canvas_width_ = w;
  r->canvas_height_ = h;
  SDL_SetRenderLogicalPresentation(r->sdl_, w, h,
                                   SDL_LOGICAL_PRESENTATION_LETTERBOX);
  SDL_SetWindowSize(r->window_, w, h);
  return 0;
}

int Renderer::L_GfxClear(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  r->clear_color_abgr_ = (uint32_t)luaL_checkinteger(L, 2);
  return 0;
}

int Renderer::L_GfxPresent(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;

  while (!r->layer_stack_.empty()) {
    LayerBuildState layer = std::move(r->layer_stack_.back());
    r->layer_stack_.pop_back();
    r->ActiveDrawList().cmds.push_back(DrawCmdLayer{layer.list, layer.state});
  }

  SDL_Color clear = ToSdlColor(r->clear_color_abgr_);
  SDL_SetRenderDrawColor(r->sdl_, clear.r, clear.g, clear.b, clear.a);
  SDL_SetRenderDrawBlendMode(r->sdl_, SDL_BLENDMODE_BLEND);
  SDL_RenderClear(r->sdl_);
  r->RenderDrawList(L, r->draw_list_);
  SDL_RenderPresent(r->sdl_);
  r->ClearDrawList(L, r->draw_list_);
  r->transform_stack_.clear();
  r->transform_stack_.push_back(Mat2D{});
  r->clip_stack_.clear();
  r->clip_stack_.push_back(ClipRect{});
  return 0;
}

int Renderer::L_GfxRect(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  const float x = (float)luaL_checknumber(L, 2);
  const float y = (float)luaL_checknumber(L, 3);
  const float w = (float)luaL_checknumber(L, 4);
  const float h = (float)luaL_checknumber(L, 5);
  const CompiledStyle style = CheckCompiledStyle(L, 6, "gfx:rect");
  const Rect rect{x, y, w, h};
  const Mat2D transform =
      Multiply(r->CurrentTransform(), MakeLocalRectTransform(rect, style));
  const DrawState state = ResolveDrawState(r->CurrentClip(), style);
  r->ActiveDrawList().cmds.push_back(
      DrawCmdRectFilled{Rect{0, 0, w, h}, transform, state});
  return 0;
}

int Renderer::L_GfxRectOutline(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  const float x = (float)luaL_checknumber(L, 2);
  const float y = (float)luaL_checknumber(L, 3);
  const float w = (float)luaL_checknumber(L, 4);
  const float h = (float)luaL_checknumber(L, 5);
  const CompiledStyle style = CheckCompiledStyle(L, 6, "gfx:rect_outline");
  const Rect rect{x, y, w, h};
  const Mat2D transform =
      Multiply(r->CurrentTransform(), MakeLocalRectTransform(rect, style));
  const DrawState state = ResolveDrawState(r->CurrentClip(), style);
  r->ActiveDrawList().cmds.push_back(
      DrawCmdRectOutline{Rect{0, 0, w, h}, transform, state});
  return 0;
}

int Renderer::L_GfxImage(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  luaL_checktype(L, 2, LUA_TUSERDATA);
  const float x = (float)luaL_checknumber(L, 3);
  const float y = (float)luaL_checknumber(L, 4);
  const float w = (float)luaL_checknumber(L, 5);
  const float h = (float)luaL_checknumber(L, 6);
  const CompiledStyle style = CheckCompiledStyle(L, 7, "gfx:image");

  lua_pushvalue(L, 2);
  const int ref = luaL_ref(L, LUA_REGISTRYINDEX);

  const Rect rect{x, y, w, h};
  const Mat2D transform =
      Multiply(r->CurrentTransform(), MakeLocalRectTransform(rect, style));
  const DrawState state = ResolveDrawState(r->CurrentClip(), style);
  r->ActiveDrawList().cmds.push_back(
      DrawCmdImage{ref, Rect{0, 0, w, h}, transform, state});
  return 0;
}

int Renderer::L_GfxText(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  auto *font = CheckLuaFont(L, 2);
  const char *text = luaL_checkstring(L, 3);
  const float x = (float)luaL_checknumber(L, 4);
  const float y = (float)luaL_checknumber(L, 5);
  const CompiledStyle style = CheckCompiledStyle(L, 6, "gfx:text");

  if (!font->font) {
    return luaL_error(L, "gfx:text: invalid font handle");
  }

  lua_pushvalue(L, 2);
  const int ref = luaL_ref(L, LUA_REGISTRYINDEX);

  const Rect rect{x, y, 0, 0};
  const Mat2D transform =
      Multiply(r->CurrentTransform(), MakeLocalRectTransform(rect, style));
  const DrawState state = ResolveDrawState(r->CurrentClip(), style);
  r->ActiveDrawList().cmds.push_back(
      DrawCmdText{ref, text ? text : "", transform, state, style.align_x,
                  style.align_y, style.wrap_width});
  return 0;
}

int Renderer::L_GfxStyle(lua_State *L) {
  CheckLuaCanvas(L, 1);
  auto *style = static_cast<LuaStyle *>(lua_newuserdata(L, sizeof(LuaStyle)));
  style->style = ParseStyleArgs(L, 2);
  luaL_getmetatable(L, LUA_STYLE_MT);
  lua_setmetatable(L, -2);
  return 1;
}

int Renderer::L_GfxPushTransform(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  Mat2D top = r->CurrentTransform();
  const float x = (float)luaL_optnumber(L, 2, 0.0);
  const float y = (float)luaL_optnumber(L, 3, 0.0);
  const float rotation = (float)luaL_optnumber(L, 4, 0.0);
  const float origin_x = (float)luaL_optnumber(L, 5, 0.0);
  const float origin_y = (float)luaL_optnumber(L, 6, 0.0);
  const float scale_x = (float)luaL_optnumber(L, 7, 1.0);
  const float scale_y = (float)luaL_optnumber(L, 8, 1.0);

  Mat2D next = Multiply(top, Translation(x, y));
  if (origin_x != 0.0f || origin_y != 0.0f) {
    next = Multiply(next, Translation(origin_x, origin_y));
  }
  if (rotation != 0.0f) {
    next = Multiply(next, Rotation(rotation));
  }
  if (scale_x != 1.0f || scale_y != 1.0f) {
    next = Multiply(next, Scale(scale_x, scale_y));
  }
  if (origin_x != 0.0f || origin_y != 0.0f) {
    next = Multiply(next, Translation(-origin_x, -origin_y));
  }

  r->transform_stack_.push_back(next);
  return 0;
}

int Renderer::L_GfxPopTransform(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  if (r->transform_stack_.size() <= 1) {
    return luaL_error(L, "gfx:pop_transform: transform stack underflow");
  }
  r->transform_stack_.pop_back();
  return 0;
}

int Renderer::L_GfxTranslate(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  const float x = (float)luaL_checknumber(L, 2);
  const float y = (float)luaL_checknumber(L, 3);
  r->transform_stack_.back() =
      Multiply(r->transform_stack_.back(), Translation(x, y));
  return 0;
}

int Renderer::L_GfxScale(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  const float x = (float)luaL_checknumber(L, 2);
  const float y = (float)luaL_optnumber(L, 3, x);
  r->transform_stack_.back() =
      Multiply(r->transform_stack_.back(), Scale(x, y));
  return 0;
}

int Renderer::L_GfxRotate(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  const float radians = (float)luaL_checknumber(L, 2);
  r->transform_stack_.back() =
      Multiply(r->transform_stack_.back(), Rotation(radians));
  return 0;
}

int Renderer::L_GfxPushClip(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  Rect rect{(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3),
            (float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5)};
  ClipRect clip{true, rect};
  if (r->CurrentClip().enabled) {
    clip.rect = IntersectRects(r->CurrentClip().rect, rect);
  }
  r->clip_stack_.push_back(clip);
  return 0;
}

int Renderer::L_GfxPopClip(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  if (r->clip_stack_.size() <= 1) {
    return luaL_error(L, "gfx:pop_clip: clip stack underflow");
  }
  r->clip_stack_.pop_back();
  return 0;
}

int Renderer::L_GfxBeginLayer(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  const CompiledStyle style = CheckCompiledStyle(L, 2, "gfx:begin_layer");
  r->layer_stack_.push_back(
      LayerBuildState{std::make_shared<DrawList>(), style.draw_state});
  return 0;
}

int Renderer::L_GfxEndLayer(lua_State *L) {
  Renderer *r = CheckLuaCanvas(L, 1)->renderer;
  if (r->layer_stack_.empty()) {
    return luaL_error(L, "gfx:end_layer: layer stack underflow");
  }
  LayerBuildState layer = std::move(r->layer_stack_.back());
  r->layer_stack_.pop_back();
  r->ActiveDrawList().cmds.push_back(DrawCmdLayer{layer.list, layer.state});
  return 0;
}

} // namespace luna
