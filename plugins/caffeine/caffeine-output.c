
#include <obs-module.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "obs-ffmpeg-formats.h"

#include <util/base.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <caffeine.h>

#include "caffeine-foreground-process.h"
#include "caffeine-settings.h"

#define CAFFEINE_LOG_TITLE "caffeine output"
#include "caffeine-log.h"

/* Uncomment this to log each call to raw_audio/video
#define TRACE_FRAMES
/**/

struct caffeine_output
{
	obs_output_t * output;
	caff_InstanceHandle instance;
	struct obs_video_info video_info;
	uint64_t start_timestamp;
	size_t audio_planes;
	size_t audio_size;

	volatile bool is_online;
	pthread_t game_detection_thread;
	char * foreground_process;
	char * game_id;
};

static const char *caffeine_get_name(void *data)
{
	UNUSED_PARAMETER(data);

	return obs_module_text("CaffeineOutput");
}

static int caffeine_to_obs_error(caff_Result error)
{
	switch (error)
	{
	case caff_ResultOutOfCapacity:
	case caff_ResultFailure:
	case caff_ResultBroadcastFailed:
		return OBS_OUTPUT_CONNECT_FAILED;
	case caff_ResultDisconnected:
		return OBS_OUTPUT_DISCONNECTED;
	case caff_ResultTakeover:
	default:
		return OBS_OUTPUT_ERROR;
	}
}

caff_VideoFormat obs_to_caffeine_format(enum video_format format)
{
	switch (format)
	{
	case VIDEO_FORMAT_I420:
		return caff_VideoFormatI420;
	case VIDEO_FORMAT_NV12:
		return caff_VideoFormatNv12;
	case VIDEO_FORMAT_YUY2:
		return caff_VideoFormatYuy2;
	case VIDEO_FORMAT_UYVY:
		return caff_VideoFormatUyvy;
	case VIDEO_FORMAT_BGRA:
		return caff_VideoFormatBgra;

	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_I444:
	case VIDEO_FORMAT_Y800:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_YVYU:
	default:
		return caff_VideoFormatUnknown;
	}
}

static void *caffeine_create(obs_data_t *settings, obs_output_t *output)
{
	trace();
	UNUSED_PARAMETER(settings);

	struct caffeine_output *context =
		bzalloc(sizeof(struct caffeine_output));
	context->output = output;

	/* TODO: can we get this from the CaffeineAuth object somehow? */
	context->instance = caff_createInstance();

	return context;
}

static void caffeine_stream_started(void *data);
static void caffeine_stream_failed(void *data, caff_Result error);

static int const enforced_height = 720;
static double const max_ratio = 3.0;
static double const min_ratio = 1.0/3.0;

static bool caffeine_authenticate(struct caffeine_output *context)
{
	trace();

	obs_output_t *output = context->output;

	obs_service_t *service    = obs_output_get_service(output);
	char const *refresh_token = obs_service_get_key(service);

	if (strcmp(refresh_token, "") == 0) {
		set_error(output, "%s", obs_module_text("ErrorMustSignIn"));
		return false;
	}

	switch (caff_refreshAuth(context->instance, refresh_token)) {
	case caff_ResultSuccess:
		return true;
	case caff_ResultOldVersion:
		set_error(output, "%s", obs_module_text("ErrorOldVersion"));
		return false;
	case caff_ResultInfoIncorrect:
		set_error(output, "%s", obs_module_text("SigninFailed"));
		return false;
	case caff_ResultLegalAcceptanceRequired:
		set_error(output, "%s", obs_module_text("TosAcceptanceRequired"));
		return false;
	case caff_ResultEmailVerificationRequired:
		set_error(output, "%s", obs_module_text("EmailVerificationRequired"));
		return false;
	case caff_ResultMfaOtpRequired:
		set_error(output, "%s", obs_module_text("OtpRequired"));
		return false;
	case caff_ResultMfaOtpIncorrect:
		set_error(output, "%s", obs_module_text("OtpIncorrect"));
		return false;
	case caff_ResultFailure:
		set_error(output, "%s", obs_module_text("NoAuthResponse"));
		return false;
	default:
		set_error(output, "%s", obs_module_text("SigninFailed"));
		return false;
	}
}

