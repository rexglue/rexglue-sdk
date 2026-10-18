test_import_thunk_runs_through_mtctr_and_bctr:
  mfspr r11, lr
  lis r5, test_import_thunk_target_first@ha
  addi r5, r5, test_import_thunk_target_first@l
  lis r6, 0x1000
  stw r5, 0x1000(r6)
  bl test_import_thunk_first
  mtspr lr, r11
  blr
  #_ REGISTER_OUT r3 0x1111

test_import_thunk_first:
  lis r12, 0x1000
  lwz r12, 0x1000(r12)
  mtctr r12
  bctr

test_import_thunk_target_first:
  li r3, 0x1111
  blr

test_import_thunk_returns_to_the_caller_of_the_thunk:
  mfspr r11, lr
  lis r5, test_import_thunk_target_second@ha
  addi r5, r5, test_import_thunk_target_second@l
  lis r6, 0x1000
  stw r5, 0x1010(r6)
  li r3, 1
  bl test_import_thunk_second
  addi r3, r3, 0x20
  mtspr lr, r11
  blr
  #_ REGISTER_OUT r3 0x2222

test_import_thunk_second:
  lis r12, 0x1000
  lwz r12, 0x1010(r12)
  mtctr r12
  bctr

test_import_thunk_target_second:
  li r3, 0x2202
  blr
