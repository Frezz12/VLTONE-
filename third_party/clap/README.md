# CLAP — vendored headers

Upstream: https://github.com/free-audio/clap
Version:  1.2.10
Source:   https://github.com/free-audio/clap/archive/refs/tags/1.2.10.tar.gz
sha256:   58fdb977c6678859c15d57942378b651399178f49a3a8ecad1813b9f231dc096
License:  MIT (see LICENSE)

Only `include/` is vendored — CLAP is header-only, so that is the whole SDK.
Nothing here is modified; to move versions, replace `include/` wholesale from a
release tarball and update the version and hash above.

Vendored rather than fetched at configure time because this repository is not a
git checkout, so a submodule is not available, and a plugin ABI is exactly the
kind of dependency that should be pinned in-tree and reviewed when it moves.
