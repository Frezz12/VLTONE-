# Run on the staged copies, before the enclosing app is signed and packaged.
include(BundleUtilities)
fixup_bundle("${TOOLS_DIRECTORY}/daw_creator_compiler"
  "${TOOLS_DIRECTORY}/clang;${TOOLS_DIRECTORY}/wasm-ld;${TOOLS_DIRECTORY}/wamrc"
  "${LIBRARY_DIRECTORIES}")
