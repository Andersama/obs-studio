
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

	/*
	pthread_cond_t screenshot_cond;
	pthread_mutex_t screenshot_mutex;
	bool screenshot_needed;
	AVPacket screenshot;
	*/
};

static const char *caffeine_get_name(void *data)
{
	UNUSED_PARAMETER(data);

	return obs_module_text("CaffeineOutput");
}

/* Converts libcaffeine log levels to OBS levels. NONE or unrecognized values
 * return 0 to indicate the message shouldn't be logged
 *
 * Note: webrtc uses INFO for debugging messages, not meant to be user-facing,
 * so this will never return LOG_INFO
 */
static int caffeine_to_obs_log_level(caff_LogLevel level)
{
	switch (level)
	{
	case caff_LogLevelSensitive:
	case caff_LogLevelVerbose:
	case caff_LogLevelInfo:
		return LOG_DEBUG;
	case caff_LogLevelWarning:
		return LOG_WARNING;
	case caff_LogLevelError:
		return LOG_ERROR;
	case caff_LogLevelNone:
	default:
		return 0;
	}
}

static int caffeine_to_obs_error(caff_Result error)
{
	switch (error)
	{
	case caff_ResultOutOfCapacity:
	case caff_ResultRequestFailed:
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

/* TODO: figure out why libcaffeine isn't calling this */
static void caffeine_log(caff_LogLevel level, char const * message)
{
	int log_level = caffeine_to_obs_log_level(level);
	if (log_level)
		blog(log_level, "[libcaffeine] %s", message);
}

static void *caffeine_create(obs_data_t *settings, obs_output_t *output)
{
	trace();
	UNUSED_PARAMETER(settings);

	struct caffeine_output *context =
		bzalloc(sizeof(struct caffeine_output));
	context->output = output;
	context->instance = caff_initialize(caffeine_log, caff_LogLevelInfo);

	/*
	pthread_mutex_init(&context->screenshot_mutex, NULL);
	pthread_cond_init(&context->screenshot_cond, NULL);
	*/

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
	case caff_ResultRequestFailed:
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

	/*
	pthread_mutex_lock(&context->screenshot_mutex);
	context->screenshot_needed = true;
	av_init_packet(&context->screenshot);
	context->screenshot.data = NULL;
	context->screenshot.size = 0;
	pthread_mutex_unlock(&context->screenshot_mutex);
	*/

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

static void caffeine_stream_started(void *data)
{
	trace();
	struct caffeine_output *context = data;
	obs_output_begin_data_capture(context->output, 0);
}

static void caffeine_stream_ended(struct caffeine_output *context);

static void caffeine_stream_failed(void *data, caff_Result error)
{
	struct caffeine_output *context = data;

	if (!obs_output_get_last_error(context->output)) {
		set_error(context->output, "%s: [%d] %s",
			obs_module_text("ErrorStartStream"),
			error,
			caff_resultString(error));
	}

	caffeine_stream_ended(context);

	obs_output_signal_stop(context->output, caffeine_to_obs_error(error));
}

/* TODO refactor
static char const *get_game_id(struct caffeine_games *games,
		char *const process_name)
{
	if (games && process_name) {
		for (size_t game_index = 0; game_index < games->num_games;
			++game_index) {

			struct caffeine_game_info *info =
				games->game_infos[game_index];
			if (!info)
				continue;

			for (size_t pname_index = 0;
				pname_index < info->num_process_names;
				++pname_index) {

				char const *pname =
					info->process_names[pname_index];
				if (!pname)
			    		continue;
				if (strcmp(process_name, pname) == 0)
			    		return info->id;
			}
		}
	}

	return NULL;
}

// Falls back to obs_id if no foreground game detected
static char const *get_running_game_id(struct caffeine_games *games,
		const char *fallback_id)
{
	char *foreground_process = get_foreground_process_name();
	char const *id = get_game_id(games, foreground_process);
	bfree(foreground_process);
	return id ? id : fallback_id;
}

*/

/* Called while screenshot_mutex is locked */
/* Adapted from https://github.com/obsproject/obs-studio/blob/3ddca5863c4d1917ad8443a9ad288f41accf9e39/UI/window-basic-main.cpp#L1741 */
/* TODO REFACTOR 
static void create_screenshot(struct caffeine_output *context, uint32_t width,
		uint32_t height, uint8_t *image_data[MAX_AV_PLANES],
		uint32_t image_data_linesize[MAX_AV_PLANES],
		enum video_format format)
{
	trace();

	AVCodec           *codec         = NULL;
	AVCodecContext    *codec_context = NULL;
	AVFrame           *frame         = NULL;
	struct SwsContext *sws_context   = NULL;
	int               got_output     = 0;
	int               ret            = 0;

	if (image_data == NULL) {
		log_warn("No image data for screenshot");
		goto err_no_image_data;
	}

	// Write JPEG output using libavcodec
	codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);

	if (codec == NULL) {
		log_warn("Unable to load screenshot encoder");
		goto err_jpeg_codec_not_found;
	}

	codec_context = avcodec_alloc_context3(codec);

	if (codec_context == NULL) {
		log_warn("Couldn't allocate codec context");
		goto err_jpeg_encoder_context_alloc;
	}

	codec_context->width         = width;
	codec_context->height        = height;
	codec_context->pix_fmt       = AV_PIX_FMT_YUVJ422P;
	codec_context->time_base.num = 1;
	codec_context->time_base.den = 30;
	codec_context->bit_rate      = 10000000;
	codec_context->codec_id      = codec->id;
	codec_context->codec_type    = AVMEDIA_TYPE_VIDEO;

	if (avcodec_open2(codec_context, codec, NULL) != 0) {
		log_warn("Couldn't open codec");
		goto err_jpeg_encoder_open;
	}

	frame = av_frame_alloc();

	if (frame == NULL) {
		log_warn("Couldn't allocate frame");
		goto err_av_frame_alloc;
	}

	frame->pts    = 1;
	frame->format = AV_PIX_FMT_YUVJ422P;
	frame->width  = width;
	frame->height = height;

	ret = av_image_alloc(frame->data, frame->linesize, codec_context->width, 
			codec_context->height, codec_context->pix_fmt, 32);

	if (ret < 0) {
		log_warn("Couldn't allocate image");
		goto err_av_image_alloc;
	}

	enum AVPixelFormat src_format = obs_to_ffmpeg_video_format(format);

	// Copy image data, converting RGBA to
	// image format expected by JPEG encoder
	sws_context = sws_getContext(frame->width, frame->height, src_format,
			frame->width, frame->height, codec_context->pix_fmt,
			0, NULL, NULL, NULL);

	if (sws_context == NULL) {
		log_warn("Couldn't get scaling context");
		goto err_sws_getContext;
	}

	// Transform RGBA to RGB24
	ret = sws_scale(sws_context, image_data, image_data_linesize, 0,
			frame->height, frame->data, frame->linesize);

	if (ret < 0) {
		log_warn("Couldn't translate image format");
		goto err_sws_scale;
	}

	av_init_packet(&context->screenshot);
	context->screenshot.data = NULL;
	context->screenshot.size = 0;

	ret = avcodec_encode_video2(codec_context, &context->screenshot,
			frame, &got_output);

	if (ret != 0 || !got_output) {
		log_warn("Failed to generate screenshot. avcodec_encode_video2 returned %d",
				ret);
		goto err_encode;
	}

err_encode:
err_sws_scale:
	sws_freeContext(sws_context);
	sws_context = NULL;
err_sws_getContext:
	av_freep(frame->data);
	frame->data[0] = NULL;
err_av_image_alloc:
	av_frame_free(&frame);
	frame = NULL;
err_av_frame_alloc:
	avcodec_close(codec_context);
err_jpeg_encoder_open:
	avcodec_free_context(&codec_context);
	codec_context = NULL;
err_jpeg_encoder_context_alloc:
err_jpeg_codec_not_found:
err_no_image_data:

	context->screenshot_needed = false;
	pthread_cond_signal(&context->screenshot_cond);
}
*/

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

	/*
	pthread_mutex_lock(&context->screenshot_mutex);
	if (context->screenshot_needed)
		create_screenshot(context, width, height, frame->data,
			frame->linesize, context->video_info.output_format);
	pthread_mutex_unlock(&context->screenshot_mutex);
	*/

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

static void caffeine_stream_ended(struct caffeine_output * context)
{
	trace();
	/*
	pthread_mutex_lock(&context->screenshot_mutex);

	if (context->screenshot.data != NULL) {
		av_free_packet(&context->screenshot);
	}

	context->screenshot_needed = false;

	pthread_mutex_unlock(&context->screenshot_mutex);
	*/
}

static void caffeine_stop(void *data, uint64_t ts)
{
	trace();
	/* TODO: do something with this? */
	UNUSED_PARAMETER(ts);

	struct caffeine_output *context = data;
	obs_output_t *output = context->output;

	caff_endBroadcast(context->instance);
	caffeine_stream_ended(context);

	obs_output_end_data_capture(output);
}

static void caffeine_destroy(void *data)
{
	trace();
	struct caffeine_output *context = data;
	/*
	pthread_mutex_destroy(&context->screenshot_mutex);
	pthread_cond_destroy(&context->screenshot_cond);
	*/
	caff_deinitialize(&context->instance);

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
