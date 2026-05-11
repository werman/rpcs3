#include "stdafx.h"
#include "rsx_replay.h"

#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/lv2/sys_rsx.h"
#include "Emu/Cell/lv2/sys_memory.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/VK/VKPipelineCompiler.h"

#include "util/asm.hpp"

#include <algorithm>
#include <cstdio>
#include <thread>

extern atomic_t<bool> g_user_asked_for_screenshot;

namespace rsx
{
	std::unique_ptr<frame_capture_data> convert_frame_capture_v5_to_v6(frame_capture_data_v5&& frame, std::string& error)
	{
		auto converted = std::make_unique<frame_capture_data>();
		converted->magic = frame.magic;
		converted->version = c_fc_version;
		converted->LE_format = frame.LE_format;
		converted->replay_commands = std::move(frame.replay_commands);
		converted->reg_state = std::move(frame.reg_state);

		u64 max_id = converted->memory_indexer;

		for (auto& [id, state] : frame.tile_map)
		{
			const auto it = converted->tile_map.find(state);
			if (it == converted->tile_map.end())
			{
				converted->tile_map.emplace(state, id);
			}
			else if (it->second != id)
			{
				error = "tile state id collision while converting v5 capture";
				return nullptr;
			}

			max_id = std::max(max_id, id);
		}

		for (auto& [id, state] : frame.display_buffers_map)
		{
			const auto it = converted->display_buffers_map.find(state);
			if (it == converted->display_buffers_map.end())
			{
				converted->display_buffers_map.emplace(state, id);
			}
			else if (it->second != id)
			{
				error = "display buffer id collision while converting v5 capture";
				return nullptr;
			}

			max_id = std::max(max_id, id);
		}

		for (auto& [id, block] : frame.memory_map)
		{
			const auto it = converted->memory_map.find(block);
			if (it == converted->memory_map.end())
			{
				converted->memory_map.emplace(block, id);
			}
			else if (it->second != id)
			{
				error = "memory block id collision while converting v5 capture";
				return nullptr;
			}

			max_id = std::max(max_id, id);
		}

		for (auto& [id, data] : frame.memory_data_map)
		{
			const auto it = converted->memory_data_map.find(data);
			if (it == converted->memory_data_map.end())
			{
				converted->memory_data_map.emplace(std::move(data), id);
			}
			else if (it->second != id)
			{
				error = "memory data id collision while converting v5 capture";
				return nullptr;
			}

			max_id = std::max(max_id, id);
		}

		converted->memory_indexer = std::max(converted->memory_indexer, max_id);
		return converted;
	}

	be_t<u32> rsx_replay_thread::allocate_context()
	{
		u32 buffer_size = 4;

		// run through replay commands to figure out how big command buffer needs to be
		for (const auto& rc : frame->replay_commands)
		{
			const u32 count = (rc.rsx_command.first >> 18) & 0x7ff;
			// allocate for register plus w/e number of arguments it has
			buffer_size += (count * 4) + 4;
		}

		// User memory + fifo size
		buffer_size = utils::align<u32>(buffer_size, 0x100000) + 0x10000000;
		// We are not allowed to drain all memory so add a little
		g_fxo->init<lv2_memory_container>(buffer_size + 0x1000000);

		const u32 contextAddr = vm::alloc(sizeof(rsx_context), vm::main);
		if (contextAddr == 0)
			fmt::throw_exception("Capture Replay: context alloc failed");
		const auto contextInfo = vm::ptr<rsx_context>::make(contextAddr);

		// 'fake' initialize usermemory
		sys_memory_allocate(*this, buffer_size, SYS_MEMORY_PAGE_SIZE_1M, contextInfo.ptr(&rsx_context::user_addr));
		ensure((user_mem_addr = contextInfo->user_addr) != 0);

		if (sys_rsx_device_map(*this, contextInfo.ptr(&rsx_context::dev_addr), vm::null, 0x8) != CELL_OK)
			fmt::throw_exception("Capture Replay: sys_rsx_device_map failed!");

		if (sys_rsx_memory_allocate(*this, contextInfo.ptr(&rsx_context::mem_handle), contextInfo.ptr(&rsx_context::mem_addr), 0x0F900000, 0, 0, 0, 0) != CELL_OK)
			fmt::throw_exception("Capture Replay: sys_rsx_memory_allocate failed!");

		if (sys_rsx_context_allocate(*this, contextInfo.ptr(&rsx_context::context_id), contextInfo.ptr(&rsx_context::dma_addr), contextInfo.ptr(&rsx_context::driver_info), contextInfo.ptr(&rsx_context::reports_addr), contextInfo->mem_handle, 0) != CELL_OK)
			fmt::throw_exception("Capture Replay: sys_rsx_context_allocate failed!");

		get_current_renderer()->main_mem_size = buffer_size;

		if (sys_rsx_context_iomap(*this, contextInfo->context_id, 0, user_mem_addr, buffer_size, 0xf000000000000800ull) != CELL_OK)
			fmt::throw_exception("Capture Replay: rsx io mapping failed!");

		return contextInfo->context_id;
	}

