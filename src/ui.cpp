#include "ui.h"

#include <cstdint>

#include <SDL3/SDL.h>

namespace luna {

static constexpr const char *UI_CONTEXT_MT = "luna.UiContext";

Ui::UiContext *Ui::CheckCtx(lua_State *L) {
  return static_cast<Ui::UiContext *>(luaL_checkudata(L, 1, UI_CONTEXT_MT));
}

void Ui::SetDrawCallback(lua_State *L, int fn_index) {
  luaL_checktype(L, fn_index, LUA_TFUNCTION);
  lua_pushvalue(L, fn_index);
  if (draw_fn_ref_ != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, draw_fn_ref_);
  }
  draw_fn_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
}

void Ui::ClearDrawCallback(lua_State *L) {
  if (draw_fn_ref_ != LUA_NOREF) {
    luaL_unref(L, LUA_REGISTRYINDEX, draw_fn_ref_);
    draw_fn_ref_ = LUA_NOREF;
  }
}

bool Ui::OnMouseMove(float x, float y) {
  mouse_x_ = x;
  mouse_y_ = y;
  hovered_prev_id_ = HitTestPrevRegionId(x, y);
  return (active_id_ != 0) || (hovered_prev_id_ != 0);
}

bool Ui::OnMouseButtonDown(int button) {
  hovered_prev_id_ = HitTestPrevRegionId(mouse_x_, mouse_y_);
  if (button == SDL_BUTTON_LEFT) {
    if (!mouse_down_) {
      mouse_pressed_ = true;
      press_candidate_id_ = hovered_prev_id_;
    }
    mouse_down_ = true;
  }
  return (active_id_ != 0) || (hovered_prev_id_ != 0);
}

bool Ui::OnMouseButtonUp(int button) {
  hovered_prev_id_ = HitTestPrevRegionId(mouse_x_, mouse_y_);
  if (button == SDL_BUTTON_LEFT) {
    if (mouse_down_) {
      mouse_released_ = true;
    }
    mouse_down_ = false;
  }
  return (active_id_ != 0) || (hovered_prev_id_ != 0);
}

bool Ui::WantsMouseCapture() const {
  return (active_id_ != 0) || (hovered_prev_id_ != 0);
}

void Ui::BeginFrame() {
  per_frame_call_index_ = 0;
  id_stack_hash_ = 1469598103934665603ULL;
  hot_id_ = 0;
  frame_hit_regions_.clear();
}

void Ui::EndFrame() {
  prev_hit_regions_ = frame_hit_regions_;
  mouse_pressed_ = false;
  mouse_released_ = false;
  press_candidate_id_ = 0;
}

uint64_t Ui::HitTestPrevRegionId(float x, float y) const {
  for (size_t i = prev_hit_regions_.size(); i > 0; --i) {
    const HitRegion &region = prev_hit_regions_[i - 1];
    const Rect &r = region.rect;
    if (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) {
      return region.id;
    }
  }
  return 0;
}

uint64_t Ui::MakeWidgetId(const std::string &label,
                          const std::optional<std::string> &explicit_id) {
  auto fnv1a = [](uint64_t h, const void *data, size_t n) {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; ++i) {
      h ^= p[i];
      h *= 1099511628211ULL;
    }
    return h;
  };

  uint64_t h = id_stack_hash_;
  const std::string &key = explicit_id ? *explicit_id : label;
  h = fnv1a(h, key.data(), key.size());
  h = fnv1a(h, &per_frame_call_index_, sizeof(per_frame_call_index_));
  ++per_frame_call_index_;
  return h;
}

void Ui::Draw(lua_State *L, DrawList *out) {
  BeginFrame();

  if (draw_fn_ref_ == LUA_NOREF) {
    EndFrame();
    return;
  }

  lua_rawgeti(L, LUA_REGISTRYINDEX, draw_fn_ref_);

  UiContext *ctx =
      static_cast<UiContext *>(lua_newuserdata(L, sizeof(UiContext)));
  *ctx = UiContext{this, out};

  luaL_getmetatable(L, UI_CONTEXT_MT);
  lua_setmetatable(L, -2);

  if (lua_pcall(L, 1, 0, 0) != 0) {
    const char *err = lua_tostring(L, -1);
    SDL_Log("UI draw error: %s", err ? err : "(unknown)");
    lua_pop(L, 1);
  }

  EndFrame();
}

int Ui::L_ScreenSize(lua_State *L) {
  auto *ctx = CheckCtx(L);
  (void)ctx;
  lua_pushinteger(L, 1280);
  lua_pushinteger(L, 720);
  return 2;
}

