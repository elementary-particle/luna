#ifndef LUNA_RENDERER_H
#define LUNA_RENDERER_H

#include "lua.hpp"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "factory.h"

namespace luna {

struct Rect {
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
};

struct Mat2D {
  float a = 1.0f;
  float b = 0.0f;
  float c = 0.0f;
  float d = 1.0f;
  float tx = 0.0f;
  float ty = 0.0f;
};

struct BlendState {
  bool enabled = true;
  SDL_BlendOperation color_op = SDL_BLENDOPERATION_ADD;
  SDL_BlendFactor src_color = SDL_BLENDFACTOR_SRC_ALPHA;
  SDL_BlendFactor dst_color = SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
  SDL_BlendOperation alpha_op = SDL_BLENDOPERATION_ADD;
  SDL_BlendFactor src_alpha = SDL_BLENDFACTOR_ONE;
  SDL_BlendFactor dst_alpha = SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
};

struct Paint {
  uint32_t color_abgr = 0xFFFFFFFFu;
  float opacity = 1.0f;
  BlendState blend{};
};

struct ClipRect {
  bool enabled = false;
  Rect rect{};
};

struct DrawState {
  Paint paint{};
  ClipRect clip{};
};

struct CompiledStyle {
  DrawState draw_state{};
  float rotation = 0.0f;
  float origin_x = 0.0f;
  float origin_y = 0.0f;
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  float align_x = 0.0f;
  float align_y = 0.0f;
  int wrap_width = 0;
};

struct DrawCmdRectFilled {
  Rect rect{};
  Mat2D transform{};
  DrawState state{};
};

struct DrawCmdRectOutline {
  Rect rect{};
  Mat2D transform{};
  DrawState state{};
};

struct DrawCmdImage {
  int texture_ref = LUA_NOREF;
  Rect rect{};
  Mat2D transform{};
  DrawState state{};
};

struct DrawCmdText {
  int font_ref = LUA_NOREF;
  std::string text;
  Mat2D transform{};
  DrawState state{};
  float align_x = 0.0f;
  float align_y = 0.0f;
  int wrap_width = 0;
};

struct DrawList;

struct DrawCmdLayer {
  std::shared_ptr<DrawList> list;
  DrawState state{};
};

using DrawCmd = std::variant<DrawCmdRectFilled, DrawCmdRectOutline, DrawCmdImage,
                             DrawCmdText, DrawCmdLayer>;

struct DrawList {
  std::vector<DrawCmd> cmds;
};

class Renderer {
private:
  struct PolledEvent {
    std::string type;
    std::optional<std::string> button;
    int x = 0;
    int y = 0;
  };

  struct LuaTexture {
    SDL_Texture *tex = nullptr;
  };

  struct LuaFont {
    TTF_Font *font = nullptr;
  };

  struct LuaCanvas {
    Renderer *renderer = nullptr;
  };

  struct LuaStyle {
    CompiledStyle style{};
  };

  struct LayerBuildState {
    std::shared_ptr<DrawList> list;
    DrawState state{};
  };

  static LuaTexture *CheckLuaTexture(lua_State *L, int idx);
  static LuaFont *CheckLuaFont(lua_State *L, int idx);
  static LuaCanvas *CheckLuaCanvas(lua_State *L, int idx);
  static LuaStyle *CheckLuaStyle(lua_State *L, int idx);
  static SDL_Color ToSdlColor(uint32_t abgr);

  SDL_Window *window_ = nullptr;
  SDL_Renderer *sdl_ = nullptr;
  int canvas_width_ = 1280;
  int canvas_height_ = 720;
  uint32_t clear_color_abgr_ = 0xFF141414u;

  std::vector<PolledEvent> polled_events_;
  DrawList draw_list_;
  std::vector<Mat2D> transform_stack_;
  std::vector<ClipRect> clip_stack_;
  std::vector<LayerBuildState> layer_stack_;

  void SetLuaGlobals(lua_State *L);
  static Renderer *GetInstance(lua_State *L);

  DrawList &ActiveDrawList();
  const ClipRect &CurrentClip() const;
  const Mat2D &CurrentTransform() const;
  void ClearDrawList(lua_State *L, DrawList &list);
  void RenderDrawList(lua_State *L, const DrawList &list);
  SDL_Texture *RenderLayerToTexture(lua_State *L, const DrawList &list);

  static int L_PollSdlEvents(lua_State *L);
  static int L_TextureDestroy(lua_State *L);
  static int L_FontDestroy(lua_State *L);
  static int L_FontMeasure(lua_State *L);
  static int L_GfxSetCanvasSize(lua_State *L);
  static int L_GfxClear(lua_State *L);
  static int L_GfxPresent(lua_State *L);
  static int L_GfxRect(lua_State *L);
  static int L_GfxRectOutline(lua_State *L);
  static int L_GfxImage(lua_State *L);
  static int L_GfxText(lua_State *L);
  static int L_GfxStyle(lua_State *L);
  static int L_GfxPushTransform(lua_State *L);
  static int L_GfxPopTransform(lua_State *L);
  static int L_GfxTranslate(lua_State *L);
  static int L_GfxScale(lua_State *L);
  static int L_GfxRotate(lua_State *L);
  static int L_GfxPushClip(lua_State *L);
  static int L_GfxPopClip(lua_State *L);
  static int L_GfxBeginLayer(lua_State *L);
  static int L_GfxEndLayer(lua_State *L);

public:
  class LoadImageJob : public AsyncJob {
  public:
    void Invoke(lua_State *L) override;
    void Run() override;
    int Finish(lua_State *L) override;
    ~LoadImageJob();

  private:
    std::string path_;
    SDL_IOStream *io_ = nullptr;
    SDL_Surface *surface_ = nullptr;
  };

  class LoadFontJob : public AsyncJob {
  public:
    void Invoke(lua_State *L) override;
    void Run() override;
    int Finish(lua_State *L) override;

  private:
    std::string path_;
    float size_pt_ = 0.0f;
  };

  Renderer();

  bool Init();
  void Fini();
  void PumpSdlEvents();
  void RegisterBindings(lua_State *L);
};

} // namespace luna

#endif