#include <filesystem>
#include <iostream>
#include <string>

#include <CLI/CLI.hpp>

#include "engine.h"
#include "log.h"

namespace {

struct StartupOptions {
  std::filesystem::path root = std::filesystem::current_path();
  std::string script_path = "main.lua";
};

std::filesystem::path NormalizeRoot(const std::filesystem::path &path) {
  std::error_code ec;
  const std::filesystem::path absolute_path =
      std::filesystem::absolute(path, ec);
  if (!ec) {
    return absolute_path.lexically_normal();
  }

  return path.lexically_normal();
}

int PrintErrorAndExit(const std::string &message) {
  std::cerr << "error: " << message << '\n';
  return 1;
}

} // namespace

int main(int argc, char **argv) {
  CLI::App app("Luna runtime");

  std::string root_arg = ".";
  std::string script_arg;

  app.add_option(
         "-r,--root", root_arg,
         "Root directory used for assets and Lua-relative file loading")
      ->check(CLI::ExistingDirectory);
  app.add_option(
      "-s,--script", script_arg,
      "Lua entry script path inside the asset VFS");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  }

  StartupOptions options;
  options.root = NormalizeRoot(std::filesystem::path(root_arg));

  if (!script_arg.empty()) {
    options.script_path = script_arg;
  }

  luna::Vfs root_vfs(options.root);
  auto entry_info = root_vfs.assets().Stat(options.script_path);
  if (!entry_info) {
    const luna::asset::AssetError &error = entry_info.error();
    const std::string resolved_entry =
        error.path.empty() ? options.script_path : error.path;
    return PrintErrorAndExit(
        "script '" + resolved_entry + "' could not be loaded from root '" +
        options.root.string() + "': " + error.message);
  }
  if (entry_info.value().kind != luna::asset::EntryKind::kFile) {
    return PrintErrorAndExit(
        "script '" + entry_info.value().path + "' is not a file");
  }

  std::error_code ec;
  // Keep Lua's default relative path behavior aligned with the selected
  // root instead of the shell's working directory.
  std::filesystem::current_path(options.root, ec);
  if (ec) {
    return PrintErrorAndExit(
        "failed to switch to root '" + options.root.string() + "': " +
        ec.message());
  }

  luna::log::Init();

  luna::Engine e(options.root);
  if (!e.Init()) {
    luna::log::Shutdown();
    return 1;
  }

  const int exit_code = e.Run(options.script_path) ? 0 : 1;
  luna::log::Shutdown();
  return exit_code;
}
