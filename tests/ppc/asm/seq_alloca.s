test_alloca_rounds_one_byte_up_to_16:
  #_ REGISTER_IN r1 0x10001100
  #_ REGISTER_IN r3 1
  #_ MEMORY_IN 10001100 10001400
  addi r3, r3, 15
  rlwinm r3, r3, 0, 0, 27
  lwz r11, 0(r1)
  neg r10, r3
  stwux r11, r1, r10
  addi r3, r1, 0x50
  blr
  #_ REGISTER_OUT r1 0x100010F0
  #_ REGISTER_OUT r3 0x10001140
  #_ MEMORY_OUT 100010F0 10001400

test_alloca_keeps_an_exact_multiple_of_16:
  #_ REGISTER_IN r1 0x10001100
  #_ REGISTER_IN r3 32
  #_ MEMORY_IN 10001100 10001400
  addi r3, r3, 15
  rlwinm r3, r3, 0, 0, 27
  lwz r11, 0(r1)
  neg r10, r3
  stwux r11, r1, r10
  addi r3, r1, 0x50
  blr
  #_ REGISTER_OUT r1 0x100010E0
  #_ REGISTER_OUT r3 0x10001130
  #_ MEMORY_OUT 100010E0 10001400

test_alloca_rounds_17_up_to_32:
  #_ REGISTER_IN r1 0x10001100
  #_ REGISTER_IN r3 17
  #_ MEMORY_IN 10001100 10001400
  addi r3, r3, 15
  rlwinm r3, r3, 0, 0, 27
  lwz r11, 0(r1)
  neg r10, r3
  stwux r11, r1, r10
  addi r3, r1, 0x50
  blr
  #_ REGISTER_OUT r1 0x100010E0
  #_ REGISTER_OUT r3 0x10001130
  #_ MEMORY_OUT 100010E0 10001400

test_alloca_of_zero_keeps_the_frame:
  #_ REGISTER_IN r1 0x10001100
  #_ REGISTER_IN r3 0
  #_ MEMORY_IN 10001100 10001400
  addi r3, r3, 15
  rlwinm r3, r3, 0, 0, 27
  lwz r11, 0(r1)
  neg r10, r3
  stwux r11, r1, r10
  addi r3, r1, 0x50
  blr
  #_ REGISTER_OUT r1 0x10001100
  #_ REGISTER_OUT r3 0x10001150
  #_ MEMORY_OUT 10001100 10001400

test_alloca_twice_links_every_frame_to_the_same_back_chain:
  #_ REGISTER_IN r1 0x10001100
  #_ REGISTER_IN r3 20
  #_ REGISTER_IN r4 8
  #_ MEMORY_IN 10001100 10001400
  addi r3, r3, 15
  rlwinm r3, r3, 0, 0, 27
  lwz r11, 0(r1)
  neg r10, r3
  stwux r11, r1, r10
  addi r4, r4, 15
  rlwinm r4, r4, 0, 0, 27
  lwz r11, 0(r1)
  neg r10, r4
  stwux r11, r1, r10
  addi r3, r1, 0x50
  blr
  #_ REGISTER_OUT r1 0x100010D0
  #_ REGISTER_OUT r3 0x10001120
  #_ MEMORY_OUT 100010E0 10001400
  #_ MEMORY_OUT 100010D0 10001400
