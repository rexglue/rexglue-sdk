/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <queue>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/file.h>
#include <rex/literals.h>
#include <rex/logging.h>
#include <rex/math.h>

REXCVAR_DEFINE_STRING(dump_source, "", "Filesystem", "Specifies the file to dump from");

REXCVAR_DEFINE_STRING(dump_path, "", "Filesystem", "Specifies the directory to dump files to");

namespace rex::filesystem {

using namespace rex::literals;

int vfs_dump_main(const std::vector<std::string>& args) {
  if (REXCVAR_GET(dump_source).empty() || REXCVAR_GET(dump_path).empty()) {
    REXFS_ERROR("Usage: {} [source] [dump_path]", rex::path_to_utf8(args[0]));
    return 1;
  }

  std::filesystem::path source = rex::to_path(REXCVAR_GET(dump_source));
  std::filesystem::path base_path = rex::to_path(REXCVAR_GET(dump_path));
  std::unique_ptr<vfs::Device> device;

  // TODO: Flags specifying the type of device.
  device = std::make_unique<vfs::StfsContainerDevice>("", source);
  if (!device->Initialize()) {
    REXFS_ERROR("Failed to initialize device");
    return 1;
  }

  // Run through all the files, breadth-first style.
  std::queue<vfs::Entry*> queue;
  auto root = device->ResolvePath("/");
  queue.push(root);

  // Allocate a buffer when needed.
  size_t buffer_size = 0;
  uint8_t* buffer = nullptr;

  while (!queue.empty()) {
    auto entry = queue.front();
    queue.pop();
    for (auto& entry : entry->children()) {
      queue.push(entry.get());
    }

    REXFS_INFO("{}", entry->path());
    auto dest_name = base_path / rex::to_path(entry->path());
    if (entry->attributes() & kFileAttributeDirectory) {
      std::filesystem::create_directories(dest_name);
      continue;
    }

    vfs::File* in_file = nullptr;
    if (entry->Open(FileAccess::kFileReadData, &in_file) != X_STATUS_SUCCESS) {
      continue;
    }

    auto file = rex::filesystem::OpenFile(dest_name, "wb");
    if (!file) {
      in_file->Destroy();
      continue;
    }

    if (entry->can_map()) {
      auto map = entry->OpenMapped(rex::memory::MappedMemory::Mode::kRead);
      fwrite(map->data(), map->size(), 1, file);
      map->Close();
    } else {
      // Can't map the file into memory. Read it into a temporary buffer.
      if (!buffer || entry->size() > buffer_size) {
        // Resize the buffer.
        if (buffer) {
          delete[] buffer;
        }

        // Allocate a buffer rounded up to the nearest 512MB.
        buffer_size = rex::round_up(entry->size(), 512_MiB);
        buffer = new uint8_t[buffer_size];
      }

      size_t bytes_read = 0;
      in_file->ReadSync(buffer, entry->size(), 0, &bytes_read);
      fwrite(buffer, bytes_read, 1, file);
    }

    fclose(file);
    in_file->Destroy();
  }

  if (buffer) {
    delete[] buffer;
  }

  return 0;
}

}  // namespace rex::filesystem

// TODO: CONSOLE APP - XE_DEFINE_CONSOLE_APP("xenia-vfs-dump", rex::filesystem::vfs_dump_main,
//                       "[source] [dump_path]", "source", "dump_path");
