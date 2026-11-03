test_mtmsrd_masks_the_calling_thread:
  #_ REGISTER_IN r13 0x40000
  mfmsr r3
  mtmsrd r13, 1
  mfmsr r4
  mtmsrd r3, 1
  mfmsr r5
  blr
  #_ REGISTER_OUT r3 0x200A000
  #_ REGISTER_OUT r4 0x2002000
  #_ REGISTER_OUT r5 0x200A000

test_mtmsrd_nested_windows:
  #_ REGISTER_IN r13 0
  mfmsr r3
  mtmsrd r13, 1
  mfmsr r4
  mtmsrd r13, 1
  mtmsrd r4, 1
  mfmsr r5
  mtmsrd r3, 1
  mfmsr r6
  blr
  #_ REGISTER_OUT r3 0x200A000
  #_ REGISTER_OUT r4 0x2002000
  #_ REGISTER_OUT r5 0x2002000
  #_ REGISTER_OUT r6 0x200A000

test_mtmsrd_other_register_keeps_ee_set:
  #_ REGISTER_IN r14 0x8000
  mfmsr r3
  mtmsrd r14, 1
  mfmsr r4
  mtmsrd r3, 1
  mfmsr r5
  blr
  #_ REGISTER_OUT r3 0x200A000
  #_ REGISTER_OUT r4 0x200A000
  #_ REGISTER_OUT r5 0x200A000

test_mtmsrd_other_register_clears_ee_with_zero:
  #_ REGISTER_IN r14 0
  mfmsr r3
  mtmsrd r14, 1
  mfmsr r4
  mtmsrd r3, 1
  mfmsr r5
  blr
  #_ REGISTER_OUT r3 0x200A000
  #_ REGISTER_OUT r4 0x2002000
  #_ REGISTER_OUT r5 0x200A000
