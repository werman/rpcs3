#include "stdafx.h"

#include "Emu/emu_callbacks.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/RSX/VK/VKGSRender.h"
#include "Emu/RSX/VK/VulkanAPI.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/Io/camera_handler_base.h"
#include "Emu/Audio/AudioBackend.h"
#include "Emu/Audio/audio_device_enumerator.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#include "Emu/Audio/Null/null_enumerator.h"
#include "Emu/Cell/Modules/cellOskDialog.h"
#include "Emu/Cell/Modules/sceNp.h"
#include "Input/pad_thread.h"
#include "Utilities/File.h"
#include "Utilities/Thread.h"
#include "rsx_replay_headless_frame.h"

extern std::string g_android_executable_dir;
extern std::string g_android_config_dir;
extern std::string g_android_cache_dir;

#include <charconv>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

#include <unistd.h>

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
		std::this_thread::sleep_for(std::chrono::microseconds{repeat_duration_ms <= 0 ? 1000 : repeat_duration_ms * 1000});
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

	struct queued_main_call
	{
		std::function<void()> func;
		atomic_t<u32>* wake_up = nullptr;
	};

	enum class replay_phase : u32
	{
		startup,
		parsed,
		vulkan_loader,
		callbacks,
		emu_init,
		config,
		booting,
		running,
		killing,
		done,
	};

	const char* phase_name(replay_phase phase)
	{
		switch (phase)
		{
		case replay_phase::startup: return "startup";
		case replay_phase::parsed: return "parsed";
		case replay_phase::vulkan_loader: return "vulkan_loader";
		case replay_phase::callbacks: return "callbacks";
		case replay_phase::emu_init: return "emu_init";
		case replay_phase::config: return "config";
		case replay_phase::booting: return "booting";
		case replay_phase::running: return "running";
		case replay_phase::killing: return "killing";
		case replay_phase::done: return "done";
		}

		return "unknown";
	}

	class main_thread_dispatcher
	{
		std::thread::id m_main_thread_id = std::this_thread::get_id();
		std::mutex m_mutex;
		std::deque<queued_main_call> m_queue;
		std::atomic<bool> m_drain_active{false};

		static void run(queued_main_call call)
		{
			if (call.func)
			{
				call.func();
			}

			if (call.wake_up)
			{
				*call.wake_up = true;
				call.wake_up->notify_one();
			}
		}

	public:
		void dispatch(std::function<void()> func, atomic_t<u32>* wake_up)
		{
			queued_main_call call{std::move(func), wake_up};

			if (std::this_thread::get_id() == m_main_thread_id)
			{
				run(std::move(call));
				return;
			}

			if (wake_up && !m_drain_active.load())
			{
				std::fprintf(stderr, "status: running blocking main-thread callback inline while dispatcher is not draining\n");
				run(std::move(call));
				return;
			}

			std::lock_guard lock(m_mutex);
			m_queue.emplace_back(std::move(call));
		}

		void drain()
		{
			m_drain_active = true;

			std::deque<queued_main_call> queue;
			{
				std::lock_guard lock(m_mutex);
				queue.swap(m_queue);
			}

			for (auto& call : queue)
			{
				run(std::move(call));
			}

			m_drain_active = false;
		}
	};

	// A fatal error kills only the faulting thread (usually RSX), so the emulator never stops on its own.
	// Fail the replay on the first fatal message instead of waiting for the frames until a timeout.
	class fatal_log_listener final : public logs::listener
	{
		std::shared_ptr<rsx_replay_frame_sink> m_sink;

	public:
		explicit fatal_log_listener(std::shared_ptr<rsx_replay_frame_sink> sink)
			: m_sink(std::move(sink))
		{
		}

		void log(u64, const logs::message& msg, std::string_view, std::string_view text) override
		{
			if (msg == logs::level::fatal && !m_sink->failed.load())
			{
				m_sink->set_error(fmt::format("fatal: %s", text));
			}
		}
	};

	class scoped_watchdog
	{
		std::atomic<bool> m_running{true};
		std::thread m_thread;

	public:
		scoped_watchdog(const std::shared_ptr<rsx_replay_frame_sink>& sink, const std::shared_ptr<std::atomic<replay_phase>>& phase)
		{
			m_thread = std::thread([this, sink, phase]()
			{
				u32 ticks = 0;
				while (m_running.load())
				{
					std::this_thread::sleep_for(std::chrono::milliseconds{100});

					if (!m_running.load())
					{
						break;
					}

					if (++ticks < 50)
					{
						continue;
					}

					ticks = 0;
					std::fprintf(stderr,
						"status: phase=%s frames_written=%llu next_frame=%u expected_frames=%llu emu_stopped=%d failed=%d\n",
						phase_name(phase->load()),
						static_cast<unsigned long long>(sink->frames_written.load()),
						sink->frame_index.load(),
						static_cast<unsigned long long>(sink->expected_frames.load()),
						Emu.IsStopped() ? 1 : 0,
						sink->failed.load() ? 1 : 0);
				}
			});
		}

		~scoped_watchdog()
		{
			m_running = false;

			if (m_thread.joinable())
			{
				m_thread.join();
			}
		}
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

	std::string dir_from_env(const char* name, std::string_view home_suffix)
	{
		std::string dir;
		if (const char* value = std::getenv(name); value && *value)
		{
			dir = value;
		}
		else
		{
			const char* home = std::getenv("HOME");
			dir = std::string(home && *home ? home : ".") + "/" + std::string(home_suffix);
		}

		if (dir.back() != '/')
		{
			dir += '/';
		}

		return dir;
	}

	// Android builds read these instead of resolving XDG paths (see Utilities/File.cpp).
	bool init_android_dirs()
	{
		char exe_path[4096]{};
		if (::readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1) <= 0)
		{
			std::fprintf(stderr, "error: failed to resolve /proc/self/exe\n");
			return false;
		}

		g_android_executable_dir = fs::get_parent_dir(exe_path) + "/";
		g_android_config_dir = dir_from_env("RPCS3_CONFIG_DIR", ".config/rpcs3");
		g_android_cache_dir = dir_from_env("RPCS3_CACHE_DIR", ".cache/rpcs3");

		for (const std::string& dir : {g_android_config_dir, g_android_cache_dir})
		{
			if (!fs::create_path(dir))
			{
				std::fprintf(stderr, "error: failed to create %s (%s)\n", dir.c_str(), fs::g_tls_error);
				return false;
			}
		}

		return true;
	}

	void configure_callbacks(const std::shared_ptr<rsx_replay_frame_sink>& sink, const std::shared_ptr<main_thread_dispatcher>& dispatcher)
	{
		emu_callbacks callbacks{};

		callbacks.call_from_main_thread = [dispatcher](std::function<void()> func, atomic_t<u32>* wake_up)
		{
			dispatcher->dispatch(std::move(func), wake_up);
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
		callbacks.get_audio = []() -> std::shared_ptr<AudioBackend> { return std::make_shared<NullAudioBackend>(); };
		callbacks.get_audio_enumerator = [](u64) -> std::shared_ptr<audio_device_enumerator> { return std::make_shared<null_enumerator>(); };
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
		callbacks.on_install_pkgs = [](const std::vector<std::string>&, bool) { return false; };
		callbacks.add_breakpoint = [](u32) {};
		callbacks.display_sleep_control_supported = []() { return false; };
		callbacks.enable_display_sleep = [](bool) {};
		callbacks.check_microphone_permissions = []() {};
		callbacks.enable_gamemode = [](bool) {};

		g_emu_callbacks = std::move(callbacks);
	}
}

