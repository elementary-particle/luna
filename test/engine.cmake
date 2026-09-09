set(root "${CMAKE_CURRENT_BINARY_DIR}/engine-regression")
file(MAKE_DIRECTORY "${root}")
file(REMOVE "${root}/game.json")
file(COPY "${LUNA_FONT}" DESTINATION "${root}")

function(check_script name expected script)
  file(WRITE "${root}/${name}.lua" "${script}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
      SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy LUNA_BACKEND=blend2d
      "XDG_DATA_HOME=${root}/data"
      "${LUNA_APP}" --root "${root}" --script "${name}.lua"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    TIMEOUT 15)
  if(NOT "${result}" STREQUAL "${expected}")
    message(FATAL_ERROR "${name}: expected exit ${expected}, got ${result}\n${output}\n${error}")
  endif()
endfunction()

check_script(svg_paths 0 [=[
local l = require('luna')
l.start(function()
  local g = l.window
  local p = g:path({svg='M10 10h30v30h-30z', fill_type='even_odd'})
  assert(g:hit_test_path(p, 20, 20))
  assert(not g:hit_test_path(p, 50, 20))
  assert(not pcall(g.path, g, 'M0 0 A1 2 0 9 0 3 4'))
  local a = g:path('M50 50 a30 10 45 0 1 40 40z')
  g:draw_path(a, {shader=0xffffffff})
  l.next_frame()
end)
]=])

check_script(duplicate_event 0 [=[
local l = require('luna')
local e, other = l.make_event(), l.make_event()
local resumed = 0
l.start(function()
  local index = l.wait(e, e, other)
  assert(index == 1 or index == 2)
  resumed = resumed + 1
end)
l.start(function()
  l.wait(e)
  resumed = resumed + 1
end)
l.start(function()
  e:signal()
  other:signal()
  l.next_frame()
  assert(resumed == 2)
end)
]=])

check_script(double_take 0 [=[
local l = require('luna')
l.start(function()
  local p = l.assets:font(l.fs.game:ref('ABeeZee-Regular.ttf'))
  while not p:poll() do l.next_frame() end
  assert(p:result())
  assert(p:result())
end)
]=])

check_script(coroutine_error 1 [=[
local l = require('luna')
l.start(function() error('expected regression-test error') end)
]=])

check_script(filesystem_futures 0 [=[
local l = require('luna')
assert(l.load_image == nil and l.load_audio == nil and l.load_fontface == nil and l.save == nil)
l.start(function()
  local fs = l.fs.user
  local p = fs:write('slot.json', assert(l.json.encode({chapter=3})))
  assert(not pcall(p.result, p))
  assert(l.await(p) == true)
  assert(p:result() == true)
  local read = fs:read('slot.json')
  local blob = assert(l.await(read))
  assert(read:result() == blob)
  assert(l.json.decode(blob).chapter == 3)
  local stream = assert(l.await(fs:open('slot.json')))
  local a = stream:read(1)
  assert(not pcall(stream.read, stream, 2))
  assert(l.await(a):string() == '{')
  assert(l.await(stream:seek('set', 0)) == 0)
  assert(l.await(stream:read(100)):string() == blob:string())
  assert(l.await(stream:read(1)):size() == 0)
  assert(l.await(stream:close()))
  local value, err = l.await(stream:read(1))
  assert(value == nil and err.code == 'io_error')
  value, err = l.await(fs:read('not-present'))
  assert(value == nil and err.code == 'not_found' and type(err.operation) == 'string')
  value, err = l.await(l.assets:image(l.fs.game:ref('not-present')))
  assert(value == nil and err.code == 'not_found')
  value, err = l.await(l.assets:image(blob))
  assert(value == nil and err.code == 'format_error')
  value, err = l.await(l.assets:audio(blob, {mode='stream'}))
  assert(value == nil and err.code == 'format_error')
  assert(not pcall(l.assets.image, l.assets, 'host-path'))
  assert(l.await(fs:remove('slot.json')))
  local font_blob = assert(l.await(l.fs.game:read('ABeeZee-Regular.ttf')))
  assert(l.await(l.assets:font(font_blob)))
end)
]=])