static bool caffeine_start(void *data)
{
	trace();
	struct caffeine_output *context = data;
	if (!caffeine_authenticate(context))
		return false;

	if (!obs_get_video_info(&context->video_info)) {
		set_error(context->output, "Failed to get video info");
		return false;
	}

	if (context->video_info.output_height != enforced_height)
		log_warn("For best video quality and reduced CPU usage, set output resolution to 720p");

	double ratio = (double)context->video_info.output_width /
		context->video_info.output_height;
	if (ratio < min_ratio || ratio > max_ratio) {
		set_error(context->output, "%s",
			obs_module_text("ErrorAspectRatio"));
		return false;
	}

	caff_VideoFormat format =
		obs_to_caffeine_format(context->video_info.output_format);

	if (format == caff_VideoFormatUnknown) {
		set_error(context->output, "%s %s",
			obs_module_text("ErrorVideoFormat"),
			get_video_format_name(context->video_info.output_format));
		return false;
	}

	struct audio_convert_info conversion = {
		.format = AUDIO_FORMAT_16BIT,
		.speakers = SPEAKERS_STEREO,
		.samples_per_sec = 48000
	};
	obs_output_set_audio_conversion(context->output, &conversion);

	context->audio_planes =
		get_audio_planes(conversion.format, conversion.speakers);
	context->audio_size =
		get_audio_size(conversion.format, conversion.speakers, 1);

	if (!obs_output_can_begin_data_capture(context->output, 0))
		return false;

	obs_service_t *service = obs_output_get_service(context->output);
	obs_data_t *settings   = obs_service_get_settings(service);
	char const *title  =
		obs_data_get_string(settings, BROADCAST_TITLE_KEY);

	if (strcmp(title, "") == 0)
		title = obs_module_text("DefaultBroadcastTitle");

	caff_Rating rating = (caff_Rating)
		obs_data_get_int(settings, BROADCAST_RATING_KEY);

	caff_Result error =
		caff_startBroadcast(context->instance, context, title, rating,
			caffeine_stream_started, caffeine_stream_failed);
	if (error) {
		set_error(context->output, "%s",
			obs_module_text("ErrorStartStream"));
		return false;
	}

	return true;
}

static void enumerate_games(void *data, char const *processName, char const *game_id, char const *gameName)
{
	struct caffeine_output *context = data;
	if (strcmp(processName, context->foreground_process) == 0) {
		log_debug("Detected game [%s]: %s", game_id, gameName);
		bfree(context->game_id);
		context->game_id = bstrdup(game_id);
	}
}

static void * game_detection_thread(void *data)
{
	trace();
	struct caffeine_output *context = data;
	uint32_t const stop_interval = 100/*ms*/;
	uint32_t const check_interval = 5000/*ms*/;

	uint32_t cur_interval = 0;
	while (context->is_online) {
		os_sleep_ms(stop_interval);
		cur_interval += stop_interval;

		if (cur_interval < check_interval)
			continue;

		cur_interval = 0;
		context->game_id = NULL;
		context->foreground_process = get_foreground_process_name();
		if (context->foreground_process) {
			caff_enumerateGames(context->instance, context, enumerate_games);
			bfree(context->foreground_process);
			context->foreground_process = NULL;
		}
		if (context->game_id) {
			caff_setGameId(context->instance, context->game_id);
			bfree(context->game_id);
		}
	}
	return NULL;
}

static void caffeine_stream_started(void *data)
{
	trace();
	struct caffeine_output *context = data;
	context->is_online = true;
	pthread_create(&context->game_detection_thread, NULL, game_detection_thread, context);
	obs_output_begin_data_capture(context->output, 0);
}

