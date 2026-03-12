#include "engine.h"

#include <SDL3/SDL_render.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>

#include <fmt/core.h>

#include "factory.h"
#include "renderer.h"

namespace luna {

static constexpr const char *LIBRARY_REGKEY = "luna";
static constexpr const char *ENGINE_PTR_REGKEY = "luna.engine_ptr";
static constexpr const char *CO_TASK_MAP_REGKEY = "luna.co_task_map";
static constexpr const char *LUA_EVENT_MT = "luna.Event";
static constexpr const char *LUA_PROMISE_MT = "luna.Promise";

Engine::PromiseState::~PromiseState() {
  if (!owner) {
    return;
  }
  for (int ref : result_refs) {
    luaL_unref(owner, LUA_REGISTRYINDEX, ref);
  }
  result_refs.clear();
}

Engine::Engine() = default;

bool Engine::Init() {
  factory_.Start();

  if (!renderer_.Init()) {
    return false;
  }
  if (!mixer_.Init()) {
    return false;
  }

  if (!InitLua()) {
    return false;
  }

  return true;
}

Engine::~Engine() {
  pending_promises_.clear();
  timers_.clear();

  if (L_) {
    mixer_.Fini(L_);
    lua_close(L_);
    L_ = nullptr;
  } else {
    mixer_.Fini();
  }

  renderer_.Fini();
}

bool Engine::InitLua() {
  L_ = luaL_newstate();
  luaL_openlibs(L_);

  RegisterLuaTypes(L_);
  RegisterBindings(L_);

  lua_getfield(L_, LUA_REGISTRYINDEX, "_PRELOAD");
  lua_pushcfunction(L_, [](lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, LIBRARY_REGKEY);
    return 1;
  });
  lua_setfield(L_, -2, "luna");
  lua_pop(L_, -1);

  return true;
}

Engine *Engine::GetEngine(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, ENGINE_PTR_REGKEY);
  auto *e = static_cast<Engine *>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return e;
}

void Engine::RegisterBindings(lua_State *L) {
  lua_pushlightuserdata(L_, this);
  lua_setfield(L_, LUA_REGISTRYINDEX, ENGINE_PTR_REGKEY);

  lua_newtable(L);

  lua_pushcfunction(L, &L_Start);
  lua_setfield(L, -2, "start");

  lua_pushcfunction(L, &L_NextFrame);
  lua_setfield(L, -2, "next_frame");

  lua_pushcfunction(L, &L_Event);
  lua_setfield(L, -2, "event");

  lua_pushcfunction(L, &L_Wait);
  lua_setfield(L, -2, "wait");

  lua_pushcfunction(L, &L_Now);
  lua_setfield(L, -2, "now");

  lua_pushcfunction(L, &L_After);
  lua_setfield(L, -2, "after");

  lua_pushcfunction(L, &L_SetFrameTime);
  lua_setfield(L, -2, "set_frame_time");

  lua_pushcfunction(L, &L_StartAsyncJob<Renderer::LoadImageJob>);
  lua_setfield(L, -2, "load_image");

  lua_pushcfunction(L, &L_StartAsyncJob<Renderer::LoadFontJob>);
  lua_setfield(L, -2, "load_font");

  lua_pushcfunction(L, &L_StartAsyncJob<Mixer::LoadAudioJob>);
  lua_setfield(L, -2, "load_audio");

  renderer_.RegisterBindings(L);
  mixer_.RegisterBindings(L);
  vfs_.RegisterBindings(L_);

  lua_setfield(L, LUA_REGISTRYINDEX, LIBRARY_REGKEY);
}

void Engine::RegisterLuaTypes(lua_State *L) {
  if (luaL_newmetatable(L, LUA_EVENT_MT)) {
    lua_pushcfunction(L, &Engine::L_EventGc);
    lua_setfield(L, -2, "__gc");

    lua_newtable(L);
    lua_pushcfunction(L, &Engine::L_EventSignal);
    lua_setfield(L, -2, "signal");
    lua_setfield(L, -2, "__index");
  }
  lua_pop(L, 1);

  if (luaL_newmetatable(L, LUA_PROMISE_MT)) {
    lua_pushcfunction(L, &Engine::L_PromiseGc);
    lua_setfield(L, -2, "__gc");

    lua_pushcfunction(L, &Engine::L_PromiseIndex);
    lua_setfield(L, -2, "__index");
  }
  lua_pop(L, 1);
}

