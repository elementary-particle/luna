#!/usr/bin/env python3
"""Opt-in real-content archive regression. Requires built Luna and liblz4.

Creates isolated distributions and logs under build; never edits the game tree.
Run from any directory: python tools/test_parfait_archive.py --game ../parfait_jp
"""
import argparse
import ctypes
import ctypes.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time

ASSETS = ('bgm', 'face', 'fonts', 'se', 'stand', 'visual', 'voice')
CORE_DIRS = ('game', 'scenarios', 'scripts', 'tests')
CORE_FILES = ('game.json', 'main.lua', 'legacy_main.lua', 'parfait_common.lua')
SMOKES = (
    ('tests/runtime.lua', 'Runtime tests passed: 161 scenes, 190 banks'),
    ('tests/luna_smoke.lua', None),
    ('tests/luna_blocking_smoke.lua', 'Real-engine blocking smoke passed'),
    ('tests/luna_visuals_smoke.lua', 'Real-engine visual composition/animation smoke passed'),
    ('tests/luna_dialogue_smoke.lua', 'Real-engine dialogue layout smoke passed'),
    ('tests/archive_access.lua', 'Archive access smoke passed'),
)


def stage_file(source, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, dest)
    except OSError:
        shutil.copyfile(source, dest)


def run(command, log, timeout=300, env=None, success=True, marker=None):
    start = time.monotonic()
    with log.open('w') as output:
        result = subprocess.run([str(v) for v in command], stdout=output,
                                stderr=subprocess.STDOUT, env=env, timeout=timeout)
    text = log.read_text(errors='replace')
    elapsed = time.monotonic() - start
    if (result.returncode == 0) != success or (marker and marker not in text):
        raise RuntimeError(f'{log.name}: exit {result.returncode}\n{text[-6000:]}')
    if success and ('[ERROR]' in text or 'stack traceback:' in text):
        raise RuntimeError(f'{log.name}: runtime error despite zero exit\n{text[-6000:]}')
    print(f'{log.stem}: {elapsed:.2f}s, exit {result.returncode}', flush=True)
    return {'seconds': round(elapsed, 3), 'exit': result.returncode, 'log': str(log)}


def compare_archive(archive, source, decode):
    """Independent wire parser + liblz4; compare every decoded byte to input."""
    start = time.monotonic()
    files = raw_bytes = stored_bytes = compressed = stored = 0
    expected_paths = {p.relative_to(source).as_posix() for p in source.rglob('*') if p.is_file()}
    seen = set()
    with archive.open('rb') as f:
        f.seek(-48, 2)
        magic, index_offset, index_size, total, count, _, _, _ = struct.unpack('<8sQQQIIII', f.read(48))
        assert magic == b'LUNAEND2' and total == archive.stat().st_size
        f.seek(index_offset)
        index = memoryview(f.read(index_size))
        pos = 0
        for _ in range(count):
            length, chunks, size = struct.unpack_from('<IIQ', index, pos)
            pos += 16
            name = bytes(index[pos:pos + length]).decode('utf-8')
            assert name not in seen, f'Duplicate path: {name}'
            seen.add(name)
            pos += length
            decoded_size = 0
            with (source / name).open('rb') as original:
                for _ in range(chunks):
                    offset, packed, raw, codec, _, _, reserved = struct.unpack_from('<QIIIIII', index, pos)
                    pos += 32
                    assert reserved == 0 and codec in (0, 1)
                    f.seek(offset)
                    payload = f.read(packed)
                    if codec:
                        buffer = ctypes.create_string_buffer(raw)
                        assert decode(payload, buffer, packed, raw) == raw
                        payload = buffer.raw
                        compressed += 1
                    else:
                        stored += 1
                    assert payload == original.read(raw), f'Byte mismatch: {archive.name}:{name}'
                    raw_bytes += raw
                    stored_bytes += packed
                    decoded_size += raw
                assert decoded_size == size and original.read(1) == b'', name
            files += 1
        assert pos == len(index)
    assert seen == expected_paths, f'File inventory mismatch in {archive.name}'
    result = dict(files=files, raw_bytes=raw_bytes, payload_bytes=stored_bytes,
                  archive_bytes=archive.stat().st_size, compressed_chunks=compressed,
                  stored_chunks=stored, compare_seconds=round(time.monotonic() - start, 3))
    print(f'{archive.name}: byte comparison passed for {files} files', flush=True)
    return result


