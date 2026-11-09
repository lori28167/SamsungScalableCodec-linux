/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: 2026 lori28167 */
/* The structure and some code are adapted from PipeWire's spa/plugins/bluez5/a2dp-codec-ldac.c, which is MIT
 * licensed (Copyright © 2018 Wim Taymans and contributors); see third_party/pipewire-bluez5/COPYING.pipewire. */
/*
 * Samsung Scalable Codec (SSC) A2DP source codec for the PipeWire bluez5 plugin.
 *
 * One A2DP packet is the RTP header followed by exactly one SSC frame (see NOTES.md). The PipeWire graph feeds
 * the codec interleaved stereo F32, S32 or S16 (the codec has 24 bit of input precision, so the wider formats are
 * preferred and go to the encoder without being rounded to 16 bit); a frame consumes 864 samples per channel.
 *
 * PipeWire loads codec plugins from a fixed list of names. This plugin exports the factory of the "aptx" slot
 * (api.bluez5.codec.media.aptx) and so it must be installed as libspa-codec-bluez5-aptx.so; it uses the aptx-hd
 * codec id only to be ordered above AAC in the host's preference list.
 *
 * Two codecs share the A2DP endpoint (like SBC and SBC-XQ): "ssc" (44.1/48 kHz, configuration byte 0x10|0x08) and
 * "ssc_uhq" (96 kHz, configuration byte 0x02|0x08, frames of 9 ms, two per packet), the latter selectable as its
 * own profile.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <spa/utils/string.h>
#include <spa/param/audio/format.h>
#include <spa/pod/builder.h>

#include "rtp.h"
#include "media-codecs.h"
#include "ssc.h"
#include "a2dp-codec-ssc.h"

#define SSC_CFG_RATE_48000	0x10
#define SSC_CFG_RATE_44100	0x20
#define SSC_CFG_UHQ		0x02	/* 96 kHz, 24 bit */
#define SSC_CFG_NO_LIMIT	0x08	/* sink states no bitrate limitation; echoed back in the configuration */

/* Codec delay in samples: the MDCT overlap */
#define SSC_CODEC_DELAY		108

typedef struct {
	a2dp_vendor_codec_t info;
	uint8_t config;
} __attribute__((packed)) a2dp_ssc_t;

static struct spa_log *log_;

struct impl {
	ssc_encoder *enc;
	int preset;
	int min_preset;
	int max_preset;
	int packets_since_reduce;
	int healthy_queue_packets;
	int stats_packets;
	int stats_empty;
	size_t stats_max_unsent;
	int rate;
	bool uhq;
	int frames_per_packet;
	int frames_in_packet;
	uint32_t format;	/* SPA_AUDIO_FORMAT_* of the PCM handed to codec_encode */
	int block_size;		/* bytes of PCM consumed per frame */
	float conv[SSC_FRAME_SAMPLES * 2];	/* S32 input converted to float */
};

extern const struct media_codec a2dp_codec_ssc_uhq;

static const struct media_codec_config ssc_rates[] = {
	{ SSC_CFG_RATE_48000, 48000, 1 },
	{ SSC_CFG_RATE_44100, 44100, 0 },
};

static int codec_fill_caps(const struct media_codec *codec, uint32_t flags,
		const struct spa_dict *settings, uint8_t caps[A2DP_MAX_CAPS_SIZE])
{
	static const a2dp_ssc_t a2dp_ssc = {
		.info.vendor_id = SSC_VENDOR_ID,
		.info.codec_id = SSC_CODEC_ID,
		.config = SSC_CFG_RATE_48000 | SSC_CFG_RATE_44100 | SSC_CFG_NO_LIMIT,
	};

	memcpy(caps, &a2dp_ssc, sizeof(a2dp_ssc));
	return sizeof(a2dp_ssc);
}

