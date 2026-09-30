// Facility Admission Control - OS single-writer exclusion.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The lock is a kernel lock on a real file, not an advisory flag in this
// process. A writer holds it exclusively for the whole time it owns the store.
// The kernel releases it when the owning process exits, including when it is
// killed abruptly, so a crashed writer never leaves the store permanently
// unusable. A reader takes a share mode that coexists with other readers but
// not with a writer, which is what makes "read-only inspection" honest: it
// cannot observe a store that is being mutated.

#ifndef FAC_DURABLE_LOCK_HPP
#define FAC_DURABLE_LOCK_HPP

#include <string>

#include "fac/core/error.hpp"

namespace fac::durable {

class WriterLock {
 public:
  WriterLock() = default;
  WriterLock(const WriterLock&) = delete;
  WriterLock& operator=(const WriterLock&) = delete;
  WriterLock(WriterLock&& other) noexcept;
  WriterLock& operator=(WriterLock&& other) noexcept;
  ~WriterLock();

  // Creates the lock file when absent and takes it exclusively without
  // blocking. A second writer receives ErrorCode::writer_lock_held.
  [[nodiscard]] static Result<WriterLock> acquire_writer(const std::string& path);

  // Takes a share lock that permits other readers and refuses to coexist with
  // a writer.
  [[nodiscard]] static Result<WriterLock> acquire_reader(const std::string& path);

  [[nodiscard]] bool held() const noexcept;
  void release() noexcept;

 private:
  void* handle_ = nullptr;  // HANDLE on Windows, fd encoded in the pointer on POSIX
  bool reader_ = false;
};

}  // namespace fac::durable

#endif  // FAC_DURABLE_LOCK_HPP
