# SSC for Linux

An encoder for the **Samsung Scalable Codec** (A2DP vendor codec `0x0075:0x0103`, the codec behind Galaxy Buds
"Seamless" / "SSC") and a PipeWire plugin that lets a Linux machine stream to Galaxy Buds with it.

The bitstream turned out to be CELT (Opus) with a short list of differences. They were found by emulating Samsung's
own encoder library and comparing its range-coder symbols with an Opus decoder, until every frame matched; the
findings are in [NOTES.md](NOTES.md). The code here is an independent implementation on top of the CELT sources of
Opus (BSD-3-Clause). It contains no Samsung code. The few parameter tables that define the bitstream (22-band
layout, bit-allocation table, pulse-cache index: `third_party/celt/ssc_tables.h`) are data read from Samsung's
encoder. The tools in `tools/` need Samsung's `libScalable_Encoder.so` (not distributed here) only to regenerate
those tables and to run the comparisons.

> **Disclaimer.** Independent interoperability project, not affiliated with, endorsed by or supported by Samsung.
> Samsung, Galaxy Buds and Seamless Codec are trademarks of their owners. Experimental software: it sends a vendor
> codec stream to your earbuds. Use at your own risk.
>
> **Tested only on Samsung Galaxy Buds3 Pro** (Fedora 44, PipeWire 1.6.10). Other earbuds, firmware versions and
> distributions are untested and may misbehave. The software is provided "as is", without warranty of any kind
> (see the GPL-3.0 licence); the author accepts no responsibility or liability for how it is used or for any
> damage, loss or malfunction (of earbuds, phone, computer or anything else) that results from using it.

## Status

* `libssc`: encoder for the 44.1/48 kHz "normal"/HiFi mode (16-bit, float or 32-bit input) at the five bitrate presets the Buds know
  (88, 96, 128, 192, 229 kbit/s) and for the 96 kHz **UHQ** mode (152, 291, 584 kbit/s; implemented from the emulated Samsung
  encoder, round trip tested against the CELT model, not yet confirmed on earbuds). Round-trip tests pass; against Samsung's encoder the quality is equal on tonal
  signals and lower above ~17 kHz (see NOTES.md).
* PipeWire plugin: builds, passes a host-style load test and streams to Galaxy Buds3 Pro (Fedora 44, PipeWire 1.6.10,
  WirePlumber 0.5) with the codec reported as `ssc` and no underruns. Other earbuds and distributions are untested.
* Not done: decoder (sink) side, Samsung's high-band allocation boost and per-frame bandwidth detection (we always code all 22 bands).

## Build and test

    meson setup build
    meson compile -C build
    meson test -C build

Needs `meson`, a C compiler, and for the plugin `pipewire-devel` and `bluez-libs-devel`. The plugin must be built
against the same PipeWire version it is installed into (the codec interface is private to PipeWire).

## PipeWire plugin

PipeWire loads A2DP codec plugins from a fixed list of names, so a new codec cannot simply be dropped in. This plugin
takes the `aptx` slot, which Fedora does not ship. It is installed as `libspa-codec-bluez5-aptx.so` and identifies
itself as the codec `ssc`; it borrows the aptx-hd codec id so that PipeWire prefers it over AAC. If you also install a
real aptx plugin, the two cannot coexist.

The easy way: `./install.sh` builds, runs the tests, installs (sudo) and restarts WirePlumber; `./install.sh --status`, `--uninstall`, `--dry-run`.
Rerun it after every PipeWire update.

By hand (needs root):

    sudo install -m 0755 build/libspa-codec-bluez5-aptx.so /usr/lib64/spa-0.2/bluez5/
    systemctl --user restart wireplumber

Reconnect the earbuds afterwards. Check which codec is in use:

    pw-dump | grep '"api.bluez5.codec"'

The plugin offers two profiles per earbud: `ssc` (44.1/48 kHz) and `ssc_uhq` (96 kHz, 24 bit; select it in the sound settings, PipeWire resamples).
The plugin adapts the bitrate to the Bluetooth link. It starts at the highest preset, steps down (229k, 128k, 96k, 88k) when packets pile up in the socket, and steps back up after about 10 seconds with a short queue. It skips 192k, which sounds wrong on Galaxy Buds2 Pro (see NOTES.md). WirePlumber logs each change at info level (`SSC: 229000 -> 128000 bit/s ...`).
Setting `SSC_PRESET=88k|96k|128k|192k|229k` (or `uhq152k|uhq291k|uhq584k` for UHQ) in WirePlumber's environment caps the bitrate at that preset.

Remove:

    sudo rm /usr/lib64/spa-0.2/bluez5/libspa-codec-bluez5-aptx.so
    systemctl --user restart wireplumber

If the earbuds refuse the codec, PipeWire falls back to the next one (AAC/SBC).

## Layout

    src/ssc.[ch]                    encoder API (one frame per call, header included)
    src/pipewire/                   PipeWire codec plugin
    third_party/celt/               Opus 1.5.2 CELT with the SSC changes (BSD-3-Clause), see tools/make_ssc_celt.py
    third_party/pipewire-bluez5/    PipeWire's codec plugin interface headers (MIT)
    tests/                          encoder round trip, plugin load test
    tools/                          reverse-engineering tools: Unicorn emulation of the Samsung library, range-coder
                                    tracing, differential tests, firmware helpers
    NOTES.md                        everything that was found and how

## License

Original code: GNU GPL version 3 or later (`LICENSE`, SPDX `GPL-3.0-or-later`).
Vendored code keeps its own licenses, which are GPL compatible: `third_party/celt/` is BSD-3-Clause (`COPYING.opus`),
`third_party/pipewire-bluez5/` is MIT (`COPYING.pipewire`).

## Arch Linux (AUR draft)

`packaging/arch/` contains a `PKGBUILD` (`ssc-codec-git`) with a pacman hook. The plugin takes PipeWire's `aptx` slot, so the hook copies it over
`libspa-codec-bluez5-aptx.so` of `pipewire-audio` after every upgrade (aptx stops working while it is installed; `pacman -S pipewire-audio` brings it back).
Rebuild the package after each PipeWire update. Untested on Arch so far.
