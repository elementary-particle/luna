#ifndef LUNA_TEST_HEADLESS_RENDERER_H
#define LUNA_TEST_HEADLESS_RENDERER_H

#include "renderer_interface.h"

#include <stdexcept>

namespace luna::test {

// Exercises the shared renderer/input contract without SDL video or a GPU.
// Drawing and asset decoding belong to the rendering suite.
class HeadlessRenderer final : public Renderer {
public:
  bool Init() override {
    UpdateWindowTransform();
    return true;
  }
  void ReleaseLua(lua_State *) override {}
  void Fini() override {}
  bool BeginFrame(lua_State *) override { return true; }
  bool EndFrame() override { return true; }
  void BindLua(lua_State *L) override { input_.BindLua(L); }
  bool HasFatalError() const override { return false; }
  const std::string &GetFatalError() const override { return error_; }

  void InjectEvent(const SDL_Event &event) {
    input_.HandleEvent(event, GetInputCoordinateSpace());
  }

  std::unique_ptr<AsyncJob> MakeLoadImageJob() override {
    throw std::logic_error("headless tests do not decode images");
  }
  std::unique_ptr<AsyncJob> MakeLoadFontfaceJob() override {
    throw std::logic_error("headless tests do not decode fonts");
  }

private:
  void UpdateWindowTransform() override {
    pixel_viewport_ = CalculateAspectFit(pixel_width_, pixel_height_);
  }
  std::string error_;
};

} // namespace luna::test

#endif
