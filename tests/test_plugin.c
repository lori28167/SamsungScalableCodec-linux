/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 lori28167 */
/*
 * Loads the built PipeWire codec plugin the way the bluez5 host does (dlopen + spa_handle_factory_enum + the
 * Bluez5CodecMedia interface) and drives the codec through its callbacks: caps, configuration selection with the
 * capabilities read from real Galaxy Buds3 Pro, encoding into RTP packets, and a decode check of the audio.
 */
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/utils/string.h>

#include "celt.h"
#include "opus_custom.h"
#include "media-codecs.h"
#include "rtp.h"
#include "ssc.h"
#include "pipewire/a2dp-codec-ssc.h"

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

#define NFRAMES 40
#define WARMUP 8
#define DELAY 108
#define CONGESTED 100000
#define SOCKET_FULL -1

/* what BlueZ reports for the vendor endpoint of the Buds3 Pro: vendor 0x75, codec 0x0103, config byte 0x3e */
static const uint8_t buds_caps[7] = { 0x75, 0x00, 0x00, 0x00, 0x03, 0x01, 0x3e };

static void make_signal(int16_t *pcm, int frames, int rate)
{
	long n;

	for (n = 0; n < (long)frames * SSC_FRAME_SAMPLES; n++) {
		double t = (double)n / rate;
		double l = 6000 * sin(2 * M_PI * 330 * t) + 3000 * sin(2 * M_PI * 1800 * t) + 1500 * sin(2 * M_PI * 5200 * t);
		double r = 6000 * sin(2 * M_PI * 660 * t) + 3000 * sin(2 * M_PI * 1800 * t + 1) + 1500 * sin(2 * M_PI * 5200 * t);

		pcm[2 * n] = (int16_t)l;
		pcm[2 * n + 1] = (int16_t)r;
	}
}

static int preset_of_frame(int frame_bytes)
{
	int p;

	for (p = SSC_PRESET_88K; p <= SSC_PRESET_229K; p++)
		if (ssc_frame_bytes(p) == frame_bytes)
			return p;
	return -1;
}

static struct spa_audio_info make_info(int rate, uint32_t format)
{
	struct spa_audio_info info;

	memset(&info, 0, sizeof(info));
	info.media_type = SPA_MEDIA_TYPE_audio;
	info.media_subtype = SPA_MEDIA_SUBTYPE_raw;
	info.info.raw.format = format;
	info.info.raw.rate = rate;
	info.info.raw.channels = 2;
	return info;
}

