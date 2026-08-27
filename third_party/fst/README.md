# FST — legacy VST compatibility headers

Upstream: https://git.iem.at/zmoelnig/FST
Version:  0.177.0
Source:   https://archive.ubuntu.com/ubuntu/pool/universe/f/fst/fst_0.177.0.orig.tar.bz2
sha256:   580fdfb93789a3842c913ab6ec63b0539271758e1b05470a116ba36fdfd5a300
License:  GPL-3.0-or-later (see LICENSE.txt)

Only `aeffect.h`, `aeffectx.h`, and their shared `fst.h` are vendored. They
describe the withdrawn VST 1.x/2.x AEffect ABI without using Steinberg's
unavailable proprietary SDK. No FST host helpers or plugin framework code is
used; VLT Studio Pro adapts the ABI directly to `PluginInstance`.

The unmodified upstream README is included as `README.upstream.md`.
