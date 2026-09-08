# VLTONE portable theme format

`.vlttheme` is a single, versioned binary container. Version 1 stores the
palette, appearance preferences, local backgrounds and custom fonts required to
reproduce a theme on another computer. Project data and notebook content are not
part of a theme.

All integers use big-endian byte order. The file begins with:

| Field | Type | Value |
| --- | --- | --- |
| Magic | 8 bytes | `VLTTHEME` |
| Container version | `uint32` | `1` |
| Manifest length | `uint32` | UTF-8 JSON byte length |
| Manifest | bytes | Compact UTF-8 JSON |

The manifest identifies the format and package, contains the serialized palette
and appearance settings, and lists each unique resource with its original base
name, decimal byte size and lowercase SHA-256 identifier. Appearance sections
refer to resources by that identifier; absolute source paths are never stored.

Resource blocks follow in the same order as the manifest list. Each block is a
`uint32` identifier length, the UTF-8 identifier, a `uint64` byte length and the
unchanged resource bytes. Media is deliberately not recompressed because common
image and video formats are already compressed and must remain directly
decodable by Qt Multimedia.

Import calculates the SHA-256 of the entire package, verifies every resource
while streaming it to a temporary directory, and publishes the library entry
only after verification succeeds. Installed assets use generated names inside
the application's local `Themes/assets/<package-sha256>` directory. This makes
the installed theme independent of both the original `.vlttheme` file and the
files from which it was exported.

Readers reject unknown versions, oversized manifests, duplicate resource IDs,
unsafe names, mismatched sizes or hashes, incomplete blocks and trailing data.
The versioned header permits a future reader to add a new format without
guessing how an older file is laid out.
