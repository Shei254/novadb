// Copyright (C) 2020 THL A29 Limited, a Tencent company.  All rights reserved.
// Please refer to the license text that comes with this novadb open source
// project for additional information.

#ifndef SRC_novadbPLUS_SCRIPT_LUA_STATE_H_
#define SRC_novadbPLUS_SCRIPT_LUA_STATE_H_

#include <memory>
#include <string>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "novadbplus/commands/command.h"
#include "novadbplus/network/blocking_tcp_client.h"
#include "novadbplus/script/rand.h"
#include "novadbplus/script/script_manager.h"
#include "novadbplus/server/session.h"

namespace novadbplus {

class ScriptManager;

#define LUA_GC_CYCLE_PERIOD 50

class LuaState {
 public:
  explicit LuaState(std::shared_ptr<ServerEntry> svr, std::string id);
  ~LuaState();

  lua_State* initLua(int setup);
  const std::string& Id() const {
    return _id;
  }
  Expected<std::string> evalCommand(Session* sess);
  Expected<std::string> evalShaCommand(Session* sess);
  void LuaClose();
  bool luaWriteDirty() const {
    return lua_write_dirty;
  }
  void setLastEndTime(uint64_t val) {
    lua_time_end = val;
  }
  uint64_t lastEndTime() const {
    return lua_time_end;
  }
  void setRunning(bool val) {
    running = val;
  }
  bool isRunning() const {
    return running;
  }
  static std::string getShaEncode(const std::string& script);
  Expected<std::string> tryLoadLuaScript(const std::string& script) {
    return luaCreateFunction(_lua, script);
  }

 private:
  Expected<std::string> evalGenericCommand(Session* sess, int evalsha);
  void updateFakeClient();
  static void sha1hex(char* digest, char* script, size_t len);
  static int luaRedisSha1hexCommand(lua_State* lua);
  void luaRemoveUnsupportedFunctions(lua_State* lua);
  int luaRedisReplicateCommandsCommand(lua_State* lua);
  Expected<std::string> luaCreateFunction(lua_State* lua,
                                          const std::string& body);
  Expected<std::string> luaReplyToRedisReply(lua_State* lua);
  static int luaRedisCallCommand(lua_State* lua);
  static int luaRedisPCallCommand(lua_State* lua);
  static int luaRedisGenericCommand(lua_State* lua, int raise_error);
  static void luaMaskCountHook(lua_State* lua, lua_Debug* ar);
  void pushThisToLua(lua_State* lua);
  static LuaState* getLuaStateFromLua(lua_State* lua);
  static int redis_math_random(lua_State* L);
  static int redis_math_randomseed(lua_State* L);

 private:
  std::string _id;
  lua_State* _lua;
  std::shared_ptr<ServerEntry> _svr;
  ScriptManager* _scriptMgr;
  Session* _sess;
  std::unique_ptr<LocalSessionGuard> _fakeSess;
  std::atomic<bool> running{false};
  int inuse = 0;               /* Recursive calls detection. */
  uint64_t lua_time_start{0};  // ms
  uint64_t lua_time_end{0};    // ms
  int lua_timedout;  // True if we reached the time limit for script execution.
  std::atomic<int>
    lua_write_dirty;          // True if a write command was called
                              // during the execution of the current script.
  int lua_random_dirty;       // True if a random command was called during the
                              // execution of the current script.
  int lua_replicate_commands; /* True if we are doing single commands repl. */
  int lua_multi_emitted;      /* True if we already proagated MULTI. */
  // bool has_command_error;  // if one redis command has error,
  // dont commit all transactions.
  RedisRandom _rand;
  int _gc_count;
};

}  // namespace novadbplus
#endif  // SRC_novadbPLUS_SCRIPT_LUA_STATE_H_
