test_stwux_frame_acquire_back_chain:
  #_ REGISTER_IN r1 0x10001100
  li r0, -128
  stwux r1, r1, r0
  blr
  #_ REGISTER_OUT r1 0x10001080
  #_ REGISTER_OUT r0 0xFFFFFFFFFFFFFF80
  #_ MEMORY_OUT 10001080 10001100

test_stwux_frame_acquire_large_frame:
  #_ REGISTER_IN r1 0x10003000
  li r12, -4096
  stwux r1, r1, r12
  blr
  #_ REGISTER_OUT r1 0x10002000
  #_ MEMORY_OUT 10002000 10003000

test_stwux_frame_acquire_leaves_old_frame_untouched:
  #_ REGISTER_IN r1 0x10001100
  #_ MEMORY_IN 10001100 DEADBEEF
  li r0, -128
  stwux r1, r1, r0
  blr
  #_ REGISTER_OUT r1 0x10001080
  #_ MEMORY_OUT 10001080 10001100
  #_ MEMORY_OUT 10001100 DEADBEEF
