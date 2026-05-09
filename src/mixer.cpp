#include "mixer.h"

#include <SDL3/SDL_iostream.h>

#include <fmt/format.h>
#include <tracy/Tracy.hpp>

#include "engine.h"
#include "log.h"

namespace luna {

static constexpr const char *MIXER_PTR_KEY = "luna.mixer_ptr";

void Mixer::MaybeDestroyAudio(LAudio *audio) {
  if (!audio) {
    return;
  }
  if (audio->audio && audio->track_refs == 0 && audio->destroy_requested) {
    MIX_DestroyAudio(audio->audio);
    audio->audio = nullptr;
  }
}

void Mixer::ReleaseTrackAudio(lua_State *L, TrackState &track_state) {
  if (track_state.audio_ref == LUA_NOREF) {
    return;
  }

  lua_rawgeti(L, LUA_REGISTRYINDEX, track_state.audio_ref);
  auto *audio = lua::Check<LAudio>(L, -1);
  if (audio->track_refs > 0) {
    audio->track_refs -= 1;
  }
  MaybeDestroyAudio(audio);
  lua_pop(L, 1);

  luaL_unref(L, LUA_REGISTRYINDEX, track_state.audio_ref);
  track_state.audio_ref = LUA_NOREF;
}

bool Mixer::Init(Engine *engine) {
  engine_ = engine;

  log::Info("mixer", "initializing");
  if (!MIX_Init()) {
    log::Error("mixer", "MIX_Init failed: {}", SDL_GetError());
    return false;
  }

  mixer_ = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
  if (!mixer_) {
    log::Error("mixer", "MIX_CreateMixerDevice failed: {}", SDL_GetError());
    MIX_Quit();
    return false;
  }

  log::Info("mixer", "initialized successfully");
  return true;
}

void Mixer::ReleaseLua(lua_State *L) {
  if (mixer_) {
    MIX_StopAllTracks(mixer_, 0);
  }
  if (L) {
    for (auto &[id, track_state] : audio_tracks_) {
      (void)id;
      ReleaseTrackAudio(L, track_state);
    }
  }
}

void Mixer::Fini() {
  for (auto &[id, track_state] : audio_tracks_) {
    (void)id;
    if (track_state.track) {
      MIX_DestroyTrack(track_state.track);
      track_state.track = nullptr;
    }
  }
  audio_tracks_.clear();

  if (mixer_) {
    MIX_DestroyMixer(mixer_);
    mixer_ = nullptr;
  }

  MIX_Quit();
}

void Mixer::BindLua(lua_State *L) {
  if (lua::NewType<LAudio>(L)) {
    lua_pushcfunction(L, &L_AudioDestroy);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, &L_AudioDestroy);
    lua_setfield(L, -2, "destroy");

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
  }
  lua_pop(L, 1);

  lua_newtable(L);
  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &L_TrackCreate, 1);
  lua_setfield(L, -2, "track_create");

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &L_TrackDestroy, 1);
  lua_setfield(L, -2, "track_destroy");

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &L_TrackSet, 1);
  lua_setfield(L, -2, "track_set");

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &L_TrackPlay, 1);
  lua_setfield(L, -2, "track_play");

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &L_TrackStop, 1);
  lua_setfield(L, -2, "track_stop");

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &L_TrackPlaying, 1);
  lua_setfield(L, -2, "track_playing");

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &L_TrackStopEvent, 1);
  lua_setfield(L, -2, "track_stop_event");

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &L_TrackSetGain, 1);
  lua_setfield(L, -2, "track_set_gain");

  lua_pushlightuserdata(L, this);
  lua_pushcclosure(L, &L_SetMixerGain, 1);
  lua_setfield(L, -2, "set_mixer_gain");

  lua_setfield(L, -2, "audio");
}

std::unique_ptr<AsyncJob> Mixer::MakeLoadAudioJob(asset::Vfs *vfs) {
  return std::make_unique<LoadAudioJob>(this, vfs);
}

Mixer::LoadAudioJob::LoadAudioJob(Mixer *mixer, asset::Vfs *vfs) : vfs_(vfs) {
  mixer_ = mixer->mixer_;
}

void Mixer::LoadAudioJob::Invoke(lua_State *L) {
  const char *path = luaL_checkstring(L, 1);
  if (!path || !*path) {
    luaL_error(L, "load_audio: path is empty");
  }

  path_ = path;
  predecode_ = lua_toboolean(L, 2) != 0;
  if (!vfs_) {
    luaL_error(L, "load_audio: VFS is not available");
  }
  auto mapped = vfs_->MapFile(path_);
  if (!mapped) {
    luaL_error(L, "load_audio: failed to open file: %s", path);
  }
  mapping_ = std::move(mapped).value();
  if (!mixer_) {
    luaL_error(L, "audio mixer is not initialized");
  }
}

