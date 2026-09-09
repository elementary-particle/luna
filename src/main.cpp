#include <filesystem>
#include <iostream>
#include <string>

#include <CLI/CLI.hpp>

#include "engine.h"
#include "log.h"

namespace {

struct StartupOptions {
  std::filesystem::path root = std::filesystem::current_path();
  std::string script_path;
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
  std::string game_id;

  app.add_option("-r,--root", root_arg,
         "Game directory or .luna archive")
      ->check(CLI::ExistingPath);
  app.add_option(
      "-s,--script", script_arg, "Lua entry script path inside the asset VFS");

  app.add_option("--game-id", game_id,
      "Persistent storage identity (overrides game.json)");
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

  luna::log::Init();

  try {
    luna::Engine e(options.root, game_id);
    if (!e.Init()) {
      luna::log::Shutdown();
      return 1;
    }

    const int exit_code = e.Run(options.script_path) ? 0 : 1;
    luna::log::Shutdown();
    return exit_code;
  } catch (const std::exception &ex) {
    luna::log::Shutdown();
    return PrintErrorAndExit(ex.what());
  }
}
