#include "tokkel/tokkel.hpp"

#include <Stk.h>
#include <miniaudio.h>

#include <mutex>
#include <unordered_map>

namespace tokkel {

namespace {

// A Source seen by miniaudio as an endless, unseekable data source.
struct Voice {
  ma_data_source_base base{}; // must stay first: miniaudio casts Voice* to it
  ma_sound sound{};
  std::shared_ptr<Source> source;
  ma_uint32 channels = 0;
  ma_uint32 sample_rate = 0;
  bool sound_ready = false;
  bool base_ready = false;

  ~Voice() {
    // The sound first: uninit detaches it from the graph, after which the
    // audio thread no longer reads from this voice.
    if (sound_ready)
      ma_sound_uninit(&sound);
    if (base_ready)
      ma_data_source_uninit(&base);
  }
};

ma_result voice_read(ma_data_source *data_source, void *frames_out, ma_uint64 frame_count,
                     ma_uint64 *frames_read) {
  auto *voice = static_cast<Voice *>(data_source);
  if (frames_out != nullptr)
    voice->source->render(static_cast<float *>(frames_out), static_cast<std::uint32_t>(frame_count),
                          voice->channels);
  if (frames_read != nullptr)
    *frames_read = frame_count;
  return MA_SUCCESS;
}

ma_result voice_seek(ma_data_source *, ma_uint64) { return MA_NOT_IMPLEMENTED; }

ma_result voice_format(ma_data_source *data_source, ma_format *format, ma_uint32 *channels,
                       ma_uint32 *sample_rate, ma_channel *channel_map, size_t channel_map_cap) {
  auto *voice = static_cast<Voice *>(data_source);
  if (format != nullptr)
    *format = ma_format_f32;
  if (channels != nullptr)
    *channels = voice->channels;
  if (sample_rate != nullptr)
    *sample_rate = voice->sample_rate;
  if (channel_map != nullptr)
    ma_channel_map_init_standard(ma_standard_channel_map_default, channel_map, channel_map_cap,
                                 voice->channels);
  return MA_SUCCESS;
}

ma_data_source_vtable voice_vtable = {
    voice_read, voice_seek, voice_format, nullptr, nullptr, nullptr, 0,
};

} // namespace

struct Engine::Impl {
  ma_engine engine{};
  bool ok = false;
  bool offline = false;
  std::mutex voices_mutex;
  std::unordered_map<VoiceId, std::unique_ptr<Voice>> voices;
  VoiceId next_voice = 1;
};

Engine::Engine(const EngineConfig &config) : impl(std::make_unique<Impl>()) {
  ma_engine_config engine_config = ma_engine_config_init();
  engine_config.sampleRate = config.sample_rate;
  engine_config.channels = config.channels;
  engine_config.noDevice = config.offline ? MA_TRUE : MA_FALSE;
  impl->offline = config.offline;
  impl->ok = ma_engine_init(&engine_config, &impl->engine) == MA_SUCCESS;
  if (impl->ok)
    stk::Stk::setSampleRate(static_cast<stk::StkFloat>(sample_rate()));
}

Engine::~Engine() {
  if (!impl->ok)
    return;
  impl->voices.clear(); // sounds must go before their engine
  ma_engine_uninit(&impl->engine);
}

bool Engine::ok() const { return impl->ok; }

std::uint32_t Engine::sample_rate() const {
  return impl->ok ? ma_engine_get_sample_rate(&impl->engine) : 0;
}

std::uint32_t Engine::channels() const {
  return impl->ok ? ma_engine_get_channels(&impl->engine) : 0;
}

void Engine::set_master_volume(float volume) {
  if (impl->ok)
    ma_engine_set_volume(&impl->engine, volume);
}

bool Engine::play_file(const std::string &path) {
  return impl->ok && ma_engine_play_sound(&impl->engine, path.c_str(), nullptr) == MA_SUCCESS;
}

VoiceId Engine::play(std::shared_ptr<Source> source, float volume) {
  if (!impl->ok || !source)
    return 0;

  auto voice = std::make_unique<Voice>();
  voice->source = std::move(source);
  voice->channels = channels();
  voice->sample_rate = sample_rate();

  ma_data_source_config source_config = ma_data_source_config_init();
  source_config.vtable = &voice_vtable;
  if (ma_data_source_init(&source_config, &voice->base) != MA_SUCCESS)
    return 0;
  voice->base_ready = true;

  // No spatialization or pitch: a Source is already at the engine's format.
  const ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH;
  if (ma_sound_init_from_data_source(&impl->engine, &voice->base, flags, nullptr, &voice->sound) !=
      MA_SUCCESS)
    return 0;
  voice->sound_ready = true;

  ma_sound_set_volume(&voice->sound, volume);
  if (ma_sound_start(&voice->sound) != MA_SUCCESS)
    return 0;

  std::lock_guard<std::mutex> lock(impl->voices_mutex);
  const VoiceId id = impl->next_voice++;
  impl->voices.emplace(id, std::move(voice));
  return id;
}

void Engine::set_volume(VoiceId voice, float volume) {
  std::lock_guard<std::mutex> lock(impl->voices_mutex);
  auto found = impl->voices.find(voice);
  if (found != impl->voices.end())
    ma_sound_set_volume(&found->second->sound, volume);
}

void Engine::stop(VoiceId voice) {
  std::unique_ptr<Voice> stopped;
  {
    std::lock_guard<std::mutex> lock(impl->voices_mutex);
    auto found = impl->voices.find(voice);
    if (found == impl->voices.end())
      return;
    stopped = std::move(found->second);
    impl->voices.erase(found);
  }
  // `stopped` is torn down here, outside the lock.
}

std::uint32_t Engine::render(float *out, std::uint32_t frames) {
  if (!impl->ok || !impl->offline)
    return 0;
  // miniaudio reports only the frames its nodes produced (none when nothing
  // is playing) but always fills the rest of `out` with silence.
  if (ma_engine_read_pcm_frames(&impl->engine, out, frames, nullptr) != MA_SUCCESS)
    return 0;
  return frames;
}

} // namespace tokkel
