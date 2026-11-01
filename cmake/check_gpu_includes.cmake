#Fails when a file under src/gpu includes the runtime or the kernel
file(GLOB_RECURSE gpu_files "${GPU_SOURCE_DIR}/*.h" "${GPU_SOURCE_DIR}/*.cpp"
     "${GPU_SOURCE_DIR}/CMakeLists.txt"
)
set(OFFENDERS "")
foreach (gpu_file IN LISTS gpu_files)
  file(STRINGS "${gpu_file}" hits REGEX "rex/(runtime|kernel)")
  if (hits)
    list(APPEND OFFENDERS "${gpu_file}")
  endif ()
endforeach ()
if (OFFENDERS)
  message(FATAL_ERROR "src/gpu includes the runtime or the kernel:\n${OFFENDERS}")
endif ()