Mixer::LoadAudioJob::~LoadAudioJob() {
  if (audio_) {
    MIX_DestroyAudio(audio_);
    audio_ = nullptr;
  }
}

void Mixer::LoadAudioJob::Run() {
  ZoneScopedN("LoadAudio");
  SDL_IOStream *io = SDL_IOFromConstMem(
      mapping_.data(), static_cast<size_t>(mapping_.size()));
  if (!io) {
    error_ =
        fmt::format("SDL_IOFromConstMem failed for {}: {}", path_,
            SDL_GetError());
    return;
  }
  audio_ = MIX_LoadAudio_IO(mixer_, io, predecode_, true);
  if (!audio_) {
    error_ =
        fmt::format("MIX_LoadAudio_IO failed for {}: {}", path_,
            SDL_GetError());
  }
}

int Mixer::LoadAudioJob::Finish(lua_State *L) {
  auto *ud = lua::New<LAudio>(L, audio_, 0, false);
  audio_ = nullptr;
  return 1;
}

int Mixer::L_TrackCreate(lua_State *L) {
  Mixer *m = static_cast<Mixer *>(lua_touserdata(L, lua_upvalueindex(1)));
  if (!m->mixer_) {
    return luaL_error(L, "audio mixer is not initialized");
  }

  MIX_Track *track = MIX_CreateTrack(m->mixer_);
  if (!track) {
    return luaL_error(L, "MIX_CreateTrack failed: %s", SDL_GetError());
  }

  const uint64_t id = m->next_audio_track_id_++;
  m->audio_tracks_.try_emplace(id, m, track);
  lua_pushinteger(L, static_cast<lua_Integer>(id));
  return 1;
}

int Mixer::L_TrackDestroy(lua_State *L) {
  Mixer *m = static_cast<Mixer *>(lua_touserdata(L, lua_upvalueindex(1)));
  const uint64_t id = static_cast<uint64_t>(luaL_checkinteger(L, 1));
  auto it = m->audio_tracks_.find(id);
  if (it == m->audio_tracks_.end()) {
    return 0;
  }

  if (it->second.track) {
    MIX_DestroyTrack(it->second.track);
  }
  ReleaseTrackAudio(L, it->second);

  m->audio_tracks_.erase(it);
  return 0;
}

int Mixer::L_TrackSet(lua_State *L) {
  Mixer *m = static_cast<Mixer *>(lua_touserdata(L, lua_upvalueindex(1)));
  const uint64_t id = static_cast<uint64_t>(luaL_checkinteger(L, 1));
  auto *audio = lua::Check<LAudio>(L, 2);

  auto it = m->audio_tracks_.find(id);
  if (it == m->audio_tracks_.end()) {
    return luaL_error(L, "track not found: %llu", (unsigned long long)id);
  }
  if (!audio->audio) {
    return luaL_error(L, "audio handle is invalid");
  }

  if (!MIX_SetTrackAudio(it->second.track, audio->audio)) {
    return luaL_error(L, "MIX_SetTrackAudio failed: %s", SDL_GetError());
  }

  ReleaseTrackAudio(L, it->second);

  lua_pushvalue(L, 2);
  it->second.audio_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  audio->track_refs += 1;
  return 0;
}

int Mixer::L_TrackPlay(lua_State *L) {
  Mixer *m = static_cast<Mixer *>(lua_touserdata(L, lua_upvalueindex(1)));
  const uint64_t id = static_cast<uint64_t>(luaL_checkinteger(L, 1));
  auto it = m->audio_tracks_.find(id);
  if (it == m->audio_tracks_.end()) {
    return luaL_error(L, "track not found: %llu", (unsigned long long)id);
  }

  Sint64 loops = 0;
  Sint64 fade_in_ms = 0;

  if (lua_istable(L, 2)) {
    lua_getfield(L, 2, "loops");
    if (!lua_isnil(L, -1)) {
      loops = (Sint64)luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "fade_in_ms");
    if (!lua_isnil(L, -1)) {
      fade_in_ms = (Sint64)luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);
  }

  SDL_PropertiesID props = SDL_CreateProperties();
  if (props == 0) {
    return luaL_error(L, "SDL_CreateProperties failed: %s", SDL_GetError());
  }

  if (!SDL_SetNumberProperty(props, MIX_PROP_PLAY_LOOPS_NUMBER, loops)) {
    SDL_DestroyProperties(props);
    return luaL_error(L, "failed to set loops property: %s", SDL_GetError());
  }

  if (fade_in_ms > 0 &&
      !SDL_SetNumberProperty(
          props, MIX_PROP_PLAY_FADE_IN_MILLISECONDS_NUMBER, fade_in_ms)) {
    SDL_DestroyProperties(props);
    return luaL_error(L, "failed to set fade property: %s", SDL_GetError());
  }

  const bool ok = MIX_PlayTrack(it->second.track, props);
  SDL_DestroyProperties(props);

  if (!ok) {
    return luaL_error(L, "MIX_PlayTrack failed: %s", SDL_GetError());
  }

  return 0;
}

