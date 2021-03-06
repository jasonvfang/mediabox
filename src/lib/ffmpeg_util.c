/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2017 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as 
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifdef HAVE_CONFIG_H
#       include <libavbox/config.h>
#endif

#ifdef ENABLE_DVD
#	include <dvdnav/dvdnav.h>
#endif

#define LOG_MODULE "ffmpegutil"

#include <libavbox/avbox.h>


/**
 * Initialize ffmpeg's filter graph
 */
INTERNAL int
avbox_ffmpegutil_initvideofilters(
	AVFormatContext *fmt_ctx,
	AVCodecContext *dec_ctx,
	AVFilterContext **buffersink_ctx,
	AVFilterContext **buffersrc_ctx,
	AVFilterGraph **filter_graph,
	enum AVPixelFormat pix_fmt,
	const char *filters_descr,
	int stream_index)
{
	char args[512];
	int ret = 0;
	AVFilter *buffersrc  = avfilter_get_by_name("buffer");
	AVFilter *buffersink = avfilter_get_by_name("buffersink");

	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs  = avfilter_inout_alloc();
	AVRational time_base = fmt_ctx->streams[stream_index]->time_base;
	enum AVPixelFormat pix_fmts[] = { pix_fmt, AV_PIX_FMT_NONE };

	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
		time_base.num, time_base.den,
		dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);
	DEBUG_VPRINT(LOG_MODULE, "Video filter args: %s", args);


	*filter_graph = avfilter_graph_alloc();
	if (!outputs || !inputs || !*filter_graph) {
		ret = AVERROR(ENOMEM);
		goto end;
	}

	ret = avfilter_graph_create_filter(buffersrc_ctx, buffersrc, "in",
                                       args, NULL, *filter_graph);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot create buffer source!");
		goto end;
	}

	/* buffer video sink: to terminate the filter chain. */
	ret = avfilter_graph_create_filter(buffersink_ctx, buffersink, "out",
                                       NULL, NULL, *filter_graph);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot create buffer sink!");
		goto end;
	}

	ret = av_opt_set_int_list(*buffersink_ctx, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot set output pixel format!");
		goto end;
	}

	/*
	 * Set the endpoints for the filter graph. The filter_graph will
	 * be linked to the graph described by filters_descr.
	 */

	/*
	 * The buffer source output must be connected to the input pad of
	 * the first filter described by filters_descr; since the first
	 * filter input label is not specified, it is set to "in" by
	 * default.
	 */
	outputs->name       = av_strdup("in");
	outputs->filter_ctx = *buffersrc_ctx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;

	/*
	 * The buffer sink input must be connected to the output pad of
	 * the last filter described by filters_descr; since the last
	 * filter output label is not specified, it is set to "out" by
	 * default.
	 */
	inputs->name       = av_strdup("out");
	inputs->filter_ctx = *buffersink_ctx;
	inputs->pad_idx    = 0;
	inputs->next       = NULL;

	if ((ret = avfilter_graph_parse_ptr(*filter_graph, filters_descr,
                                    &inputs, &outputs, NULL)) < 0) {
		goto end;
	}

	if ((ret = avfilter_graph_config(*filter_graph, NULL)) < 0) {
	        goto end;
	}


end:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	return ret;
}


