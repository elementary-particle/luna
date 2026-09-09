#include "skia/font_manager.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <skia/core/SkData.h>
#include <skia/core/SkFontArguments.h>
#include <skia/core/SkFontMgr.h>
#include <skia/core/SkFontScanner.h>
#include <skia/core/SkFontStyle.h>
#include <skia/core/SkStream.h>
#include <skia/core/SkString.h>
#include <skia/core/SkTypeface.h>
#include <skia/ports/SkFontMgr_empty.h>
#include <skia/ports/SkFontScanner_FreeType.h>

namespace luna::backend::skia {
namespace {

class RegisteredFontStyleSet final : public SkFontStyleSet {
public:
  struct RegisteredTypeface {
    sk_sp<SkTypeface> typeface;
    SkFontStyle style;
    std::shared_ptr<const file::MappedFile> mapping;
  };

  RegisteredFontStyleSet(
      SkString family_name, std::vector<RegisteredTypeface> entries)
      : family_name_(std::move(family_name)), entries_(std::move(entries)) {}

  int count() override { return static_cast<int>(entries_.size()); }

  void getStyle(int index, SkFontStyle *style, SkString *name) override {
    if (index < 0 || index >= this->count()) {
      return;
    }
    if (style) {
      *style = entries_[index].style;
    }
    if (name) {
      *name = family_name_;
    }
  }

  sk_sp<SkTypeface> createTypeface(int index) override {
    if (index < 0 || index >= this->count()) {
      return nullptr;
    }
    return entries_[index].typeface;
  }

  sk_sp<SkTypeface> matchStyle(const SkFontStyle &pattern) override {
    return this->matchStyleCSS3(pattern);
  }

private:
  SkString family_name_;
  std::vector<RegisteredTypeface> entries_;
};

class RuntimeFontManager final : public SkFontMgr {
public:
  RuntimeFontManager()
      : loader_(SkFontMgr_New_Custom_Empty()),
        scanner_(SkFontScanner_Make_FreeType()) {}

  bool RegisterFont(file::MappedFile mapping) {
    if (!loader_ || !scanner_ || mapping.size() == 0) {
      return false;
    }

    auto mapping_ref =
        std::make_shared<const file::MappedFile>(std::move(mapping));
    std::unique_ptr<SkStreamAsset> stream = SkMemoryStream::MakeDirect(
        mapping_ref->data(), static_cast<size_t>(mapping_ref->size()));
    if (!stream) {
      return false;
    }

    int num_faces = 0;
    if (!scanner_->scanFile(stream.get(), &num_faces) || num_faces <= 0) {
      return false;
    }

    bool registered_any = false;
    for (int face_index = 0; face_index < num_faces; ++face_index) {
      int num_instances = 0;
      if (!scanner_->scanFace(stream.get(), face_index, &num_instances)) {
        continue;
      }

      for (int instance_index = 0; instance_index <= num_instances;
          ++instance_index) {
        SkString real_name;
        SkFontStyle style;
        bool is_fixed_pitch = false;
        SkFontScanner::VariationPosition position;
        if (!scanner_->scanInstance(stream.get(), face_index, instance_index,
                &real_name, &style, &is_fixed_pitch, nullptr, &position)) {
          continue;
        }

        std::unique_ptr<SkStreamAsset> instance_stream = stream->duplicate();
        if (!instance_stream) {
          continue;
        }

        SkFontArguments args;
        args.setCollectionIndex(face_index);
        if (!position.empty()) {
          args.setVariationDesignPosition(
              {position.data(), static_cast<int>(position.size())});
        }

        sk_sp<SkTypeface> typeface =
            scanner_->MakeFromStream(std::move(instance_stream), args);
        if (!typeface) {
          continue;
        }

        registered_any |= RegisterTypeface(typeface, style, mapping_ref);
      }
    }

    return registered_any;
  }

protected:
  int onCountFamilies() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(family_names_.size());
  }

