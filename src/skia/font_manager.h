#ifndef LUNA_FONT_MANAGER_H
#define LUNA_FONT_MANAGER_H

#include <memory>

#include <skia/core/SkFontMgr.h>
#include <skia/core/SkFontStyle.h>

#include "asset_vfs.h"

namespace luna::backend::skia {

sk_sp<SkFontMgr> MakeRuntimeFontManager();
bool RegisterRuntimeFont(const sk_sp<SkFontMgr> &font_mgr,
                         asset::MappedAsset mapping);

} // namespace luna

#endif
