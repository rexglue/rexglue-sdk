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
  std::filesystem::path cache_root; /**< Folder of the storage files. */
  u32 title_id = 0;                 /**< Title the storage belongs to. */

  std::fstream shader_file;                 /**< Shader storage stream. */
  std::filesystem::path shader_file_path;   /**< Path of the shader storage file. */
  u32 shader_index = 0;                     /**< Incremented when switching the storage. */
  bool shader_file_flush_needed = false;    /**< True when the shader stream has unflushed data. */
  std::fstream pipeline_file;               /**< Pipeline storage stream. */
  std::filesystem::path pipeline_file_path; /**< Path of the pipeline storage file. */
  bool pipeline_file_flush_needed = false; /**< True when the pipeline stream has unflushed data. */

  std::mutex write_request_lock;              /**< Guards the queues and flags below. */
  std::condition_variable write_request_cond; /**< Signaled when the queues or flags change. */
  std::deque<const ShaderType*> write_shader_queue;    /**< Shaders waiting to be written. */
  std::deque<PipelineType> write_pipeline_queue;       /**< Pipelines waiting to be written. */
  bool write_flush_shaders = false;                    /**< Request to flush the shader stream. */
  bool write_flush_pipelines = false;                  /**< Request to flush the pipeline stream. */
  bool write_thread_shutdown = false;                  /**< Request to end the writer thread. */
  std::unique_ptr<rex::platform::Thread> write_thread; /**< The writer thread. */

  /** Stops the writer thread, drops the queues and closes both streams. */
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
