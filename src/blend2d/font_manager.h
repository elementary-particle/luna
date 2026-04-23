#ifndef LUNA_BLEND2D_FONT_MANAGER_H
#define LUNA_BLEND2D_FONT_MANAGER_H

#include <blend2d/blend2d.h>

namespace luna::backend::blend2d {

BLFontManager MakeRuntimeFontManager();
bool RegisterRuntimeFont(BLFontManager *font_mgr, const char path[]);

} // namespace luna::backend::blend2d

#endif
