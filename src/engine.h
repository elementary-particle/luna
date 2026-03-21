#ifndef LUNA_ENGINE_H
#define LUNA_ENGINE_H

#include "lua.hpp"

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "factory.h"
#include "mixer.h"
#include "renderer.h"
#include "vfs.h"

namespace luna {

enum class WaitKind {
  NONE = 0,
  NEXT_FRAME,
  EVENTS,
};

struct Task;
struct EventState;

struct WaitLink {
  Task *task = nullptr;
  EventState *event = nullptr;
  int index = 0; // 1-based Lua index for ready_index

  // Links for the event's waiter list (EventState owns head)
  WaitLink *prev_task = nullptr;
  WaitLink *next_task = nullptr;

  // Links for the task's waited-events list (Task owns head)
  WaitLink *prev_event = nullptr;
  WaitLink *next_event = nullptr;
};

struct EventState {
  WaitLink *waiters_head = nullptr;
};

struct WaitState {
  WaitKind kind = WaitKind::NONE;
  WaitLink *links_head = nullptr;
  int ready_index = 0;
};

struct Task {
  lua_State *co = nullptr;
  int co_ref = LUA_NOREF;
  WaitState wait;
};

class Engine {
public:
  Engine();
  ~Engine();

  bool Init();
  void Run(const std::string &entry_path = "main.lua");

private:
  struct TimerEntry {
    double deadline_seconds = 0.0;
    std::shared_ptr<EventState> event;
  };

  struct PromiseState {
    enum class Settlement {
      PENDING = 0,
      FULFILLED,
      REJECTED,
    };

    lua_State *owner = nullptr;
    PromiseId id = 0;
    Settlement settlement = Settlement::PENDING;
    std::shared_ptr<EventState> event;
    std::unique_ptr<AsyncJob> completed_job;
    std::string error;
    std::vector<int> result_refs;

    ~PromiseState();
  };

  struct LEvent {
    static constexpr const char *MT = "luna.Event";
    std::shared_ptr<EventState> state;
  };

  struct LPromise {
    static constexpr const char *MT = "luna.Promise";
    std::shared_ptr<PromiseState> state;
  };

  Renderer renderer_;
  Mixer mixer_;
  VFS vfs_;

  lua_State *L_;

  double now_seconds_ = 0.0;
  std::chrono::duration<double> frame_time_;

  Factory factory_;

  std::unordered_map<PromiseId, std::shared_ptr<PromiseState>>
      pending_promises_;
  std::deque<Task *> tasks_;
  std::vector<Task *> next_frame_tasks_;
  int alive_task_count_ = 0;
  std::vector<TimerEntry> timers_;

  bool InitLua();

  void RegisterBindings(lua_State *L);
  void RegisterLuaTypes(lua_State *L);
  bool CallLuaMain(const std::string &entry_path);

  void DrainPromiseCompletions();
  void AdvanceTime(double dt);
  void SignalExpiredTimers();
  void Tick();

  static void EnsureCoroutineTaskMap(lua_State *L);
  static Task *GetCurrentTask(lua_State *L);
  int ResumeTask(Task *t);
  void RemoveTask(Task *t);

  static void UnlinkWaitNode(WaitLink *node);
  static void UnlinkAllEvents(Task *t);
  static void LinkEvent(Task *t, EventState *e, int index);

  std::shared_ptr<EventState> CreateEvent() const;
  void SignalEvent(const std::shared_ptr<EventState> &event);

  static int YieldWithWait(lua_State *L);

  static int L_Start(lua_State *L);
  static int L_NextFrame(lua_State *L);
  static int L_MakeEvent(lua_State *L);
  static int L_Wait(lua_State *L);
  static int L_Now(lua_State *L);
  static int L_After(lua_State *L);
  static int L_SetFrameTime(lua_State *L);
  static int L_SetWindowSize(lua_State *L);

  static int L_EventSignal(lua_State *L);
  static int L_PromisePoll(lua_State *L);
  static int L_PromiseTake(lua_State *L);
  static int L_PromiseEvent(lua_State *L);

  static inline int L_StartAsyncJob(lua_State *L, Engine *e,
                                    std::unique_ptr<AsyncJob> &&job) {
    job->Invoke(L);

    const PromiseId promise_id = e->factory_.EnqueueJob(std::move(job));
    auto promise = std::make_shared<PromiseState>();
    promise->owner = L;
    promise->id = promise_id;
    promise->event = e->CreateEvent();
    e->pending_promises_[promise_id] = promise;
    lua::New<LPromise>(L, promise);
    return 1;
  }
};

} // namespace luna

#endif
