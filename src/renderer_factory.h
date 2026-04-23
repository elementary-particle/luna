#ifndef LUNA_RENDERER_FACTORY_H
#define LUNA_RENDERER_FACTORY_H

#include <cstdint>
#include <memory>
#include <optional>

namespace luna {

class Renderer;

struct RendererBackendParams {
  std::optional<uint32_t> blend2d_thread_count;
};

std::unique_ptr<Renderer> MakeRendererBackend(
    RendererBackendParams params = {});

} // namespace luna

#endif
