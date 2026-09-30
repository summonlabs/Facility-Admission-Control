// Facility Admission Control - file and path primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "durable/file_handle.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "fac/core/limits.hpp"
#include "fac/core/text.hpp"

#ifdef _WIN32
#include <windows.h>

#include <cstddef>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fac::durable {
namespace {

// UTF-8 <-> filesystem path conversions, shared by both platforms. The C++20
// u8string type holds char8_t, so the conversion is explicit rather than a
// locale-dependent reinterpretation.
[[nodiscard]] std::filesystem::path path_from_utf8(const std::string& utf8) {
  const auto* begin = reinterpret_cast<const char8_t*>(utf8.data());
  return std::filesystem::path(std::u8string(begin, begin + utf8.size()));
}

[[nodiscard]] std::string utf8_from_path(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

#ifdef _WIN32

[[nodiscard]] Result<std::wstring> to_wide_path_impl(const std::string& utf8_path) {
  if (utf8_path.empty()) {
    return make_error(ErrorCode::path_invalid, "path is empty");
  }
  if (utf8_path.size() > kMaxPathLength) {
    return make_error(ErrorCode::path_too_long, "path exceeds the supported length");
  }
  const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.c_str(),
                                         static_cast<int>(utf8_path.size()), nullptr, 0);
  if (needed <= 0) {
    return make_error(ErrorCode::path_invalid, "path is not valid UTF-8");
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.c_str(),
                                          static_cast<int>(utf8_path.size()), wide.data(), needed);
  if (written != needed) {
    return make_error(ErrorCode::path_invalid, "path could not be converted");
  }
  // Windows limits a path to MAX_PATH unless it carries the extended-length
  // prefix. The prefix is added only for absolute paths, which is what this
  // runtime produces after normalisation.
  if (wide.size() >= 248 && wide.compare(0, 4, L"\\\\?\\") != 0) {
    if (wide.size() >= 2 && wide[1] == L':') {
      wide.insert(0, L"\\\\?\\");
    } else if (wide.size() >= 2 && wide[0] == L'\\' && wide[1] == L'\\') {
      // A UNC path becomes \\\\?\\UNC\\server\\share.
      wide.replace(0, 2, L"\\\\?\\UNC\\");
    }
  }
  return wide;
}

[[nodiscard]] Error win32_error(const char* what) {
  const DWORD code = GetLastError();
  return Error(ErrorCode::io_error, std::string(what) + " failed (windows error " + std::to_string(code) + ")");
}

constexpr DWORD kFileFlags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS;

#else

[[nodiscard]] Error posix_error(const char* what) {
  return Error(ErrorCode::io_error, std::string(what) + " failed (errno " + std::to_string(errno) + ")");
}

#endif

}  // namespace

Result<NativePath> to_native_path(const std::string& utf8_path) {
#ifdef _WIN32
  return to_wide_path_impl(utf8_path);
#else
  if (utf8_path.empty()) {
    return make_error(ErrorCode::path_invalid, "path is empty");
  }
  if (utf8_path.size() > kMaxPathLength) {
    return make_error(ErrorCode::path_too_long, "path exceeds the supported length");
  }
  if (!is_valid_utf8(utf8_path)) {
    return make_error(ErrorCode::path_invalid, "path is not valid UTF-8");
  }
  return utf8_path;
#endif
}

FileHandle::FileHandle(FileHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

FileHandle::~FileHandle() { close(); }

void FileHandle::close() noexcept {
  if (handle_ == nullptr) {
    return;
  }
#ifdef _WIN32
  CloseHandle(static_cast<HANDLE>(handle_));
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  ::close(fd);
#endif
  handle_ = nullptr;
}

Result<FileHandle> FileHandle::open_read(const std::string& path) {
#ifdef _WIN32
  auto wide = to_wide_path_impl(path);
  if (!wide.ok()) {
    return wide.error();
  }
  const HANDLE handle = CreateFileW(wide.value().c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                    kFileFlags, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return make_error(ErrorCode::file_not_found, "file does not exist");
    }
    return win32_error("CreateFileW(read)");
  }
  FileHandle result;
  result.handle_ = handle;
  return result;
#else
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) {
      return make_error(ErrorCode::file_not_found, "file does not exist");
    }
    return posix_error("open(read)");
  }
  FileHandle result;
  result.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
  return result;
