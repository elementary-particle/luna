#ifndef LUNA_CANVAS_H
#define LUNA_CANVAS_H

#include "lua.hpp"

#include <memory>
#include <skia/core/SkCanvas.h>
#include <skia/core/SkFont.h>
#include <skia/core/SkFontMgr.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkPaint.h>
#include <skia/core/SkSurface.h>
#include <skia/core/SkTypeface.h>
#include <skia/modules/skparagraph/include/FontCollection.h>
#include <skia/modules/skparagraph/include/Paragraph.h>

class SkCanvas;

namespace luna {

struct LImage {
  static constexpr const char *MT = "luna.Image";
  sk_sp<SkImage> sk;

  LImage() = default;
  explicit LImage(sk_sp<SkImage> image) : sk(std::move(image)) {}
};

struct LTypeface {
  static constexpr const char *MT = "luna.Typeface";
  sk_sp<SkTypeface> sk;

  LTypeface() = default;
  explicit LTypeface(sk_sp<SkTypeface> typeface) : sk(std::move(typeface)) {}
};

struct LParagraph {
  static constexpr const char *MT = "luna.Paragraph";
  sk_sp<skia::textlayout::FontCollection> font_collection;
  std::unique_ptr<skia::textlayout::Paragraph> sk;
  SkScalar layout_width = 0;

  LParagraph() = default;
  LParagraph(sk_sp<skia::textlayout::FontCollection> font_collection,
             std::unique_ptr<skia::textlayout::Paragraph> paragraph,
             SkScalar width)
      : font_collection(std::move(font_collection)),
        sk(std::move(paragraph)),
        layout_width(width) {}
};

class LCanvas {
private:
  struct LPaint {
    static constexpr const char *MT = "luna.Paint";
    SkPaint sk;
  };

  struct LFont {
    static constexpr const char *MT = "luna.Font";
    SkFont sk;
  };

  SkCanvas *sk_ = nullptr;
  sk_sp<SkSurface> surface_;
  sk_sp<SkFontMgr> font_mgr_;

  static void RegisterLuaHelpers(lua_State *L);

public:
  static constexpr const char *MT = "luna.Canvas";

  void set_sk(SkCanvas *sk) {
    surface_.reset();
    sk_ = sk;
  }

  void set_font_manager(sk_sp<SkFontMgr> font_mgr) { font_mgr_ = std::move(font_mgr); }
  sk_sp<SkFontMgr> font_mgr() const { return font_mgr_; }

  void set_surface(sk_sp<SkSurface> surface) {
    surface_ = std::move(surface);
    sk_ = surface_ ? surface_->getCanvas() : nullptr;
  }

  LCanvas() = default;
  static void RegisterBindings(lua_State *L);
};

} // namespace luna

#endif
