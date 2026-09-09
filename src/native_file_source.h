#ifndef LUNA_NATIVE_FILE_SOURCE_H
#define LUNA_NATIVE_FILE_SOURCE_H

#include "file_vfs.h"

#include <span>
#include <string>
#include <string_view>

namespace luna::file {

FileStatus OpenNativeFileSource(std::string root,
    const NativeSourceOptions &options, std::shared_ptr<FileSource> *out);

// Resolve relative paths without following symlinks/reparse points below root.
// Atomic same-directory replacement. Does not promise power-loss durability.
FileStatus NativeAtomicWriteUnderRoot(std::string_view root,
    std::string_view path, std::span<const std::byte> bytes);
FileStatus NativeRemoveFileUnderRoot(
    std::string_view root, std::string_view path);

FileStatus NativeCreateDirectories(std::string_view host_path);
FileStatus NativeWriteFile(
    std::string_view host_path, std::span<const std::byte> bytes);
FileStatus NativeReadFile(
    std::string_view host_path, std::vector<std::byte> *out);

} // namespace luna::file

#endif
