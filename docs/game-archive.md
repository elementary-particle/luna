# Luna game archive, revision 2

A `.luna` archive is an immutable collection of UTF-8 named files. Ship one
archive containing `game.json` and all assets, or a loose manifest alongside
separately mounted archives. Runtime access needs no extraction. Revision 2
replaces the initial development layout; there is no legacy reader.

```sh
cmake --build build --target luna_pack
./build/luna_pack create path/to/content game.luna
./build/luna_pack verify game.luna
./build/luna --root game.luna
```

For single-file distribution, include a `game.json` with `id` and `entry`, without
mounts. For multiple files, package content directories separately:

```json
{
  "id": "my-game",
  "entry": "main.lua",
  "mounts": [
    {"type": "package", "path": "scripts.luna", "prefix": "/"},
    {"type": "package", "path": "images.luna", "prefix": "images"},
    {"type": "package", "path": "audio.luna", "prefix": "audio"}
  ]
}
```

Names inside an archive are relative to its mount prefix. Existing VFS priority
and first-resolving-source listing semantics apply; overlapping directory
listings are not merged. Missing or structurally damaged packages fail startup.
The manifest does not bind packages cryptographically or detect substitution of
an otherwise valid archive. These are independent archives, not split volumes.

## Access and compression

A stored file is **one contiguous byte range**. Its chunk checksums and redundant
metadata follow the payload, so they never interrupt file bytes. Native archive
backings use a read-only OS mapping; the OS loads pages on demand. `MapFile` for a
stored asset returns a direct view retaining that mapping, without allocating or
copying an asset-sized buffer. Streams copy directly from mapped stored ranges
into the caller's output. Separate cursors do not share a seek lock.

Files have 256 KiB integrity/compression chunks, with a smaller final chunk. The
writer probes LZ4 compression and requires at least 1% savings across the entire
file before enabling it. Otherwise the entire asset is stored and remains
mappable. This avoids giving up direct mapping for negligible media compression.
Within a compressed file, independent LZ4 blocks use stored fallback when a
block would not shrink. There are no shared dictionaries or solid streams.
The compression codec is [the LZ4 block format](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md).

A stream retains at most one decoded compressed chunk. Mapping a compressed
asset creates an owned buffer and honors `ReadOptions::max_size`.
`prefer_native_mapping=false` on `PackageOpenOptions` uses a pinned seekable
stream instead of an archive mapping. It is also the fallback when native
mapping is unavailable. This path uses bounded chunk buffers and a short lock
for native seek/read; decoding happens outside that lock. It never falls back
to loading the entire archive into an owned buffer.

`ReadOptions::prefer_native_mapping=false` requests an owned asset buffer.
Forbidding owned fallback reports `unsupported` when direct mapping is
unavailable. Empty files use empty owned buffers. A mapped asset retains its
backing independently of the package source; replacing the archive path does
not redirect live streams or views.

## Integrity and its performance cost

Opening checks the header, footer, index, paths, and all indexed extents, without
scanning payloads. Opening an asset compares its local chunk table with the
checked index and validates its member trailer. A stored chunk's CRC is checked
on first access; successful checks are remembered for that immutable mount.
Repeated reads and mappings avoid repeated checksum work. A whole-file mapping
checks every chunk before exposing any bytes. Compressed blocks check the stored
CRC, decoded length and decoded CRC on decode; stream-local decoded buffers are
reused. The portable CRC implementation processes eight bytes at a time.

`verify` and `CachePolicy::kBypassCache` deliberately repeat integrity checks,
even after earlier successful access. A stream spanning several chunks may
return already verified bytes and an error if a later chunk is damaged. Callers
must check both status and byte count. Payload corruption is `format_error`;
I/O and budget errors remain distinct. An unrelated damaged asset need not
prevent access to a healthy asset.

Warm stored reads can approach native FS costs. **First access is not free:**
checking a 4 KiB read may touch and checksum its 256 KiB chunk, and the first
whole-file mapping checks the entire asset. Native FS does not provide this
application-level checksum guarantee. Benchmarks must distinguish first access,
warm verified access, page-cache state, and mapping versus copying.

