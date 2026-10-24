#include "stdafx.h"
#include "media_utils.h"

namespace utils
{
	std::string av_error_to_string(int error)
	{
		return fmt::format("ffmpeg unavailable on this Android build: 0x%x", error);
	}

	std::vector<ffmpeg_codec> list_ffmpeg_decoders()
	{
		return {};
	}

	std::vector<ffmpeg_codec> list_ffmpeg_encoders()
	{
		return {};
	}

	template <>
	std::string media_info::get_metadata<std::string>(const std::string& key, const std::string& def) const
	{
		const auto found = metadata.find(key);
		return found == metadata.end() ? def : found->second;
	}

	template <>
	s64 media_info::get_metadata<s64>(const std::string& key, const s64& def) const
	{
		const auto found = metadata.find(key);
		return found == metadata.end() ? def : std::strtoll(found->second.c_str(), nullptr, 10);
	}

	std::pair<bool, media_info> get_media_info(const std::string& path, s32)
	{
		media_info info{};
		info.path = path;
		return {false, std::move(info)};
	}

	audio_decoder::audio_decoder() = default;
	audio_decoder::~audio_decoder() = default;

	void audio_decoder::set_context(music_selection_context&& context)
	{
		m_context = std::move(context);
	}

	void audio_decoder::set_swap_endianness(bool swapped)
	{
		m_swap_endianness = swapped;
	}

	void audio_decoder::clear()
	{
		std::lock_guard lock(m_mtx);
		data.clear();
		timestamps_ms.clear();
		m_size = 0;
		track_fully_decoded = 0;
		track_fully_consumed = 0;
		has_error = false;
	}

	void audio_decoder::stop()
	{
		clear();
	}

	void audio_decoder::decode()
	{
		has_error = true;
		track_fully_decoded = 1;
		track_fully_consumed = 1;
	}

	u32 audio_decoder::set_next_index(bool)
	{
		return 0;
	}

	video_encoder::video_encoder() = default;
	video_encoder::~video_encoder() = default;

	std::string video_encoder::path() const
	{
		return m_path;
	}

	s64 video_encoder::last_video_pts() const
	{
		return m_last_video_pts;
	}

	void video_encoder::set_path(const std::string& path)
	{
		m_path = path;
	}

	void video_encoder::set_framerate(u32 framerate)
	{
		m_framerate = framerate;
	}

	void video_encoder::set_video_bitrate(u32 bitrate)
	{
		m_video_bitrate_bps = bitrate;
	}

	void video_encoder::set_output_format(frame_format format)
	{
		m_out_format = format;
	}

	void video_encoder::set_video_codec(s32 codec_id, std::string_view codec_name)
	{
		m_video_codec_id = codec_id;
		m_video_codec_name = codec_name;
	}

	void video_encoder::set_max_b_frames(s32 max_b_frames)
	{
		m_max_b_frames = max_b_frames;
	}

	void video_encoder::set_gop_size(s32 gop_size)
	{
		m_gop_size = gop_size;
	}

	void video_encoder::set_sample_rate(u32 sample_rate)
	{
		m_sample_rate = sample_rate;
	}

	void video_encoder::set_audio_channels(u32 channels)
	{
		m_channels = channels;
	}

	void video_encoder::set_audio_bitrate(u32 bitrate)
	{
		m_audio_bitrate_bps = bitrate;
	}

	void video_encoder::set_audio_codec(s32 codec_id, std::string_view codec_name)
	{
		m_audio_codec_id = codec_id;
		m_audio_codec_name = codec_name;
	}

	void video_encoder::pause(bool flush)
	{
		m_flush = flush;
		m_paused = true;
	}

	void video_encoder::stop(bool flush)
	{
		std::scoped_lock lock(m_video_mtx, m_audio_mtx);
		m_flush = flush;
		m_paused = false;
		m_frames_to_encode.clear();
		m_samples_to_encode.clear();
	}

	void video_encoder::resume()
	{
		m_flush = false;
		m_paused = false;
	}

	void video_encoder::encode()
	{
		has_error = true;
	}
}
