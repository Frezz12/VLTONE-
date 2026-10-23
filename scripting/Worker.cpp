#include "Api.hpp"
#include "Sdk.hpp"
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <unordered_set>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <sys/resource.h>
#endif

namespace {
using namespace vlt::script;
using Clock = std::chrono::steady_clock;
constexpr std::size_t luaMemoryLimit = 64 * 1024 * 1024;
constexpr const char *bufferType = "vlt.PCM";
struct Buffer {
    std::size_t count = 0;
    std::span<float> samples() {
        return {reinterpret_cast<float *>(this + 1), count};
    }
};
struct Runtime {
    lua_State *lua = nullptr;
    std::size_t used = 0;
    Clock::time_point deadline;
    Json sources;
    std::uint64_t sequence = 0;
    std::size_t serializationBytes = 0;
    std::unordered_set<int> callbacks;
    ~Runtime() {
        if (lua)
            lua_close(lua);
    }
    static void *allocate(void *ud, void *pointer, size_t oldSize, size_t newSize) {
        auto &self = *static_cast<Runtime *>(ud);
        if (!pointer)
            oldSize = 0;
        if (!newSize) {
            self.used -= oldSize;
            std::free(pointer);
            return nullptr;
        }
        if (newSize > luaMemoryLimit || self.used - oldSize > luaMemoryLimit - newSize)
            return nullptr;
        void *result = std::realloc(pointer, newSize);
        if (result)
            self.used = self.used - oldSize + newSize;
        return result;
    }
    static Runtime &from(lua_State *state) {
        void *data = nullptr;
        lua_getallocf(state, &data);
        return *static_cast<Runtime *>(data);
    }
    static void hook(lua_State *state, lua_Debug *) {
        if (Clock::now() > from(state).deadline)
            luaL_error(state, "Handler exceeded its 2 second execution budget; split work into "
                              "jobs or timer callbacks");
    }
    void budget() {
        deadline = Clock::now() + std::chrono::seconds(2);
    }
    Json toJson(lua_State *state, int index, int depth = 0) {
        if (depth > 32)
            throw std::runtime_error("Cyclic or deeply nested Lua value");
        if (depth == 0)
            serializationBytes = 0;
        serializationBytes += 32;
        if (serializationBytes > maxJsonBytes)
            throw std::runtime_error("API value exceeds IPC limit");
        index = lua_absindex(state, index);
        switch (lua_type(state, index)) {
        case LUA_TNIL:
            return nullptr;
        case LUA_TBOOLEAN:
            return lua_toboolean(state, index) != 0;
        case LUA_TNUMBER: {
            // Lua division produces floats even for exact frame counts. Preserve
            // integer-valued results without rounding fractions or overflowing int64.
            int exact = 0;
            const auto integer = lua_tointegerx(state, index, &exact);
            if (exact)
                return integer;
            if (!std::isfinite(lua_tonumber(state, index)))
                throw std::runtime_error("Non-finite number");
            return lua_tonumber(state, index);
        }
        case LUA_TSTRING: {
            size_t size = 0;
            const char *text = lua_tolstring(state, index, &size);
            if (size > maxJsonBytes / 2)
                throw std::runtime_error("String exceeds IPC limit");
            serializationBytes += size;
            if (serializationBytes > maxJsonBytes)
                throw std::runtime_error("API value exceeds IPC limit");
            return std::string(text, size);
        }
        case LUA_TFUNCTION: {
            lua_pushvalue(state, index);
            const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
            callbacks.insert(reference);
            return Json{{"$callback", reference}};
        }
        case LUA_TTABLE: {
            const auto length = lua_rawlen(state, index);
            if (length > 100000)
                throw std::runtime_error("Table exceeds IPC limit");
            bool array = length != 0;
            if (lua_getmetatable(state, index)) {
                luaL_getmetatable(state, "vlt.array");
                array |= lua_rawequal(state, -1, -2) != 0;
                lua_pop(state, 2);
            }
            Json result = array ? Json::array() : Json::object();
            if (array) {
                for (size_t i = 1; i <= length; ++i) {
                    lua_rawgeti(state, index, i);
                    result.push_back(toJson(state, -1, depth + 1));
                    lua_pop(state, 1);
                }
            } else {
                lua_pushnil(state);
                size_t count = 0;
                while (lua_next(state, index)) {
                    if (++count > 10000 || lua_type(state, -2) != LUA_TSTRING)
                        throw std::runtime_error("Expected string keys");
                    size_t size = 0;
                    const char *key = lua_tolstring(state, -2, &size);
                    serializationBytes += size;
                    if (serializationBytes > maxJsonBytes)
                        throw std::runtime_error("API keys exceed IPC limit");
                    result[std::string(key, size)] = toJson(state, -1, depth + 1);
                    lua_pop(state, 1);
                }
            }
            return result;
        }
        default:
            throw std::runtime_error("Unsupported API value (use audio.write for PCM)");
        }
    }
    void push(lua_State *state, const Json &value, int depth = 0) {
        if (depth > 32)
            throw std::runtime_error("Host value too deeply nested");
        if (value.is_null())
            lua_pushnil(state);
        else if (value.is_boolean())
            lua_pushboolean(state, value.get<bool>());
        else if (value.is_number_integer())
            lua_pushinteger(state, value.get<lua_Integer>());
        else if (value.is_number())
            lua_pushnumber(state, value.get<double>());
        else if (value.is_string()) {
            const auto &s = value.get_ref<const std::string &>();
            lua_pushlstring(state, s.data(), s.size());
        } else {
            lua_newtable(state);
            if (value.is_array()) {
                luaL_setmetatable(state, "vlt.array");
                int i = 0;
                for (const auto &item : value) {
                    push(state, item, depth + 1);
                    lua_rawseti(state, -2, ++i);
                }
            } else
                for (const auto &[key, item] : value.items()) {
                    push(state, item, depth + 1);
                    lua_setfield(state, -2, key.c_str());
                }
        }
    }
    void retainCallbacks(const Json &message) {
        if (!message.contains("callbacks"))
            return;
        const auto active = message.at("callbacks").get<std::unordered_set<int>>();
        std::erase_if(callbacks, [&](int reference) {
            if (active.contains(reference))
                return false;
            luaL_unref(lua, LUA_REGISTRYINDEX, reference);
            return true;
        });
    }
    static int rpc(lua_State *state) {
        bool failed = false;
        int returns = 1;
        {
            auto &self = from(state);
            try {
                const auto method = std::string(lua_tostring(state, lua_upvalueindex(1)));
                Json args = lua_isnoneornil(state, 1) ? Json::object() : self.toJson(state, 1);
                Message request{{{"type", "call"},
                                 {"id", ++self.sequence},
                                 {"method", method},
                                 {"arguments", args}},
                                {}};
                if (method == "audio.write") {
                    auto *buffer = static_cast<Buffer *>(luaL_testudata(state, 2, bufferType));
                    if (!buffer)
                        throw std::runtime_error(
                            "audio.write expects a PCM buffer as second argument");
                    request.binary.resize(buffer->count * sizeof(float));
                    std::memcpy(request.binary.data(), buffer->samples().data(),
                                request.binary.size());
                }
                write(std::cout, request);
                Message response;
                const auto waiting = Clock::now();
                if (!read(std::cin, response) || response.json.value("type", "") != "reply" ||
                    response.json.value("id", std::uint64_t(0)) != self.sequence)
                    throw std::runtime_error("Invalid host response");
                self.deadline += Clock::now() - waiting;
                self.retainCallbacks(response.json);
                if (!response.json.value("ok", false))
                    throw std::runtime_error(response.json.value("error", "Host call failed"));
                self.push(state, response.json.value("value", Json()));
                if (method == "audio.read") {
                    if (response.binary.size() % sizeof(float))
                        throw std::runtime_error("Invalid PCM block");
                    // PCM memory belongs to the Lua allocator and its limit, including on macOS.
                    auto *buffer =
                        new (lua_newuserdatauv(state, sizeof(Buffer) + response.binary.size(), 0))
                            Buffer{response.binary.size() / sizeof(float)};
                    luaL_setmetatable(state, bufferType);
                    std::memcpy(buffer->samples().data(), response.binary.data(),
                                response.binary.size());
                    returns = 2;
                }
            } catch (const std::exception &error) {
                lua_pushstring(state, error.what());
                failed = true;
            }
        }
        // Lua is built with C++ exceptions so its allocator/errors unwind these frames.
        return failed ? luaL_error(state, "%s", lua_tostring(state, -1)) : returns;
    }
    static int requireModule(lua_State *state) {
        const char *name = luaL_checkstring(state, 1);
        lua_getfield(state, LUA_REGISTRYINDEX, "vlt.loaded");
        lua_getfield(state, -1, name);
        if (!lua_isnil(state, -1))
            return 1;
        lua_pop(state, 1);
        bool failed = false;
        {
            auto &self = from(state);
            std::string path(name);
            std::replace(path.begin(), path.end(), '.', '/');
            path += ".lua";
            if (!safeRelativePath(path) || !self.sources.contains(path)) {
                lua_pushliteral(state, "Only bundled Lua source modules may be required");
                failed = true;
            } else {
                const auto source = self.sources[path].get<std::string>();
                const auto chunk = "@" + path;
                failed = luaL_loadbufferx(state, source.data(), source.size(), chunk.c_str(),
                                          "t") != LUA_OK;
            }
        }
        if (failed)
            return lua_error(state);
        // Mark cycles before executing; the registry is private to this state.
        lua_pushboolean(state, 1);
        lua_setfield(state, -3, name);
        if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
            lua_pushnil(state);
            lua_setfield(state, -3, name);
            return lua_error(state);
        }
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            lua_pushboolean(state, 1);
        }
        lua_pushvalue(state, -1);
        lua_setfield(state, -3, name);
        return 1;
    }
    static int print(lua_State *state) {
        auto &self = from(state);
        std::string text;
        for (int i = 1; i <= lua_gettop(state); ++i) {
            if (i > 1)
                text += '\t';
            if (const auto *s = lua_tostring(state, i))
                text += s;
            else
                text += luaL_typename(state, i);
            if (text.size() > 8192) {
                text.resize(8192);
                break;
            }
        }
        write(std::cout, {{{"type", "log"}, {"text", text}}, {}});
        return 0;
    }
    void initialize() {
        lua = lua_newstate(allocate, this, 0);
        if (!lua)
            throw std::runtime_error("Could not allocate Lua state");
        const luaL_Reg libs[] = {{"_G", luaopen_base},
                                 {LUA_TABLIBNAME, luaopen_table},
                                 {LUA_STRLIBNAME, luaopen_string},
                                 {LUA_MATHLIBNAME, luaopen_math},
                                 {LUA_UTF8LIBNAME, luaopen_utf8},
                                 {LUA_COLIBNAME, luaopen_coroutine},
                                 {nullptr, nullptr}};
        for (const auto *lib = libs; lib->func; ++lib) {
            luaL_requiref(lua, lib->name, lib->func, 1);
            lua_pop(lua, 1);
        }
        for (const char *name : {"dofile", "loadfile", "load", "collectgarbage"}) {
            lua_pushnil(lua);
            lua_setglobal(lua, name);
        }
        lua_pushcfunction(lua, requireModule);
        lua_setglobal(lua, "require");
        lua_newtable(lua);
        lua_setfield(lua, LUA_REGISTRYINDEX, "vlt.loaded");
        lua_pushcfunction(lua, print);
        lua_setglobal(lua, "print");
        luaL_newmetatable(lua, "vlt.array");
        lua_pop(lua, 1);
        luaL_newmetatable(lua, bufferType);
        lua_pushliteral(lua, "PCM buffer");
        lua_setfield(lua, -2, "__metatable");
        lua_pushcfunction(lua, [](lua_State *l) {
            static_cast<Buffer *>(luaL_checkudata(l, 1, bufferType))->~Buffer();
            return 0;
        });
        lua_setfield(lua, -2, "__gc");
        lua_pushcfunction(lua, [](lua_State *l) {
            lua_pushinteger(l, static_cast<Buffer *>(luaL_checkudata(l, 1, bufferType))->count);
            return 1;
        });
        lua_setfield(lua, -2, "__len");
        lua_pushcfunction(lua, [](lua_State *l) {
            auto b = static_cast<Buffer *>(luaL_checkudata(l, 1, bufferType))->samples();
            auto i = luaL_checkinteger(l, 2);
            if (i < 1 || size_t(i) > b.size())
                return luaL_error(l, "PCM index out of bounds");
            lua_pushnumber(l, b[i - 1]);
            return 1;
        });
        lua_setfield(lua, -2, "__index");
        lua_pushcfunction(lua, [](lua_State *l) {
            auto b = static_cast<Buffer *>(luaL_checkudata(l, 1, bufferType))->samples();
            auto i = luaL_checkinteger(l, 2);
            double value = luaL_checknumber(l, 3);
            if (i < 1 || size_t(i) > b.size() || !std::isfinite(float(value)))
                return luaL_error(l, "Invalid PCM sample");
            b[i - 1] = float(value);
            return 0;
        });
        lua_setfield(lua, -2, "__newindex");
        lua_pop(lua, 1);
        lua_newtable(lua);
        lua_pushinteger(lua, 1);
        lua_setfield(lua, -2, "sdk");
        lua_pushcfunction(lua, [](lua_State *l) {
            luaL_checktype(l, 1, LUA_TTABLE);
            lua_settop(l, 1);
            luaL_setmetatable(l, "vlt.array");
            return 1;
        });
        lua_setfield(lua, -2, "array");
        for (const auto &spec : apiCatalog()["methods"]) {
            const auto name = spec["name"].get<std::string>();
            const auto dot = name.find('.');
            if (dot != name.npos) {
                const auto group = name.substr(0, dot);
                lua_getfield(lua, -1, group.c_str());
                if (lua_isnil(lua, -1)) {
                    lua_pop(lua, 1);
                    lua_newtable(lua);
                    lua_pushvalue(lua, -1);
                    lua_setfield(lua, -3, group.c_str());
                }
            }
            lua_pushstring(lua, name.c_str());
            lua_pushcclosure(lua, rpc, 1);
            lua_setfield(lua, -2, (dot == name.npos ? name : name.substr(dot + 1)).c_str());
            if (dot != name.npos)
                lua_pop(lua, 1);
        }
        lua_setglobal(lua, "vlt");
        lua_sethook(lua, hook, LUA_MASKCOUNT, 1000);
        budget();
    }
    bool protectedCall(int arguments, int results) {
        budget();
        if (lua_pcall(lua, arguments, results, 0) == LUA_OK)
            return true;
        const char *message = lua_tostring(lua, -1);
        const std::string error = message ? message : "Lua memory limit exceeded";
        write(std::cout, {{{"type", "error"}, {"error", error}}, {}});
        lua_settop(lua, 0);
        return false;
    }
};
} // namespace

