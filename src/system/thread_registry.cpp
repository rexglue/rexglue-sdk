#include <rex/system/thread_registry.h>

#include <mutex>
#include <shared_mutex>

namespace rex::system {

ThreadRef::~ThreadRef() {
  registry->NoteHandlesClosed(thread_id);
}

ThreadRegistry::~ThreadRegistry() = default;

void ThreadRegistry::Dispose(std::unique_ptr<ThreadRecord> record) {
  if (!record) {
    return;
  }
  ThreadRecord* raw = record.release();
  if (raw->pins.Retire()) {
    delete raw;
  }
}

X_STATUS ThreadRegistry::Launch(std::unique_ptr<ThreadRecord> record, X_HANDLE* out_handle) {
  ThreadRecord* raw = record.get();
  GuestBlock block = AllocateGuestBlock(raw->memory(), sizeof(X_KTHREAD));
  if (!block.address) {
    return X_STATUS_NO_MEMORY;
  }
  raw->SetGuestObject(block.address);
  block.owned = false;
  {
    std::unique_lock lock(mutex_);
    threads_.emplace(raw->thread_id(), std::move(record));
  }
  X_HANDLE handle = 0;
  X_STATUS status =
      table_->Emplace<ThreadRef>(&handle, AddOptions{{}, block}, raw->thread_id(), this);
  if (XFAILED(status)) {
    std::unique_ptr<ThreadRecord> failed;
    {
      std::unique_lock lock(mutex_);
      auto it = threads_.find(raw->thread_id());
      failed = std::move(it->second);
      threads_.erase(it);
    }
    return status;
  }
  StashHandle(&raw->guest_object<X_KTHREAD>()->header, handle);
  raw->SetHandle(handle);
  status = raw->Create();
  if (XFAILED(status)) {
    NoteExited(*raw);
    table_->Close(handle);
    return status;
  }
  if (out_handle) {
    *out_handle = handle;
  }
  return X_STATUS_SUCCESS;
}

ThreadPin ThreadRegistry::Find(u32 thread_id) {
  std::shared_lock lock(mutex_);
  auto it = threads_.find(thread_id);
  if (it == threads_.end()) {
    return {};
  }
  it->second->pins.Pin();
  return ThreadPin(it->second.get());
}

ThreadPin ThreadRegistry::FindByHandle(X_HANDLE handle) {
  auto ref = table_->Get<ThreadRef>(handle);
  if (!ref) {
    return {};
  }
  return Find(ref->thread_id);
}

std::vector<ThreadPin> ThreadRegistry::All() {
  std::vector<ThreadPin> results;
  std::shared_lock lock(mutex_);
  results.reserve(threads_.size());
  for (auto& [id, record] : threads_) {
    record->pins.Pin();
    results.emplace_back(record.get());
  }
  return results;
}

void ThreadRegistry::NoteExited(ThreadRecord& record) {
  std::unique_ptr<ThreadRecord> retired;
  {
    std::unique_lock lock(mutex_);
    record.exited_ = true;
    if (record.handles_closed_) {
      auto it = threads_.find(record.thread_id());
      retired = std::move(it->second);
      threads_.erase(it);
    }
  }
  Dispose(std::move(retired));
}

void ThreadRegistry::NoteHandlesClosed(u32 thread_id) {
  std::unique_ptr<ThreadRecord> retired;
  {
    std::unique_lock lock(mutex_);
    auto it = threads_.find(thread_id);
    if (it == threads_.end()) {
      return;
    }
    it->second->handles_closed_ = true;
    if (it->second->exited_) {
      retired = std::move(it->second);
      threads_.erase(it);
    }
  }
  Dispose(std::move(retired));
}

}  // namespace rex::system