bool Engine::CallLuaMain(const std::string &entry_path) {
  if (luaL_dofile(L_, entry_path.c_str()) != 0) {
    SDL_Log("Lua error: %s", lua_tostring(L_, -1));
    lua_pop(L_, 1);
    return false;
  }
  return true;
}

void Engine::Run(const std::string &entry_path) {
  if (!CallLuaMain(entry_path))
    return;

  auto last = std::chrono::steady_clock::now();

  while (alive_task_count_ > 0) {
    renderer_.PumpSdlEvents();
    DrainPromiseCompletions();

    SignalExpiredTimers();
    Tick();

    auto script_time = std::chrono::steady_clock::now() - last;
    if (script_time < frame_time_) {
      std::this_thread::sleep_for(frame_time_ - script_time);
    }

    auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = now - last;
    AdvanceTime(elapsed.count());
    last = now;
  }
}

void Engine::DrainPromiseCompletions() {
  for (auto &[promise_id, job] : factory_.DrainCompletions()) {
    auto it = pending_promises_.find(promise_id);
    if (it == pending_promises_.end()) {
      continue;
    }

    std::shared_ptr<PromiseState> promise = std::move(it->second);
    pending_promises_.erase(it);

    if (job->Rejected()) {
      promise->settlement = PromiseState::Settlement::REJECTED;
      promise->error = job->GetError();
    } else {
      promise->settlement = PromiseState::Settlement::FULFILLED;
      promise->completed_job = std::move(job);
    }

    SignalEvent(promise->event);
  }
}

void Engine::AdvanceTime(double dt) { now_seconds_ += dt; }

void Engine::SignalExpiredTimers() {
  auto it = timers_.begin();
  while (it != timers_.end()) {
    if (now_seconds_ >= it->deadline_seconds) {
      SignalEvent(it->event);
      it = timers_.erase(it);
    } else {
      ++it;
    }
  }
}

void Engine::Tick() {
  for (Task *t : next_frame_tasks_) {
    tasks_.push_back(t);
  }
  next_frame_tasks_.clear();

  while (!tasks_.empty()) {
    Task *t = tasks_.front();
    tasks_.pop_front();

    int const status = ResumeTask(t);
    if (status == LUA_OK) {
      RemoveTask(t);
      continue;
    }

    if (status != LUA_YIELD) {
      // No handler, must panic
      const char *msg = lua_tostring(t->co, -1);
      luaL_traceback(t->co, t->co, msg ? msg : "(unknown)", 1);
      const char *trace = lua_tostring(t->co, -1);
      SDL_Log("Coroutine error:\n%s", trace ? trace : "(unknown)");
      lua_pop(t->co, 2);
      RemoveTask(t);
      alive_task_count_ = 0;
      break;
    }

    switch (t->wait.kind) {
    case WaitKind::NEXT_FRAME:
      next_frame_tasks_.push_back(t);
      break;
    case WaitKind::EVENTS:
      break;
    default:
      break;
    }
  }
}

void Engine::EnsureCoroutineTaskMap(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, CO_TASK_MAP_REGKEY);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);

    lua_newtable(L);

    lua_newtable(L);
    lua_pushliteral(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);

    lua_setfield(L, LUA_REGISTRYINDEX, CO_TASK_MAP_REGKEY);
  } else {
    lua_pop(L, 1);
  }
}

Task *Engine::GetCurrentTask(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, CO_TASK_MAP_REGKEY);
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return nullptr;
  }

  lua_pushthread(L);
  lua_gettable(L, -2);
  Task *t = nullptr;
  if (lua_islightuserdata(L, -1)) {
    t = static_cast<Task *>(lua_touserdata(L, -1));
  }
  lua_pop(L, 2);

  return t;
}

void Engine::RemoveTask(Task *t) {
  if (!t)
    return;
  UnlinkAllEvents(t);
  if (t->co_ref != LUA_NOREF) {
    luaL_unref(L_, LUA_REGISTRYINDEX, t->co_ref);
    t->co_ref = LUA_NOREF;
    t->co = nullptr;
  }
  --alive_task_count_;
  delete t;
}

void Engine::UnlinkWaitNode(WaitLink *node) {
  // Remove from event's waiter list
  if (node->prev_task) {
    node->prev_task->next_task = node->next_task;
  } else if (node->event) {
    node->event->waiters_head = node->next_task;
  }
  if (node->next_task) {
    node->next_task->prev_task = node->prev_task;
  }

  // Remove from task's wait-links list
  if (node->prev_event) {
    node->prev_event->next_event = node->next_event;
  } else if (node->task) {
    node->task->wait.links_head = node->next_event;
  }
  if (node->next_event) {
    node->next_event->prev_event = node->prev_event;
  }

  delete node;
}

