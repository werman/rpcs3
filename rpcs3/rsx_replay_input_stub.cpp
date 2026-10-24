#include "stdafx.h"
#include "Utilities/Thread.h"
#include "Input/pad_thread.h"
#include "Input/product_info.h"
#include "Input/ps_move_tracker.h"

extern "C" int JNI_GetCreatedJavaVMs(void**, int, int* n_vms)
{
	if (n_vms)
	{
		*n_vms = 0;
	}

	return -1;
}

namespace pad
{
	atomic_t<pad_thread*> g_pad_thread = nullptr;
	shared_mutex g_pad_mutex;
	std::string g_title_id;
	atomic_t<bool> g_enabled{true};
	atomic_t<bool> g_reset{false};
	atomic_t<bool> g_started{false};
	atomic_t<bool> g_home_menu_requested{false};
}

pad_thread::pad_thread(void*, void*, std::string_view title_id)
{
	pad::g_title_id = title_id;
	pad::g_pad_thread = this;
}

pad_thread::~pad_thread()
{
	pad::g_pad_thread = nullptr;
}

void pad_thread::operator()() {}
void pad_thread::SetRumble(u32, u8, u8) {}
void pad_thread::SetIntercepted(bool) {}
void pad_thread::UnregisterLddPad(u32) {}
void pad_thread::open_home_menu() {}
void pad_thread::Init() {}
void pad_thread::InitLddPad(u32, const u32*) {}
void pad_thread::apply_copilots() {}
void pad_thread::update_pad_states() {}

s32 pad_thread::AddLddPad()
{
	return -1;
}

std::shared_ptr<PadHandlerBase> pad_thread::GetHandler(pad_handler)
{
	return nullptr;
}

void pad_thread::InitPadConfig(cfg_pad&, pad_handler, std::shared_ptr<PadHandlerBase>&)
{
}

namespace input
{
	product_info get_product_info(product_type type)
	{
		return {type, 0, 0, 0, 0, 0};
	}

	product_type get_product_by_vid_pid(u16, u16)
	{
		return product_type::playstation_3_controller;
	}

	std::vector<product_info> get_products_by_class(int)
	{
		return {};
	}
}

template <bool DiagnosticsEnabled>
ps_move_tracker<DiagnosticsEnabled>::ps_move_tracker() = default;

template <bool DiagnosticsEnabled>
ps_move_tracker<DiagnosticsEnabled>::~ps_move_tracker() = default;

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::set_image_data(const void*, u64, u32, u32, s32)
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::init_worker(u32)
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::join_worker(u32)
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::process_image()
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::convert_image(s32)
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::process_hues()
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::process_contours(ps_move_info&, u32)
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::set_active(u32 index, bool active)
{
	if (index < m_config.size())
	{
		m_config[index].active = active;
	}
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::set_hue(u32 index, u16 hue)
{
	if (index < m_config.size())
	{
		m_config[index].hue = hue;
	}
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::set_hue_threshold(u32 index, u16 threshold)
{
	if (index < m_config.size())
	{
		m_config[index].hue_threshold = threshold;
	}
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::set_saturation_threshold(u32 index, u16 threshold)
{
	if (index < m_config.size())
	{
		m_config[index].saturation_threshold_u8 = threshold;
	}
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::ps_move_config::calculate_values()
{
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::set_valid(ps_move_info& info, u32, bool valid)
{
	info.valid = valid;
}

template <bool DiagnosticsEnabled>
void ps_move_tracker<DiagnosticsEnabled>::draw_sphere_size_range(f32)
{
}

template <bool DiagnosticsEnabled>
std::tuple<u8, u8, u8> ps_move_tracker<DiagnosticsEnabled>::hsv_to_rgb(u16, f32, f32)
{
	return {0, 0, 0};
}

template <bool DiagnosticsEnabled>
std::tuple<s16, f32, f32> ps_move_tracker<DiagnosticsEnabled>::rgb_to_hsv(f32, f32, f32)
{
	return {0, 0.0f, 0.0f};
}

template class ps_move_tracker<false>;
template class ps_move_tracker<true>;
