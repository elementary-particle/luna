#ifndef LUNA_UI_H
#define LUNA_UI_H

#include <cstdint>

#include "lua.hpp"

#include <optional>
#include <string>
#include <vector>

namespace luna {

class Ui {
public:
  Ui() = default;

  void SetDrawCallback(lua_State *L, int fn_index);
  void ClearDrawCallback(lua_State *L);

  bool OnMouseMove(float x, float y);
  bool OnMouseButtonDown(int button);
  bool OnMouseButtonUp(int button);
  bool WantsMouseCapture() const;

  // Called each frame (main thread).
  void Draw(lua_State *L, DrawList *out);

  static void RegisterUiBindings(lua_State *L);

private:
  int draw_fn_ref_ = LUA_NOREF;

  float mouse_x_ = 0.0f;
  float mouse_y_ = 0.0f;
  bool mouse_down_ = false;
  bool mouse_pressed_ = false;
  bool mouse_released_ = false;

  uint64_t hot_id_ = 0;
  uint64_t active_id_ = 0;
  uint64_t hovered_prev_id_ = 0;
  uint64_t press_candidate_id_ = 0;

  struct HitRegion {
    uint64_t id = 0;
    Rect rect{};
  };
  std::vector<HitRegion> prev_hit_regions_;
  std::vector<HitRegion> frame_hit_regions_;

  // Per-frame transient state for immediate mode IDs.
  uint64_t id_stack_hash_ = 1469598103934665603ULL; // FNV-1a offset basis
  int per_frame_call_index_ = 0;

  void BeginFrame();
  void EndFrame();
  uint64_t HitTestPrevRegionId(float x, float y) const;
  uint64_t MakeWidgetId(const std::string &label,
                        const std::optional<std::string> &explicit_id);

  // Binding helpers
  static int L_ScreenSize(lua_State *L);
  static int L_Rect(lua_State *L);
  static int L_RectOutline(lua_State *L);
  static int L_Image(lua_State *L);
  static int L_Button(lua_State *L);

  struct UiContext {
    Ui *ui = nullptr;
    DrawList *draw_list = nullptr;

    // Simple layout cursor could go here later.
  };

  static UiContext *CheckCtx(lua_State *L);
};

} // namespace luna

#endif
