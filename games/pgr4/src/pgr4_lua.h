// The game's UI is Lua 5.1: 247 compiled scripts in a "BZUI" archive inside the image (the
// menus, the HUD layout, the screen geometry), run by the Lua the game links (lapi.c's
// functions are at 0x82505960-0x82507400, in lapi.c's order). pgr4_lua.cpp replaces the
// archive's chunk loader so the game also loads scripts of this project (games/pgr4/ui/, written
// in Fennel): screens of its own, and patches that run after a game script of the same name.
// Their host functions are the global table Pgr4: Pgr4.Text, and those of Pgr4LuaFunctions().
//
// The game's text functions (UIButtons.AddItem, AddItemHelp, a button's text) take a string ID,
// the hash its string table (sub_8289B1D0) finds the string by, as UIText holds them; a Lua
// string there is a name to look up or a variable ("0.PROFILE.NAME"), never shown as it is.
// Pgr4.Text(s) is the ID of the port's own string s, which the table's lookup answers.
#pragma once

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pgr4::lua {

// lua.h's type tags.
enum Type : int { kNone = -1, kNil, kBoolean, kLightUserdata, kNumber, kString, kTable, kFunction };

// The guest lua_State L, driven from the host on the guest thread that runs it: each call is the
// game's own Lua API function, run on ctx. Strings go to Lua through the guest stack below r1.
class Lua {
 public:
  static constexpr int kGlobalsIndex = -10002;  // LUA_GLOBALSINDEX

  Lua(PPCContext& ctx, uint8_t* base, uint32_t L) : ctx_(ctx), base_(base), L_(L) {}

  int GetTop();
  void SetTop(int index);
  void Pop(int n) { SetTop(-n - 1); }
  void Insert(int index);
  void PushValue(int index);
  int TypeOf(int index);
  double ToNumber(int index);
  bool ToBoolean(int index);
  std::string ToString(int index);  // "" for what is not a string or number

  void PushNil();
  void PushNumber(double n);
  void PushBoolean(bool b);
  void PushString(std::string_view s);
  void PushFunction(uint32_t guest_function);  // a lua_CFunction at that guest address

  void GetField(int index, std::string_view key);
  void SetField(int index, std::string_view key);
  void GetGlobal(std::string_view name) { GetField(kGlobalsIndex, name); }
  void SetGlobal(std::string_view name) { SetField(kGlobalsIndex, name); }
  void CreateTable(int array, int records);
  void RawSetI(int index, int n);

  int PCall(int args, int results);
  // The string ID of `s` (UTF-8) for the game's text functions; the same ID for the same text.
  uint32_t Text(std::string_view s);
  // luaL_loadbuffer: the chunk at guest address `chunk`, `size` bytes, as `name`.
  int LoadBuffer(uint32_t chunk, uint32_t size, std::string_view name);

 private:
  uint64_t Call(PPCFunc* f, int64_t a = 0, int64_t b = 0, int64_t c = 0);
  // `s` NUL-terminated below r1, which moves past it; the caller restores r1.
  uint32_t StackString(std::string_view s);

  PPCContext& ctx_;
  uint8_t* base_;
  uint32_t L_;
};

// A host function in the Pgr4 table: a lua_CFunction, called with L in r3, its arguments on L's
// stack; it returns its result count in r3 (Results).
struct Function {
  std::string_view name;
  PPCFunc* function;
};
std::span<const Function> Pgr4LuaFunctions();  // pgr4_settings.cpp

inline void Results(PPCContext& ctx, int n) { ctx.r3.u64 = uint64_t(n); }

}  // namespace pgr4::lua
