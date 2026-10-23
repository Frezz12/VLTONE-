Lua 5.5.1, released 2026-08-03, from https://www.lua.org/ftp/lua-5.5.1.tar.gz.

SHA-256: `1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce`.
The original archive is retained so offline builds use identical source. CMake
checks its digest before extraction. The application only links Lua into its
isolated extension helper. No native module loader is exposed to extensions.
