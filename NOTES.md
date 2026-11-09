# SSC port to Linux (PipeWire SPA plugin): recon notes

## Source firmware
- `SAMFW.COM_SM-S901B_EUX_S901BXXS1AVB3_fac.zip`: Galaxy S22 (Exynos), Android 12 / One UI 4.1, build Feb 2022.
- Extracted: `AP_*.tar.md5` -> `super.img.lz4` -> `work/super/super.raw.img` -> `lpunpack` -> `work/super/parts/{system,vendor,product,odm}.img` (all F2FS).
- Mounted read-only with `udisksctl loop-setup -r -f X.img` + `udisksctl mount -b /dev/loopN -o ro`
  at `/run/media/$USER/{_,vendor,product}` (`_` = system).
- Copies of the relevant files are in `work/libs/`.

## What this firmware contains
- Only the **Samsung Scalable Codec** (SSC, incl. a "SSC HiFi" mode). No "Seamless"/"UHQ" strings anywhere in `libbluetooth.so`.
  The newer Seamless/UHQ variant (Buds2 Pro era, One UI 4.1.1/5+) needs a newer firmware to confirm.

## Key files (system partition, aarch64)
- `/system/lib64/libScalable_Encoder.so` (102 KB, dated 2021-01-01): **not stripped**, 88 exported symbols.
  Imports only malloc/free/memcpy/memset/__stack_chk_fail (+ liblog/libc/libm/libdl sonames), so it is easy to emulate.
  - API: `ssc_encoder_get_size/create/init/destroy`, `ssc_encode` (19.7 KB), `ssc_custom_mode_create`, `ssc_mdct_forward`, `ssc_fft`, ...
  - Internals use CELT/Opus names: `ec_enc_*` (range coder), `ec_laplace_encode`, `alg_quant`, `normalise_bands`,
    `haar1`, `stereo_itheta`, `patch_transient_decision`, `tf_select_table`, `eMeans`. => SSC is a CELT/Opus derivative.
  - Also embeds a `SoundBooster_DRC` (dynamic range control) block.
  - `ssc_encoder_create(Fs, channels, ?, int *err)`; internal custom mode uses frame size 0x360 (864); Fs 48000 is remapped to 44100 internally.
- `/system/lib64/libbluetooth.so`: dlopens `libScalable_Encoder.so` ("cannot open SSC encoder library"); contains
  `A2DP_ParseInfoSsc`, `A2DP_VendorCodecEqualsSsc`, `a2dp_vendor_ssc_encoder[_update]`, `a2dp_ssc_encode_frames`,
  `a2dp_ssc_get_num_frame`, `bta_av_co_is_ssc_hifi_codec`, `btif_av_need_dual_play_for_ssc`.
  Function names live in `.gnu_debugdata` (not visible to `readelf`); radare2 `aaa` (~10 min) loads them. Code is a contiguous block
  at `0x335700-0x338600`: `A2DP_Vendor*Ssc` helpers, `A2dpCodecConfigSsc::{init,setCodecConfig,setPeerCodecCapabilities}`,
  `A2DP_VendorLoadEncoderSsc` (0x3370c8), `a2dp_vendor_ssc_encoder_update` (0x337630), encode loop `fcn.003377b0` (0x3377b0).

## A2DP negotiation (verified in disassembly of libbluetooth.so, Android 12 / One UI 4.1)
Media codec info element is 10 bytes (`A2DP_VendorCodecEqualsSsc`, `...GetTrackSampleRateSsc`, `setCodecConfig`):

| byte | value |
|------|-------|
| 0 | `0x09` (length of the rest) |
| 1 | media type, `<= 0x0f` (audio = 0x00) |
| 2 | `0xFF` (non-A2DP / vendor codec) |
| 3-6 | vendor ID, little endian: `0x00000075` (Samsung) |
| 7-8 | vendor codec ID, little endian: `0x0103` |
| 9 | the single SSC config/capability byte (below) |

Byte 9:
- bits 7..4, sample rate: `0x10`=48000, `0x20`=44100, `0x40`=32000, `0x80`=96000 (`GetTrackSampleRate`; exactly one bit must be set unless bit 2 is set).
  In `setCodecConfig` these map to BTAV rate flags 0x10->0x02(48k), 0x20->0x01(44.1k), 0x80->0x08(96k); 0x40 is not handled there.
- bit 2 (`0x04`): "HiFi" mode: sample rate is forced to 48000 and track bits per sample = 24 (otherwise 16). When the chosen config is HiFi, byte 9 is `0x04 | (peer & 0x08)`.
- bit 3 (`0x08`): bitrate limitation flag (`GetTrackBitrateLimitation` returns true when the bit is clear).
- bits 1..0: not used by the helpers seen so far.
- `A2DP_VendorCodecEqualsSsc` compares only the upper nibble (`(a^b) < 0x10`).
- 96 kHz (`0x80`) makes `GetTrackBitsPerSample` return `0x22` (meaning unknown; probably a Samsung-specific sample-format code).
- `A2DP_VendorUsesRtpHeaderSsc` always returns true: media packets carry the standard RTP header.
- `A2DP_VendorGetPacketTimestampSsc` copies the first 32-bit word of the buffer as the timestamp.
- Android 12 build: no "Seamless"/"UHQ" strings, so UHQ capability bits (if any) are not described here. Buds3 Pro Modalias is `v0075pA013`.

## Still to determine
1. (done, see above) vendor ID / codec ID / capability byte. Still: read the Buds3 Pro's real capability byte from BlueZ
   (needs the buds connected to this PC; they are paired, `XX:XX:XX:XX:XX:XX`, currently disconnected) to see UHQ bits.
