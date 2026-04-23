#include "blend2d/region.h"

#include <algorithm>
#include <cstddef>

namespace luna::backend::blend2d {

namespace {

bool IsEmptyBox(const BLBoxI &box) {
  return box.x0 >= box.x1 || box.y0 >= box.y1;
}

BLBoxI IntersectBoxes(const BLBoxI &a, const BLBoxI &b) {
  return BLBoxI(std::max(a.x0, b.x0), std::max(a.y0, b.y0),
      std::min(a.x1, b.x1), std::min(a.y1, b.y1));
}

std::vector<BLBoxI> SubtractBox(const BLBoxI &rect, const BLBoxI &cut) {
  const BLBoxI overlap = IntersectBoxes(rect, cut);
  if (IsEmptyBox(overlap)) {
    return {rect};
  }

  std::vector<BLBoxI> out;
  if (rect.y0 < overlap.y0) {
    out.emplace_back(rect.x0, rect.y0, rect.x1, overlap.y0);
  }
  if (overlap.y1 < rect.y1) {
    out.emplace_back(rect.x0, overlap.y1, rect.x1, rect.y1);
  }
  if (rect.x0 < overlap.x0) {
    out.emplace_back(rect.x0, overlap.y0, overlap.x0, overlap.y1);
  }
  if (overlap.x1 < rect.x1) {
    out.emplace_back(overlap.x1, overlap.y0, rect.x1, overlap.y1);
  }
  return out;
}

bool CanMerge(const BLBoxI &a, const BLBoxI &b) {
  const bool same_rows = a.y0 == b.y0 && a.y1 == b.y1;
  const bool touching_x = a.x1 >= b.x0 && b.x1 >= a.x0;
  if (same_rows && touching_x) {
    return true;
  }

  const bool same_cols = a.x0 == b.x0 && a.x1 == b.x1;
  const bool touching_y = a.y1 >= b.y0 && b.y1 >= a.y0;
  return same_cols && touching_y;
}

BLBoxI MergeBoxes(const BLBoxI &a, const BLBoxI &b) {
  return BLBoxI(std::min(a.x0, b.x0), std::min(a.y0, b.y0),
      std::max(a.x1, b.x1), std::max(a.y1, b.y1));
}

} // namespace

bool Region::IsEmpty() const { return rects_.empty(); }

void Region::Clear() { rects_.clear(); }

void Region::UnionRect(const BLBoxI &rect) {
  if (IsEmptyBox(rect)) {
    return;
  }

  std::vector<BLBoxI> pending = {rect};
  for (const BLBoxI &existing : rects_) {
    std::vector<BLBoxI> next;
    next.reserve(pending.size());
    for (const BLBoxI &piece : pending) {
      std::vector<BLBoxI> remainder = SubtractBox(piece, existing);
      next.insert(next.end(), remainder.begin(), remainder.end());
    }
    pending = std::move(next);
    if (pending.empty()) {
      return;
    }
  }

  rects_.insert(rects_.end(), pending.begin(), pending.end());
  Coalesce();
}

void Region::Union(const Region &other) {
  for (const BLBoxI &rect : other.rects_) {
    UnionRect(rect);
  }
}

void Region::Intersect(const BLBoxI &clip) {
  if (IsEmptyBox(clip) || rects_.empty()) {
    rects_.clear();
    return;
  }

  std::vector<BLBoxI> out;
  out.reserve(rects_.size());
  for (const BLBoxI &rect : rects_) {
    const BLBoxI overlap = IntersectBoxes(rect, clip);
    if (!IsEmptyBox(overlap)) {
      out.push_back(overlap);
    }
  }
  rects_ = std::move(out);
  Coalesce();
}

BLBoxI Region::Bounds() const {
  if (rects_.empty()) {
    return BLBoxI(0, 0, 0, 0);
  }

  BLBoxI bounds = rects_.front();
  for (size_t i = 1; i < rects_.size(); ++i) {
    bounds = MergeBoxes(bounds, rects_[i]);
  }
  return bounds;
}

std::span<const BLBoxI> Region::Rects() const { return rects_; }

void Region::Coalesce() {
  if (rects_.size() < 2) {
    return;
  }

  std::sort(rects_.begin(), rects_.end(),
      [](const BLBoxI &a, const BLBoxI &b) {
        if (a.y0 != b.y0) {
          return a.y0 < b.y0;
        }
        if (a.x0 != b.x0) {
          return a.x0 < b.x0;
        }
        if (a.y1 != b.y1) {
          return a.y1 < b.y1;
        }
        return a.x1 < b.x1;
      });

  bool merged = true;
  while (merged) {
    merged = false;
    for (size_t i = 0; i < rects_.size() && !merged; ++i) {
      for (size_t j = i + 1; j < rects_.size(); ++j) {
        if (!CanMerge(rects_[i], rects_[j])) {
          continue;
        }
        rects_[i] = MergeBoxes(rects_[i], rects_[j]);
        rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(j));
        merged = true;
        break;
      }
    }
  }
}

} // namespace luna::backend::blend2d