#endif
}

Result<FileHandle> FileHandle::open_read_write(const std::string& path, bool create_if_missing) {
#ifdef _WIN32
  auto wide = to_wide_path_impl(path);
  if (!wide.ok()) {
    return wide.error();
  }
  const HANDLE handle = CreateFileW(wide.value().c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ, nullptr,
                                    create_if_missing ? OPEN_ALWAYS : OPEN_EXISTING, kFileFlags,
                                    nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return make_error(ErrorCode::file_not_found, "file does not exist");
    }
    return win32_error("CreateFileW(read/write)");
  }
  FileHandle result;
  result.handle_ = handle;
  return result;
#else
  const int flags = O_RDWR | O_CLOEXEC | (create_if_missing ? O_CREAT : 0);
  const int fd = ::open(path.c_str(), flags, 0600);
  if (fd < 0) {
    if (errno == ENOENT) {
      return make_error(ErrorCode::file_not_found, "file does not exist");
    }
    return posix_error("open(read/write)");
  }
  FileHandle result;
  result.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
  return result;
#endif
}

Result<FileHandle> FileHandle::open_truncate(const std::string& path) {
#ifdef _WIN32
  auto wide = to_wide_path_impl(path);
  if (!wide.ok()) {
    return wide.error();
  }
  const HANDLE handle = CreateFileW(wide.value().c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ, nullptr, CREATE_ALWAYS, kFileFlags, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return win32_error("CreateFileW(truncate)");
  }
  FileHandle result;
  result.handle_ = handle;
  return result;
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    return posix_error("open(truncate)");
  }
  FileHandle result;
  result.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
  return result;
#endif
}

Status FileHandle::read_at(std::uint64_t offset, std::span<std::byte> buffer,
                           std::size_t& bytes_read) const {
  bytes_read = 0;
  if (handle_ == nullptr) {
    return make_error(ErrorCode::internal, "read on a closed handle");
  }
  if (buffer.empty()) {
    return Status::success();
  }
#ifdef _WIN32
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
  overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFFull);
  DWORD read = 0;
  const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), 0x40000000u));
  if (ReadFile(static_cast<HANDLE>(handle_), buffer.data(), wanted, &read, &overlapped) == 0) {
    const DWORD code = GetLastError();
    if (code == ERROR_HANDLE_EOF) {
      return Status::success();
    }
    return win32_error("ReadFile");
  }
  bytes_read = read;
  return Status::success();
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  const ssize_t read = ::pread(fd, buffer.data(), buffer.size(), static_cast<off_t>(offset));
  if (read < 0) {
    return posix_error("pread");
  }
  bytes_read = static_cast<std::size_t>(read);
  return Status::success();
#endif
}

Status FileHandle::write_at(std::uint64_t offset, std::span<const std::byte> bytes) {
  if (handle_ == nullptr) {
    return make_error(ErrorCode::internal, "write on a closed handle");
  }
#ifdef _WIN32
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
  overlapped.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFFull);
  DWORD written = 0;
  if (WriteFile(static_cast<HANDLE>(handle_), bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                &overlapped) == 0) {
    return win32_error("WriteFile");
  }
  if (written != bytes.size()) {
    return make_error(ErrorCode::io_error, "short write");
  }
  return Status::success();
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t written =
        ::pwrite(fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(offset + done));
    if (written < 0) {
      return posix_error("pwrite");
    }
    if (written == 0) {
      return make_error(ErrorCode::io_error, "short write");
    }
    done += static_cast<std::size_t>(written);
  }
  return Status::success();
#endif
}