2. Media packet framing (`a2dp_ssc_encode_frames`, `a2dp_ssc_get_num_frame`): frames per packet, header bytes, bitrate/frame size per mode.
3. Meaning of `ssc_encoder_create/init` args and the ctl calls made in init (fcn.17930 / fcn.17940).
4. The encoder algorithm (`ssc_encode`) vs. Opus CELT reference.

## Plan
1. Oracle: run `libScalable_Encoder.so` under Unicorn (aarch64 emulation, malloc/memcpy/memset hooks) to produce reference bitstreams.
2. Reimplement the encoder in C (starting from the Opus CELT structure), diff against the oracle until bit-exact.
3. Wrap as `libspa-codec-bluez5-ssc.so` (`spa_bluez5_codec_a2dp`), build against PipeWire 1.6.10.
4. Do not redistribute Samsung binaries; the plugin must be an independent reimplementation.

## Encoder oracle (works): `tools/ssc_oracle.py`
Run with the project venv (`.venv`, unicorn 2.1.4 + pyelftools): `.venv/bin/python -I tools/ssc_oracle.py work/libs/libScalable_Encoder.so --fs 48000 --ch 2 --bits 16 --rate 128000 --frames 4`.
Verified API (from disassembly + emulation):
- `ssc_encoder_get_size(channels, Fs)` -> 9620 bytes for stereo 48 kHz.
- `ssc_encoder_create(Fs, channels, bits, int *err)`; `ssc_encoder_init(st, Fs, channels, bits)`; `bits` is stored at `st+0x6c` (0x18 = 24-bit HiFi/UHQ path).
  Fs=48000 is remapped to 44100 inside the encoder state (`st+0x60`); frame size is fixed at 864 samples/channel.
- `ssc_encode(st, const int16_t *pcm, int frame_size, uint8_t *out, int bitrate_bps)` -> bytes written. Accepted bitrates seen in the code: 87000, 96000, 128000, 192000, 229000.
- Output packet header (4 bytes): `ff ee` (sync word 0xEEFF, little endian), byte 2 = sequence nibble in the high nibble (`01,11,21,31,...`) + a low nibble of 1,
  byte 3 = bitrate/mode info (`0xf2` at 128 kbps, 16-bit; `0xf4` for 229 kbps; depends on bitrate and on bits==24). Then the CELT-like payload.
- Example: stereo 48 kHz 16-bit 128 kbps -> 324 bytes per frame (4 header + 320 payload), i.e. 864 samples/ch = 19.6 ms at the internal 44.1 kHz.
- Observed (oracle, stereo, 44.1/48 kHz): the stream is CBR, packet size depends only on the bitrate:

  | bitrate | packet bytes (incl. 4 header) | byte 2 low nibble | byte 3 |
  |---|---|---|---|
  | 87000  | 220 | 1 | f2 |
  | 96000  | 244 | 1 | f1 |
  | 128000 | 324 | 1 | f2 |
  | 192000 | 484 | d | f3 |
  | 229000 | 576 | d | f4 |

  Payload = 216/240/320/480/572 bytes, i.e. about `bitrate * 20 ms / 8` rounded down to even.
  (87000 and 128000 give the same byte 3; the difference must be inferred from the packet length.)
- 96 kHz, 24-bit, 229000: 266-byte packets (262 payload) -> a different size rule than <=48 kHz; low nibble d, byte 3 f4.
- 32 kHz crashes the emulated library (`ssc_encoder_get_size` reads unmapped memory): not a supported encoder rate, even though the capability byte has a 32 kHz bit.
- OPEN: PCM input format when `bits=24`. Output differs from the 16-bit path for every input layout tried (s16, s32<<8, s32<<16, packed s24),
  so the 24-bit path has its own coding; read `ssc_encode` for `st+0x6c == 0x18` to learn the sample layout.

## Galaxy S23 firmware (Android 14 / One UI 6.1, `S911BXXS8CYBD`, Feb 2025): newer encoder + stack
- Source: `~/Downloads/AP_S911BXXS8CYBD_..._OS14.tar.md5.zip` -> `work/ap14/super.img.lz4` -> `work/super14/parts/*.img` (all **EROFS**).
  Kernel mount via udisks FAILED for EROFS; use userspace instead: `dump.erofs --ls --path=/system/lib64 system.img` and
  `fsck.erofs --extract=<NEW-file-or-dir> --path=/system/lib64/libScalable_Encoder.so system.img` (destination must not exist).
- `work/s23/libScalable_Encoder.so` (103,608 B, NDK r25c): same API as Android 12 but all internals renamed `ssc_*` and many more are exported:
  `ssc_encode`, `ssc_quant_all_bands`, `ssc_pitch_search`, `ssc_comb_filter`, `ssc_remove_doubling`, `ssc_compute_allocation`, `ssc_ec_enc_*`,
  `ssc_mdct_forward`, `ssc_fft`, `tone_detect`, `powercalc`, `last_band_info_calc`, and UHQ-specific **`uhq_headroom`, `normal_headroom`,
  `headroom_gain`, `headroom_process`, `cutoff_start_uhq_index`, `previous_uhq_index`** (+ SSC_DRC_*). Best reference for the UHQ path.
