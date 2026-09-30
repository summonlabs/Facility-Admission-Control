// Facility Admission Control - OS single-writer exclusion.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fac/durable/lock.hpp"

#include "durable/file_handle.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fac::durable {

WriterLock::WriterLock(WriterLock&& other) noexcept
    : handle_(other.handle_), reader_(other.reader_) {
  other.handle_ = nullptr;
  other.reader_ = false;
}

WriterLock& WriterLock::operator=(WriterLock&& other) noexcept {
  if (this != &other) {
    release();
    handle_ = other.handle_;
    reader_ = other.reader_;
    other.handle_ = nullptr;
    other.reader_ = false;
  }
  return *this;
}

WriterLock::~WriterLock() { release(); }

bool WriterLock::held() const noexcept { return handle_ != nullptr; }

void WriterLock::release() noexcept {
  if (handle_ == nullptr) {
    return;
  }
#ifdef _WIN32
  CloseHandle(static_cast<HANDLE>(handle_));
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  (void)::flock(fd, LOCK_UN);
  ::close(fd);
#endif
  handle_ = nullptr;
}

Result<WriterLock> WriterLock::acquire_writer(const std::string& path) {
  auto native = to_native_path(path);
  if (!native.ok()) {
    return native.error();
  }
#ifdef _WIN32
  // dwShareMode of 0 is what makes this exclusion real: while this handle is
  // open, no other process can open the file for any access at all. The kernel
  // closes the handle when the process dies, so a killed writer does not leave
  // the store locked.
  const HANDLE handle = CreateFileW(native.value().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
      return make_error(ErrorCode::writer_lock_held, "another process holds the store write lock");
    }
    return make_error(ErrorCode::io_error, "store write lock could not be acquired");
  }
  WriterLock lock;
  lock.handle_ = handle;
  lock.reader_ = false;
  return lock;
#else
  const int fd = ::open(native.value().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    return make_error(ErrorCode::io_error, "store write lock file could not be opened");
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return make_error(ErrorCode::writer_lock_held, "another process holds the store write lock");
    }
    return make_error(ErrorCode::io_error, "store write lock could not be acquired");
  }
  WriterLock lock;
  lock.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
  lock.reader_ = false;
  return lock;
#endif
}

Result<WriterLock> WriterLock::acquire_reader(const std::string& path) {
  auto native = to_native_path(path);
  if (!native.ok()) {
    return native.error();
  }
#ifdef _WIN32
  // A reader asks for a share mode that coexists with other readers. It cannot
  // coexist with a writer, whose share mode admits nothing, so an inspection
  // never observes a store that is being mutated.
  const HANDLE handle = CreateFileW(native.value().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
      return make_error(ErrorCode::writer_lock_held, "a writer holds the store lock");
    }
    if (code == ERROR_FILE_NOT_FOUND) {
      return make_error(ErrorCode::file_not_found, "store lock file does not exist");
    }
    return make_error(ErrorCode::io_error, "store read lock could not be acquired");
  }
  WriterLock lock;
  lock.handle_ = handle;
  lock.reader_ = true;
  return lock;
#else
  const int fd = ::open(native.value().c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return make_error(ErrorCode::file_not_found, "store lock file does not exist");
  }
  if (::flock(fd, LOCK_SH | LOCK_NB) != 0) {
    ::close(fd);
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return make_error(ErrorCode::writer_lock_held, "a writer holds the store lock");
    }
    return make_error(ErrorCode::io_error, "store read lock could not be acquired");
  }
  WriterLock lock;
  lock.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
  lock.reader_ = true;
  return lock;
#endif
}

}  // namespace fac::durable