int main(int argc, char **argv)
{
	void *lib;
	int (*factory_enum)(const struct spa_handle_factory **, uint32_t *);
	const struct spa_handle_factory *factory = NULL;
	uint32_t index = 0;
	struct spa_handle *handle;
	void *iface = NULL;
	const struct spa_bluez5_codec_a2dp *host;
	const struct media_codec *codec = NULL;
	uint8_t caps[A2DP_MAX_CAPS_SIZE], config[A2DP_MAX_CAPS_SIZE];
	struct media_codec_audio_info want = { 48000, 2 };
	int res, i;

	if (argc < 2) {
		fprintf(stderr, "usage: %s plugin.so\n", argv[0]);
		return 2;
	}
	lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (lib == NULL) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 2;
	}
	factory_enum = (int (*)(const struct spa_handle_factory **, uint32_t *))dlsym(lib, "spa_handle_factory_enum");
	CHECK(factory_enum != NULL, "spa_handle_factory_enum not exported");
	if (!factory_enum || factory_enum(&factory, &index) != 1)
		return 1;
	CHECK(spa_streq(factory->name, MEDIA_CODEC_FACTORY_NAME("aptx")), "unexpected factory name %s", factory->name);

	handle = calloc(1, factory->get_size(factory, NULL));
	CHECK(factory->init(factory, handle, NULL, NULL, 0) == 0, "factory init failed");
	CHECK(handle->get_interface(handle, SPA_TYPE_INTERFACE_Bluez5CodecMedia, &iface) == 0 && iface != NULL, "no codec interface");
	host = iface;
	codec = host->codecs[0];
	CHECK(codec != NULL && host->codecs[1] != NULL && host->codecs[2] == NULL, "the plugin must export the normal and the UHQ codec");
	CHECK(spa_streq(codec->name, "ssc"), "codec name %s", codec->name);
	CHECK(codec->codec_id == A2DP_CODEC_VENDOR && codec->vendor.vendor_id == SSC_VENDOR_ID && codec->vendor.codec_id == SSC_CODEC_ID, "wrong A2DP ids");
	CHECK(codec->id == SPA_BLUETOOTH_AUDIO_CODEC_APTX_HD, "wrong ordering id");

	/* our endpoint capabilities */
	res = codec->fill_caps(codec, 0, NULL, caps);
	CHECK(res == 7 && memcmp(caps, (const uint8_t[]){ 0x75, 0, 0, 0, 0x03, 0x01, 0x38 }, 7) == 0, "fill_caps returned %d / %02x", res, caps[6]);

	/* configuration chosen against the Buds3 Pro capabilities */
	res = codec->select_config(codec, 0, buds_caps, sizeof(buds_caps), &want, NULL, config, NULL);
	CHECK(res == 7 && config[6] == 0x18, "48 kHz: select_config returned %d, config byte %02x (expected 18)", res, config[6]);
	want.rate = 44100;
	res = codec->select_config(codec, 0, buds_caps, sizeof(buds_caps), &want, NULL, config, NULL);
	CHECK(res == 7 && config[6] == 0x28, "44.1 kHz: config byte %02x (expected 28)", config[6]);
	{
		uint8_t hifi_only[7] = { 0x75, 0, 0, 0, 0x03, 0x01, 0x04 };
		uint8_t wrong_codec[7] = { 0x75, 0, 0, 0, 0x04, 0x01, 0x3e };

		CHECK(codec->select_config(codec, 0, hifi_only, 7, &want, NULL, config, NULL) == -ENOTSUP, "caps without a normal-mode rate must be refused");
		CHECK(codec->select_config(codec, 0, wrong_codec, 7, &want, NULL, config, NULL) == -ENOTSUP, "a different vendor codec id must be refused");
	}
	want.rate = 48000;
	res = codec->select_config(codec, 0, buds_caps, sizeof(buds_caps), &want, NULL, config, NULL);

	{
		uint8_t buf[512];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
		struct spa_pod *param = NULL;

		CHECK(codec->enum_config(codec, 0, config, 7, SPA_PARAM_EnumFormat, 0, &b, &param) == 1 && param != NULL, "enum_config failed");
		CHECK(codec->enum_config(codec, 0, config, 7, SPA_PARAM_EnumFormat, 1, &b, &param) == 0, "enum_config must stop after one format");
	}

	/* encoding: every input format, and an MTU that is too small for the 229k preset (12 + 576 bytes) */
	static const struct { uint32_t format; size_t mtu; int expect_frame; int sample_size; } cases[] = {
		{ SPA_AUDIO_FORMAT_S16, 895, 576, 2 }, { SPA_AUDIO_FORMAT_S16, 300, 244, 2 },
		{ SPA_AUDIO_FORMAT_F32, 895, 576, 4 }, { SPA_AUDIO_FORMAT_S32, 895, 576, 4 },
	};
	for (size_t mtu_case = 0; mtu_case < sizeof(cases) / sizeof(cases[0]); mtu_case++) {
		size_t mtu = cases[mtu_case].mtu;
		int expect_frame = cases[mtu_case].expect_frame;
		int block = SSC_FRAME_SAMPLES * 2 * cases[mtu_case].sample_size;
		struct spa_audio_info info = make_info(48000, cases[mtu_case].format);
		void *enc = codec->init(codec, 0, config, 7, &info, NULL, mtu);
		int16_t *pcm = malloc(sizeof(int16_t) * 2 * SSC_FRAME_SAMPLES * NFRAMES);
		uint8_t *in_buf = malloc(block);
		float *dec_out = malloc(sizeof(float) * 2 * SSC_FRAME_SAMPLES * NFRAMES);
		int celt_err = 0;
		OpusCustomMode *mode = opus_custom_mode_create(44100, SSC_FRAME_SAMPLES, &celt_err);
		OpusCustomDecoder *dec = opus_custom_decoder_create(mode, 2, &celt_err);
		double sig = 0, noise = 0;

		CHECK(enc != NULL, "codec init failed (mtu %zu)", mtu);
		if (!enc)
			continue;
		opus_custom_decoder_ctl(dec, CELT_SET_SIGNALLING(0));
		CHECK(codec->get_block_size(enc) == block, "block size %d, expected %d", codec->get_block_size(enc), block);
		make_signal(pcm, NFRAMES, 48000);
		for (i = 0; i < NFRAMES; i++) {
			uint8_t packet[1024];
			size_t out = 0;
			int flush = -1, hdr, used;
			struct rtp_header *rtp = (struct rtp_header *)packet;

			hdr = codec->start_encode(enc, packet, sizeof(packet), (uint16_t)(1000 + i), 864u * i);
			CHECK(hdr == 12 && rtp->v == 2 && rtp->pt == 96 && ntohs(rtp->sequence_number) == 1000 + i && ntohl(rtp->timestamp) == 864u * i,
			      "bad RTP header (len %d)", hdr);
			for (int k = 0; k < SSC_FRAME_SAMPLES * 2; k++) {
				int16_t v = pcm[2 * SSC_FRAME_SAMPLES * i + k];

				if (cases[mtu_case].format == SPA_AUDIO_FORMAT_S16)
					((int16_t *)in_buf)[k] = v;
				else if (cases[mtu_case].format == SPA_AUDIO_FORMAT_S32)
					((int32_t *)in_buf)[k] = (int32_t)v << 16;
				else
					((float *)in_buf)[k] = v * (1.0f / 32768.0f);
			}
			used = codec->encode(enc, in_buf, block, packet + hdr, sizeof(packet) - hdr, &out, &flush);
			CHECK(used == block && flush == NEED_FLUSH_ALL && (int)out == expect_frame,
			      "mtu %zu frame %d: consumed %d, flush %d, wrote %zu (expected %d)", mtu, i, used, flush, out, expect_frame);
			CHECK(packet[hdr] == 0xff && packet[hdr + 1] == 0xee && (packet[hdr + 2] >> 4) == (i & 15), "bad frame header at %d", i);
			CHECK(hdr + (int)out <= (int)mtu, "packet %d bytes exceeds the MTU %zu", hdr + (int)out, mtu);
			res = opus_custom_decode_float(dec, packet + hdr + SSC_HEADER_SIZE, (int)out - SSC_HEADER_SIZE, dec_out + 2 * SSC_FRAME_SAMPLES * i, SSC_FRAME_SAMPLES);
			CHECK(res == SSC_FRAME_SAMPLES, "decoder returned %d", res);
		}
		for (long n = (long)WARMUP * SSC_FRAME_SAMPLES; n < (long)NFRAMES * SSC_FRAME_SAMPLES; n++)
			for (int c = 0; c < 2; c++) {
				double x = pcm[2 * (n - DELAY) + c], y = dec_out[2 * n + c] * 32768.0;

				sig += x * x;
				noise += (x - y) * (x - y);
			}
		printf("format %u, mtu %zu: %d byte frames, plugin round trip SNR %.1f dB\n", cases[mtu_case].format, mtu, expect_frame, 10 * log10(sig / (noise + 1e-9)));
		CHECK(10 * log10(sig / (noise + 1e-9)) > 22, "round trip SNR too low");
		{
			uint32_t enc_delay = 0, dec_delay = 99;

			codec->get_delay(enc, &enc_delay, &dec_delay);
			CHECK(enc_delay == 108 && dec_delay == 0, "delay %u/%u", enc_delay, dec_delay);
		}
		codec->deinit(enc);
		opus_custom_decoder_destroy(dec);
		opus_custom_mode_destroy(mode);
		free(pcm);
		free(in_buf);
		free(dec_out);
	}
	{
		struct spa_audio_info bad = make_info(48000, SPA_AUDIO_FORMAT_U8);
		void *enc;

		enc = codec->init(codec, 0, config, 7, &bad, NULL, 895);
		CHECK(enc == NULL, "a format the codec does not take must be refused");
		CHECK(codec->init(codec, 0, config, 7, &(struct spa_audio_info){ 0 }, NULL, 100) == NULL, "garbage info must be refused");
	}
	{
		static const struct { const char *step; int calls; long unsent; int frame; uint8_t info; } steps[] = {
			{ "start", 0, 0, 576, 0xf4 },
			{ "empty queue at the top", SSC_ABR_INCREASE_HOLD, 0, 576, 0xf4 },
			{ "queue at the limit", 1, 3 * (12 + 576), 576, 0xf4 },
			{ "queue over the limit skips 192k", 1, 3 * (12 + 576) + 1, 324, 0xf2 },
			{ "congested during the cooldown", SSC_ABR_REDUCE_COOLDOWN - 1, CONGESTED, 324, 0xf2 },
			{ "congested after the cooldown", 1, CONGESTED, 244, 0xf1 },
			{ "congested", SSC_ABR_REDUCE_COOLDOWN, CONGESTED, 224, 0xf0 },
			{ "congested at the floor", SSC_ABR_REDUCE_COOLDOWN, CONGESTED, 224, 0xf0 },
			{ "empty queue before the hold", SSC_ABR_INCREASE_HOLD - 1, 0, 224, 0xf0 },
			{ "queue above one packet", 1, 12 + 224 + 1, 224, 0xf0 },
			{ "one packet in flight before the hold", SSC_ABR_INCREASE_HOLD - 1, 12 + 224, 224, 0xf0 },
			{ "one packet in flight after the hold", 1, 12 + 224, 244, 0xf1 },
			{ "empty queue before the hold", SSC_ABR_INCREASE_HOLD - 1, 0, 244, 0xf1 },
			{ "socket full", 1, SOCKET_FULL, 224, 0xf0 },
			{ "empty queue before the hold", SSC_ABR_INCREASE_HOLD - 1, 0, 224, 0xf0 },
			{ "empty queue after the hold", 1, 0, 244, 0xf1 },
			{ "empty queue after the hold", SSC_ABR_INCREASE_HOLD, 0, 324, 0xf2 },
			{ "empty queue after the hold skips 192k", SSC_ABR_INCREASE_HOLD, 0, 576, 0xf4 },
		};
		struct spa_audio_info info = make_info(48000, SPA_AUDIO_FORMAT_S16);
		void *enc = codec->init(codec, 0, config, 7, &info, NULL, 895);
		int16_t *pcm = malloc(sizeof(int16_t) * 2 * SSC_FRAME_SAMPLES);
		int celt_err = 0;
		OpusCustomMode *mode = opus_custom_mode_create(44100, SSC_FRAME_SAMPLES, &celt_err);
		OpusCustomDecoder *dec = opus_custom_decoder_create(mode, 2, &celt_err);
		float dec_out[2 * SSC_FRAME_SAMPLES];

		CHECK(enc != NULL && codec->abr_process != NULL && codec->reduce_bitpool != NULL, "the codec must adapt its bitrate");
		CHECK(codec->increase_bitpool == NULL, "the bitrate must go up only from the queue size");
		if (enc && codec->abr_process && codec->reduce_bitpool) {
			opus_custom_decoder_ctl(dec, CELT_SET_SIGNALLING(0));
			make_signal(pcm, 1, 48000);
			for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
				uint8_t packet[1024];
				size_t out = 0;
				int flush, hdr, bitrate = 0;

				for (i = 0; i < steps[s].calls; i++)
					bitrate = steps[s].unsent == SOCKET_FULL ? codec->reduce_bitpool(enc) : codec->abr_process(enc, (size_t)steps[s].unsent);
				CHECK(steps[s].calls == 0 || bitrate == ssc_preset_bitrate(preset_of_frame(steps[s].frame)),
				      "step %zu (%s): returned %d", s, steps[s].step, bitrate);
				hdr = codec->start_encode(enc, packet, sizeof(packet), (uint16_t)s, 864u * s);
				codec->encode(enc, pcm, SSC_FRAME_SAMPLES * 4, packet + hdr, sizeof(packet) - hdr, &out, &flush);
				CHECK((int)out == steps[s].frame && packet[hdr + 3] == steps[s].info,
				      "step %zu (%s): %zu byte frame, info %02x (expected %d, %02x)", s, steps[s].step, out, packet[hdr + 3], steps[s].frame, steps[s].info);
				CHECK(opus_custom_decode_float(dec, packet + hdr + SSC_HEADER_SIZE, (int)out - SSC_HEADER_SIZE, dec_out, SSC_FRAME_SAMPLES) == SSC_FRAME_SAMPLES,
				      "step %zu: frame does not decode", s);
			}
		}
		if (enc)
			codec->deinit(enc);
		opus_custom_decoder_destroy(dec);
		opus_custom_mode_destroy(mode);
		free(pcm);
	}
	{
		struct spa_audio_info info = make_info(48000, SPA_AUDIO_FORMAT_S16);
		int16_t *pcm = calloc(2 * SSC_FRAME_SAMPLES, sizeof(int16_t));
		void *enc;
		uint8_t packet[1024];
		size_t out = 0;
		int flush, hdr;

		setenv("SSC_PRESET", "192k", 1);
		enc = codec->init(codec, 0, config, 7, &info, NULL, 895);
		unsetenv("SSC_PRESET");
		CHECK(enc != NULL, "init with SSC_PRESET=192k");
		if (enc && codec->abr_process) {
			hdr = codec->start_encode(enc, packet, sizeof(packet), 0, 0);
			codec->encode(enc, pcm, SSC_FRAME_SAMPLES * 4, packet + hdr, sizeof(packet) - hdr, &out, &flush);
			CHECK(out == 484, "SSC_PRESET=192k: starts with %zu byte frames", out);
			CHECK(codec->abr_process(enc, CONGESTED) == 128000, "SSC_PRESET=192k: congestion must step down to 128k");
			for (i = 0; i < SSC_ABR_INCREASE_HOLD; i++)
				codec->abr_process(enc, 0);
			hdr = codec->start_encode(enc, packet, sizeof(packet), 1, 864);
			codec->encode(enc, pcm, SSC_FRAME_SAMPLES * 4, packet + hdr, sizeof(packet) - hdr, &out, &flush);
			CHECK(out == 484, "SSC_PRESET=192k: must climb back to the 192k ceiling, got %zu byte frames", out);
		}
		if (enc)
			codec->deinit(enc);
		free(pcm);
	}
	/* UHQ variant: 96 kHz, two 9 ms frames per packet */
	{
		const struct media_codec *uhq = host->codecs[1];
		uint8_t uconf[A2DP_MAX_CAPS_SIZE], nouhq[7] = { 0x75, 0, 0, 0, 0x03, 0x01, 0x38 };
		struct media_codec_audio_info want96 = { 96000, 2 };
		struct spa_audio_info info = make_info(96000, SPA_AUDIO_FORMAT_F32);
		float *pcm = malloc(sizeof(float) * 2 * SSC_FRAME_SAMPLES * 20), *dec_out = malloc(sizeof(float) * 2 * SSC_FRAME_SAMPLES * 20);
		int celt_err = 0;
		OpusCustomMode *mode = opus_custom_mode_create(96000, SSC_FRAME_SAMPLES, &celt_err);
		OpusCustomDecoder *dec = opus_custom_decoder_create(mode, 2, &celt_err);
		void *enc;
		double sig = 0, noise = 0;

		CHECK(spa_streq(uhq->name, "ssc_uhq") && uhq->id != codec->id && uhq->fill_caps == NULL, "UHQ codec definition");
		res = uhq->select_config(uhq, 0, buds_caps, sizeof(buds_caps), &want96, NULL, uconf, NULL);
		CHECK(res == 7 && uconf[6] == 0x0a, "UHQ select_config: %d, config byte %02x (expected 0a)", res, uconf[6]);
		CHECK(uhq->select_config(uhq, 0, nouhq, 7, &want96, NULL, uconf, NULL) == -ENOTSUP, "peers without the UHQ bit must be refused");
		res = uhq->select_config(uhq, 0, buds_caps, sizeof(buds_caps), &want96, NULL, uconf, NULL);
		enc = uhq->init(uhq, 0, uconf, 7, &info, NULL, 895);
		CHECK(enc != NULL, "UHQ init");
		if (enc) {
			opus_custom_decoder_ctl(dec, CELT_SET_SIGNALLING(0));
			for (long n = 0; n < 20L * SSC_FRAME_SAMPLES; n++) {
				double t = (double)n / 96000;

				pcm[2 * n] = (float)(0.2 * sin(2 * M_PI * 330 * t) + 0.1 * sin(2 * M_PI * 5200 * t));
				pcm[2 * n + 1] = (float)(0.2 * sin(2 * M_PI * 660 * t));
			}
			for (int pkt = 0; pkt < 10; pkt++) {
				uint8_t packet[2048];
				int hdr = uhq->start_encode(enc, packet, sizeof(packet), (uint16_t)pkt, 0), total = hdr;

				for (int fr = 0; fr < 2; fr++) {
					size_t out = 0;
					int flush = -1, used, idx = 2 * pkt + fr;

					used = uhq->encode(enc, pcm + 2 * SSC_FRAME_SAMPLES * idx, SSC_FRAME_SAMPLES * 8, packet + total, sizeof(packet) - total, &out, &flush);
					CHECK(used == SSC_FRAME_SAMPLES * 8 && out == 330 && flush == (fr == 1 ? NEED_FLUSH_ALL : NEED_FLUSH_NO),
					      "UHQ packet %d frame %d: used %d out %zu flush %d", pkt, fr, used, out, flush);
					CHECK(packet[total] == 0xff && packet[total + 1] == (0xe0 | (idx >> 4)) && (packet[total + 2] & 0x0f) % 4 == 1 && (packet[total + 2] >> 4) == (idx & 15) && packet[total + 3] == 0,
					      "UHQ frame %d header %02x %02x %02x %02x", idx, packet[total], packet[total + 1], packet[total + 2], packet[total + 3]);
					opus_custom_decoder_ctl(dec, CELT_SET_END_BAND(22 - ((packet[total + 2] & 15) - 1) / 4));
					opus_custom_decode_float(dec, packet + total + 4, (int)out - 4, dec_out + 2 * SSC_FRAME_SAMPLES * idx, SSC_FRAME_SAMPLES);
					total += (int)out;
				}
				CHECK(total == 12 + 660, "UHQ packet size %d", total);
			}
			for (long n = 8L * SSC_FRAME_SAMPLES; n < 20L * SSC_FRAME_SAMPLES; n++)
				for (int c = 0; c < 2; c++) {
					double x = pcm[2 * (n - DELAY) + c], y = dec_out[2 * n + c];

					sig += x * x;
					noise += (x - y) * (x - y);
				}
			printf("UHQ plugin round trip SNR %.1f dB\n", 10 * log10(sig / (noise + 1e-30)));
			CHECK(10 * log10(sig / (noise + 1e-30)) > 25, "UHQ round trip SNR too low");
			uhq->deinit(enc);
		}
		opus_custom_decoder_destroy(dec);
		opus_custom_mode_destroy(mode);
		free(pcm);
		free(dec_out);
	}
	if (handle->clear)
		handle->clear(handle);
	free(handle);
	dlclose(lib);
	printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
	return failures != 0;
}
