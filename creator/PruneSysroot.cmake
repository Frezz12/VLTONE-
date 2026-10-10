# macOS packaging: `codesign --deep` treats any directory whose name contains
# a dot as a bundle and then fails with "bundle format unrecognized, invalid,
# or unsuitable" when that directory is not a real bundle.  The WASI sysroot
# ships two such layouts:
#
#   lib/<triple>/llvm-lto/<llvm-version>-wasi-sdk
#       LTO copies of crt1.o and the wasi-libc archives.  Only reachable
#       through -flto, which the Creator compiler never passes, and already
#       duplicated (in their plain form) directly in lib/<triple>/.
#
#   share/libc++/v1/std.compat
#       C++23 `import std.compat` module sources, unused by the Creator
#       toolchain.
#
# Drop the first, rename the second, so the bundled VLTONE.app can be signed
# and verified with --deep.
if(NOT DEFINED SYSROOT)
  message(FATAL_ERROR "PruneSysroot.cmake requires -DSYSROOT=<copied sysroot>")
endif()
file(GLOB _prune_lto_dirs "${SYSROOT}/lib/*/llvm-lto")
foreach(_prune_dir IN LISTS _prune_lto_dirs)
  message(STATUS "CreatorTools: pruning ${_prune_dir}")
  file(REMOVE_RECURSE "${_prune_dir}")
endforeach()
set(_prune_std_compat "${SYSROOT}/share/libc++/v1/std.compat")
set(_prune_std_compat_renamed "${SYSROOT}/share/libc++/v1/std_compat")
if(EXISTS "${_prune_std_compat}")
  # The sysroot is copied again on every build, so a previous rename may have
  # left the target behind; drop it and replace it with the fresh copy.
  file(REMOVE_RECURSE "${_prune_std_compat_renamed}")
  file(RENAME "${_prune_std_compat}" "${_prune_std_compat_renamed}")
endif()
