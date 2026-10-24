#include "stdafx.h"
#include "rsx_replay_headless_frame.h"

#include "Emu/System.h"

#include <png.h>

#include <cstdio>

void rsx_replay_frame_sink::set_size(u32 new_width, u32 new_height)
{
	if (new_width && new_height)
	{
		width = new_width;
		height = new_height;
	}
}

void rsx_replay_frame_sink::set_error(std::string message)
{
	{
		std::lock_guard lock(error_mutex);
		error = std::move(message);
	}

	failed = true;
}

std::string rsx_replay_frame_sink::get_error() const
{
	std::lock_guard lock(error_mutex);
	return error;
}

bool rsx_replay_frame_sink::try_reserve_frame(u32& index)
{
	while (true)
	{
		const u64 expected = expected_frames.load();
		u32 current = frame_index.load();

		if (expected && current >= expected)
		{
			return false;
		}

		if (frame_index.compare_exchange_weak(current, current + 1))
		{
			index = current;
			return true;
		}
	}
}

bool rsx_replay_frame_sink::is_complete() const
{
	const u64 expected = expected_frames.load();
	return expected && frames_written.load() >= expected;
}

rsx_replay_headless_frame::rsx_replay_headless_frame(std::shared_ptr<rsx_replay_frame_sink> sink)
	: m_sink(std::move(sink))
{
}

void rsx_replay_headless_frame::close()
{
}

void rsx_replay_headless_frame::reset()
{
}

bool rsx_replay_headless_frame::shown()
{
	return true;
}

void rsx_replay_headless_frame::hide()
{
}

void rsx_replay_headless_frame::show()
{
}

void rsx_replay_headless_frame::toggle_fullscreen()
{
}

void rsx_replay_headless_frame::update_title(double)
{
}

void rsx_replay_headless_frame::delete_context(draw_context_t)
{
}

draw_context_t rsx_replay_headless_frame::make_context()
{
	return nullptr;
}

void rsx_replay_headless_frame::set_current(draw_context_t)
{
}

void rsx_replay_headless_frame::flip(draw_context_t, bool)
{
}

int rsx_replay_headless_frame::client_width()
{
	return static_cast<int>(m_sink->width.load());
}

int rsx_replay_headless_frame::client_height()
{
	return static_cast<int>(m_sink->height.load());
}

f64 rsx_replay_headless_frame::client_display_rate()
{
	return 60.;
}

bool rsx_replay_headless_frame::has_alpha()
{
	return false;
}

display_handle_t rsx_replay_headless_frame::handle() const
{
#ifdef ANDROID
	struct ANativeWindow* window = nullptr;
	return display_handle_t{window};
#else
	return display_handle_t{};
#endif
}

bool rsx_replay_headless_frame::can_consume_frame() const
{
	return false;
}

void rsx_replay_headless_frame::present_frame(std::vector<u8>&&, u32, u32, u32, bool) const
{
}

void rsx_replay_headless_frame::take_screenshot(std::vector<u8>&& sshot_data, u32 sshot_width, u32 sshot_height, bool is_bgra)
{
	u32 frame_index = 0;
	if (!m_sink->try_reserve_frame(frame_index))
	{
		return;
	}

	if (frame_index < m_sink->skip_frames)
	{
		m_sink->frames_written++;
		return;
	}

	const std::string path = fmt::format("%s%04u.png", m_sink->output_prefix, frame_index);

	FILE* file = std::fopen(path.c_str(), "wb");
	if (!file)
	{
		m_sink->set_error(fmt::format("Failed to open output image '%s'", path));
		Emu.CallFromMainThread([]()
		{
			Emu.GracefulShutdown(false);
			Emu.Quit(true);
		});
		return;
	}

	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!png)
	{
		std::fclose(file);
		m_sink->set_error(fmt::format("Failed to initialize PNG writer for '%s'", path));
		return;
	}

	png_infop info = png_create_info_struct(png);
	if (!info)
	{
		png_destroy_write_struct(&png, nullptr);
		std::fclose(file);
		m_sink->set_error(fmt::format("Failed to initialize PNG info for '%s'", path));
		return;
	}

	if (setjmp(png_jmpbuf(png)))
	{
		png_destroy_write_struct(&png, &info);
		std::fclose(file);
		m_sink->set_error(fmt::format("Failed to write PNG image '%s'", path));
		return;
	}

	std::vector<u8> rgba(static_cast<usz>(sshot_width) * sshot_height * 4);
	for (u32 i = 0; i < sshot_width * sshot_height; i++)
	{
		const u8* src = &sshot_data[i * 4];
		u8* dst = &rgba[i * 4];

		if (is_bgra)
		{
			dst[0] = src[2];
			dst[1] = src[1];
			dst[2] = src[0];
		}
		else
		{
			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
		}

		dst[3] = 255;
	}

	std::vector<png_bytep> rows(sshot_height);
	for (u32 y = 0; y < sshot_height; y++)
	{
		rows[y] = rgba.data() + static_cast<usz>(y) * sshot_width * 4;
	}

	png_init_io(png, file);
	png_set_IHDR(png, info, sshot_width, sshot_height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	png_write_image(png, rows.data());
	png_write_end(png, nullptr);
	png_destroy_write_struct(&png, &info);
	std::fclose(file);

	m_sink->frames_written++;
}
