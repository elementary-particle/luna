#ifndef LUNA_PACKAGE_FILE_SOURCE_H
#define LUNA_PACKAGE_FILE_SOURCE_H

#include "file_vfs.h"
#include <ostream>

namespace luna::file {

inline constexpr uint32_t kPackageChunkSize = 256 * 1024;

FileStatus OpenPackageFileSource(std::string package_file,
    const PackageOpenOptions &options, std::shared_ptr<FileSource> *out);
// Opens a mapped backing when supported, otherwise a pinned seekable stream.
FileStatus OpenPackageFileSource(FileSource &source, std::string_view path,
    const PackageOpenOptions &options, std::shared_ptr<FileSource> *out);
// Pins the opened file; later replacement of its host path does not affect
// reads.
FileStatus OpenPackageFileSource(std::unique_ptr<FileStream> stream,
    const PackageOpenOptions &options, std::shared_ptr<FileSource> *out);
// Writes at the current output position, which must be the start of a new file.
// Inputs are sorted; timestamps and host paths are not serialized. On failure,
// discard the partial output. Empty directories are not stored.
FileStatus WritePackage(
    std::ostream &output, FileSource &source, std::vector<std::string> paths);
// Reads and validates every chunk, including assets never opened by the game.
FileStatus VerifyPackage(FileSource &source);

} // namespace luna::file
#endif
