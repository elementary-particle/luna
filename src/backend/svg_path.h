#pragma once

#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>

namespace luna::backend {

// SVG path data only, not an SVG document. Path::ArcTo takes degrees.
template <class Path> std::optional<Path> ParseSvgPath(std::string_view data) {
  size_t pos = 0;
  auto whitespace = [&] {
    while (pos < data.size() && (data[pos] == ' ' || data[pos] == '\t' ||
           data[pos] == '\r' || data[pos] == '\n')) ++pos;
  };
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  auto number = [&](double &value, bool separator, bool flag = false) {
    whitespace();
    if (separator && pos < data.size() && data[pos] == ',') { ++pos; whitespace(); }
    if (pos == data.size()) return false;
    if (flag) {
      if (data[pos] != '0' && data[pos] != '1') return false;
      value = data[pos++] - '0';
      return true;
    }
    size_t start = pos;
    if (data[pos] == '+' || data[pos] == '-') ++pos;
    size_t digits = 0;
    while (pos < data.size() && digit(data[pos])) { ++pos; ++digits; }
    if (pos < data.size() && data[pos] == '.') {
      ++pos;
      while (pos < data.size() && digit(data[pos])) { ++pos; ++digits; }
    }
    if (!digits) return false;
    if (pos < data.size() && (data[pos] == 'e' || data[pos] == 'E')) {
      ++pos;
      if (pos < data.size() && (data[pos] == '+' || data[pos] == '-')) ++pos;
      size_t exponent = pos;
      while (pos < data.size() && digit(data[pos])) ++pos;
      if (exponent == pos) return false;
    }
    if (data[start] == '+') ++start;
    auto result = std::from_chars(data.data() + start, data.data() + pos, value);
    return result.ec == std::errc{} && result.ptr == data.data() + pos && std::isfinite(value);
  };
  Path path;
  double x = 0, y = 0, sx = 0, sy = 0, cx = 0, cy = 0;
  char command = 0, previous = 0;
  bool first = true;
  while (true) {
    whitespace();
    if (pos == data.size()) return path;
    char token = data[pos];
    bool explicit_command = (token >= 'A' && token <= 'Z') || (token >= 'a' && token <= 'z');
    if (explicit_command) { command = token; ++pos; }
    bool relative = command >= 'a' && command <= 'z';
    char op = relative ? command - ('a' - 'A') : command;
    if (first && op != 'M') return std::nullopt;
    if (op == 'Z') {
      path.Close(); x = sx; y = sy; previous = op; command = 0;
      continue;
    }
    int count = 0;
    switch (op) {
    case 'H': case 'V': count = 1; break;
    case 'M': case 'L': case 'T': count = 2; break;
    case 'S': case 'Q': count = 4; break;
    case 'C': count = 6; break;
    case 'A': count = 7; break;
    default: return std::nullopt;
    }
    double v[7]{};
    for (int i = 0; i < count; ++i)
      if (!number(v[i], i != 0 || !explicit_command, op == 'A' && (i == 3 || i == 4)))
        return std::nullopt;
    if (op == 'A' && (v[0] < 0 || v[1] < 0)) return std::nullopt;
    if (relative) {
      if (op == 'H') v[0] += x;
      else if (op == 'V') v[0] += y;
      else if (op == 'A') { v[5] += x; v[6] += y; }
      else for (int i = 0; i < count; i += 2) { v[i] += x; v[i + 1] += y; }
    }
    for (int i = 0; i < count; ++i) if (!std::isfinite(v[i])) return std::nullopt;
    double rx = x, ry = y;
    if ((op == 'S' && (previous == 'C' || previous == 'S')) ||
        (op == 'T' && (previous == 'Q' || previous == 'T'))) {
      rx = x + (x - cx); ry = y + (y - cy);
      if (!std::isfinite(rx) || !std::isfinite(ry)) return std::nullopt;
    }
    switch (op) {
    case 'M': path.MoveTo(v[0], v[1]); sx = v[0]; sy = v[1]; break;
    case 'L': path.LineTo(v[0], v[1]); break;
    case 'H': path.LineTo(v[0], y); break;
    case 'V': path.LineTo(x, v[0]); break;
    case 'C': path.CubicTo(v[0], v[1], v[2], v[3], v[4], v[5]); cx = v[2]; cy = v[3]; break;
    case 'S': path.CubicTo(rx, ry, v[0], v[1], v[2], v[3]); cx = v[0]; cy = v[1]; break;
    case 'Q': path.QuadTo(v[0], v[1], v[2], v[3]); cx = v[0]; cy = v[1]; break;
    case 'T': path.QuadTo(rx, ry, v[0], v[1]); cx = rx; cy = ry; break;
    case 'A': path.ArcTo(v[0], v[1], v[2], v[3] != 0, v[4] != 0, v[5], v[6]); break;
    }
    if (op == 'H') x = v[0];
    else if (op == 'V') y = v[0];
    else { x = v[count - 2]; y = v[count - 1]; }
    previous = op; first = false;
    if (op == 'M') command = relative ? 'l' : 'L';
  }
}

} // namespace luna::backend