static int codec_select_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size,
		const struct media_codec_audio_info *info,
		const struct spa_dict *settings, uint8_t config[A2DP_MAX_CAPS_SIZE],
		void **config_data)
{
	a2dp_ssc_t conf;
	int i;

	if (caps_size < sizeof(conf))
		return -EINVAL;
	memcpy(&conf, caps, sizeof(conf));

	if (codec->vendor.vendor_id != conf.info.vendor_id ||
	    codec->vendor.codec_id != conf.info.codec_id)
		return -ENOTSUP;

	if (codec == &a2dp_codec_ssc_uhq) {
		if (!(conf.config & SSC_CFG_UHQ))
			return -ENOTSUP;
		conf.config = SSC_CFG_UHQ | (conf.config & SSC_CFG_NO_LIMIT);
		memcpy(config, &conf, sizeof(conf));
		return sizeof(conf);
	}

	i = media_codec_select_config(ssc_rates, SPA_N_ELEMENTS(ssc_rates), conf.config,
			info ? info->rate : A2DP_CODEC_DEFAULT_RATE);
	if (i < 0)
		return -ENOTSUP;
	/* Normal (non HiFi, non UHQ) mode: one sample-rate bit, plus the sink's "no limitation" bit */
	conf.config = ssc_rates[i].config | (conf.config & SSC_CFG_NO_LIMIT);

	memcpy(config, &conf, sizeof(conf));
	return sizeof(conf);
}

static int codec_enum_config(const struct media_codec *codec, uint32_t flags,
		const void *caps, size_t caps_size, uint32_t id, uint32_t idx,
		struct spa_pod_builder *b, struct spa_pod **param)
{
	a2dp_ssc_t conf;
	struct spa_pod_frame f[2];
	struct spa_pod_choice *choice;
	uint32_t position[2] = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR };
	uint32_t i = 0;

	if (caps_size < sizeof(conf))
		return -EINVAL;
	memcpy(&conf, caps, sizeof(conf));

	if (idx > 0)
		return 0;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(b,
			SPA_FORMAT_mediaType,	SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_S16),
			0);
	spa_pod_builder_prop(b, SPA_FORMAT_AUDIO_rate, 0);

	spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_None, 0);
	choice = (struct spa_pod_choice *)spa_pod_builder_frame(b, &f[1]);
	if (conf.config & SSC_CFG_UHQ) {
		/* a UHQ configuration is 96 kHz only */
		spa_pod_builder_int(b, 96000);
		spa_pod_builder_int(b, 96000);
		i = 1;
	} else if (conf.config & SSC_CFG_RATE_48000) {
		if (i++ == 0)
			spa_pod_builder_int(b, 48000);
		spa_pod_builder_int(b, 48000);
	}
	if (!(conf.config & SSC_CFG_UHQ) && (conf.config & SSC_CFG_RATE_44100)) {
		if (i++ == 0)
			spa_pod_builder_int(b, 44100);
		spa_pod_builder_int(b, 44100);
	}
	if (i > 1)
		choice->body.type = SPA_CHOICE_Enum;
	spa_pod_builder_pop(b, &f[1]);

	if (i == 0)
		return -EINVAL;

	spa_pod_builder_add(b,
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(2),
			SPA_FORMAT_AUDIO_position, SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Id, 2, position),
			0);
	*param = spa_pod_builder_pop(b, &f[0]);
	return *param == NULL ? -EIO : 1;
}

static int preset_from_name(const char *s)
{
	static const struct { const char *name; int preset; } names[] = {
		{ "88k", SSC_PRESET_88K }, { "96k", SSC_PRESET_96K }, { "128k", SSC_PRESET_128K },
		{ "192k", SSC_PRESET_192K }, { "229k", SSC_PRESET_229K },
		{ "uhq152k", SSC_PRESET_UHQ_152K }, { "uhq291k", SSC_PRESET_UHQ_291K }, { "uhq584k", SSC_PRESET_UHQ_584K },
	};
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(names); i++)
		if (spa_streq(s, names[i].name))
			return names[i].preset;
	return -1;
}

