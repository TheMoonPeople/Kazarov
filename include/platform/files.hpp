#pragma once

#include <cstddef>
#include <filesystem>

enum class FilesResult {
    kWritten,
    kExists, // Not written: the path was taken and replacing was not allowed.
    kFailed,
};

/**
 * Writes data to path through a unique temporary file beside it. Where supported, the file and
 * directory are flushed before kWritten returns. Horizon cannot sync directories or replace names:
 * replacement moves the old file to a unique temporary name before publishing the new one, so an
 * interruption can leave the old file under that name or leave path absent. Without replace, a
 * taken path gives kExists. Temporary files left by an interruption or failed cleanup need manual
 * recovery or removal.
 */
FilesResult FilesWrite(const std::filesystem::path &path, const void *data, std::size_t size, bool replace);

/**
 * Reads up to size bytes of the regular file at path and returns how many.
 * A directory, FIFO or other file type reads as nothing without blocking.
 */
std::size_t FilesRead(const std::filesystem::path &path, void *data, std::size_t size);

/**
 * Creates a directory at path, where nothing may be; kExists means something is already there.
 * Windows publishes with a write-through move, while POSIX syncs the parent directory.
 */
FilesResult FilesCreateDirectory(const std::filesystem::path &path);
