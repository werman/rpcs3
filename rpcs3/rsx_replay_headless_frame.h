#pragma once

#include "Emu/RSX/GSFrameBase.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

struct rsx_replay_frame_sink
{
	std::string output_prefix;
	std::atomic<u32> width{1280};
	std::atomic<u32> height{720};
	std::atomic<u32> frame_index{0};
	std::atomic<bool> failed{false};
	mutable std::mutex error_mutex;
	std::string error;

	void set_size(u32 new_width, u32 new_height);
	void set_error(std::string message);
	std::string get_error() const;
};

class rsx_replay_headless_frame final : public GSFrameBase
{
	std::shared_ptr<rsx_replay_frame_sink> m_sink;

public:
	explicit rsx_replay_headless_frame(std::shared_ptr<rsx_replay_frame_sink> sink);

	void close() override;
	void reset() override;
	bool shown() override;
	void hide() override;
	void show() override;
	void toggle_fullscreen() override;

	void delete_context(draw_context_t ctx) override;
	draw_context_t make_context() override;
	void set_current(draw_context_t ctx) override;
	void flip(draw_context_t ctx, bool skip_frame = false) override;
	int client_width() override;
	int client_height() override;
	f64 client_display_rate() override;
	bool has_alpha() override;

	display_handle_t handle() const override;

	bool can_consume_frame() const override;
	void present_frame(std::vector<u8>&& data, u32 pitch, u32 width, u32 height, bool is_bgra) const override;
	void take_screenshot(std::vector<u8>&& sshot_data, u32 sshot_width, u32 sshot_height, bool is_bgra) override;
};