	std::vector<u32> rsx_replay_thread::alloc_write_fifo(be_t<u32> /*context_id*/) const
	{
		// copy commands into fifo buffer
		// todo: could change rsx_command to just be values to avoid this loop,
		auto fifo_addr = vm::ptr<u32>::make(user_mem_addr + 0x10000000);
		u32 count = 0;
		std::vector<u32> fifo_stops;
		u32 currentOffset = 0x10000000;
		for (const auto& rc : frame->replay_commands)
		{
			bool hasState = (!rc.memory_state.empty()) || (rc.display_buffer_state != 0) || (rc.tile_state != 0);
			if (hasState)
			{
				if (count != 0)
				{
					// todo: support memory state in the middle of incremented command
					// This shouldn't ever happen as long as captures stay in 'strict' aka non-multidraw mode
					fmt::throw_exception("capture replay: state change not supported between increment commands");
				}

				fifo_stops.emplace_back(currentOffset);
			}

			// spit out command
			if (count == 0)
			{
				count = (rc.rsx_command.first >> 18) & 0x7ff;
				*fifo_addr = rc.rsx_command.first;
				fifo_addr++;
				currentOffset += 4;
			}

			if (count != 0)
			{
				*fifo_addr = rc.rsx_command.second;
				fifo_addr++;
				count--;
				currentOffset += 4;
			}
		}

		fifo_stops.emplace_back(currentOffset);
		return fifo_stops;
	}