void Engine::UnlinkAllEvents(Task *t) {
  WaitLink *link = t->wait.links_head;
  while (link) {
    WaitLink *next = link->next_event;

    // Remove from event's waiter list
    if (link->prev_task) {
      link->prev_task->next_task = link->next_task;
    } else if (link->event) {
      link->event->waiters_head = link->next_task;
    }
    if (link->next_task) {
      link->next_task->prev_task = link->prev_task;
    }

    delete link;
    link = next;
  }
  t->wait.links_head = nullptr;
}

void Engine::LinkEvent(Task *t, EventState *e, int index) {
  auto *node = new WaitLink{};
  node->task = t;
  node->event = e;
  node->index = index; // 1-based Lua index

  // Insert at head of event's waiter list
  node->next_task = e->waiters_head;
  if (e->waiters_head) {
    e->waiters_head->prev_task = node;
  }
  e->waiters_head = node;

  // Insert at head of task's wait-links list
  node->next_event = t->wait.links_head;
  if (t->wait.links_head) {
    t->wait.links_head->prev_event = node;
  }
  t->wait.links_head = node;
}

std::shared_ptr<EventState> Engine::CreateEvent() const {
  return std::make_shared<EventState>();
}

void Engine::SignalEvent(const std::shared_ptr<EventState> &event) {
  if (!event)
    return;
  WaitLink *link = event->waiters_head;
  while (link) {
    WaitLink *next = link->next_task;
    Task *t = link->task;
    t->wait.ready_index = link->index;
    UnlinkAllEvents(t);
    tasks_.push_back(t);
    link = next;
  }
}

int Engine::PushEventObject(lua_State *L,
                            const std::shared_ptr<EventState> &event) {
  auto *ud = static_cast<LuaEvent *>(lua_newuserdata(L, sizeof(LuaEvent)));
  new (ud) LuaEvent{event};
  luaL_getmetatable(L, LUA_EVENT_MT);
  lua_setmetatable(L, -2);
  return 1;
}

int Engine::PushPromiseObject(lua_State *L,
                              const std::shared_ptr<PromiseState> &promise) {
  auto *ud = static_cast<LuaPromise *>(lua_newuserdata(L, sizeof(LuaPromise)));
  new (ud) LuaPromise{promise};
  luaL_getmetatable(L, LUA_PROMISE_MT);
  lua_setmetatable(L, -2);
  return 1;
}

Engine::LuaEvent *Engine::CheckEvent(lua_State *L, int idx) {
  return static_cast<LuaEvent *>(luaL_checkudata(L, idx, LUA_EVENT_MT));
}

Engine::LuaPromise *Engine::CheckPromise(lua_State *L, int idx) {
  return static_cast<LuaPromise *>(luaL_checkudata(L, idx, LUA_PROMISE_MT));
}

int Engine::L_Start(lua_State *L) {
  Engine *e = GetEngine(L);
  luaL_checktype(L, 1, LUA_TFUNCTION);

  lua_State *co = lua_newthread(L);
  const int co_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  Task *t = new Task{};
  t->co = co;
  t->co_ref = co_ref;

  lua_pushvalue(L, 1);
  lua_xmove(L, co, 1);

  EnsureCoroutineTaskMap(L);
  lua_getfield(L, LUA_REGISTRYINDEX, CO_TASK_MAP_REGKEY);
  lua_rawgeti(L, LUA_REGISTRYINDEX, co_ref);
  lua_pushlightuserdata(L, t);
  lua_settable(L, -3);
  lua_pop(L, 1);

  e->tasks_.push_back(t);
  ++e->alive_task_count_;

  return 0;
}

int Engine::L_NextFrame(lua_State *L) {
  Task *t = GetCurrentTask(L);
  if (!t) {
    return luaL_error(L, "Yield can only be called from coroutine");
  }
  t->wait = {WaitKind::NEXT_FRAME};
  return lua_yield(L, 0);
}

int Engine::L_Event(lua_State *L) {
  Engine *e = GetEngine(L);
  return PushEventObject(L, e->CreateEvent());
}

