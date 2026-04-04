#ifndef LUNA_CANVAS_BINDINGS_H
#define LUNA_CANVAS_BINDINGS_H

#include "canvas.h"

namespace luna::canvas {

void RegisterPathBindings(lua_State *L);
void RegisterPathCanvasMethods(lua_State *L);
void RegisterPathLuaHelpers(lua_State *L);

void RegisterParagraphBindings(lua_State *L);
void RegisterParagraphCanvasMethods(lua_State *L);
void RegisterParagraphLuaHelpers(lua_State *L);

void RegisterCanvasLuaHelpers(lua_State *L);

} // namespace luna::canvas

#endif
