// Facility Admission Control - file and path primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal only. Every path that reaches the operating system goes through
// here so that the rules are stated once: the path must be valid UTF-8, inside
// the configured length bound, and long paths are given the extended-length
// prefix Windows requires instead of failing at the 260-character limit.

#ifndef FAC_DURABLE_FILE_HANDLE_HPP
#define FAC_DURABLE_FILE_HANDLE_HPP

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fac/core/error.hpp"

namespace fac::durable {

#ifdef _WIN32
using NativePath = std::wstring;
#else
using NativePath = std::string;
#endif

// Converts a validated UTF-8 path into the form the operating system wants,
// applying the Windows extended-length prefix when the path needs it.
[[nodiscard]] Result<NativePath> to_native_path(const std::string& utf8_path);

class FileHandle {
 public:
  FileHandle() = default;
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  FileHandle(FileHandle&& other) noexcept;
  FileHandle& operator=(FileHandle&& other) noexcept;
  ~FileHandle();

  [[nodiscard]] static Result<FileHandle> open_read(const std::string& path);
  [[nodiscard]] static Result<FileHandle> open_read_write(const std::string& path, bool create_if_missing);
  [[nodiscard]] static Result<FileHandle> open_truncate(const std::string& path);

  [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

  // Reads up to buffer.size() bytes. Reports how many were available, which may
  // be fewer than requested at end of file.
  [[nodiscard]] Status read_at(std::uint64_t offset, std::span<std::byte> buffer,
                               std::size_t& bytes_read) const;

  [[nodiscard]] Status write_at(std::uint64_t offset, std::span<const std::byte> bytes);

  [[nodiscard]] Status append(std::span<const std::byte> bytes);

  [[nodiscard]] Status flush();

  [[nodiscard]] Result<std::uint64_t> size();

  [[nodiscard]] Status truncate_to(std::uint64_t length);

  void close() noexcept;

 private:
  void* handle_ = nullptr;
};

// Path helpers.
[[nodiscard]] Result<std::string> normalize_directory(const std::string& path);
[[nodiscard]] std::string join_path(const std::string& directory, std::string_view name);
[[nodiscard]] Status ensure_directory(const std::string& path);
[[nodiscard]] bool path_exists(const std::string& path);
[[nodiscard]] Status remove_file(const std::string& path);
[[nodiscard]] Status read_file(const std::string& path, std::vector<std::byte>& out, std::size_t max_bytes);
[[nodiscard]] Result<std::uint64_t> file_size(const std::string& path);
[[nodiscard]] Status write_file_atomic(const std::string& path, std::span<const std::byte> bytes);
[[nodiscard]] Status flush_directory(const std::string& path);

}  // namespace fac::durable

#endif  // FAC_DURABLE_FILE_HANDLE_HPP
