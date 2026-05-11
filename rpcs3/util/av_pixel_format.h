#pragma once

#ifndef WITHOUT_FFMPEG
extern "C"
{
#include <libavutil/pixfmt.h>
}
#else
enum AVPixelFormat
{
	AV_PIX_FMT_YUV420P = 0,
	AV_PIX_FMT_ARGB = 25,
	AV_PIX_FMT_RGBA = 26,
	AV_PIX_FMT_BGRA = 28,
	AV_PIX_FMT_YUVA420P = 33,
	AV_PIX_FMT_RGB565BE = 36,
};
#endif
