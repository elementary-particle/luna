#include "native_file_source.h"
#include "native_path_boundaries.h"

#if defined(_WIN32)
int main() {
  luna::test::CheckNativePathBoundaries();
  return 0;
}
#else
int main() { return 0; }
#endif
