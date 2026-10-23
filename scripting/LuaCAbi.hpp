#pragma once
// Predeclare the public API with C linkage when compiling Lua's own sources
// as C++. Lua's private implementation keeps native C++ exception handling.
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