- Bluetooth stack is now in the APEX `com.android.btservices.apex` (unzip -> `apex_payload.img` is ext4 -> `debugfs -R "dump /lib64/libbluetooth_jni.so out"`).
  Extracted to `work/s23/bt/libbluetooth_jni.so` (14.6 MB). SSC symbols: `A2DP_VendorGetSscType`, `A2DP_VendorGetBitRateSsc`, `A2DP_VendorAdjustCodecSsc`,
  `A2DP_BuildInfoSsc`, `A2DP_ParseInfoSsc`, `A2DP_IsVendor{Peer,Source}CodecValidSsc`, `A2dpSscToHalConfig`, `a2dp_vendor_ssc_{send_frames,set_current_encoder_info,...}`,
  and AIDL `SehCodecConfiguration::sscConfig` / `SehCodecCapabilities::sscCapabilities`.
- TODO: disassemble `A2DP_VendorGetSscType` / `GetBitRateSsc` / `AdjustCodecSsc` / `A2dpSscToHalConfig` to get the Normal/HiFi/UHQ types, bitrate table and
  the bits=24 PCM format; run the S23 encoder in the oracle (`tools/ssc_oracle.py`; the lib is still self-contained: check its imports first).

## LIVE capabilities of the Buds3 Pro (read from BlueZ over D-Bus, buds connected to this laptop)
Remote endpoints under `/org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX/` (`busctl --system get-property org.bluez <path> org.bluez.MediaEndpoint1 Codec|Capabilities`):
- `sep1` SBC: caps `3f ff 02 34`.   `sep2` AAC (codec 2): caps `80 01 8c 83 e8 00` (currently the active transport).
- **`sep3` vendor codec (255): caps `75 00 00 00 | 03 01 | 3e`** = vendor 0x00000075, codec 0x0103, config byte **0x3E = 0b00111110**.
  BlueZ's `Capabilities` for a vendor codec is `vendor(4) + codec_id(2) + codec-specific bytes`; the 4 leading bytes of the AVDTP element
  (len, media type, codec type = 0xff) are not included. SetConfiguration takes the same layout.
- All endpoints report `DelayReporting: true`.