	void rsx_replay_thread::apply_frame_state(be_t<u32> context_id, const frame_capture_data::replay_command& replay_cmd)
	{
		// apply memory needed for command
		for (const auto& state : replay_cmd.memory_state)
		{
			auto it = std::find_if(frame->memory_map.begin(), frame->memory_map.end(), FN(x.second == state));
			if (it == frame->memory_map.end())
				fmt::throw_exception("requested memory state for command not found in memory_map");

			const auto& memblock = it->first;
			auto it_data = std::find_if(frame->memory_data_map.begin(), frame->memory_data_map.end(), FN(x.second == memblock.data_state));
			if (it_data == frame->memory_data_map.end())
				fmt::throw_exception("requested memory data state for command not found in memory_data_map");

			const auto& data_block = it_data->first;
			std::memcpy(vm::base(get_address(memblock.offset, memblock.location)), data_block.data.data(), data_block.data.size());
		}

		if (replay_cmd.display_buffer_state != 0 && replay_cmd.display_buffer_state != cs.display_buffer_hash)
		{
			auto it = std::find_if(frame->display_buffers_map.begin(), frame->display_buffers_map.end(), FN(x.second == replay_cmd.display_buffer_state));
			if (it == frame->display_buffers_map.end())
				fmt::throw_exception("requested display buffer for command not found");

			const auto& dbstate = it->first;
			u32 display_width = 0;
			u32 display_height = 0;

			for (u32 i = 0; i < dbstate.count; ++i)
			{
				const auto& buf = dbstate.buffers[i];
				if (cs.display_buffer_hash != 0 && memcmp(&cs.buffer_state.buffers[i], &buf, sizeof(rsx::frame_capture_data::buffer_state)) == 0)
					continue;

				cs.buffer_state.buffers[i] = buf;
				sys_rsx_context_attribute(context_id, 0x104, i,
					u64{dbstate.buffers[i].width} << 32 | dbstate.buffers[i].height, u64{dbstate.buffers[i].pitch} << 32 | dbstate.buffers[i].offset, 0);
			}

			for (u32 i = 0; i < dbstate.count; ++i)
			{
				if (dbstate.buffers[i].width && dbstate.buffers[i].height)
				{
					display_width = dbstate.buffers[i].width;
					display_height = dbstate.buffers[i].height;
					break;
				}
			}

			if (display_width && display_height && Emu.GetCallbacks().set_rsx_capture_display_size)
			{
				Emu.GetCallbacks().set_rsx_capture_display_size(display_width, display_height);
			}

			cs.display_buffer_hash = replay_cmd.display_buffer_state;
		}

		if (replay_cmd.tile_state != 0 && replay_cmd.tile_state != cs.tile_hash)
		{
			auto it = std::find_if(frame->tile_map.begin(), frame->tile_map.end(), FN(x.second == replay_cmd.tile_state));
			if (it == frame->tile_map.end())
				fmt::throw_exception("requested tile state command not found");

			const auto& tstate = it->first;
			for (u32 i = 0; i < limits::tiles_count; ++i)
			{
				const auto& ti = tstate.tiles[i];
				if (cs.tile_hash != 0 && memcmp(&cs.tile_state.tiles[i], &ti, sizeof(rsx::frame_capture_data::tile_info)) == 0)
					continue;

				cs.tile_state.tiles[i] = ti;
				sys_rsx_context_attribute(context_id, 0x300, i, u64{ti.tile} << 32 | ti.limit, u64{ti.pitch} << 32 | ti.format, 0);
			}

			for (u32 i = 0; i < limits::zculls_count; ++i)
			{
				const auto& zci = tstate.zculls[i];
				if (cs.tile_hash != 0 && memcmp(&cs.tile_state.zculls[i], &zci, sizeof(rsx::frame_capture_data::zcull_info)) == 0)
					continue;

				cs.tile_state.zculls[i] = zci;
				sys_rsx_context_attribute(context_id, 0x301, i, u64{zci.region} << 32 | zci.size, u64{zci.start} << 32 | zci.offset, u64{zci.status0} << 32 | zci.status1);
			}

			cs.tile_hash = replay_cmd.tile_state;
		}
	}

