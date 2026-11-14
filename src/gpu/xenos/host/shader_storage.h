/**
 * @file        gpu/xenos/host/shader_storage.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

#include <rex/platform/thread.h>
#include <rex/platform/sync.h>
#include <rex/types.h>

namespace rex::gpu {

/**
 * The shader and pipeline storage files of a pipeline cache and the thread that writes them.
 *
 * @tparam ShaderType The shader type the writer queue holds.
 * @tparam PipelineType The stored pipeline description the writer queue holds.
 */
template <typename ShaderType, typename PipelineType>
struct ShaderStorage {
  std::filesystem::path cache_root;
  u32 title_id = 0;

  std::fstream shader_file;
  std::filesystem::path shader_file_path;
  u32 shader_index = 0; /**< Incremented when switching the storage. */
  bool shader_file_flush_needed = false;
  std::fstream pipeline_file;
  std::filesystem::path pipeline_file_path;
  bool pipeline_file_flush_needed = false;

  std::mutex write_request_lock;              /**< Guards the queues and flags below. */
  std::condition_variable write_request_cond; /**< Signaled when the queues or flags change. */
  std::deque<const ShaderType*> write_shader_queue;
  std::deque<PipelineType> write_pipeline_queue;
  bool write_flush_shaders = false;
  bool write_flush_pipelines = false;
  bool write_thread_shutdown = false;
  std::unique_ptr<rex::platform::Thread> write_thread;

  /** Stops the writer thread, discards the queues and closes both streams. */
  void Shutdown() {
    if (write_thread) {
      {
        std::lock_guard<std::mutex> lock(write_request_lock);
        write_thread_shutdown = true;
      }
      write_request_cond.notify_all();
      rex::platform::Wait(write_thread.get(), false);
      write_thread.reset();
    }
    write_shader_queue.clear();
    write_pipeline_queue.clear();

    if (pipeline_file.is_open()) {
      pipeline_file.close();
      pipeline_file_flush_needed = false;
    }

    if (shader_file.is_open()) {
      shader_file.close();
      shader_file_flush_needed = false;
    }

    cache_root.clear();
    title_id = 0;
  }
};

}  // namespace rex::gpu
