#ifndef LUNA_BLEND2D_FONT_MANAGER_H
#define LUNA_BLEND2D_FONT_MANAGER_H

#include <blend2d/blend2d.h>

#include "file_vfs.h"

namespace luna::backend::blend2d {

BLFontManager MakeRuntimeFontManager();
bool RegisterRuntimeFont(BLFontManager *font_mgr, file::MappedFile mapping);

} // namespace luna::backend::blend2d

#endif