int Mixer::L_TrackStop(lua_State *L) {
  Mixer *m = static_cast<Mixer *>(lua_touserdata(L, lua_upvalueindex(1)));
  const uint64_t id = static_cast<uint64_t>(luaL_checkinteger(L, 1));
  const Sint64 fade_out_ms = (Sint64)luaL_optinteger(L, 2, 0);

  auto it = m->audio_tracks_.find(id);
  if (it == m->audio_tracks_.end()) {
    return luaL_error(L, "track not found: %llu", (unsigned long long)id);
  }

  Sint64 fade_frames = 0;
  if (fade_out_ms > 0) {
    fade_frames = MIX_TrackMSToFrames(it->second.track, fade_out_ms);
  }

  if (!MIX_StopTrack(it->second.track, fade_frames)) {
    return luaL_error(L, "MIX_StopTrack failed: %s", SDL_GetError());
  }

  return 0;
}

int Mixer::L_TrackPlaying(lua_State *L) {
  Mixer *m = static_cast<Mixer *>(lua_touserdata(L, lua_upvalueindex(1)));
  const uint64_t id = static_cast<uint64_t>(luaL_checkinteger(L, 1));

  auto it = m->audio_tracks_.find(id);
  if (it == m->audio_tracks_.end()) {
    return luaL_error(L, "track not found: %llu", (unsigned long long)id);
  }

  TrackState &track_state = it->second;
  lua_pushboolean(L, MIX_TrackPlaying(track_state.track));
  return 1;
}

int Mixer::L_TrackStopEvent(lua_State *L) {
  Mixer *m = static_cast<Mixer *>(lua_touserdata(L, lua_upvalueindex(1)));
  Engine *e = m->engine();
  const uint64_t id = static_cast<uint64_t>(luaL_checkinteger(L, 1));

  auto it = m->audio_tracks_.find(id);
  if (it == m->audio_tracks_.end()) {
    return luaL_error(L, "track not found: %llu", (unsigned long long)id);
  }

  TrackState &track_state = it->second;
  if (!track_state.stop_event) {
    auto stop_event = e->CreateEvent();
    track_state.stop_event = stop_event;
    if (!MIX_SetTrackStoppedCallback(
            it->second.track,
            [](void *userdata, MIX_Track *track) {
              auto track_state = static_cast<TrackState *>(userdata);
              track_state->mixer->engine()->SignalEvent(
                  track_state->stop_event);
            },
            &track_state)) {
      track_state.stop_event.reset();
      return luaL_error(
          L, "MIX_SetTrackStoppedCallback failed: %s", SDL_GetError());
    }
  }

  e->PushEvent(L, track_state.stop_event);
  return 1;
}

int Mixer::L_TrackSetGain(lua_State *L) {
  Mixer *m = static_cast<Mixer *>(lua_touserdata(L, lua_upvalueindex(1)));
  const uint64_t id = static_cast<uint64_t>(luaL_checkinteger(L, 1));
  const float gain = (float)luaL_checknumber(L, 2);

  auto it = m->audio_tracks_.find(id);
  if (it == m->audio_tracks_.end()) {
    return luaL_error(L, "track not found: %llu", (unsigned long long)id);
  }

  if (gain < 0.0f) {
    return luaL_error(L, "gain must be >= 0");
  }

  if (!MIX_SetTrackGain(it->second.track, gain)) {
    return luaL_error(L, "MIX_SetTrackGain failed: %s", SDL_GetError());
  }

  return 0;
}

int Mixer::L_SetMixerGain(lua_State *L) {
  Mixer *m = static_cast<Mixer *>(lua_touserdata(L, lua_upvalueindex(1)));
  const float gain = (float)luaL_checknumber(L, 1);
  if (gain < 0.0f) {
    return luaL_error(L, "gain must be >= 0");
  }

  if (!MIX_SetMixerGain(m->mixer_, gain)) {
    return luaL_error(L, "MIX_SetMixerGain failed: %s", SDL_GetError());
  }

  return 0;
}

int Mixer::L_AudioDestroy(lua_State *L) {
  auto *audio = lua::Check<LAudio>(L, 1);
  audio->destroy_requested = true;
  Mixer::MaybeDestroyAudio(audio);
  return 0;
}

} // namespace luna
