#include "stdafx.h"

#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/RSX/VK/VKGSRender.h"
#include "Emu/RSX/VK/VulkanAPI.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Input/pad_thread.h"
#include "Utilities/File.h"
#include "Utilities/Thread.h"
#include "rsx_replay_headless_frame.h"

#include <charconv>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <thread>

LOG_CHANNEL(sys_log, "SYS");

[[noreturn]] void report_fatal_error(std::string_view text, bool, bool)
{
	std::fprintf(stderr, "fatal: %.*s\n", static_cast<int>(text.size()), text.data());
	std::abort();
}

void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> wrapped_op)
{
	while (!wrapped_op())
	{
		thread_ctrl::wait_for(repeat_duration_ms <= 0 ? 1000 : repeat_duration_ms * 1000);
	}
}

namespace
{
	struct replay_cli
	{
		std::string capture_path;
		std::string config_path;
		std::string output_prefix;
		u64 loops = 1;
	};

	void print_usage(const char* argv0)
	{
		std::fprintf(stderr, "usage: %s --rsx-capture <file.rrc[.gz|.zst]> --output-prefix <path-prefix> [--rsx-capture-repeat <n>] [--config <config.yml>]\n", argv0);
	}

	bool parse_u64(std::string_view text, u64& value)
	{
		const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 10);
		return result.ec == std::errc{} && result.ptr == text.data() + text.size();
	}

	bool parse_cli(int argc, char** argv, replay_cli& cli)
	{
		for (int i = 1; i < argc; i++)
		{
			const std::string_view arg = argv[i];
			if (arg == "--rsx-capture" && i + 1 < argc)
			{
				cli.capture_path = argv[++i];
			}
			else if (arg == "--config" && i + 1 < argc)
			{
				cli.config_path = argv[++i];
			}
			else if (arg == "--output-prefix" && i + 1 < argc)
			{
				cli.output_prefix = argv[++i];
			}
			else if (arg == "--rsx-capture-repeat" && i + 1 < argc)
			{
				if (!parse_u64(argv[++i], cli.loops))
				{
					std::fprintf(stderr, "error: invalid --rsx-capture-repeat value '%s'\n", argv[i]);
					return false;
				}
			}
			else
			{
				std::fprintf(stderr, "error: unknown or incomplete argument '%.*s'\n", static_cast<int>(arg.size()), arg.data());
				return false;
			}
		}

		if (cli.capture_path.empty())
		{
			std::fprintf(stderr, "error: --rsx-capture is required\n");
			return false;
		}

		if (!cli.config_path.empty() && !fs::is_file(cli.config_path))
		{
			std::fprintf(stderr, "error: no config file found: %s\n", cli.config_path.c_str());
			return false;
		}

		if (cli.output_prefix.empty())
		{
			std::fprintf(stderr, "error: --output-prefix is required\n");
			return false;
		}

		return true;
	}

	void configure_callbacks(const std::shared_ptr<rsx_replay_frame_sink>& sink)
	{
		EmuCallbacks callbacks{};

		callbacks.call_from_main_thread = [](std::function<void()> func, atomic_t<u32>* wake_up)
		{
			func();

			if (wake_up)
			{
				*wake_up = true;
				wake_up->notify_one();
			}
		};

		callbacks.try_to_quit = [](bool, std::function<void()> on_exit) -> bool
		{
			if (on_exit)
			{
				on_exit();
			}

			return true;
		};

		callbacks.init_gs_render = [](utils::serial* ar)
		{
			if (g_cfg.video.renderer != video_renderer::vulkan)
			{
				fmt::throw_exception("rsx-replay requires the Vulkan renderer");
			}

			g_fxo->init<rsx::thread, named_thread<VKGSRender>>(ar);
		};

		callbacks.get_gs_frame = [sink]() -> std::unique_ptr<GSFrameBase>
		{
			return std::make_unique<rsx_replay_headless_frame>(sink);
		};

		callbacks.set_rsx_capture_display_size = [sink](u32 width, u32 height)
		{
			sink->set_size(width, height);
		};

		callbacks.init_pad_handler = [](std::string_view)
		{
			pad::g_started = true;
		};

		callbacks.close_gs_frame = []() {};
		callbacks.on_run = [](bool) {};
		callbacks.on_pause = []() {};
		callbacks.on_resume = []() {};
		callbacks.on_stop = []() {};
		callbacks.on_ready = []() {};
		callbacks.on_missing_fw = []() {};
		callbacks.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>>, int) {};
		callbacks.on_save_state_progress = [](std::shared_ptr<atomic_t<bool>>, stx::shared_ptr<utils::serial>, stx::atomic_ptr<std::string>*, std::shared_ptr<void>) {};
		callbacks.enable_disc_eject = [](bool) {};
		callbacks.enable_disc_insert = [](bool) {};
		callbacks.handle_taskbar_progress = [](s32, s32) {};
		callbacks.init_kb_handler = []() {};
		callbacks.init_mouse_handler = []() {};
		callbacks.update_emu_settings = []() {};
		callbacks.save_emu_settings = []() {};
		callbacks.get_camera_handler = []() -> std::shared_ptr<camera_handler_base> { return nullptr; };
		callbacks.get_music_handler = []() -> std::shared_ptr<music_handler_base> { return std::make_shared<null_music_handler>(); };
		callbacks.get_audio = []() -> std::shared_ptr<AudioBackend> { return nullptr; };
		callbacks.get_audio_enumerator = [](u64) -> std::shared_ptr<audio_device_enumerator> { return nullptr; };
		callbacks.get_msg_dialog = []() -> std::shared_ptr<MsgDialogBase> { return nullptr; };
		callbacks.get_osk_dialog = []() -> std::shared_ptr<OskDialogBase> { return nullptr; };
		callbacks.get_sendmessage_dialog = []() -> std::shared_ptr<SendMessageDialogBase> { return nullptr; };
		callbacks.get_recvmessage_dialog = []() -> std::shared_ptr<RecvMessageDialogBase> { return nullptr; };
		callbacks.get_localized_string = [](localized_string_id, const char*) -> std::string { return {}; };
		callbacks.get_localized_u32string = [](localized_string_id, const char*) -> std::u32string { return {}; };
		callbacks.get_localized_setting = [](const cfg::_base*, u32) -> std::string { return {}; };
		callbacks.get_photo_path = [](std::string_view title) -> std::string { return std::string{title}; };
		callbacks.play_sound = [](const std::string&, std::optional<f32>) {};
		callbacks.get_image_info = [](const std::string&, std::string&, s32&, s32&, s32&) { return false; };
		callbacks.get_scaled_image = [](const std::string&, s32, s32, s32&, s32&, u8*, bool) { return false; };
		callbacks.get_font_dirs = []() { return std::vector<std::string>{}; };
		callbacks.on_install_pkgs = [](const std::vector<std::string>&) { return false; };
		callbacks.add_breakpoint = [](u32) {};
		callbacks.display_sleep_control_supported = []() { return false; };
		callbacks.enable_display_sleep = [](bool) {};
		callbacks.check_microphone_permissions = []() {};
		callbacks.enable_gamemode = [](bool) {};

		Emu.SetCallbacks(std::move(callbacks));
	}
}