	void rsx_replay_thread::cpu_task()
	{
		be_t<u32> context_id = allocate_context();

		auto fifo_stops = alloc_write_fifo(context_id);
		const bool use_vulkan = (g_cfg.video.renderer == video_renderer::vulkan);
		const bool original_disable_video_output = g_cfg.video.disable_video_output.get();
		bool suppress_output = false;
		bool first_frame = true;
#ifdef ANDROID
		u64 replay_loop = 0;
#endif

		if (use_vulkan && !original_disable_video_output)
		{
			g_cfg.video.disable_video_output.set(true);
			suppress_output = true;
		}

		while (thread_ctrl::state() != thread_state::aborting)
		{
#ifdef ANDROID
			const u64 current_loop = replay_loop++;
			std::fprintf(stderr, "status: replay_loop=%llu begin\n", static_cast<unsigned long long>(current_loop));
#endif

			// Load registers while the RSX is still idle
			method_registers = frame->reg_state;
			atomic_fence_seq_cst();

			// start up fifo buffer by dumping the put ptr to first stop
			sys_rsx_context_attribute(context_id, 0x001, 0x10000000, fifo_stops[0], 0, 0);

			auto render = get_current_renderer();
#ifndef ANDROID
			auto last_flip = render->int_flip_index;
#endif

			usz stopIdx = 0;
			for (const auto& replay_cmd : frame->replay_commands)
			{
				while (Emu.IsPaused())
					thread_ctrl::wait_for(10'000);

				if (thread_ctrl::state() == thread_state::aborting)
					break;

				// Loop and hunt down our next state change that needs to be done
				if (!(!replay_cmd.memory_state.empty() || (replay_cmd.display_buffer_state != 0) || (replay_cmd.tile_state != 0)))
					continue;

				// wait until rsx idle and at our first 'stop' to apply state
#ifdef ANDROID
				u64 state_wait_start = get_system_time();
				u64 state_wait_next_log = 5'000'000;
#endif
				while (thread_ctrl::state() != thread_state::aborting && !render->is_fifo_idle() && (render->ctrl->get != fifo_stops[stopIdx]))
				{
					if (Emu.IsPaused())
						thread_ctrl::wait_for(10'000);
					else
						std::this_thread::yield();

#ifdef ANDROID
					const u64 waited_us = get_system_time() - state_wait_start;
					if (waited_us >= state_wait_next_log)
					{
						std::fprintf(stderr, "status: replay_loop=%llu waiting_for_state stop=%u get=0x%x expected=0x%x wait_us=%llu\n",
							static_cast<unsigned long long>(current_loop), static_cast<unsigned>(stopIdx), +render->ctrl->get,
							fifo_stops[stopIdx], static_cast<unsigned long long>(waited_us));
						state_wait_next_log += 5'000'000;
					}
#endif
				}

				stopIdx++;

				apply_frame_state(context_id, replay_cmd);

				// move put ptr to next stop
				if (stopIdx >= fifo_stops.size())
					fmt::throw_exception("Capture Replay: StopIdx greater than size of fifo_stops");

				render->ctrl->put = fifo_stops[stopIdx];
			}

			// dump put to end of stops, which should have actual end
			u32 end = fifo_stops.back();
			render->ctrl->put = end;

#ifdef ANDROID
			u64 idle_wait_start = get_system_time();
			u64 idle_wait_next_log = 5'000'000;
#endif
			while (!render->is_fifo_idle() && thread_ctrl::state() != thread_state::aborting)
			{
				if (Emu.IsPaused())
					thread_ctrl::wait_for(10'000);
				else
					std::this_thread::yield();

#ifdef ANDROID
				const u64 waited_us = get_system_time() - idle_wait_start;
				if (waited_us >= idle_wait_next_log)
				{
					std::fprintf(stderr, "status: replay_loop=%llu waiting_for_idle get=0x%x put=0x%x wait_us=%llu\n",
						static_cast<unsigned long long>(current_loop), +render->ctrl->get, +render->ctrl->put,
						static_cast<unsigned long long>(waited_us));
					idle_wait_next_log += 5'000'000;
				}
#endif
			}

#ifdef ANDROID
			if (first_frame)
			{
				if (use_vulkan)
				{
					vk::wait_for_pipe_compiler_idle();
				}

				if (suppress_output)
				{
					g_cfg.video.disable_video_output.set(original_disable_video_output);
					suppress_output = false;
				}

				first_frame = false;
			}

			g_user_asked_for_screenshot = true;
			std::fprintf(stderr, "status: replay_loop=%llu screenshot_requested buffer=%u\n",
				static_cast<unsigned long long>(current_loop), render->current_display_buffer);
			render->request_emu_flip(render->current_display_buffer);

			u64 screenshot_wait_us = 0;
			while (g_user_asked_for_screenshot && thread_ctrl::state() != thread_state::aborting)
			{
				const u64 wait_us = Emu.IsPaused() ? 10'000 : 1'000;
				thread_ctrl::wait_for(wait_us);
				screenshot_wait_us += wait_us;

				if (screenshot_wait_us && screenshot_wait_us % 5'000'000 == 0)
				{
					std::fprintf(stderr, "status: replay_loop=%llu waiting_for_screenshot wait_us=%llu\n",
						static_cast<unsigned long long>(current_loop), static_cast<unsigned long long>(screenshot_wait_us));
				}
			}

			std::fprintf(stderr, "status: replay_loop=%llu screenshot_consumed wait_us=%llu\n",
				static_cast<unsigned long long>(current_loop), static_cast<unsigned long long>(screenshot_wait_us));
#else
			// Check if the captured application used syscall instead of a gcm command to flip
			if (render->int_flip_index == last_flip)
			{
				// Capture did not include a display flip, flip manually
				render->request_emu_flip(1u);
			}

			if (first_frame)
			{
				if (use_vulkan)
				{
					vk::wait_for_pipe_compiler_idle();
				}

				if (suppress_output)
				{
					g_cfg.video.disable_video_output.set(original_disable_video_output);
					suppress_output = false;
				}

				first_frame = false;
			}
#endif

			if (!Emu.OnRsxCaptureLoopFinished())
			{
				break;
			}

			// random pause to not destroy gpu
			thread_ctrl::wait_for(10'000);
		}

		if (suppress_output)
		{
			g_cfg.video.disable_video_output.set(original_disable_video_output);
		}

		get_current_cpu_thread()->state += (cpu_flag::exit + cpu_flag::wait);
	}
}
