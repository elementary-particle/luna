#ifndef LUNA_BLEND2D_REGION_H
#define LUNA_BLEND2D_REGION_H

#include <span>
#include <vector>

#include <blend2d/blend2d.h>

namespace luna::backend::blend2d {

struct Region {
  bool IsEmpty() const;
  void Clear();
  void UnionRect(const BLBoxI &rect);
  void Union(const Region &other);
  void Intersect(const BLBoxI &clip);
  BLBoxI Bounds() const;
  std::span<const BLBoxI> Rects() const;

private:
  std::vector<BLBoxI> rects_;

  void Coalesce();
};

} // namespace luna::backend::blend2d

#endif // LUNA_BLEND2D_REGION_H
