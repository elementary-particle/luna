#ifndef LUNA_MIXER_H
#define LUNA_MIXER_H

#include "lua.hpp"

#include "factory.h"

#include <SDL3_mixer/SDL_mixer.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace luna {

class Mixer {
private:
  struct LuaAudio {
    MIX_Audio *audio = nullptr;
    uint32_t track_refs = 0;
    bool destroy_requested = false;
  };

  struct AudioTrackState {
    MIX_Track *track = nullptr;
    int audio_ref = LUA_NOREF;
  };

  MIX_Mixer *mixer_ = nullptr;
  uint64_t next_audio_track_id_ = 1;
  std::unordered_map<uint64_t, AudioTrackState> audio_tracks_;

  void SetLuaGlobals(lua_State *L);
  static Mixer *GetInstance(lua_State *L);

  static LuaAudio *CheckLuaAudio(lua_State *L, int idx);
  static void MaybeDestroyAudio(LuaAudio *audio);
  static void ReleaseTrackAudio(lua_State *L, AudioTrackState &track_state);

  static int L_AudioTrackCreate(lua_State *L);
  static int L_AudioTrackDestroy(lua_State *L);
  static int L_AudioTrackSet(lua_State *L);
  static int L_AudioTrackPlay(lua_State *L);
  static int L_AudioTrackStop(lua_State *L);
  static int L_AudioTrackSetGain(lua_State *L);
  static int L_AudioSetMixerGain(lua_State *L);
  static int L_AudioDestroy(lua_State *L);

public:
  class LoadAudioJob : public AsyncJob {
  public:
    ~LoadAudioJob() override;
    void Invoke(lua_State *L) override;
    void Run() override;
    int Finish(lua_State *L) override;

  private:
    std::string path_;
    bool predecode_ = false;
    MIX_Mixer *mixer_ = nullptr;
    MIX_Audio *audio_ = nullptr;
  };

  bool Init();
  void Fini(lua_State *L = nullptr);

  void RegisterBindings(lua_State *L);
};

}

#endif