The redundancy follows [lzip's integrity principle](https://www.nongnu.org/lzip/manual/lzip_manual.html#File-format):
check checksums and independently recorded decoded/member lengths. The local
chunk table duplicates the index's offsets, codecs, lengths and CRCs. A file
trailer repeats decoded size, chunk count and total member size. Member lengths
permit backward traversal from the index boundary; no recovery tool is included.

CRCs detect accidental corruption; they neither authenticate hostile changes
nor guarantee detection of all multi-bit errors. No parity, repair, signatures,
encryption or patch chain is included. Packages must remain immutable while
mounted. Replace files instead of writing/truncating them in place: mappings and
cached integrity results are not safe against in-place modification. Verification
is not continuous monitoring of the underlying storage.

## Byte specification

All integers are unsigned little-endian; offsets are relative to byte zero.
No native structs, padding, timestamps, permissions or alignment gaps are stored.
CRC is CRC-32/ISO-HDLC: reflected polynomial `0xEDB88320`, initial `0xFFFFFFFF`,
final XOR `0xFFFFFFFF`. `123456789` gives `0xCBF43926`; empty input gives zero.

Layout: **header, file members in index order, index, footer**.

### Header (32 bytes)

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 8 | ASCII `LUNAPACK` |
| 8 | 4 | Revision: 2 |
| 12 | 4 | Chunk size: 262144 |
| 16 | 12 | Reserved zeros |
| 28 | 4 | CRC of header bytes 0–27 |

### File member

Each file member consists of:

1. All chunk payloads, adjacent in order, with no inline trailers.
2. A local chunk table, byte-for-byte identical to this file's index descriptors.
3. A fixed 32-byte file trailer, including for empty files.

For stored chunks, `stored_size == decoded_size` and both CRC fields must agree.
LZ4 chunks require `0 < stored_size < decoded_size <= 262144` and exact decoded
length. A file whose chunks are all stored is directly mappable. Empty files
have no payload and an empty chunk table.

| Offset in file trailer | Size | Value |
| --- | --- | --- |
| 0 | 8 | ASCII `LUNAFIL2` |
| 8 | 8 | File decoded size |
| 16 | 8 | Member size: payload + local chunk table + 32 |
| 24 | 4 | Chunk count |
| 28 | 4 | CRC of trailer bytes 0–27 |

### Index (uncompressed)

Exactly `file_count` records, sorted by unsigned UTF-8 bytes:

| Size | Value |
| --- | --- |
| 4 | Path byte length |
| 4 | Chunk count |
| 8 | Decoded file size |
| Path length | UTF-8 path, no terminator |
| Chunk count × 32 | Chunk descriptors |

Each 32-byte descriptor is: payload offset (8), stored size (4), decoded size
(4), codec (4: 0 stored, 1 LZ4), decoded CRC (4), stored CRC (4), reserved zero
(4). Each descriptor occurs once in the index and once after its file's payload.

Chunk count is `ceil(file_size / 262144)`. All nonfinal chunks decode to exactly
262144 bytes. The last chunk's size is implied by file size. Payloads cover each
member's payload region consecutively; members (including their local tables
and trailers) cover the region from byte 32 to the index offset consecutively.
Gaps, overlaps, reuse and unreferenced bytes are forbidden.

Paths are 1–4096 bytes of valid UTF-8 scalar values, with canonical relative `/`
separators. Empty components, `.`, `..`, backslashes, colons, NUL, ASCII controls
and DEL are forbidden. No Unicode normalization or case folding is performed.
These are virtual names, not extraction destinations; Windows reserved names
can exist in the archive. Duplicate files and file/directory conflicts are
rejected. Directories are implicit; empty directories are omitted. Reserved
fields must be zero and unknown revisions/codecs are rejected.

### Footer (48 bytes, exactly at EOF)

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 8 | ASCII `LUNAEND2` |
| 8 | 8 | Index offset |
| 16 | 8 | Index size |
| 24 | 8 | Total archive size including footer |
| 32 | 4 | File count |
| 36 | 4 | Index CRC |
| 40 | 4 | Repeated header CRC |
| 44 | 4 | CRC of footer bytes 0–43 |

Archives and decoded files are at most `INT64_MAX` bytes. The index is at most
64 MiB; readers may lower `PackageOpenOptions::max_index_size`. The reference
implementation limits files plus implicit directories to one million and total
implicit directory path bytes to 64 MiB. These bound metadata expansion, rather
than prescribing an exact resident-memory budget. Integrity-cache state is
bounded by the index, not payload size.

Framing costs 80 bytes per archive, 48 plus path length per file, and 64 per
chunk (two descriptors). Full stored chunks have approximately 0.025% chunk
metadata overhead. OS mappings reserve virtual address space; touched pages
belong to the OS file cache, not a second archive-sized heap buffer.

## Writer and testing

`WritePackage` sorts paths and performs two bounded passes over each seekable
input: compression selection, then payload emission. It retains metadata, not
whole assets. Stable input with the same LZ4 encoder produces deterministic
bytes; encoder versions may produce different compatible blocks. Size changes
fail the write, but inputs are not snapshots: package a stable content tree.
On failure callers must discard partial output streams.

The CLI rejects existing destinations and outputs inside the input tree. It
writes an exclusive temporary sibling file, closes and verifies it, then
publishes with a no-overwrite hard link. The build destination must support hard
links (for example NTFS or common Linux filesystems); the finished archive may
be copied to other media. Publication is not a power-loss durability promise.

With the example game's assets and a loadable LZ4 shared library available:

```sh
python3 tools/test_parfait_archive.py --game ../parfait_jp
```

This opt-in Python test creates isolated distributions under
`build/parfait-archive-*`, independently compares all decoded files and their
inventory to inputs, and runs the same real-engine checks against loose, single
and split content. It covers scenario banks, UI, visuals, dialogue, blocking
sound, chunk-boundary seeks, Japanese filenames and streamed playback. It also
rejects corruption, truncation, trailing data and missing declared packages.
`report.json` records sizes, timings and logs. Game source files are not edited;
test save data is isolated. It does not establish full-route or visual parity.

`tools/benchmark_package.cpp` is an opt-in native/package access comparison.
Compile it and the file-source implementations with identical optimization
settings. Its first-access checks do not control OS page-cache warmth; warmed
checks explicitly validate the asset before timing repeated access.
