#include <string>

#include "engine.h"
#include "log.h"

int main(int argc, char **argv) {
  luna::log::Init();
  const std::string entry_path = argc > 1 ? argv[1] : "main.lua";

  luna::Engine e;
  if (!e.Init()) {
    luna::log::Shutdown();
    return 1;
  }

  e.Run(entry_path);
  luna::log::Shutdown();
  return 0;
}