int scriptMain(int argc, char **argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#else
    // Secondary containment for C++ buffers/JSON as well as Lua's allocator.
    rlimit limit{512u * 1024u * 1024u, 512u * 1024u * 1024u};
    setrlimit(RLIMIT_AS, &limit);
#endif
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--sdk") {
            std::cout << vlt::script::apiCatalog().dump(2) << '\n';
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--write-sdk") {
            vlt::script::writeSdk(std::filesystem::u8path(argv[2]));
            return 0;
        }
        if (argc == 3 && std::string_view(argv[1]) == "--check") {
            if (std::filesystem::file_size(std::filesystem::u8path(argv[2])) >
                vlt::script::maxJsonBytes / 2) {
                std::cerr << "Source exceeds 1 MiB\n";
                return 1;
            }
            std::ifstream file(std::filesystem::u8path(argv[2]), std::ios::binary);
            if (!file) {
                std::cerr << "Cannot open source\n";
                return 1;
            }
            const std::string source{std::istreambuf_iterator<char>(file), {}};
            if (source.size() > vlt::script::maxJsonBytes / 2)
                return 1;
            Runtime runtime;
            runtime.initialize();
            const int status =
                luaL_loadbufferx(runtime.lua, source.data(), source.size(), argv[2], "t");
            if (status != LUA_OK)
                std::cerr << lua_tostring(runtime.lua, -1) << '\n';
            return status == LUA_OK ? 0 : 1;
        }
        std::unique_ptr<Runtime> runtime;
        vlt::script::write(std::cout, {{{"type", "ready"}, {"protocol", 1}}, {}});
        vlt::script::Message message;
        while (vlt::script::read(std::cin, message)) {
            const auto type = message.json.value("type", "");
            if (type == "reset") {
                runtime.reset();
                vlt::script::write(std::cout, {{{"type", "ready"}, {"protocol", 1}}, {}});
            } else if (type == "run" || type == "check") {
                runtime = std::make_unique<Runtime>();
                runtime->initialize();
                runtime->sources = message.json.at("sources");
                if (!runtime->sources.is_object())
                    throw std::runtime_error("Invalid sources");
                const auto entry = message.json.at("entry").get<std::string>();
                const auto source = runtime->sources.at(entry).get<std::string>();
                const auto name = "@" + entry;
                if (luaL_loadbufferx(runtime->lua, source.data(), source.size(), name.c_str(),
                                     "t") != LUA_OK) {
                    vlt::script::write(
                        std::cout,
                        {{{"type", "error"}, {"error", lua_tostring(runtime->lua, -1)}}, {}});
                } else if (type == "check" || runtime->protectedCall(0, 1)) {
                    vlt::script::Json result;
                    if (type != "check") {
                        try {
                            result = runtime->toJson(runtime->lua, -1);
                        } catch (const std::exception &error) {
                            lua_settop(runtime->lua, 0);
                            vlt::script::write(std::cout,
                                               {{{"type", "error"}, {"error", error.what()}}, {}});
                            continue;
                        }
                    }
                    lua_settop(runtime->lua, 0);
                    vlt::script::write(std::cout, {{{"type", "idle"}, {"value", result}}, {}});
                }
            } else if (type == "event" && runtime) {
                runtime->retainCallbacks(message.json);
                const int callback = message.json.at("callback");
                lua_rawgeti(runtime->lua, LUA_REGISTRYINDEX, callback);
                runtime->push(runtime->lua, message.json.value("value", vlt::script::Json()));
                if (runtime->protectedCall(1, 0))
                    vlt::script::write(std::cout, {{{"type", "idle"}}, {}});
            } else
                throw std::runtime_error("Unexpected host message");
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> text;
    std::vector<char *> arguments;
    text.reserve(argc);
    arguments.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        const auto utf8 = std::filesystem::path(argv[i]).u8string();
        text.emplace_back(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    }
    for (auto &value : text)
        arguments.push_back(value.data());
    return scriptMain(argc, arguments.data());
}
#else
int main(int argc, char **argv) {
    return scriptMain(argc, argv);
}
#endif
