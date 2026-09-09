#include "backend/svg_path.h"
#include <cassert>
#include <vector>

struct Path {
  struct Command { char kind; std::vector<double> values; };
  std::vector<Command> commands;
  void MoveTo(double x, double y) { commands.push_back({'M', {x,y}}); }
  void LineTo(double x, double y) { commands.push_back({'L', {x,y}}); }
  void QuadTo(double a,double b,double c,double d) { commands.push_back({'Q',{a,b,c,d}}); }
  void CubicTo(double a,double b,double c,double d,double e,double f) { commands.push_back({'C',{a,b,c,d,e,f}}); }
  void ArcTo(double a,double b,double c,bool d,bool e,double f,double g) { commands.push_back({'A',{a,b,c,double(d),double(e),f,g}}); }
  void Close() { commands.push_back({'Z',{}}); }
};
int main() {
  using luna::backend::ParseSvgPath;
  auto p = ParseSvgPath<Path>("m1 2 3 4h5v6z l1-2");
  assert(p && p->commands.size() == 6);
  assert((p->commands[1].values == std::vector<double>{4,6}));
  assert((p->commands[3].values == std::vector<double>{9,12}));
  assert((p->commands[5].values == std::vector<double>{2,0}));
  p = ParseSvgPath<Path>("M0 0 C1 2 3 4 5 6 s2 3 4 5 Q10 12 14 16 t2 3 L0 0 T2 3");
  assert(p);
  assert((p->commands[2].values == std::vector<double>{7,8,7,9,9,11}));
  assert((p->commands[4].values == std::vector<double>{18,20,16,19}));
  assert((p->commands[6].values == std::vector<double>{0,0,2,3}));
  p = ParseSvgPath<Path>("M+.5-.2 a10 20 90 011e2,2e1");
  assert(p && p->commands[1].values[5] == 100.5);
  for (auto text : {"", "M0 0Z", "M1. 2. L.1.2", "M0 0H1 2V3 4", "M0 0A0 0 0 0 0 1 1"})
    assert(ParseSvgPath<Path>(text));
  for (auto text : {"L0 0", "M", "M0", "M,0 0", "M0 0,", "M0 0,,1 2", "M0 0Z1 2", "M0 0X1", "Mnan 0", "M1e999 0", "M1e 0", "M0 0 A-1 2 0 0 0 3 4", "M0 0 A1 2 0 2 0 3 4"})
    assert(!ParseSvgPath<Path>(text));
}