int Engine::L_Wait(lua_State *L) {
  const int arg_count = lua_gettop(L);
  if (arg_count <= 0) {
    return luaL_error(L, "wait: expected at least one event");
  }

  Task *t = GetCurrentTask(L);
  if (!t) {
    return luaL_error(L, "Yield can only be called from coroutine");
  }
  if (t->wait.links_head) {
    return luaL_error(L, "internal error: task wait event list not cleared");
  }

  t->wait = {WaitKind::EVENTS};

  for (int i = 1; i <= arg_count; ++i) {
    LuaEvent *event_ud = CheckEvent(L, i);
    EventState *event = event_ud->state.get();

    LinkEvent(t, event, i);
  }

  return lua_yield(L, 0);
}

int Engine::L_Now(lua_State *L) {
  Engine *e = GetEngine(L);
  lua_pushnumber(L, e->now_seconds_);
  return 1;
}

int Engine::L_After(lua_State *L) {
  Engine *e = GetEngine(L);
  const double seconds = luaL_checknumber(L, 1);
  if (seconds < 0.0) {
    return luaL_error(L, "after: seconds must be >= 0");
  }

  const double deadline = e->now_seconds_ + seconds;
  auto event = e->CreateEvent();
  e->timers_.push_back(TimerEntry{deadline, event});

  lua_pushnumber(L, deadline);
  PushEventObject(L, event);
  return 2;
}

int Engine::ResumeTask(Task *t) {
  int resume_args = 0;
  switch (t->wait.kind) {
  case WaitKind::NONE:
    break;
  case WaitKind::NEXT_FRAME:
    break;
  case WaitKind::EVENTS:
    lua_pushinteger(t->co, t->wait.ready_index);
    resume_args = 1;
    break;
  }
  t->wait = WaitState{};
  return lua_resume(t->co, resume_args);
}

int Engine::L_SetFrameTime(lua_State *L) {
  Engine *e = GetEngine(L);
  double frame_time = luaL_checknumber(L, 1);
  if (frame_time < 0.0) {
    return luaL_error(L, "set_frame_time: frame time must be >= 0");
  }
  if (frame_time > 1.0) {
    return luaL_error(L, "set_frame_time: frame time must be <= 1.0");
  }
  e->frame_time_ = std::chrono::duration<double>(frame_time);
  return 0;
}

int Engine::L_EventSignal(lua_State *L) {
  Engine *e = GetEngine(L);
  LuaEvent *event = CheckEvent(L, 1);
  e->SignalEvent(event->state);
  return 0;
}

int Engine::L_EventGc(lua_State *L) {
  auto *event = static_cast<LuaEvent *>(luaL_checkudata(L, 1, LUA_EVENT_MT));
  event->~LuaEvent();
  return 0;
}

int Engine::L_PromisePoll(lua_State *L) {
  LuaPromise *promise_ud = CheckPromise(L, 1);
  std::shared_ptr<PromiseState> &promise = promise_ud->state;

  if (!promise || promise->settlement == PromiseState::Settlement::PENDING) {
    lua_pushboolean(L, 0);
    return 1;
  }

  lua_pushboolean(L, 1);

  if (promise->settlement == PromiseState::Settlement::REJECTED) {
    lua_pushboolean(L, 0);
  } else {
    lua_pushboolean(L, 1);
  }
  return 2;
}

int Engine::L_PromiseTake(lua_State *L) {
  LuaPromise *promise_ud = CheckPromise(L, 1);
  std::shared_ptr<PromiseState> &promise = promise_ud->state;

  if (promise->settlement == PromiseState::Settlement::PENDING) {
    return luaL_error(L, "promise.take: promise is not settled");
  }

  if (promise->settlement == PromiseState::Settlement::REJECTED) {
    return luaL_error(L, "promise rejected: %s", promise->error.c_str());
  }

  int produced = promise->completed_job->Finish(L);
  promise->completed_job.reset();

  return produced;
}

int Engine::L_PromiseIndex(lua_State *L) {
  LuaPromise *promise = CheckPromise(L, 1);
  const char *key = luaL_checkstring(L, 2);

  if (std::strcmp(key, "poll") == 0) {
    lua_pushcfunction(L, &Engine::L_PromisePoll);
    return 1;
  }

  if (std::strcmp(key, "take") == 0) {
    lua_pushcfunction(L, &Engine::L_PromiseTake);
    return 1;
  }

  if (std::strcmp(key, "event") == 0) {
    return PushEventObject(L, promise->state ? promise->state->event : nullptr);
  }

  lua_pushnil(L);
  return 1;
}

int Engine::L_PromiseGc(lua_State *L) {
  auto *promise =
      static_cast<LuaPromise *>(luaL_checkudata(L, 1, LUA_PROMISE_MT));
  promise->~LuaPromise();
  return 0;
}

} // namespace luna
