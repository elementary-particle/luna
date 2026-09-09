# Testing

Tests share the flat `test/` source layout.

- `test/behavior.cmake` groups filesystem, package, SVG path, save API, and input
  behavior tests. Input tests use `test/headless_renderer.h`, exercising the
  shared renderer/input contract without opening a window or initializing
  graphics. Filesystem tests run directly against the real VFS.
- `test/rendering.cmake` groups paragraph metrics and render parity tests using
  real Skia and Blend2D backends. Both backends are required for those tests.
  Pixel parity fuzzing remains opt-in with `LUNA_ENABLE_RENDER_PARITY_TEST=ON`.
- On Linux, `test/engine.cmake` also exercises the real engine using SDL dummy
  drivers. This rendering-labeled smoke test requires the application and
  Blend2D, but not Skia.
- Add tests to the relevant suite using `luna_register_test(target behavior)`
  or `luna_register_test(target rendering)` for consistent labels, timeouts,
  and build targets. All tests respect `BUILD_TESTING`.

Run commands from the repository root after [configuring a build](../README.md#building).
Build and run each suite independently:

```sh
cmake --build build --target luna_behavior_tests
ctest --test-dir build -L behavior --output-on-failure
cmake --build build --target luna_rendering_tests
ctest --test-dir build -L rendering --output-on-failure
```

For a behavior-only build, configure a separate build directory with the usual
platform preset and disable the application and graphics backends:

```sh
cmake --preset vcpkg-x64-linux -B build-behavior \
  -DLUNA_BUILD_APP=OFF -DLUNA_BUILD_RENDERING_TESTS=OFF \
  -DLUNA_BACKEND_SKIA=OFF -DLUNA_BACKEND_BLEND2D=OFF
cmake --build build-behavior --target luna_behavior_tests
ctest --test-dir build-behavior -L behavior --output-on-failure
```

The behavior targets do not link Skia, Blend2D, Vulkan, or the audio mixer.
The vcpkg manifest may still provision graphics dependencies during configure.
