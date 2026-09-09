#ifndef LUNA_MIXER_H
#define LUNA_MIXER_H

#include "vfs.h"

#include "factory.h"
#include "file_vfs.h"

#include <SDL3_mixer/SDL_mixer.h>

#include <cstdint>
#include <string>
#include <unordered_map>

#include "lua_util.hpp"

namespace luna {

class Engine;
struct EventState;
struct AudioStreamSource;

class Mixer {
private:
  struct LAudio {
    static constexpr const char *MT = "luna.Audio";
    MIX_Audio *audio = nullptr;
    uint32_t track_refs = 0;
    bool destroy_requested = false;
    std::shared_ptr<AudioStreamSource> streamed;
  };

  struct TrackState {
    Mixer *mixer = nullptr;
    MIX_Track *track = nullptr;
    int audio_ref = LUA_NOREF;
    std::shared_ptr<EventState> stop_event;
  };

  class LoadAudioJob : public AsyncJob {
  public:
    LoadAudioJob(Mixer *mixer);
    ~LoadAudioJob() override;
    void Invoke(lua_State *L) override;
    void Run() override;
    int Finish(lua_State *L) override;

  private:
    std::string path_;
    AssetInput input_;
    std::shared_ptr<AudioStreamSource> source_;
    bool predecode_ = false;
    bool stream_ = false;
    MIX_Mixer *mixer_ = nullptr;
    MIX_Audio *audio_ = nullptr;
  };

  Engine *engine_ = nullptr;
  MIX_Mixer *mixer_ = nullptr;
  uint64_t next_audio_track_id_ = 1;
  std::unordered_map<uint64_t, TrackState> audio_tracks_;

  Engine *engine() const { return engine_; }

  static void MaybeDestroyAudio(LAudio *audio);
  static void ReleaseTrackAudio(lua_State *L, TrackState &track_state);

  static int L_TrackCreate(lua_State *L);
  static int L_TrackDestroy(lua_State *L);
  static int L_TrackSet(lua_State *L);
  static int L_TrackPlay(lua_State *L);
  static int L_TrackStop(lua_State *L);
  static int L_TrackPlaying(lua_State *L);
  static int L_TrackStopEvent(lua_State *L);
  static int L_TrackSetGain(lua_State *L);
  static int L_SetMixerGain(lua_State *L);
  static int L_AudioDestroy(lua_State *L);

public:
  bool Init(Engine *engine);
  void ReleaseLua(lua_State *L);
  void Fini();

  void BindLua(lua_State *L);

  std::unique_ptr<AsyncJob> MakeLoadAudioJob();
};

} // namespace luna

#endif
