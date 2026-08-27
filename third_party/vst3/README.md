# VST3 — vendored `pluginterfaces`

Upstream: https://github.com/steinbergmedia/vst3_pluginterfaces
Version:  3.8.0_build_66
Source:   https://github.com/steinbergmedia/vst3_pluginterfaces/archive/refs/tags/v3.8.0_build_66.tar.gz
sha256:   c922f0a35f2c093dfcb24ccd95282e70d00cc2de42fc0c8ac1577398ffe7b6d4
License:  MIT (see LICENSE.txt)

**The version matters for the licence, not only for the ABI.** Steinberg
relicensed the SDK to MIT in **3.8.0**; the change was not applied to older
tags. `LICENSE.txt` at v3.7.14 is still the dual GPLv3 / proprietary Steinberg
licence, which would require a signed agreement to ship a closed-source host.
Do not move this dependency *backwards*, and check `LICENSE.txt` — not a blog
post or an answer online, nearly all of which describe the pre-3.8 situation —
whenever it moves.

The tarball's contents live under `pluginterfaces/`, because the SDK includes
itself as `<pluginterfaces/...>` and that path has to resolve.

Only `pluginterfaces` is vendored. A host needs the interface declarations and
the four `base/*.cpp` files that define the IIDs and the `FUnknown` helpers;
it does not need `base`, `public_sdk` or `vstgui`. `vstgui` in particular is
**not** MIT — it carries its own Steinberg licence — and it is the GUI toolkit
for the *plugin* side, so a host has no reason to pull it in.

Vendored rather than fetched at configure time because this repository is not a
git checkout, so a submodule is not available, and a plugin ABI is exactly the
kind of dependency that should be pinned in-tree and reviewed when it moves.
`README.upstream.md` is Steinberg's own README, kept as it came.