static void caffeine_stream_failed(void *data, caff_Result error)
{
	struct caffeine_output *context = data;

	if (!obs_output_get_last_error(context->output)) {
		set_error(context->output, "%s: [%d] %s",
			obs_module_text("ErrorStartStream"),
			error,
			caff_resultString(error));
	}

	if (context->is_online) {
		context->is_online = false;
		pthread_join(context->game_detection_thread, NULL);
	}

	obs_output_signal_stop(context->output, caffeine_to_obs_error(error));
}

static void caffeine_raw_video(void *data, struct video_data *frame)
{
#ifdef TRACE_FRAMES
	trace();
#endif
	struct caffeine_output *context = data;

	uint32_t width = context->video_info.output_width;
	uint32_t height = context->video_info.output_height;
	size_t total_bytes = frame->linesize[0] * height;
	caff_VideoFormat caff_VideoFormat =
		obs_to_caffeine_format(context->video_info.output_format);

	if (!context->start_timestamp)
		context->start_timestamp = frame->timestamp;

	caff_sendVideo(context->instance, frame->data[0], total_bytes,
		width, height, caff_VideoFormat);
}

/* This fixes an issue where unencoded outputs have video & audio out of sync
 *
 * Copied/adapted from obs-outputs/flv-output
 */
static bool prepare_audio(struct caffeine_output *context,
		const struct audio_data *frame, struct audio_data *output)
{
	*output = *frame;

	const uint64_t NANOSECONDS = 1000000000;
	const uint64_t SAMPLES = 48000;

	if (frame->timestamp < context->start_timestamp) {
		uint64_t duration = (uint64_t)frame->frames * NANOSECONDS / SAMPLES;
		uint64_t end_ts = (frame->timestamp + duration);
		uint64_t cutoff;

		if (end_ts <= context->start_timestamp)
			return false;

		cutoff = context->start_timestamp - frame->timestamp;
		output->timestamp += cutoff;

		cutoff = cutoff * SAMPLES / NANOSECONDS;

		for (size_t i = 0; i < context->audio_planes; i++)
			output->data[i] += context->audio_size * (uint32_t)cutoff;
		output->frames -= (uint32_t)cutoff;
	}

	return true;
}

static void caffeine_raw_audio(void *data, struct audio_data *frames)
{
#ifdef TRACE_FRAMES
	trace();
#endif
	struct caffeine_output *context = data;
	struct audio_data in;

	if (!context->start_timestamp)
		return;
	if (!prepare_audio(context, frames, &in))
		return;

	caff_sendAudio(context->instance, in.data[0], in.frames);
}

static void caffeine_stop(void *data, uint64_t ts)
{
	trace();
	/* TODO: do something with this? */
	UNUSED_PARAMETER(ts);

	struct caffeine_output *context = data;
	obs_output_t *output = context->output;

	if (context->is_online) {
		context->is_online = false;
		pthread_join(context->game_detection_thread, NULL);
	}

	caff_endBroadcast(context->instance);

	obs_output_end_data_capture(output);
}

static void caffeine_destroy(void *data)
{
	trace();
	struct caffeine_output *context = data;
	caff_freeInstance(&context->instance);

	bfree(data);
}

static float caffeine_get_congestion(void * data)
{
	struct caffeine_output * context = data;

	caff_ConnectionQuality quality = caff_getConnectionQuality(context->instance);

	switch (quality) {
	case caff_ConnectionQualityGood:
		return 0.f;
	case caff_ConnectionQualityPoor:
		return 1.f;
	default:
		return 0.5f;
	}
}

struct obs_output_info caffeine_output_info = {
	.id             = "caffeine_output",
	.flags          = OBS_OUTPUT_AV | OBS_OUTPUT_SERVICE,
	.get_name       = caffeine_get_name,
	.create         = caffeine_create,
	.start          = caffeine_start,
	.raw_video      = caffeine_raw_video,
	.raw_audio      = caffeine_raw_audio,
	.stop           = caffeine_stop,
	.destroy        = caffeine_destroy,
	.get_congestion = caffeine_get_congestion,
};
