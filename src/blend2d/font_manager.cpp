#include "blend2d/font_manager.h"

namespace luna::backend::blend2d {

BLFontManager MakeRuntimeFontManager() {
  BLFontManager font_mgr;
  font_mgr.create();
  return font_mgr;
}

bool RegisterRuntimeFont(BLFontManager *font_mgr, const char path[]) {
  if (font_mgr == nullptr || !font_mgr->is_valid() || path == nullptr ||
      !*path) {
    return false;
  }

  BLFontFace face;
  if (face.create_from_file(path, BL_FILE_READ_MMAP_ENABLED) != BL_SUCCESS ||
      !face.is_valid()) {
    return false;
  }

  return font_mgr->add_face(face) == BL_SUCCESS;
}

} // namespace luna::backend::blend2d