int main(int argc, char** argv)
{
	std::setvbuf(stderr, nullptr, _IONBF, 0);
	std::setlocale(LC_ALL, "C");
	std::fprintf(stderr, "status: phase=startup\n");

	replay_cli cli;
	if (!parse_cli(argc, argv, cli))
	{
		print_usage(argv[0]);
		return 2;
	}

	auto sink = std::make_shared<rsx_replay_frame_sink>();
	sink->output_prefix = cli.output_prefix;
	sink->expected_frames = cli.loops;

	auto phase = std::make_shared<std::atomic<replay_phase>>(replay_phase::parsed);
	scoped_watchdog watchdog(sink, phase);

	const auto set_phase = [&phase](replay_phase next)
	{
		phase->store(next);
		std::fprintf(stderr, "status: phase=%s\n", phase_name(next));
	};

	if (!init_android_dirs())
	{
		return 1;
	}

	// RPCS3.log in the cache dir, capped like rpcs3.cpp does it
	fs::device_stat log_stats{};
	fs::statfs(fs::get_cache_dir(), log_stats);
	auto log_file = logs::make_file_listener(fs::get_cache_dir() + "RPCS3.log", std::max<u64>(log_stats.avail_free / 4, 64 * 1024 * 1024));

	// Listeners stay registered until exit
	logs::listener::add(new fatal_log_listener(sink));

	set_phase(replay_phase::vulkan_loader);
	std::string vulkan_error;
	if (!vk::init_android_vulkan_loader(&vulkan_error))
	{
		std::fprintf(stderr, "error: %s\n", vulkan_error.c_str());
		return 1;
	}

	set_phase(replay_phase::callbacks);
	auto dispatcher = std::make_shared<main_thread_dispatcher>();
	configure_callbacks(sink, dispatcher);

	set_phase(replay_phase::emu_init);
	Emu.SetHasGui(false);
	Emu.SetSupportedRenderers({video_renderer::null, video_renderer::vulkan});
	Emu.SetUsr("00000001");
	Emu.Init();
	if (!cli.config_path.empty())
	{
		set_phase(replay_phase::config);
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
	g_cfg.misc.use_native_interface.set(false);
	g_cfg.audio.renderer.set(audio_renderer::null);

	// BootRsxCapture re-runs Emu.Init(), which reloads the global config from the config dir.
	Emulator::SaveSettings(g_cfg.to_string(), {});

	Emu.SetRsxCaptureReplayLimit(0);

	set_phase(replay_phase::booting);
	if (!Emu.BootRsxCapture(cli.capture_path))
	{
		std::fprintf(stderr, "error: failed to boot RSX capture '%s'\n", cli.capture_path.c_str());
		return 1;
	}

	set_phase(replay_phase::running);
	bool shutdown_requested = false;

	while (true)
	{
		dispatcher->drain();

		if (sink->failed.load())
		{
			break;
		}

		if (!shutdown_requested && sink->is_complete())
		{
			set_phase(replay_phase::killing);
			Emu.Kill(false);
			shutdown_requested = true;
		}

		dispatcher->drain();

		if (sink->failed.load() || Emu.IsStopped())
		{
			break;
		}

		std::this_thread::sleep_for(std::chrono::microseconds{10'000});
	}

	dispatcher->drain();
	set_phase(replay_phase::done);

	int exit_code = 0;

	if (sink->failed.load())
	{
		std::fprintf(stderr, "error: %s\n", sink->get_error().c_str());
		exit_code = 1;
	}
	else if (sink->expected_frames.load() && !sink->is_complete())
	{
		std::fprintf(stderr,
			"error: replay stopped after writing %llu/%llu frame(s)\n",
			static_cast<unsigned long long>(sink->frames_written.load()),
			static_cast<unsigned long long>(sink->expected_frames.load()));
		exit_code = 1;
	}

	// Static destructors free Vulkan memory after the VMM bookkeeping is gone, so skip them.
	log_file.reset();
	std::fflush(nullptr);
	std::_Exit(exit_code);
}