Status FileHandle::append(std::span<const std::byte> bytes) {
#ifdef _WIN32
  if (handle_ == nullptr) {
    return make_error(ErrorCode::internal, "append on a closed handle");
  }
  LARGE_INTEGER position{};
  LARGE_INTEGER zero{};
  if (SetFilePointerEx(static_cast<HANDLE>(handle_), zero, &position, FILE_END) == 0) {
    return win32_error("SetFilePointerEx");
  }
  DWORD written = 0;
  if (WriteFile(static_cast<HANDLE>(handle_), bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                nullptr) == 0) {
    return win32_error("WriteFile(append)");
  }
  if (written != bytes.size()) {
    return make_error(ErrorCode::io_error, "short append");
  }
  return Status::success();
#else
  auto size_now = size();
  if (!size_now.ok()) {
    return size_now.error();
  }
  return write_at(size_now.value(), bytes);
#endif
}

Status FileHandle::flush() {
  if (handle_ == nullptr) {
    return make_error(ErrorCode::internal, "flush on a closed handle");
  }
#ifdef _WIN32
  if (FlushFileBuffers(static_cast<HANDLE>(handle_)) == 0) {
    return win32_error("FlushFileBuffers");
  }
  return Status::success();
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  if (::fsync(fd) != 0) {
    return posix_error("fsync");
  }
  return Status::success();
#endif
}

Result<std::uint64_t> FileHandle::size() {
  if (handle_ == nullptr) {
    return make_error(ErrorCode::internal, "size on a closed handle");
  }
#ifdef _WIN32
  LARGE_INTEGER value{};
  if (GetFileSizeEx(static_cast<HANDLE>(handle_), &value) == 0) {
    return win32_error("GetFileSizeEx");
  }
  return static_cast<std::uint64_t>(value.QuadPart);
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    return posix_error("fstat");
  }
  return static_cast<std::uint64_t>(info.st_size);
#endif
}

Status FileHandle::truncate_to(std::uint64_t length) {
  if (handle_ == nullptr) {
    return make_error(ErrorCode::internal, "truncate on a closed handle");
  }
#ifdef _WIN32
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(length);
  if (SetFilePointerEx(static_cast<HANDLE>(handle_), position, nullptr, FILE_BEGIN) == 0) {
    return win32_error("SetFilePointerEx");
  }
  if (SetEndOfFile(static_cast<HANDLE>(handle_)) == 0) {
    return win32_error("SetEndOfFile");
  }
  return Status::success();
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  if (::ftruncate(fd, static_cast<off_t>(length)) != 0) {
    return posix_error("ftruncate");
  }
  return Status::success();
#endif
}

Result<std::string> normalize_directory(const std::string& path) {
  if (path.empty()) {
    return make_error(ErrorCode::path_invalid, "store directory is empty");
  }
  if (!is_valid_utf8(path)) {
    return make_error(ErrorCode::path_invalid, "store directory is not valid UTF-8");
  }
  if (path.size() > kMaxPathLength) {
    return make_error(ErrorCode::path_too_long, "store directory path exceeds the supported length");
  }
  std::error_code code;
  std::filesystem::path candidate = path_from_utf8(path);
  if (candidate.is_relative()) {
    candidate = std::filesystem::absolute(candidate, code);
    if (code) {
      return make_error(ErrorCode::path_invalid, "store directory could not be resolved");
    }
  }
  candidate = candidate.lexically_normal();
  const std::string normalized = utf8_from_path(candidate);
  if (normalized.size() > kMaxPathLength) {
    return make_error(ErrorCode::path_too_long, "resolved store directory path exceeds the limit");
  }
  return normalized;
}

std::string join_path(const std::string& directory, std::string_view name) {
  std::string result = directory;
  if (!result.empty() && result.back() != '\\' && result.back() != '/') {
    result.push_back('/');
  }
  result.append(name);
  return result;
}