static void *codec_init(const struct media_codec *codec, uint32_t flags,
		void *config, size_t config_len, const struct spa_audio_info *info,
		void *props, size_t mtu)
{
	struct impl *this;
	a2dp_ssc_t *conf = config;
	const char *env;
	int res = 0, preset, lowest, sample_size;
	bool uhq;

	if (config_len < sizeof(*conf) || info == NULL)
		return NULL;
	if (info->media_type != SPA_MEDIA_TYPE_audio || info->media_subtype != SPA_MEDIA_SUBTYPE_raw ||
	    info->info.raw.channels != 2) {
		errno = EINVAL;
		return NULL;
	}
	switch (info->info.raw.format) {
	case SPA_AUDIO_FORMAT_F32:
	case SPA_AUDIO_FORMAT_S32:
		sample_size = 4;
		break;
	case SPA_AUDIO_FORMAT_S16:
		sample_size = 2;
		break;
	default:
		errno = EINVAL;
		return NULL;
	}

	this = calloc(1, sizeof(*this));
	if (this == NULL)
		return NULL;

	uhq = (conf->config & SSC_CFG_UHQ) != 0;
	this->uhq = uhq;
	this->rate = uhq ? 96000 : (conf->config & SSC_CFG_RATE_48000) ? 48000 : 44100;
	preset = uhq ? SSC_PRESET_UHQ_291K : SSC_PRESET_229K;
	lowest = uhq ? SSC_PRESET_UHQ_152K : SSC_PRESET_88K;
	/* Preset for experiments: SSC_PRESET=88k|96k|128k|192k|229k (44.1/48 kHz) or uhq152k|uhq291k|uhq584k in the PipeWire environment */
	env = getenv("SSC_PRESET");
	if (env != NULL && preset_from_name(env) >= 0 && (ssc_preset_is_uhq(preset_from_name(env)) == 1) == uhq)
		preset = preset_from_name(env);
	/* the RTP header and one frame must fit in the L2CAP MTU */
	while (preset > lowest && sizeof(struct rtp_header) + (size_t)ssc_frame_bytes(preset) > mtu)
		preset--;
	/* UHQ: like the phone, two 9 ms frames per packet when they fit (the sink takes that for the 152k and 291k presets) */
	this->frames_per_packet = (uhq && preset != SSC_PRESET_UHQ_584K &&
			sizeof(struct rtp_header) + 2 * (size_t)ssc_frame_bytes(preset) <= mtu) ? 2 : 1;
	if (getenv("SSC_UHQ_FRAMES") != NULL && atoi(getenv("SSC_UHQ_FRAMES")) == 1)
		this->frames_per_packet = 1;	/* experiment: one frame per packet */
	if (sizeof(struct rtp_header) + (size_t)ssc_frame_bytes(preset) > mtu) {
		errno = EINVAL;
		goto error;
	}
	this->preset = preset;
	this->min_preset = lowest;
	this->max_preset = preset;
	this->packets_since_reduce = SSC_ABR_REDUCE_COOLDOWN;
	this->format = info->info.raw.format;
	this->block_size = SSC_FRAME_SAMPLES * 2 * sample_size;
	if (uhq && info->info.raw.rate != 96000) {
		errno = EINVAL;
		goto error;
	}
	this->enc = ssc_encoder_new(this->rate, 2, &res);
	if (this->enc == NULL) {
		errno = res == SSC_ERR_MEMORY ? ENOMEM : EINVAL;
		goto error;
	}
	if (getenv("SSC_NOPF") != NULL)
		ssc_encoder_set_prefilter(this->enc, 0);
	spa_log_info(log_, "SSC: rate %d%s, format %s, preset %d (%d bit/s, %d byte frames, %d per packet), mtu %zu",
			this->rate, uhq ? " (UHQ)" : "", this->format == SPA_AUDIO_FORMAT_F32 ? "F32" : this->format == SPA_AUDIO_FORMAT_S32 ? "S32" : "S16",
			preset, ssc_preset_bitrate(preset), ssc_frame_bytes(preset), this->frames_per_packet, mtu);
	return this;

error:
	free(this);
	return NULL;
}

