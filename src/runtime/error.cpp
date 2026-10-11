#include <rex/runtime/error.h>

#include <format>

#include <rex/filesystem/error.h>

namespace rex::runtime {

rex::Error StatusError(X_STATUS status) {
  return {ErrorCategory::Runtime, std::format("status {:#010x}", status), static_cast<int>(status)};
}

rex::Error ResultError(X_RESULT result) {
  return {ErrorCategory::Runtime, std::format("result {:#010x}", result), static_cast<int>(result)};
}

X_STATUS ToXStatus(const rex::Error& error, X_STATUS unsupported) {
  if (error.category == ErrorCategory::Runtime) {
    return static_cast<X_STATUS>(error.code);
  }
  using filesystem::FileSystemError;
  switch (filesystem::CodeOf(error)) {
    case FileSystemError::kNotFound:
    case FileSystemError::kNoMount:
      return X_STATUS_NO_SUCH_FILE;
    case FileSystemError::kExists:
      return X_STATUS_OBJECT_NAME_COLLISION;
    case FileSystemError::kIsDirectory:
      return X_STATUS_FILE_IS_A_DIRECTORY;
    case FileSystemError::kNotDirectory:
      return X_STATUS_NOT_A_DIRECTORY;
    case FileSystemError::kAccessDenied:
      return X_STATUS_ACCESS_DENIED;
    case FileSystemError::kSharingViolation:
      return X_STATUS_SHARING_VIOLATION;
    case FileSystemError::kNotSameDevice:
      return X_STATUS_NOT_SAME_DEVICE;
    case FileSystemError::kEndOfFile:
      return X_STATUS_END_OF_FILE;
    case FileSystemError::kUnsupported:
      return unsupported;
    case FileSystemError::kInvalidPath:
      return X_STATUS_OBJECT_NAME_INVALID;
    default:
      return X_STATUS_UNSUCCESSFUL;
  }
}

X_RESULT ToXResult(const rex::Error& error, X_RESULT unsupported) {
  return error.category == ErrorCategory::Runtime ? static_cast<X_RESULT>(error.code) : unsupported;
}

}  // namespace rex::runtime