### Byte-9 bits, as the S23 (One UI 6.1) stack parses them (`A2DP_ParseInfoSsc` fills `tA2DP_SSC_CIE`)
CIE layout: `[0] vendor u32, [4] codec id u16, [6] rate = b & 0xf0, [7] = b & 8, [8] = b & 4, [9] = b & 2, [10] = b & 1`.
A *configuration* is valid if bit1 or bit2 is set (no rate bit needed), otherwise exactly one rate bit is required (else error 0xc3). A capability is always accepted.
`A2DP_VendorGetSscType`: parse fails -> 0x80; **bit1 (0x02) -> 0x400 (UHQ)**; else rate 0x80 (96 kHz) -> 0x200; else **bit2 (0x04) -> 0x100 (HiFi, 24-bit/48k)**; else 0x80 (normal).
`A2DP_VendorGetTrackBitsPerSampleSsc`: bit1 (UHQ) or 96 kHz -> **0x22**; else bit2 -> 24 (0x18); else 16. (Meaning of 0x22 still unknown; likely selects the encoder's UHQ input mode.)
`A2DP_VendorGetBitRateSsc` returns a constant 192000.
`A2DP_VendorGetTrackSampleRateSsc` (S23, 0x6dea14): **bit1 (UHQ) returns 96000 Hz regardless of the rate bits**; otherwise bit2 (HiFi) returns 48000; otherwise the single rate bit
(0x10 -> 48000, 0x20 -> 44100, 0x40 -> 32000, 0x80 -> 96000). So UHQ is the 96 kHz / 24-bit mode: it uses the e-class presets (330/660/174-byte frames, one or two frames per packet,
8-bit sequence in bytes 1-2) and the second mode stored at mode+0x80 of the library (Fs=96000). The Buds3 Pro advertise bit1 but not the 0x80 rate bit, because UHQ already implies 96 kHz.
=> Buds3 Pro 0x3E = 44.1 kHz + 48 kHz (0x20|0x10) + bit3 (0x08, no bitrate limitation) + HiFi (0x04) + **UHQ (0x02)**; **no 96 kHz, no 32 kHz**.
=> A source that wants UHQ should configure byte 9 = 0x02 (optionally `| 0x08`); HiFi = 0x04; normal = one rate bit (0x10 or 0x20) `| 0x08`.
S23 stack function addresses (libbluetooth_jni.so, symbol table recovered from `.gnu_debugdata` -> xz -> ELF): `A2DP_ParseInfoSsc` 0x6dde24, `A2DP_VendorGetSscType` 0x6ded9c,
`A2dpCodecConfigSsc::setCodecConfig` 0x6dfa3c (3108 B), `setPeerCodecCapabilities` 0x6e0660, `a2dp_vendor_ssc_encoder_init` 0x6e10e0, `a2dp_vendor_ssc_send_frames` 0x6e1b40 (6872 B), `A2DP_VendorLoadEncoderSsc` 0x6e0c84.

## S23 encoder behaviour + how the phone feeds it (from `a2dp_vendor_ssc_encoder_init` 0x6e10e0 / `send_frames` 0x6e1b40, plus oracle runs)
- `work/s23/libScalable_Encoder.so` runs in `tools/ssc_oracle.py` unchanged (imports: malloc/free/memcpy/memset/__stack_chk_fail/__cxa_*/__register_atfork).
  Same `create(Fs, ch, bits, &err)` / `init` / `encode(st, pcm, frame, out, bitrate)` signatures; state is 9540 B (was 9620).
  16-bit/48k/128k stereo: first frame identical to the Android 12 encoder, later frames differ (state-dependent change between versions), so do not expect bit-exactness vs. both.
- `bits` argument (also `st+0x6c`) = `A2DP_VendorGetTrackBitsPerSampleSsc`: **16 -> s16 samples, 24 (0x18, HiFi) -> packed s24 (3 bytes), 0x22 (UHQ / 96 kHz) -> s32 (4 bytes)**.
  The stack computes PCM bytes per interval as `rate * interval_ms * (bits >> 3) * channels / 1000` (stored at `+0x1c`); channels = 2.
  Phone read sizes seen: 0xd80 = 3456 B = 864 stereo s16; 0x1440 = 5184 B = 864 stereo s24; 0x3600 = 13824 B = 96 kHz, 18 ms, s32 stereo.
- Encode interval (`[+0x340]`): **UHQ = 10 ms** (`strb 10`), others use the default byte (20 ms). Encoder state also stores rate, bits, channel count, bitrate-limitation flag, `isUHQ` flag (`GetSscType == 0x400`).
- Oracle, S23, UHQ (bits=34), 48 kHz stereo, rate 229000: frame 432 -> 290 B packet, **480 (10 ms) -> 322 B**, 864 -> 576 B, 960 -> 640 B (about 2/3 byte per sample/ch);
  frame 1728 faults in the emulator (stack/limit, not investigated). Header is the same `ff ee <seq<<4|d> f4`.
- NOT yet known: frames per A2DP packet / MTU packing (`a2dp_ssc_get_num_frame`), exact bitrate list for UHQ, the PLC/volume handling (`volumeStereo(short*,int,float,float)` is applied on s16 PCM in the phone, irrelevant for Linux).

## RESULT: the SSC bitstream is Opus CELT with a short list of Samsung differences (validated)
`ssc_encode` is a fixed-point CELT encoder (Opus-1.1/1.2 era names) with the following differences from Opus 1.5.2's CELT. All were found by
tracing the emulated encoder's range-coder calls (`tools/ssc_trace.py`) against an Opus-based decoder, and fixed one by one until the traces matched.
1. **Range coder width: `EC_CODE_BITS` = 24 instead of 32** (init: rng=0x800000, nbits_total=25; renormalise below 0x8000, byte = val>>23).
   Everything else in entenc/entdec/laplace/cwrs/pulse enumeration is Opus-compatible: byte-exact against Opus on random symbol sequences
   (`tools/rc_compare.py` + `work/ref/build/rc_replay`), PVQ enumeration identical in 79/80 random cases (the odd one is N=32, K=8; not yet explained).
2. **Mode (one mode per sample rate class; the 44100 one is used for 44.1 and 48 kHz input)**: 8 short MDCTs of 108 bins (frame = 864), overlap 108, **22 bands**
   `eBands = 0 1 2 3 4 5 6 7 8 10 12 14 16 20 24 28 34 40 48 60 72 84 102`, 11 allocation rows (`ssc_tables.h`, from the library).
   The library holds two allocation tables (mode+0x28 "A" and mode+0x30 "B"); `ssc_compute_allocation` picks one with a flag. **Table B reproduces the 229/128 kbps
   16-bit streams** (table A gives 4 vs 3 fine-energy bits for band 20). Which flag selects B is not known yet (candidates: bits==16, stereo, bitrate).
   Pulse-cache `index` differs from Opus' for band 20 (band 20 uses the row of the 18-wide band 21); `bits` and `caps` are identical to Opus' computation.
3. **No pre-emphasis / de-emphasis** (`ssc_preemphasis` is only `out = in << 12`, or `<< 4` for 24-bit/UHQ). The encoder input is **int32 per sample**, value in the low bits.
4. **The postfilter tapset symbol is not transmitted** (pitch octave `uint(6)`, period bits, gain bits (3), then straight to the transient flag). Tapset used by the real decoder: still to check with `ssc_comb_filter`.
5. Frame layout: 4 header bytes (`ff ee`, `seq<<4 | 1|d`, bitrate byte) + CELT payload of fixed size (CBR) written into `out+4` with `ec_enc_shrink(size)`.
6. Reconstruction conventions equal Opus': decoded band energies match the encoder's own `oldBandE` to 3 decimals; delay = overlap = 108 samples.
Validation (`work/ref/build`, `decode_test24_b`, `enc_test24`; vectors in `work/vec`, two seconds of stereo tones): Samsung packets decoded with the Opus-based decoder:
corr 0.9996, SNR 31.5 dB (229 kbps) / 24.9 dB (128 kbps); **my Opus-based encoder -> same decoder: 32.3 / 25.7 dB**. Encoder energies agree with Samsung's to 3 decimals.
Reproduce: `tools/make_ssc_celt.py work/ref/opus/celt work/ref/build/celt24 --debug`, `tools/make_ssc_modes.py work/ref/build/celt24 work/ref/build/ssc_modes24.c`, tables from `tools/ssc_gen_tables.py`.
Caveat: the decoder used for validation is *my* model (validated against Samsung's encoder), not the real Buds decoder.
Still open: UHQ (bits=0x22/24, second mode at mode+0x80 with Fs=96000), HiFi 24-bit, tapset value, header bytes 2-3 semantics, frames per A2DP packet, DRC/headroom blocks,
the encoder-input pitfall (int32) must be remembered in every tool that calls `ssc_encode`.

## Wire format of one SSC frame / A2DP packet (S23 encoder in the oracle + Buds3 Pro decoder, `tools/bes_*.py`)
Buds firmware memory map: file offset F is code at `0x0c01ffe4+F` and data at `0x2c01ffe4+F` (header size 0x1c, base 0x0c020000).
Main-core functions: frame validation `0x0c0af1e4`, `preparse_packet` `0x0c0af2f0`, `store_packet` `0x0c0af6b0`, 96 kHz "UHQ" parser `0x0c0ac828`. The CELT decoder itself lives in another core's image
(tables at file 0x3c1a6c..0x3c2a00, base unknown) and was NOT analysed.
- **One A2DP packet = RTP header (12 B, standard, RTP sequence +1 per packet; gap > 1 triggers PLC) + exactly ONE SSC frame (<= 800 B).** One frame = 864 samples/channel (`0x360`).
- Frame = 4-byte header + CELT payload of fixed size: byte0 `0xFF`; byte1 `0xEE` (f-class); byte2 = `seq<<4 | flags`; byte3 = info.
- Buds validation (`0x0c0af1e4`): byte0 == 0xFF, (byte1 & 0xF0) == 0xE0, len >= 4, then `info = byte3 & 0x1f` selects the EXPECTED frame length (frame rejected if the packet is shorter):

  | byte3 | bitrate preset | expected frame length |
  |---|---|---|
  | f0 | 88 000 | 224 |
  | f1 | 96 000 | 244 |
  | f2 | 128 000 | 324 |
  | f3 | 192 000 (also the default for unlisted values) | 484 |
  | f4 | 229 000 | 576 |
  | f5 / f6 | 328 000 / 256 000 @ 96 kHz (flag=1) | 372 / 292 |
  | e0 / e3 / e6 | 291 000 / 584 000 / 152 000 @ 96 kHz (flag=1) | 330 / 660 / 174 |
  | e1 e2 e4 e5 | 308 000 / 442 000 / 886 000 / 250 000 | 484 (default) |
  e-class frames carry an 8-bit sequence: byte1 = `0xE0 | seq>>4`, byte2 = `(seq&15)<<4 | flags`; the 96 kHz UHQ parser also accepts 2 frames per packet.
- Payload bytes = frame length - 4 = `bitrate * 0.02 / 8` rounded (220, 240, 320, 480, 572 for f0..f4). The encoder accepts any bitrate but the Buds only know the presets above.
- byte2 low nibble = `1 + 4*level`, level 0..3 = a 2-bit "high-band energy" hint written by the encoder (1: none, 5: band 20, 9: band 21, d: both; first frame always d). It does NOT depend on codedBands.
  Streams decode correctly without it (Opus-model decoder ignores it), and the Buds validator/seq parser ignore it, so the plugin writes level 0 (`1`).
- byte3 bits 7..5 are overwritten by a 3-bit signal-dependent value (always 111 in the tests: f?); the Buds validator masks them out.
- Input to `ssc_encode`: int32 per sample (16-bit values as is; 24-bit values for bits != 16).
- Encoder-side quirks worth knowing: bandwidth detection (`last_band_info_calc`: last band whose L+R mean log-energy exceeds a threshold) and per-bitrate cutoff tables
  (`cutoff_start_index`, `cutoff_start_uhq_index`: bin indices 306..864 in steps of 18) decide how many bands are coded; Samsung codes band 21 (17-21 kHz) when it has energy,
  stock Opus rules skip it at these bitrates (my encoder: 7.3 dB vs Samsung's 22.9 dB on an 18 kHz tone; equal on tones/noise).

## Implementation status (library + PipeWire plugin)
- `src/ssc.c` + vendored CELT (`third_party/celt`, generated by `tools/make_ssc_celt.py` and `tools/make_ssc_modes.py`) produce the frames; `meson test` covers header layout, determinism, a round trip
  through the CELT decoder (SNR 23-32 dB at 44.1 kHz, 29-37 dB at 48 kHz for the five presets on the tone test signal) and the plugin loaded with `dlopen`.
- Cost: ~0.15 ms per 864-sample frame (1% of a core), worst case 0.6 ms.
- PipeWire hosts the bluez5 SPA monitor inside WirePlumber (the `spa.bluez5` log lines come from the wireplumber process), so restarting wireplumber is enough to load a new codec plugin.
- The host loads codec plugins from a hard-coded list (`codec-loader.c`: aac aptx faststream ldac sbc lc3plus opus opus-g lc3 g722 hfp-*). The plugin therefore occupies the `aptx` slot
  (libspa-codec-bluez5-aptx.so, factory `api.codec.bluez5.media.aptx`) and uses id `APTX_HD` for ordering (index 2 of the host's list: above AAC). `LC3PLUS_HR` would sit below AAC.
  The proper long-term route is adding an `ssc` entry upstream in PipeWire.
- Plugin behaviour: stereo S16 only; configuration = one sample-rate bit (48 kHz preferred) plus the sink's bit 3 echoed back (as Samsung's stack does); starts at preset 229k unless the L2CAP MTU is too small
  (RTP 12 B + frame must fit), then adapts the preset to the socket queue (see the 2026-11-09 section); one frame per packet; RTP payload type 96, SSRC 1; flags nibble written as 1.
- Verified on hardware (Galaxy Buds3 Pro, 2026-10-30, Fedora 44, PipeWire 1.6.10): the Buds accept configuration byte 0x18 (48 kHz), the node runs with codec `ssc`, 48 kHz S16 stereo and
  no underruns, with the flags nibble fixed at 1 and the RTP marker bit 0, at the default preset (229k unless the MTU forces a lower one). Still unknown: 44.1 kHz (0x28) and the other presets on the
  sink, and what the 2-bit high-band hint does there.

## HiFi (24 bit) and how the Buds read the configuration byte (firmware `a2dp_scalable_config` 0x0c19958c, helper 0x0c199430)
The Buds3 Pro decode `elements[6]` (the SSC byte) like this (Thumb code in the main-core image, `tools/bes_dis.py`):
- sample rate code: bit7 or bit1 -> 96 kHz (code 8); else bit6 -> 32 kHz; else bit2 -> 48 kHz (code 0x10); else bit5 -> 44.1 kHz; else bit4 -> 48 kHz, none -> 44.1 kHz.
- **bit3 (0x08) = 24 bit per sample** ("CodecCfg bits per sampe = 24"), reset to 16 when the rate is 44.1 kHz ("reset bit per sample to 16 when samplerate is 44.1k"); bit3 clear = 16 bit.
  The phone calls this bit "no bitrate limitation" and echoes the sink's bit; to the sink it is the 24 bit output flag.
- So `0x18` (48 kHz + bit3, what the plugin sends) and the phone's HiFi value `0x0c` (bit2 | bit3) give the very same sink state: 48 kHz, 24 bit output. HiFi changes nothing in the bitstream.
- Oracle (S23 library, 48 kHz, every preset): `bits=24` with 24-bit input scaled by 256 gives exactly the same frames as `bits=16` with the 16-bit input. The 24 bit mode only adds input precision.
- Plugin consequence: no separate HiFi profile is needed. The plugin offers F32 (preferred), S32 and S16 to the graph and the float entry point `ssc_encode_float` feeds the
  float CELT encoder directly (full scale = +-1.0, same scale as the 16 bit path: x16/32768). Test: a -90 dBFS two-tone signal round-trips at 34 dB SNR as float, 9 dB after rounding to 16 bit.
- The Buds' own capability selector (`a2dp_select_scalable_capability`, 0x0c16b520, used when the Buds are the source) builds 96 kHz = `0x80|bit3`, HiFi = `0x04|bit3`, 44.1 kHz = `0x20|bit3`, 48 kHz = `0x10|bit3`.

## UHQ (96 kHz, 24 bit): what the S23 encoder does at Fs=96000, bits=0x22 (validated by decoding its frames with the CELT model)
Method: `tools/ssc_vectors.py` (exact-integer PCM, float32 stored) -> Samsung frames; decoder = vendored CELT with the tables below; symbol-level comparison
with replayed range-coder traces (the encoder works on one `ec_enc` object and re-writes trial passes, so the final stream is rebuilt by replaying candidate
event lists through Opus' 24-bit coder until the bytes match); `tools/ssc_combtrace.py`, MDCT/allocation probes in the Unicorn oracle.
- It is the same CELT with the **second mode of the library** (mode struct +0x80): Fs=96000, frame 864 (**9 ms**), 8 short blocks of 108, overlap 108, the same 22 bands
  but `eBands = 0 1 2 3 4 5 6 7 8 9 10 11 12 14 16 20 24 30 36 54 72 90 108` (last band 40-48 kHz), allocation **table B of that mode** (table A is the same as for 44.1 kHz and
  wrong), pulse cache index equal to what Opus computes. All in `third_party/celt/ssc_tables.h` (`ssc96_*`). Opus would pick 4 short blocks for 864/96 kHz: forced to 8.
- Forward MDCT (window, scale, overlap) is stock Opus: Samsung's MDCT output equals `clt_mdct_forward` of its own input buffer exactly. PVQ enumeration identical (596 vectors).
- **No tf symbols at all** (tf_res = `tf_select_table[LM][4*isTransient]`).
- **The number of coded bands varies per frame and is signalled in the frame header**: `end = 22 - level`, level = (low nibble of byte 2 - 1) / 4 (nibble 1,5,9,d). The encoder
  takes it from the last band whose L+R energy is above about -4 (log2, Q10 -4096..-3000), minimum 19 bands. A decoder that assumes 22 bands loses sync from the second frame on.
  (The same field exists in the 44.1/48 kHz frames; this is the "high-band hint" noted earlier.)
- Byte 3 bits 7..5 (called v) are NOT the cause of anything we need: Samsung writes 0 for ordinary music; noise gives 7. We write 0 for UHQ frames.
- **High-band offset copy** (encoder `0xeb60..0xecd4`, decoder must do the same): after the dynalloc flags, for k = 19, 20, 21 (k < end): if `offsets[k] == 0` and `offsets[k-1] >= 1001`
  and (`E[k] > 7000` or `E[k] > E[k-1]/2`) then `offsets[k] = offsets[k-1]` (cascading). E is the encoder's mean L/R log2 energy (Q10); a decoder using the decoded energies
  (mean of both channels, threshold 7.0) reproduces the encoder's symbols on 39 of 40 frames of a sweep. The exact decoder-side rule is not known. Our encoder avoids the question: boosts of
  bands 18-20 are kept below 1001 (1/8 bit), so the rule can never fire (`tools/make_ssc_celt.py`).
- Packets / sink: e-class frame header `FF, E0|seq>>4, (seq&15)<<4 | nibble, info`; the sink's UHQ parser (firmware 0x0c0ac828) accepts info e0 (330 B), e3 (660 B), e6 (174 B), 1 frame per packet or,
  for e0 with packet >= 504 B and e6 with >= 348 B, two frames (the phone sends two, 18 ms). Presets (Samsung encoder): e6 152 kbit/s, e0 291, e3 584 (e1 308, e2 442, e4 886, e5 250 and f4/f5/f6 exist
  in the encoder but are not accepted by that parser). Sequence number is 8 bit (byte 1 low nibble = high half).
- Input scale: 24-bit integers (full scale 2^23) in int32, `<<4` inside = same internal scale as 16-bit `<<12`; float full scale 1.0 gives unity gain through the model decoder (gain 1.0001).
- Buds firmware: config bit1 (0x02) means 96 kHz (`a2dp_scalable_config` helper 0x0c199430), bit3 24 bit (reset to 16 at 44.1 kHz).
- Results (model decoder, 291 kbit/s): tones 38.0 dB (ours) vs 37.7 dB (Samsung), noise 1.0 vs 1.1, 40 kHz tone 11 vs 23 (Samsung boosts the top bands), chirp 16.9 vs 8-10.
- **tf_res is 0 also for transient frames** (not `tf_select_table[LM][4*isTransient]`): Samsung's kick-drum frames decode at 23 dB that way, 3 dB with the table value.
- Our encoder uses long blocks only in UHQ: Opus' transient detector (tuned for 48 kHz) flagged every 96 kHz frame, smearing the bass with 8 short blocks (heard as "compressed bass").
- Our encoder picks `end` per frame (last band with mean log2 energy > -4, between 19 and 22) and writes the level into the flags nibble. Coding the empty top bands cost 15 dB of SNR on
  real music (15 -> 31 dB, equal to Samsung's encoder on the same 3.6 s of music, model decoder). 82% of Samsung's frames use end = 19.
- Plugin pitfall found the hard way: `meson test` does not rebuild the plugin; run `meson compile` before installing.
- Round trip on 40 s of two bass-heavy remixes (model decoder): 44.1/48 kHz 229k 28-29 dB, UHQ 291k 18-26 dB (broadband clicks in long blocks lose their HF part, that is the low figure).
- NOT verified on the earbuds yet beyond: the first build had every frame as short blocks and sounded wrong.

## Buds3 Pro firmware (decoder side): `FOTA_R630XXU0AZG2.bin` (project root, 8.7 MB, build Jul 10 2026)
- NOT encrypted. Starts with BES (Bestechnic) image header magic `0xcafecafe`, then Thumb code at file offset 0x28
  (`ldr r0,[pc,#0x40]; bx r0`). Load base not yet determined (try correlating literal-pool pointers with string offsets).
- Contains the SSC **decoder** (stripped, but with log strings): `a2dp_decoder_scalable.cpp`, `a2dp_audio_scalable_{init,preparse_packet,
  store_packet,synchronize_packet,discards_packet,convert_list_to_samples}`, `a2dp_audio_cc_scalable_{init,decoder_reinit,set_sbm_param}`,
  `a2dp_select_scalable_capability`, `a2dp_scalable_config` ("Vendor ID:0x%08x, Codec ID:0x%04x, Scalable Codec"; "config value:0x%02x, elements[6]:0x%02x").
- **UHQ and Normal are the same codec**, selected inside the same init: `SSC UHQ ssc_decoder_init sampling rate = %d` vs `SSC Normal ssc_decoder_init ...`.
  So "Seamless/UHQ" is very likely the HiFi mode (byte-9 bits 0x04 / 0x80) of the Scalable Codec 0x0075:0x0103 already present in the Android 12 firmware.
  => the Android 14 phone firmware is probably not needed for the bitstream; still useful only to double-check phone-side UHQ encoder parameters.
- Stream details visible in strings: packets have a header with **sync words** (`SCALABLE_HEAD_SIZE`), a **sequence number** (gap triggers PLC),
  UHQ packets log `UHQ seq` from `data[3]` and a timestamp; bitrate adaptation uses `SSC bitrate{info:0x%02X->QosLevel:%d}`; "SBM" (SoundBooster) offsets/direction.
- Use: decoder = round-trip oracle for the encoder; its packet pre-parser defines the exact packet header.

## 2026-10-30: the evening's "Failure in Bluetooth audio transport" is NOT this codec (checked against all logs)
Symptom the user saw: every playback session dies after seconds-to-minutes and the player (KDE Elisa) crashes in its `QAudioContext`
thread (`SIGSEGV`, 5 times today: 22:06:08, 22:07:17, 22:08:37, 22:15:19, 22:19:37 — each crash follows a pipewire node error by ~2 s).

What the logs say (`journalctl --user -u wireplumber --since today | grep 'Failure in Bluetooth'`, `-u bluetooth`, `-k`):
- 34 occurrences of `spa.bluez5: Failure in Bluetooth audio transport <path>` since Oct 28, **before any SSC plugin existed** (repo born Oct 30 20:30):
  Oct 28 15:45 a *different headset* on `hci0` (`dev_XX_XX_XX_XX_XX_XX/sep2`, AAC); Oct 29 repeatedly on the Buds' `sep1` (SBC) and `sep2` (AAC)
  and on HFP paths (`/fd0`, `/fd52`, `/fd64` = SCO). Tonight: sep3 (SSC) failures at 22:06:06, 22:07:15, 22:08:35 and 22:19:35, one error coming from
  the socket write path alone at 22:15:17, and sep2 (AAC) failing too at 21:29:27 and 21:35:05.
- Adapter: the laptop's only, integrated Realtek RTL8852BT controller (USB-attached internally, 13d3:3619; `hci0`/`hci1` are the same chip re-enumerated after a USB reset, Delete Index/New Index in btmon); firmware loads fine in the kernel log, no HCI/L2CAP errors at the drop times, `bluetoothd`
  logs nothing on disconnect at default level, nothing in the kernel log either — a silent link drop.
- Acquire-to-failure intervals today: 7 s, 41 s, 49 s, 248 s, 107 s — random like an RF/USB co-existence drop, not codec-related.
- `wireplumber` stayed healthy through each failure (the 8.7k log lines in 20 s at connect time are only WP DEBUG on setup); the last session did
  stream 107 s of music (fd acquired 22:17:48, Buds3 Pro, UHQ profile, config byte 0x0a exactly as `A2DP_ParseInfoSsc` expects).

Why the SSC stream cannot be the cause: the firmware (above) treats bad SSC frames per packet — wrong length or a sequence gap rejects the frame
and triggers PLC, never a hang-up — `meson test` verifies every header field against the reverse-engineered tables, and AAC failed the same evening.

If it recurs (environment, not this repo):
- `sudo btmon -T` during playback to capture the disconnect reason, `journalctl -k -f | grep -i bluetooth`;
- Realtek combo chips share the radio with WiFi: try disabling deep powersave (`sudo modprobe rtw89_core disable_lps_deep=Y`),
  disable USB autosuspend on the controller (`/sys/bus/usb/devices/*/power/autosuspend = -1`);
- the Elisa `QAudioContext` SIGSEGV after the stream error is an Elisa/Qt bug on its own; other players survive the same event.

## 2026-10-31: follow-up (btmon capture of a drop, then stable runs)
- btmon during a `ssc_uhq` session (672-byte packets, 2 frames, every 18 ms): the stream is regular (no RTP sequence gaps, constant timestamp
  step, same frame headers) until the Buds themselves send `Disconnect Complete`, reason `0x0e` (Connection Rejected due to Security Reasons), 107-121 s in.
  The host never saw an error before that.
- Afterwards `ssc_uhq` ran for more than 5 minutes with 1 frame per packet (`SSC_UHQ_FRAMES=1`: 346 bytes every 9 ms) and then again for
  more than 5 minutes with the default 2 frames per packet, with no drop. So frame pairing is not the cause; the drops of the previous evening
  happened with WirePlumber at debug level 5 and btmon running and are not reproduced since. Cause still unknown (link/RF, or Buds-side).
- Kernel log: the Realtek controller does full USB resets (`usb 2-3: reset full-speed USB device`) with firmware reload about every 13 hours,
  unrelated to the drop times.

## 2026-11-09: Galaxy Buds2 Pro, the 192k preset and bitrate adaptation
Setup: Galaxy Buds2 Pro (Modalias `v0075pA013d0001`), CachyOS, PipeWire 1.6.10, WirePlumber 0.5.18, BlueZ 5.87, Intel AX210. Report in issue #1.
- Capability byte `0x3c` (44.1 + 48 kHz, bit 3, bit 2 HiFi; no 96 kHz bit, so no `ssc_uhq` profile). Configuration `0x18` is accepted, as on the Buds3 Pro.
- Presets heard on these Buds, each fixed for the whole stream (`SSC_PRESET`, plugin without adaptation): 229k, 128k, 96k and 88k sound right;
  **192k sounds wrong** ("strange" for as long as it plays, back to normal when the preset changes). The table above shows Samsung's encoder writing
  flags nibble `d` at 192k and 229k and `1` below; we always write `1`. Tested and ruled out: `main` changed to code 19 bands at 192k
  (`CELT_SET_END_BAND(19)`) and write nibble `d`, as Samsung's encoder does, still sounds wrong at fixed 192k on the Buds2 Pro.
- Stutters at 229k: with WirePlumber at debug level each one is a burst of `send_buffer: Resource temporarily unavailable` (socket full, packets dropped)
  plus `reduce bitpool: 0`, the host asking the codec for a lower bitrate and the plugin having none. No xruns in the graph, no Wi-Fi scan or BlueZ event at those times.
- The host (`media-sink.c`) calls `reduce_bitpool` only after a failed send, at most every 0.5 s, and skips it when its once-a-second check ran less than
  0.5 s before; one drop at 18:52:46 got no reduction for that reason. `abr_process(unsent)` is called before every packet with the bytes still in the
  socket, which lets the codec act before the socket fills (LDAC uses it).
- Queue at 229k on a healthy link, per 556 packets (about 10 s): empty 86-94% of the time, at most 2 packets (1176 bytes). It also reaches 2 packets
  at 192k and 128k, so "queue empty" alone almost never lasts 10 s; one packet in flight has to count as healthy.
- The plugin now implements `abr_process`: more than 3 packets queued steps down (at most once per 28 packets, about 0.5 s), at most one packet queued
  for 556 packets in a row steps up, `reduce_bitpool` steps down on a full socket. 192k is skipped both ways unless `SSC_PRESET=192k` makes it the ceiling.
  A first version stepped down above 2 packets: in 32 minutes on a clean link it went 229k -> 128k about 40 times, nearly always at exactly 3 packets
  (1764 bytes), and spent about a quarter of the time at 128k. `SO_SNDBUF` is 5432 bytes (`FILL_FRAMES` 4 x MTU 679, doubled by the kernel), and at fixed
  229k the queue reached 3 packets about once a minute with a single drop in 15 minutes, so 3 packets is normal jitter; the limit is now more than 3.
- Heard on the Buds2 Pro: none of those ~80 changes between 229k, 128k, 96k and 88k was audible. Walking away from the PC took it 229k -> 128k -> 96k -> 88k
  within 8 s, and back to 229k 10 s per step after returning.