static void codec_deinit(void *data)
{
	struct impl *this = data;

	ssc_encoder_free(this->enc);
	free(this);
}

static int codec_get_block_size(void *data)
{
	struct impl *this = data;

	return this->block_size;
}

static int codec_start_encode(void *data, void *dst, size_t dst_size, uint16_t seqnum, uint32_t timestamp)
{
	struct impl *this = data;
	struct rtp_header *header = dst;

	this->frames_in_packet = 0;
	if (dst_size < sizeof(*header))
		return -EINVAL;
	memset(header, 0, sizeof(*header));
	header->v = 2;
	header->pt = 96;
	header->sequence_number = htons(seqnum);
	header->timestamp = htonl(timestamp);
	header->ssrc = htonl(1);
	return sizeof(*header);
}

static int codec_encode(void *data, const void *src, size_t src_size,
		void *dst, size_t dst_size, size_t *dst_out, int *need_flush)
{
	struct impl *this = data;
	int n;

	*dst_out = 0;
	*need_flush = NEED_FLUSH_NO;
	if (src == NULL || src_size < (size_t)this->block_size)
		return 0;

	switch (this->format) {
	case SPA_AUDIO_FORMAT_F32:
		n = ssc_encode_float(this->enc, src, this->preset, dst, dst_size);
		break;
	case SPA_AUDIO_FORMAT_S32: {
		const int32_t *in = src;
		int i;

		for (i = 0; i < SSC_FRAME_SAMPLES * 2; i++)
			this->conv[i] = (float)in[i] * (1.0f / 2147483648.0f);
		n = ssc_encode_float(this->enc, this->conv, this->preset, dst, dst_size);
		break;
	}
	default:
		n = ssc_encode(this->enc, src, this->preset, dst, dst_size);
		break;
	}
	if (SPA_UNLIKELY(n < 0))
		return -EINVAL;

	*dst_out = n;
	this->frames_in_packet++;
	*need_flush = this->frames_in_packet >= this->frames_per_packet ? NEED_FLUSH_ALL : NEED_FLUSH_NO;
	return this->block_size;
}

static void change_preset(struct impl *this, int preset, const char *reason, size_t unsent)
{
	spa_log_info(log_, "SSC: %d -> %d bit/s (%s, %zu bytes queued)",
			ssc_preset_bitrate(this->preset), ssc_preset_bitrate(preset), reason, unsent);
	this->preset = preset;
}

/*
 * Bitrate adaptation skips the 192k preset: on Galaxy Buds2 Pro it sounds wrong even when it is the only preset of
 * the stream (tested 2026-11-09), while 229k, 128k, 96k and 88k sound right. It has not been confirmed on any other
 * earbuds either. It stays reachable as the ceiling when SSC_PRESET=192k asks for it.
 */
static int preset_below(const struct impl *this)
{
	int preset = this->preset - 1;

	return preset == SSC_PRESET_192K ? preset - 1 : preset;
}

static int preset_above(const struct impl *this)
{
	int preset = this->preset + 1;

	return preset == SSC_PRESET_192K && this->max_preset > SSC_PRESET_192K ? preset + 1 : preset;
}

static void step_down(struct impl *this, const char *reason, size_t unsent)
{
	if (this->preset > this->min_preset)
		change_preset(this, preset_below(this), reason, unsent);
	this->packets_since_reduce = 0;
	this->healthy_queue_packets = 0;
}

static size_t packet_bytes(const struct impl *this)
{
	return sizeof(struct rtp_header) + (size_t)this->frames_per_packet * (size_t)ssc_frame_bytes(this->preset);
}