int main(int argc, char** argv)
{
	std::setlocale(LC_ALL, "C");

	replay_cli cli;
	if (!parse_cli(argc, argv, cli))
	{
		print_usage(argv[0]);
		return 2;
	}

	std::string vulkan_error;
	if (!vk::init_android_vulkan_loader(&vulkan_error))
	{
		std::fprintf(stderr, "error: %s\n", vulkan_error.c_str());
		return 1;
	}

	auto sink = std::make_shared<rsx_replay_frame_sink>();
	sink->output_prefix = cli.output_prefix;

	configure_callbacks(sink);

	Emu.SetHasGui(false);
	Emu.SetUsr("00000001");
	Emu.Init();
	if (!cli.config_path.empty())
	{
		if (const fs::file cfg_file{cli.config_path})
		{
			if (!g_cfg.from_string(cfg_file.to_string()))
			{
				std::fprintf(stderr, "error: failed to apply config: %s\n", cli.config_path.c_str());
				return 1;
			}

			g_cfg.name = cli.config_path;
		}
		else
		{
			std::fprintf(stderr, "error: failed to access config: %s (%s)\n", cli.config_path.c_str(), fs::g_tls_error);
			return 1;
		}
	}

	g_cfg.video.renderer.set(video_renderer::vulkan);
	g_cfg.video.stretch_to_display_area.set(true);
	g_cfg.video.disable_video_output.set(false);

	Emu.SetRsxCaptureReplayLimit(cli.loops);

	if (!Emu.BootRsxCapture(cli.capture_path))
	{
		std::fprintf(stderr, "error: failed to boot RSX capture '%s'\n", cli.capture_path.c_str());
		return 1;
	}

	while (!Emu.IsStopped() && !sink->failed.load())
	{
		thread_ctrl::wait_for(10'000);
	}

	if (sink->failed.load())
	{
		std::fprintf(stderr, "error: %s\n", sink->get_error().c_str());
		return 1;
	}

	return 0;
}