INTERNAL int
avbox_ffmpegutil_initaudiofilters(
	AVFilterContext **buffersink_ctx,
	AVFilterContext **buffersrc_ctx,
	AVFilterGraph **filter_graph,
	const char *filters_descr,
	int sample_rate,
	AVRational time_base,
	uint64_t channel_layout,
	const char *sample_fmt_name)
{
	char args[512];
	int ret = 0;
	AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
	AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs  = avfilter_inout_alloc();
	static const enum AVSampleFormat out_sample_fmts[] = { AV_SAMPLE_FMT_S16, -1 };
	static const int64_t out_channel_layouts[] = { AV_CH_LAYOUT_STEREO, -1 };
	static const int out_sample_rates[] = { 48000, -1 };
	const AVFilterLink *outlink;

	DEBUG_PRINT("player", "Initializing audio filters");

	*filter_graph = avfilter_graph_alloc();
	if (!outputs || !inputs || !*filter_graph) {
		ret = AVERROR(ENOMEM);
		goto end;
	}

	snprintf(args, sizeof(args),
		"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
		time_base.num, time_base.den, sample_rate,
		sample_fmt_name, channel_layout);
	DEBUG_VPRINT(LOG_MODULE, "Audio filtergraph args: %s", args);
	ret = avfilter_graph_create_filter(buffersrc_ctx, abuffersrc, "in",
		args, NULL, *filter_graph);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot create audio buffer source!");
		goto end;
	}

	/* buffer audio sink: to terminate the filter chain. */
	ret = avfilter_graph_create_filter(buffersink_ctx, abuffersink, "out",
		NULL, NULL, *filter_graph);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot create audio buffer sink!");
		goto end;
	}

	ret = av_opt_set_int_list(*buffersink_ctx, "sample_fmts", out_sample_fmts, -1,
		AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot set output sample format!");
		goto end;
	}

	ret = av_opt_set_int_list(*buffersink_ctx, "channel_layouts", out_channel_layouts, -1,
		AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot set output channel layout!");
		goto end;
	}

	ret = av_opt_set_int_list(*buffersink_ctx, "sample_rates", out_sample_rates, -1,
		AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		LOG_PRINT_ERROR("Cannot set output sample rate!");
		goto end;
	}

	/*
	 * Set the endpoints for the filter graph. The filter_graph will
	 * be linked to the graph described by filters_descr.
	 */

	/*
	 * The buffer source output must be connected to the input pad of
	 * the first filter described by filters_descr; since the first
	 * filter input label is not specified, it is set to "in" by
	 * default.
	 */
	outputs->name       = av_strdup("in");
	outputs->filter_ctx = *buffersrc_ctx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;

	/*
	 * The buffer sink input must be connected to the output pad of
	 * the last filter described by filters_descr; since the last
	 * filter output label is not specified, it is set to "out" by
	 * default.
	 */
	inputs->name       = av_strdup("out");
	inputs->filter_ctx = *buffersink_ctx;
	inputs->pad_idx    = 0;
	inputs->next       = NULL;

	if ((ret = avfilter_graph_parse_ptr(*filter_graph,
		filters_descr, &inputs, &outputs, NULL)) < 0) {
		goto end;
	}

	if ((ret = avfilter_graph_config(*filter_graph, NULL)) < 0) {
		goto end;
	}

	/* Print summary of the sink buffer
	 * Note: args buffer is reused to store channel layout string */
	outlink = (*buffersink_ctx)->inputs[0];
	av_get_channel_layout_string(args, sizeof(args), -1, outlink->channel_layout);
	DEBUG_VPRINT("player", "Output: srate:%dHz fmt:%s chlayout:%s",
		(int) outlink->sample_rate,
		(char*) av_x_if_null(av_get_sample_fmt_name(outlink->format), "?"),
		args);

end:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	return ret;
}


INTERNAL AVCodecContext *
avbox_ffmpegutil_opencodeccontext(int *stream_idx,
	AVFormatContext *fmt_ctx, enum AVMediaType type)
{
	int ret;
	AVStream *st;
	AVCodecContext *dec_ctx = NULL;
	AVCodec *dec = NULL;
	AVDictionary *opts = NULL;

	if (*stream_idx == -1) {
		*stream_idx = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
		if (*stream_idx == -1) {
			LOG_VPRINT_ERROR("Could not find %s stream in input file!",
				av_get_media_type_string(type));
			return NULL;
		}
	}

	st = fmt_ctx->streams[*stream_idx];

#if 0 && defined(ENABLE_VC4)
	/* try to use the hardware decoder on the raspberry pi */
	if (st->codecpar->codec_id == AV_CODEC_ID_H264) {
		if ((dec = avcodec_find_decoder_by_name("h264_mmal")) == NULL) {
			LOG_PRINT_ERROR("Could not find decoder: h264_mmal");
		} else {
			DEBUG_PRINT(LOG_MODULE, "Hardware decoding with mmal!");
		}
	}
#endif

	/* find decoder for the stream */
	if (dec == NULL) {
		dec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!dec) {
			LOG_VPRINT_ERROR("Failed to find '%s' codec!",
				av_get_media_type_string(type));
			return NULL;
		}
	}

	/* allocate decoder context */
	if ((dec_ctx = avcodec_alloc_context3(dec)) == NULL) {
		LOG_PRINT_ERROR("Could not allocate decoder context!");
		return NULL;
	}
	if ((ret = avcodec_parameters_to_context(dec_ctx, st->codecpar)) < 0) {
		LOG_VPRINT_ERROR("Could not convert decoder params to context: %d!",
			ret);
		return NULL;
	}

	/* Init the video decoder */
	av_dict_set(&opts, "flags2", "+export_mvs", 0);
	if ((ret = avcodec_open2(dec_ctx, dec, &opts)) < 0) {
		LOG_VPRINT_ERROR("Failed to open '%s' codec!",
			av_get_media_type_string(type));
		return NULL;
	}

	return dec_ctx;
}