static void collect_queue_stats(struct impl *this, size_t unsent)
{
	this->stats_max_unsent = SPA_MAX(this->stats_max_unsent, unsent);
	if (unsent == 0)
		this->stats_empty++;
	if (++this->stats_packets < SSC_ABR_INCREASE_HOLD)
		return;
	spa_log_debug(log_, "SSC: %d bit/s, last %d packets: queue empty %d times, max %zu bytes (packet %zu bytes)",
			ssc_preset_bitrate(this->preset), this->stats_packets, this->stats_empty, this->stats_max_unsent, packet_bytes(this));
	this->stats_packets = 0;
	this->stats_empty = 0;
	this->stats_max_unsent = 0;
}

static int codec_reduce_bitpool(void *data)
{
	struct impl *this = data;

	step_down(this, "socket full", 0);
	return ssc_preset_bitrate(this->preset);
}

static int codec_abr_process(void *data, size_t unsent)
{
	struct impl *this = data;

	collect_queue_stats(this, unsent);
	if (this->packets_since_reduce < SSC_ABR_REDUCE_COOLDOWN)
		this->packets_since_reduce++;
	if (unsent > SSC_ABR_QUEUE_PACKETS * packet_bytes(this)) {
		this->healthy_queue_packets = 0;
		if (this->packets_since_reduce >= SSC_ABR_REDUCE_COOLDOWN)
			step_down(this, "queue growing", unsent);
	} else if (unsent > packet_bytes(this)) {
		this->healthy_queue_packets = 0;
	} else if (this->preset < this->max_preset && ++this->healthy_queue_packets >= SSC_ABR_INCREASE_HOLD) {
		change_preset(this, preset_above(this), "queue healthy", unsent);
		this->healthy_queue_packets = 0;
	}
	return ssc_preset_bitrate(this->preset);
}

static void codec_get_delay(void *data, uint32_t *encoder, uint32_t *decoder)
{
	if (encoder)
		*encoder = SSC_CODEC_DELAY;
	if (decoder)
		*decoder = 0;
}

static void codec_set_log(struct spa_log *global_log)
{
	log_ = global_log;
	spa_log_topic_init(log_, &codec_plugin_log_topic);
}

const struct media_codec a2dp_codec_ssc = {
	/* aptx-hd id: ordered above AAC and not special-cased anywhere else in the host */
	.id = SPA_BLUETOOTH_AUDIO_CODEC_APTX_HD,
	.kind = MEDIA_CODEC_A2DP,
	.codec_id = A2DP_CODEC_VENDOR,
	.vendor = { .vendor_id = SSC_VENDOR_ID, .codec_id = SSC_CODEC_ID },
	.name = "ssc",
	.description = "Samsung Scalable Codec (SSC)",
	.fill_caps = codec_fill_caps,
	.select_config = codec_select_config,
	.enum_config = codec_enum_config,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.abr_process = codec_abr_process,
	.reduce_bitpool = codec_reduce_bitpool,
	.get_delay = codec_get_delay,
	.set_log = codec_set_log,
};

const struct media_codec a2dp_codec_ssc_uhq = {
	/* an id of its own is needed for the profile list; aptx-ll is not available next to this plugin */
	.id = SPA_BLUETOOTH_AUDIO_CODEC_APTX_LL,
	.kind = MEDIA_CODEC_A2DP,
	.codec_id = A2DP_CODEC_VENDOR,
	.vendor = { .vendor_id = SSC_VENDOR_ID, .codec_id = SSC_CODEC_ID },
	.name = "ssc_uhq",
	.description = "Samsung Scalable Codec UHQ (96 kHz)",
	.endpoint_name = "ssc",
	.fill_caps = NULL,
	.select_config = codec_select_config,
	.enum_config = codec_enum_config,
	.init = codec_init,
	.deinit = codec_deinit,
	.get_block_size = codec_get_block_size,
	.start_encode = codec_start_encode,
	.encode = codec_encode,
	.abr_process = codec_abr_process,
	.reduce_bitpool = codec_reduce_bitpool,
	.get_delay = codec_get_delay,
	.set_log = codec_set_log,
};

MEDIA_CODEC_EXPORT_DEF(
	"aptx",
	&a2dp_codec_ssc,
	&a2dp_codec_ssc_uhq
);
