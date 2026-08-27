# Third-party source notices

## FST 0.177.0

VLT Studio Pro vendors the clean-room FST compatibility headers
`aeffect.h`, `aeffectx.h`, and `fst.h` to describe the legacy AEffect plugin
ABI. FST is copyright 2019 IOhannes m zmölnig and IEM and is distributed under
the GNU General Public License, version 3 or later.

- Upstream: https://git.iem.at/zmoelnig/FST
- Source release: `fst_0.177.0.orig.tar.bz2`
- Archive SHA-256: `580FDFB93789A3842C913AB6EC63B0539271758E1B05470A116BA36FDFD5A300`
- Local license: `third_party/fst/LICENSE.txt`
- Upstream README: `third_party/fst/README.upstream.md`

No header from the withdrawn proprietary Steinberg VST2 SDK is included.

## Other vendored SDKs

- CLAP headers: MIT; see `third_party/clap/LICENSE` and
  `third_party/clap/README.md`.
- VST3 pluginterfaces: GPLv3 branch; see `third_party/vst3/LICENSE.txt` and
  `third_party/vst3/README.md`.