check_script(shared_future 0 [=[
local l = require('luna')
local p = l.fs.game:read('ABeeZee-Regular.ttf')
local first, second
l.start(function() first = assert(l.await(p)) end)
l.start(function() second = assert(l.await(p)) end)
l.start(function()
  while not first or not second do l.next_frame() end
  assert(first == second)
  collectgarbage()
  assert(p:result() == first)
end)
]=])

check_script(audio_lifetime 0 [=[
local l = require('luna')
local function u32(n)
  return string.char(n % 256, math.floor(n/256) % 256, math.floor(n/65536) % 256, math.floor(n/16777216) % 256)
end
local pcm = string.rep(string.char(128), 800)
local wav = 'RIFF' .. u32(36 + #pcm) .. 'WAVEfmt ' .. u32(16)
  .. string.char(1,0,1,0) .. u32(8000) .. u32(8000) .. string.char(1,0,8,0)
  .. 'data' .. u32(#pcm) .. pcm
l.start(function()
  local fs = l.fs.user
  assert(l.await(fs:write('sound.wav', wav)))
  local streamed = assert(l.await(l.assets:audio(fs:ref('sound.wav'), {mode='stream'})))
  -- A loaded stream pins its opened file, not its mutable path.
  assert(l.await(fs:write('sound.wav', 'no longer an audio file')))
  local a, b = l.audio.track_create(), l.audio.track_create()
  l.audio.track_set(a, streamed)
  l.audio.track_set(b, streamed)
  streamed = nil
  collectgarbage()
  l.audio.track_play(a)
  l.audio.track_play(b)
  assert(l.audio.track_playing(a) and l.audio.track_playing(b))
  local _, done = l.after(0.2)
  l.wait(done)
  assert(not l.audio.track_playing(a) and not l.audio.track_playing(b))
  l.audio.track_destroy(a)
  l.audio.track_destroy(b)
  collectgarbage()
  assert(l.await(fs:write('sound.wav', wav)))
  local bytes = assert(l.await(fs:read('sound.wav')))
  for _, mode in ipairs({'memory', 'decode', 'stream'}) do
    local sound = assert(l.await(l.assets:audio(bytes, {mode=mode})))
    local track = l.audio.track_create()
    l.audio.track_set(track, sound)
    sound:destroy() -- pinned by track until detached
    l.audio.track_play(track)
    l.audio.track_destroy(track)
  end
  assert(l.await(fs:remove('sound.wav')))
  -- Shutdown must release completed-but-unclaimed resources before mixer teardown.
  l.assets:audio(bytes)
  l.assets:audio(bytes, {mode='stream'})
end)
]=])

file(MAKE_DIRECTORY "${root}/content/modules")
file(WRITE "${root}/content/modules/test.lua" "return {marker = 'vfs-mounted'}")
file(WRITE "${root}/game.json" [=[
{"id":"luna-manifest-test","entry":"manifest_entry.lua","mounts":[
  {"type":"directory","path":"content/modules","prefix":"modules","priority":10}
]}
]=])
check_script(manifest_entry 0 [=[
local l = require('luna')
assert(require('modules.test').marker == 'vfs-mounted')
assert(not pcall(require, 'engine-regression.content.modules.test'))
l.start(function()
  local info = assert(l.await(l.fs.game:stat('modules/test.lua')))
  assert(info.kind == 'file')
  local entries = assert(l.await(l.fs.game:list('modules')))
  assert(#entries == 1 and entries[1].path == 'modules/test.lua')
  local read = assert(l.await(l.fs.game:read('modules/test.lua')))
  assert(read:string():find('vfs%-mounted'))
  assert(l.await(l.fs.user:write('identity', 'manifest')))
end)
]=])
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
    LUNA_BACKEND=blend2d "XDG_DATA_HOME=${root}/data"
    "${LUNA_APP}" --root "${root}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "manifest default entry failed: ${output}\n${error}")
endif()
if(NOT EXISTS "${root}/data/luna/luna-manifest-test/user/identity")
  message(FATAL_ERROR "manifest game id did not isolate user storage")
endif()
file(REMOVE "${root}/game.json")