Status ensure_directory(const std::string& path) {
  std::error_code code;
  std::filesystem::path candidate = path_from_utf8(path);
  if (std::filesystem::exists(candidate, code)) {
    if (code) {
      return make_error(ErrorCode::io_error, "store directory could not be inspected");
    }
    if (!std::filesystem::is_directory(candidate, code) || code) {
      return make_error(ErrorCode::path_invalid, "store path exists and is not a directory");
    }
    return Status::success();
  }
  std::filesystem::create_directories(candidate, code);
  if (code) {
    return make_error(ErrorCode::io_error, "store directory could not be created");
  }
  return Status::success();
}

bool path_exists(const std::string& path) {
  std::error_code code;
  return std::filesystem::exists(path_from_utf8(path), code) && !code;
}

Status remove_file(const std::string& path) {
  std::error_code code;
  std::filesystem::remove(path_from_utf8(path), code);
  if (code) {
    return make_error(ErrorCode::io_error, "file could not be removed");
  }
  return Status::success();
}

Result<std::uint64_t> file_size(const std::string& path) {
  std::error_code code;
  const auto size = std::filesystem::file_size(path_from_utf8(path), code);
  if (code) {
    if (code == std::errc::no_such_file_or_directory) {
      return make_error(ErrorCode::file_not_found, "file does not exist");
    }
    return make_error(ErrorCode::io_error, "file size could not be read");
  }
  return static_cast<std::uint64_t>(size);
}

Status read_file(const std::string& path, std::vector<std::byte>& out, std::size_t max_bytes) {
  auto handle = FileHandle::open_read(path);
  if (!handle.ok()) {
    return handle.error();
  }
  auto size = handle.value().size();
  if (!size.ok()) {
    return size.error();
  }
  if (size.value() > max_bytes) {
    return make_error(ErrorCode::limit_exceeded, "file exceeds the configured maximum size");
  }
  out.assign(static_cast<std::size_t>(size.value()), std::byte{0});
  if (out.empty()) {
    return Status::success();
  }
  std::size_t read = 0;
  const Status status = handle.value().read_at(0, std::span<std::byte>(out.data(), out.size()), read);
  if (!status.ok()) {
    return status.error();
  }
  if (read != out.size()) {
    return make_error(ErrorCode::truncated_input, "file is shorter than its reported size");
  }
  return Status::success();
}

Status write_file_atomic(const std::string& path, std::span<const std::byte> bytes) {
  const std::string temporary = path + ".tmp";
  {
    auto handle = FileHandle::open_truncate(temporary);
    if (!handle.ok()) {
      return handle.error();
    }
    const Status written = handle.value().write_at(0, bytes);
    if (!written.ok()) {
      (void)remove_file(temporary);
      return written.error();
    }
    const Status flushed = handle.value().flush();
    if (!flushed.ok()) {
      (void)remove_file(temporary);
      return flushed.error();
    }
    handle.value().close();
  }

#ifdef _WIN32
  auto wide_target = to_wide_path_impl(path);
  auto wide_temp = to_wide_path_impl(temporary);
  if (!wide_target.ok()) {
    (void)remove_file(temporary);
    return wide_target.error();
  }
  if (!wide_temp.ok()) {
    (void)remove_file(temporary);
    return wide_temp.error();
  }
  if (MoveFileExW(wide_temp.value().c_str(), wide_target.value().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    const Error error = win32_error("MoveFileExW");
    (void)remove_file(temporary);
    return error;
  }
#else
  std::error_code code;
  std::filesystem::rename(path_from_utf8(temporary), path_from_utf8(path), code);
  if (code) {
    (void)remove_file(temporary);
    return make_error(ErrorCode::io_error, "atomic rename failed");
  }
#endif
  return Status::success();
}

Status flush_directory(const std::string& path) {
#ifdef _WIN32
  // Windows has no directory fsync. The manifest and snapshot publications use
  // MOVEFILE_WRITE_THROUGH, which is the documented equivalent guarantee, and
  // this function is a no-op rather than a silent claim of a stronger one.
  (void)path;
  return Status::success();
#else
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return posix_error("open(directory)");
  }
  const int result = ::fsync(fd);
  ::close(fd);
  if (result != 0) {
    return posix_error("fsync(directory)");
  }
  return Status::success();
#endif
}

}  // namespace fac::durable