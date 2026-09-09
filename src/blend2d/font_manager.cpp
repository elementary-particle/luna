#include "blend2d/font_manager.h"

namespace luna::backend::blend2d {
namespace {

void DestroyMappedFontData(void *, void *, void *user_data) noexcept {
  delete static_cast<file::MappedFile *>(user_data);
}

} // namespace

BLFontManager MakeRuntimeFontManager() {
  BLFontManager font_mgr;
  font_mgr.create();
  return font_mgr;
}

bool RegisterRuntimeFont(BLFontManager *font_mgr, file::MappedFile mapping) {
  if (font_mgr == nullptr || !font_mgr->is_valid() || mapping.size() == 0) {
    return false;
  }

  auto *font_mapping = new file::MappedFile(std::move(mapping));
  BLFontData font_data;
  if (font_data.create_from_data(font_mapping->data(),
          static_cast<size_t>(font_mapping->size()), DestroyMappedFontData,
          font_mapping) != BL_SUCCESS) {
    delete font_mapping;
    return false;
  }

  BLFontFace face;
  if (face.create_from_data(font_data, 0) != BL_SUCCESS || !face.is_valid()) {
    return false;
  }

  return font_mgr->add_face(face) == BL_SUCCESS;
}

} // namespace luna::backend::blend2d
