#include "engine.h"

#include <string>

int main(int argc, char **argv) {
  const std::string entry_path = argc > 1 ? argv[1] : "main.lua";

  luna::Engine e;
  if (!e.Init())
    return 1;

  e.Run(entry_path);
  return 0;
}
