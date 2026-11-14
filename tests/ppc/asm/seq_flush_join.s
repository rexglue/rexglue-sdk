test_flush_join_scalar_after_vector_path:
  #_ REGISTER_IN r3 1
  #_ REGISTER_IN f1 0x0000000000000001
  #_ REGISTER_IN f2 0x3FF0000000000000
  #_ REGISTER_IN v2 [00000000, 00000000, 00000000, 00000000]
  #_ REGISTER_IN v3 [00000000, 00000000, 00000000, 00000000]
  cmpwi r3, 0
  beq flush_join_a_skip
  vaddfp v1, v2, v3
flush_join_a_skip:
  fmul f3, f1, f2
  blr
  #_ REGISTER_OUT f3 0x0000000000000001

test_flush_join_scalar_after_scalar_path:
  #_ REGISTER_IN r3 0
  #_ REGISTER_IN f1 0x0000000000000001
  #_ REGISTER_IN f2 0x3FF0000000000000
  #_ REGISTER_IN v2 [00000000, 00000000, 00000000, 00000000]
  #_ REGISTER_IN v3 [00000000, 00000000, 00000000, 00000000]
  cmpwi r3, 0
  beq flush_join_b_skip
  vaddfp v1, v2, v3
flush_join_b_skip:
  fmul f3, f1, f2
  blr
  #_ REGISTER_OUT f3 0x0000000000000001

test_flush_join_loop_back_edge:
  #_ REGISTER_IN r3 3
  #_ REGISTER_IN f1 0x0000000000000001
  #_ REGISTER_IN f2 0x3FF0000000000000
  #_ REGISTER_IN v2 [00000000, 00000000, 00000000, 00000000]
  #_ REGISTER_IN v3 [00000000, 00000000, 00000000, 00000000]
  fmul f4, f1, f2
flush_join_c_loop:
  fmul f3, f1, f2
  vaddfp v1, v2, v3
  addic. r3, r3, -1
  bne flush_join_c_loop
  blr
  #_ REGISTER_OUT r3 0
  #_ REGISTER_OUT f3 0x0000000000000001
  #_ REGISTER_OUT f4 0x0000000000000001
