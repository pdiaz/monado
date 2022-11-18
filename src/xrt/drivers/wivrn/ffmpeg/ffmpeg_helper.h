/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "vk/vk_helpers.h"
#include <memory>
#include <stdexcept>
#include <system_error>
#include <vector>

struct AVBufferRef;
struct AVFilterGraph;
struct AVFrame;
struct AVCodecContext;

extern "C"
{
#include <libavutil/avutil.h>
}

const std::error_category &
av_error_category();

AVPixelFormat
vk_format_to_av_format(VkFormat vk_fmt);

uint32_t
vk_format_to_fourcc(VkFormat vk_fmt);

struct AvDeleter
{
	void
	operator()(AVBufferRef *);
	void
	operator()(AVFrame *);
	void
	operator()(AVCodecContext *);
	void
	operator()(AVFilterGraph *);
};

using av_buffer_ptr = std::unique_ptr<AVBufferRef, AvDeleter>;
using av_frame_ptr = std::unique_ptr<AVFrame, AvDeleter>;
using av_codec_context_ptr = std::unique_ptr<AVCodecContext, AvDeleter>;
using av_filter_graph_ptr = std::unique_ptr<AVFilterGraph, AvDeleter>;

av_buffer_ptr
make_av_buffer(AVBufferRef *);

av_frame_ptr
make_av_frame(AVFrame *);

av_frame_ptr
make_av_frame();