def access_script(source):
    # Several unaligned positions, a chunk boundary and the tail of real assets.
    lines = ["local luna = require('luna')", "luna.start(function()"]
    for group in ASSETS:
        asset = max((p for p in (source / group).iterdir() if p.is_file()), key=lambda p: p.stat().st_size)
        path = asset.relative_to(source).as_posix()
        size = asset.stat().st_size
        lines.append(f"do local s = assert(luna.await(luna.fs.game:open({json.dumps(path, ensure_ascii=False)})))")
        with asset.open('rb') as f:
            for offset in sorted({0, min(262144 - 11, max(0, size - 32)), max(0, size - 32), size // 2}):
                f.seek(offset)
                expected = ''.join(f'\\{b:03d}' for b in f.read(32))
                lines.extend((f"assert(luna.await(s:seek('set', {offset})) == {offset})",
                              f"assert(luna.await(s:read(32)):string() == '{expected}')"))
        lines.extend((f"assert(luna.await(s:seek('end', 0)) == {size})",
                      "assert(luna.await(s:read(1)):size() == 0)", "assert(luna.await(s:close())) end"))
    bgm = next(p for p in (source / 'bgm').iterdir() if p.suffix.lower() in ('.ogg', '.mp3', '.wav'))
    voice = next(p for p in (source / 'voice').iterdir() if p.suffix.lower() in ('.ogg', '.mp3', '.wav'))
    for asset in (bgm, voice):
        name = json.dumps(asset.relative_to(source).as_posix(), ensure_ascii=False)
        lines.extend((
            f"do local sound = assert(luna.await(luna.assets:audio(luna.fs.game:ref({name}), {{mode='stream'}})))",
            "local a, b = luna.audio.track_create(), luna.audio.track_create()",
            "luna.audio.track_set(a, sound); luna.audio.track_set(b, sound)",
            "sound = nil; collectgarbage()",
            "luna.audio.track_play(a); luna.audio.track_play(b)",
            "assert(luna.audio.track_playing(a) and luna.audio.track_playing(b))",
            "local _, done = luna.after(0.15); luna.wait(done)",
            "luna.audio.track_stop(a); luna.audio.track_stop(b)",
            "luna.audio.track_destroy(a); luna.audio.track_destroy(b) end",
        ))
    lines.extend(("print('Archive access smoke passed')", 'end)'))
    return '\n'.join(lines) + '\n'


def main():
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, default=repo.parent / 'parfait_jp')
    parser.add_argument('--build', type=Path, default=repo / 'build')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--reuse-assets', type=Path, help='Reuse split asset packages from an earlier output; still verify and byte-compare them')
    args = parser.parse_args()
    game, build = args.game.resolve(), args.build.resolve()
    output = (args.output or build / ('parfait-archive-' + time.strftime('%Y%m%d-%H%M%S'))).resolve()
    output.mkdir(parents=True, exist_ok=False)
    stage, core, multi, logs = (output / n for n in ('content', 'core', 'multi', 'logs'))
    for directory in (stage, core, multi, logs):
        directory.mkdir()
    lib = ctypes.CDLL(ctypes.util.find_library('lz4') or 'lz4.dll')
    decode = lib.LZ4_decompress_safe
    decode.argtypes = (ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int)
    decode.restype = ctypes.c_int
    suffix = '.exe' if os.name == 'nt' else ''
    pack, engine = build / ('luna_pack' + suffix), build / ('luna' + suffix)
    for name in CORE_FILES:
        stage_file(game / name, stage / name)
    for name in ASSETS + CORE_DIRS:
        for source in sorted((game / name).rglob('*')):
            if source.is_symlink():
                raise RuntimeError(f'Unexpected content symlink: {source}')
            if source.is_file() and (name in ASSETS or source.suffix == '.lua'):
                stage_file(source, stage / source.relative_to(game))
    (stage / 'tests/archive_access.lua').write_text(access_script(stage), encoding='utf-8')
    for source in stage.rglob('*'):
        if source.is_file() and source.relative_to(stage).parts[0] not in ASSETS:
            stage_file(source, core / source.relative_to(stage))
    # The core manifest is only read by a single-file root. The loose multi-file
    # manifest below controls mounts for the split distribution.
    report = {'game': str(game), 'output': str(output), 'packages': {}, 'runtime': {}}
    def save_report():
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    def package(name, source, dest):
        if args.reuse_assets and name in ASSETS:
            stage_file(args.reuse_assets.resolve() / 'multi' / f'{name}.luna', dest)
            entry = {'reused_from': str(args.reuse_assets.resolve())}
        else:
            entry = run([pack, 'create', source, dest], logs / f'create-{name}.log')
        entry['verify'] = run([pack, 'verify', dest], logs / f'verify-{name}.log', marker='Package verified')
        entry.update(compare_archive(dest, source, decode))
        report['packages'][name] = entry
        save_report()
    # Validate the generated Lua (including UTF-8 paths) before expensive packing.
    preflight_env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                         LUNA_BACKEND='blend2d', XDG_DATA_HOME=str(output / 'userdata/preflight'))
    run([engine, '--root', stage, '--script', 'tests/archive_access.lua'],
        logs / 'preflight.log', env=preflight_env, timeout=30, marker='Archive access smoke passed')
    single = output / 'parfait.luna'
    package('single', stage, single)
    package('scripts', core, multi / 'scripts.luna')
    mounts = [{'type': 'package', 'path': 'scripts.luna', 'prefix': '/'}]
    for group in ASSETS:
        package(group, stage / group, multi / f'{group}.luna')
        mounts.append({'type': 'package', 'path': f'{group}.luna', 'prefix': group})
    (multi / 'game.json').write_text(json.dumps({'id': 'parfait-jp', 'entry': 'main.lua', 'mounts': mounts}, indent=2) + '\n')
    base_env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
                    LUNA_BACKEND='blend2d', XDG_DATA_HOME=str(output / 'userdata/rejection'))
    for key in ('PARFAIT_START_SCENE', 'PARFAIT_START_BANK'):
        base_env.pop(key, None)
    for mode, root in (('loose', stage), ('single', single), ('multi', multi)):
        report['runtime'][mode] = {}
        for script, marker in SMOKES:
            name = Path(script).stem
            env = dict(base_env, XDG_DATA_HOME=str(output / 'userdata' / mode))
            report['runtime'][mode][name] = run([engine, '--root', root, '--script', script],
                logs / f'{mode}-{name}.log', timeout=90, env=env, marker=marker)
            save_report()
    # Corrupt a disposable copy of the small scripts archive, not the artifacts.
    damaged = output / 'damaged.luna'
    original = (multi / 'scripts.luna').read_bytes()
    index_offset = struct.unpack_from('<Q', original, len(original) - 40)[0]
    name_size, chunks, _ = struct.unpack_from('<IIQ', original, index_offset)
    assert chunks > 0
    payload_offset = struct.unpack_from('<Q', original, index_offset + 16 + name_size)[0]
    for kind, data in (('payload', original[:payload_offset] + bytes([original[payload_offset] ^ 1]) + original[payload_offset + 1:]),
                       ('truncated', original[:-1]), ('trailing', original + b'junk')):
        damaged.write_bytes(data)
        run([pack, 'verify', damaged], logs / f'reject-{kind}.log', success=False)
    damaged.unlink()
    missing = multi / 'fonts.luna'
    missing.rename(multi / 'fonts.hidden')
    try:
        run([engine, '--root', multi, '--script', 'tests/archive_access.lua'],
            logs / 'reject-missing-package.log', env=base_env, success=False, timeout=20)
    finally:
        (multi / 'fonts.hidden').rename(missing)
    report['result'] = 'passed'
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'All archive checks passed. Report: {output / "report.json"}', flush=True)


if __name__ == '__main__':
    main()