int Ui::L_Rect(lua_State *L) {
  auto *ctx = CheckCtx(L);
  const float x = (float)luaL_checknumber(L, 2);
  const float y = (float)luaL_checknumber(L, 3);
  const float w = (float)luaL_checknumber(L, 4);
  const float h = (float)luaL_checknumber(L, 5);
  const uint32_t abgr = (uint32_t)luaL_optinteger(L, 6, 0xFFFFFFFF);

  ctx->draw_list->cmds.push_back(DrawCmdRectFilled{Rect{x, y, w, h}, abgr});
  return 0;
}

int Ui::L_RectOutline(lua_State *L) {
  auto *ctx = CheckCtx(L);
  const float x = (float)luaL_checknumber(L, 2);
  const float y = (float)luaL_checknumber(L, 3);
  const float w = (float)luaL_checknumber(L, 4);
  const float h = (float)luaL_checknumber(L, 5);
  const uint32_t abgr = (uint32_t)luaL_optinteger(L, 6, 0xFFFFFFFF);

  ctx->draw_list->cmds.push_back(DrawCmdRect{Rect{x, y, w, h}, abgr});
  return 0;
}

int Ui::L_Image(lua_State *L) {
  auto *ctx = CheckCtx(L);
  luaL_checktype(L, 2, LUA_TUSERDATA);

  const float x = (float)luaL_checknumber(L, 3);
  const float y = (float)luaL_checknumber(L, 4);
  const float w = (float)luaL_checknumber(L, 5);
  const float h = (float)luaL_checknumber(L, 6);

  lua_pushvalue(L, 2);
  const int ref = luaL_ref(L, LUA_REGISTRYINDEX);

  ctx->draw_list->cmds.push_back(DrawCmdImage{ref, Rect{x, y, w, h}});
  return 0;
}

static bool PointInRect(float mx, float my, const Rect &r) {
  return mx >= r.x && my >= r.y && mx < r.x + r.w && my < r.y + r.h;
}

int Ui::L_Button(lua_State *L) {
  auto *ctx = CheckCtx(L);
  const char *label = luaL_checkstring(L, 2);
  const float x = (float)luaL_checknumber(L, 3);
  const float y = (float)luaL_checknumber(L, 4);
  const float w = (float)luaL_checknumber(L, 5);
  const float h = (float)luaL_checknumber(L, 6);

  std::optional<std::string> explicit_id;
  if (lua_istable(L, 7)) {
    lua_getfield(L, 7, "id");
    if (lua_isstring(L, -1))
      explicit_id = lua_tostring(L, -1);
    lua_pop(L, 1);
  }

  const std::string lbl = label ? label : "";
  const uint64_t id = ctx->ui->MakeWidgetId(lbl, explicit_id);

  const Rect r{x, y, w, h};
  ctx->ui->frame_hit_regions_.push_back(Ui::HitRegion{id, r});

  const bool hovered = PointInRect(ctx->ui->mouse_x_, ctx->ui->mouse_y_, r);
  if (hovered) {
    ctx->ui->hot_id_ = id;
  }

  bool pressed = false;
  if (ctx->ui->mouse_pressed_ && ctx->ui->press_candidate_id_ == id) {
    ctx->ui->active_id_ = id;
    pressed = true;
  }

  const bool held = (ctx->ui->active_id_ == id) && ctx->ui->mouse_down_;

  bool released = false;
  bool clicked = false;
  if (ctx->ui->active_id_ == id && ctx->ui->mouse_released_) {
    released = true;
    clicked = hovered;
    ctx->ui->active_id_ = 0;
  }

  lua_createtable(L, 0, 6);

  lua_pushboolean(L, hovered ? 1 : 0);
  lua_setfield(L, -2, "hovered");

  lua_pushboolean(L, held ? 1 : 0);
  lua_setfield(L, -2, "held");

  lua_pushboolean(L, pressed ? 1 : 0);
  lua_setfield(L, -2, "pressed");

  lua_pushboolean(L, released ? 1 : 0);
  lua_setfield(L, -2, "released");

  lua_pushboolean(L, clicked ? 1 : 0);
  lua_setfield(L, -2, "clicked");

  lua_pushinteger(L, (lua_Integer)id);
  lua_setfield(L, -2, "id");

  return 1;
}

void Ui::RegisterUiBindings(lua_State *L) {
  if (luaL_newmetatable(L, UI_CONTEXT_MT)) {
    lua_pushcfunction(L, &Ui::L_ScreenSize);
    lua_setfield(L, -2, "screen_size");

    lua_pushcfunction(L, &Ui::L_Rect);
    lua_setfield(L, -2, "rect");

    lua_pushcfunction(L, &Ui::L_RectOutline);
    lua_setfield(L, -2, "rect_outline");

    lua_pushcfunction(L, &Ui::L_Image);
    lua_setfield(L, -2, "image");

    lua_pushcfunction(L, &Ui::L_Button);
    lua_setfield(L, -2, "button");

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
  }
  lua_pop(L, 1);
}

} // namespace luna