  void onGetFamilyName(int index, SkString *family_name) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!family_name) {
      return;
    }
    if (index < 0 || index >= static_cast<int>(family_names_.size())) {
      family_name->reset();
      return;
    }
    *family_name = family_names_[index];
  }

  sk_sp<SkFontStyleSet> onCreateStyleSet(int index) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < 0 || index >= static_cast<int>(family_names_.size())) {
      return SkFontStyleSet::CreateEmpty();
    }
    return MakeStyleSetLocked(family_names_[index].c_str());
  }

  sk_sp<SkFontStyleSet> onMatchFamily(const char family_name[]) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!family_name || !*family_name) {
      return SkFontStyleSet::CreateEmpty();
    }
    return MakeStyleSetLocked(family_name);
  }

  sk_sp<SkTypeface> onMatchFamilyStyle(
      const char family_name[], const SkFontStyle &pattern) const override {
    sk_sp<SkFontStyleSet> style_set;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      style_set = MakeStyleSetLocked(family_name);
    }
    if (!style_set || style_set->count() == 0) {
      return nullptr;
    }
    return style_set->matchStyle(pattern);
  }

  sk_sp<SkTypeface> onMatchFamilyStyleCharacter(const char family_name[],
      const SkFontStyle &style, const char *bcp47[], int bcp47_count,
      SkUnichar character) const override {
    if (loader_) {
      return loader_->matchFamilyStyleCharacter(
          family_name, style, bcp47, bcp47_count, character);
    }
    return nullptr;
  }

  sk_sp<SkTypeface> onMakeFromData(
      sk_sp<SkData> data, int ttc_index) const override {
    if (!loader_) {
      return nullptr;
    }
    return loader_->makeFromData(std::move(data), ttc_index);
  }

  sk_sp<SkTypeface> onMakeFromStreamIndex(
      std::unique_ptr<SkStreamAsset> stream, int ttc_index) const override {
    if (!loader_) {
      return nullptr;
    }
    return loader_->makeFromStream(std::move(stream), ttc_index);
  }

  sk_sp<SkTypeface> onMakeFromStreamArgs(std::unique_ptr<SkStreamAsset> stream,
      const SkFontArguments &args) const override {
    if (!loader_) {
      return nullptr;
    }
    return loader_->makeFromStream(std::move(stream), args);
  }

  sk_sp<SkTypeface> onMakeFromFile(
      const char path[], int ttc_index) const override {
    if (!loader_) {
      return nullptr;
    }
    return loader_->makeFromFile(path, ttc_index);
  }

  sk_sp<SkTypeface> onLegacyMakeTypeface(
      const char family_name[], SkFontStyle style) const override {
    return this->onMatchFamilyStyle(family_name, style);
  }

private:
  sk_sp<SkFontStyleSet> MakeStyleSetLocked(const char family_name[]) const {
    if (!family_name || !*family_name) {
      return SkFontStyleSet::CreateEmpty();
    }

    auto it = families_.find(family_name);
    if (it == families_.end()) {
      return SkFontStyleSet::CreateEmpty();
    }

    return sk_make_sp<RegisteredFontStyleSet>(
        it->second.name, it->second.typefaces);
  }

  bool RegisterTypeface(sk_sp<SkTypeface> typeface, const SkFontStyle &style,
      std::shared_ptr<const file::MappedFile> mapping) const {
    if (!typeface) {
      return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    SkString family_name;
    typeface->getFamilyName(&family_name);
    if (family_name.isEmpty()) {
      return false;
    }

    FamilyEntry &entry = families_[family_name.c_str()];
    if (entry.typefaces.empty()) {
      entry.name = family_name;
      family_names_.push_back(family_name);
    }
    for (const RegisteredFontStyleSet::RegisteredTypeface &registered :
        entry.typefaces) {
      if (registered.typeface->uniqueID() == typeface->uniqueID() &&
          registered.style == style) {
        return true;
      }
    }
    entry.typefaces.push_back({std::move(typeface), style, std::move(mapping)});
    return true;
  }

  struct FamilyEntry {
    SkString name;
    std::vector<RegisteredFontStyleSet::RegisteredTypeface> typefaces;
  };

  sk_sp<SkFontMgr> loader_;
  std::unique_ptr<SkFontScanner> scanner_;
  mutable std::mutex mutex_;
  mutable std::vector<SkString> family_names_;
  mutable std::unordered_map<std::string, FamilyEntry> families_;
};

} // namespace

sk_sp<SkFontMgr> MakeRuntimeFontManager() {
  return sk_make_sp<RuntimeFontManager>();
}

bool RegisterRuntimeFont(
    const sk_sp<SkFontMgr> &font_mgr, file::MappedFile mapping) {
  if (!font_mgr) {
    return false;
  }
  return static_cast<RuntimeFontManager *>(font_mgr.get())
      ->RegisterFont(std::move(mapping));
}

} // namespace luna::backend::skia
