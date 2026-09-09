# Parfait archive validation — 2026-09-09

Revision 2 passed on Linux using the real Blend2D engine and SDL dummy drivers.
The verified single archive is `../parfait_jp/parfait.luna`. Game source content
was not changed. Test distributions include the Lua smoke scripts.

```sh
./build/luna --root ../parfait_jp/parfait.luna
```

## Correctness

- 29,414 files; 2,115,950,141 source bytes → 2,090,811,663 archive bytes.
- Both single and eight-package distributions passed full verification,
  independent decoding, byte comparison and complete path-inventory comparison.
- Eighteen engine runs passed: six each against loose, single and split content.
  Coverage includes 161 scenario modules, 190 banks with two state seeds,
  frontend startup, blocking sound effects, visual composition/animation,
  dialogue layout, chunk-boundary seeks, Japanese filenames, and simultaneous
  streamed BGM/voice playback with independent track cursors.
- Payload corruption, truncation, trailing data and a missing package all failed
  as expected.
- Unit tests cover direct mappings outliving their source, owned/stream fallback,
  independent concurrent streams, explicit re-verification after cached success,
  independent CRC reference checks, every single-bit mutation of a small archive,
  every truncation point and malformed metadata with repaired checksums.
  AddressSanitizer/UndefinedBehaviorSanitizer checks and engine regressions pass.

Reports, split packages and logs are in `build/parfait-archive-20260909-200939/`.
Revision 1 reports remain in the earlier test directory for history; its obsolete
split packages have been removed. The first harness attempt emitted JSON Unicode
escapes into Lua. That generator error was fixed before the successful runs.

## Stored-asset access

Revision 2 keeps stored files contiguous, outside their checksum tables, and
returns direct native mappings. It caches successful integrity checks for the
immutable mount. Compression now requires at least 1% whole-file savings.
9,988 files (1,606,182,424 bytes, about 76% of input bytes) are stored and directly
mappable. This adds only 3,460,615 bytes compared with the first layout.

Measured on the 23,939,708-byte stored file `bgm/哀しみ.wav`, using the same
Clang `-O3` build for native and archive code. Numbers below are medians across
five runs. Each run performs 10,000 warm random reads and 50 warm mappings.

| Operation | Native FS | Revision 2 archive |
| --- | ---: | ---: |
| First 4 KiB read, OS cache uncontrolled | 6.42 µs | 183.59 µs |
| Warm random 4 KiB read | 0.732 µs | 0.263 µs |
| Warm whole-file mapping | 9.87 µs | 0.919 µs |

Warm phases verify all archive chunks before timing. First access includes a
256 KiB integrity check for the archive; native FS has no equivalent check.
Archive mounting takes roughly 23–33 ms in these runs and is excluded above.
Mapping timings measure obtaining a view, not consuming every byte. The native
comparison uses FileSource directly, without the upper VFS cache. These are
local microbenchmarks, not universal latency promises or cold-storage tests.
The initial layout measured about 1,648 µs per warm random read and 133 ms per
mapping on this file because it repeatedly read, copied and checked chunks.

Reproduce the optimized benchmark on Linux:

```sh
clang++ -O3 -std=c++20 -Isrc tools/benchmark_package.cpp \
  src/file_vfs.cpp src/package_file_source.cpp src/native_file_source_linux.cpp \
  -llz4 -o build/package_bench
./build/package_bench ../parfait_jp ../parfait_jp/parfait.luna 'bgm/哀しみ.wav'
```

Reproduce content validation with
`python3 tools/test_parfait_archive.py --game ../parfait_jp`. Its explicit runtime
file selection excludes the archive now in the game directory. Save data is
isolated. The engine test build has an empty `CMAKE_BUILD_TYPE`; its recorded
smoke durations are functional observations, not optimized-release benchmarks.

Windows was not executed. Headless tests do not establish visual parity or
full-route correctness. Mounted archives must remain immutable; use file
replacement rather than in-place writes/truncation.
