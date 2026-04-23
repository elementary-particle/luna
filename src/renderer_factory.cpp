#include "renderer_factory.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <memory>
#include <limits>
#include <optional>
#include <string>

#if defined(LUNA_HAVE_BACKEND_BLEND2D)
#include "blend2d/renderer.h"
#endif
#include "log.h"
#include "renderer_interface.h"
#if defined(LUNA_HAVE_BACKEND_SKIA)
#include "skia/renderer.h"
#endif

namespace luna {
namespace {

constexpr const char *kBackendEnvVar = "LUNA_BACKEND";
constexpr const char *kBlend2dThreadCountEnvVar = "LUNA_BLEND2D_THREAD_COUNT";

std::string NormalizeBackendName(const char *name) {
  std::string normalized = name ? name : "";
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return normalized;
}

std::string AvailableBackends() {
  std::string backends;
#if defined(LUNA_HAVE_BACKEND_SKIA)
  backends = "skia";
#endif
#if defined(LUNA_HAVE_BACKEND_BLEND2D)
  if (!backends.empty()) {
    backends += ", ";
  }
  backends += "blend2d";
#endif
  return backends;
}

std::optional<uint32_t> ParseUint32Env(const char *name) {
  const char *value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return std::nullopt;
  }

  try {
    size_t consumed = 0;
    unsigned long parsed = std::stoul(value, &consumed, 10);
    if (value[consumed] != '\0') {
      throw std::invalid_argument("trailing characters");
    }
    if (parsed > std::numeric_limits<uint32_t>::max()) {
      throw std::out_of_range("too large");
    }
    return static_cast<uint32_t>(parsed);
  } catch (std::exception const &e) {
    log::Warn("renderer", "ignoring invalid {}='{}': {}", name, value, e.what());
    return std::nullopt;
  }
}

RendererBackendParams ResolveRendererBackendParams(RendererBackendParams params) {
  if (!params.blend2d_thread_count.has_value()) {
    params.blend2d_thread_count = ParseUint32Env(kBlend2dThreadCountEnvVar);
  }
  return params;
}

std::unique_ptr<Renderer> MakeDefaultRendererBackend(
    const RendererBackendParams &params) {
#if defined(LUNA_HAVE_BACKEND_SKIA)
  return std::make_unique<backend::skia::SkiaRenderer>();
#elif defined(LUNA_HAVE_BACKEND_BLEND2D)
  return std::make_unique<backend::blend2d::Blend2dRenderer>(
      params.blend2d_thread_count.value_or(0));
#else
#error "No renderer backend enabled"
#endif
}

std::unique_ptr<Renderer> MakeRequestedRendererBackend(
    const std::string &backend_name, const RendererBackendParams &params) {
  if (backend_name == "skia") {
#if defined(LUNA_HAVE_BACKEND_SKIA)
    return std::make_unique<backend::skia::SkiaRenderer>();
#else
    log::Error("renderer",
               "{}='{}' requested Skia, but this build does not include it. "
               "Available backends: {}",
               kBackendEnvVar, backend_name, AvailableBackends());
    return nullptr;
#endif
  }

  if (backend_name == "blend2d" || backend_name == "blend2d_cpu") {
#if defined(LUNA_HAVE_BACKEND_BLEND2D)
    return std::make_unique<backend::blend2d::Blend2dRenderer>(
        params.blend2d_thread_count.value_or(0));
#else
    log::Error("renderer",
               "{}='{}' requested Blend2D, but this build does not include it. "
               "Available backends: {}",
               kBackendEnvVar, backend_name, AvailableBackends());
    return nullptr;
#endif
  }

  log::Error("renderer",
             "invalid {}='{}'. Expected one of: skia, blend2d. "
             "Available backends in this build: {}",
             kBackendEnvVar, backend_name, AvailableBackends());
  return nullptr;
}

} // namespace

std::unique_ptr<Renderer> MakeRendererBackend(RendererBackendParams params) {
  params = ResolveRendererBackendParams(std::move(params));
  const char *requested_backend = std::getenv(kBackendEnvVar);
  if (!requested_backend || requested_backend[0] == '\0') {
    return MakeDefaultRendererBackend(params);
  }

  const std::string backend_name = NormalizeBackendName(requested_backend);
  log::Info("renderer", "selecting backend '{}' from {}", backend_name,
            kBackendEnvVar);
  return MakeRequestedRendererBackend(backend_name, params);
}

} // namespace luna
