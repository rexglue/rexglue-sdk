test_cr_fields_2_to_5_survive_a_call:
  mfspr r11, lr
  li r4, 5
  cmpwi cr0, r4, 5
  cmpwi cr1, r4, 1
  cmpwi cr2, r4, 9
  cmpwi cr3, r4, 5
  cmpwi cr4, r4, 1
  cmpwi cr5, r4, 9
  cmpwi cr6, r4, 5
  cmpwi cr7, r4, 1
  bl test_cr_persistence_callee_volatile
  mfcr r3
  mtspr lr, r11
  blr
  #_ REGISTER_OUT r3 0x88824888

test_cr_persistence_callee_volatile:
  li r5, -1
  cmpwi cr0, r5, 0
  cmpwi cr1, r5, 0
  cmpwi cr6, r5, 0
  cmpwi cr7, r5, 0
  blr

test_cr_fields_0_1_6_7_are_clobbered_by_the_callee:
  mfspr r11, lr
  li r4, 5
  cmpwi cr0, r4, 5
  cmpwi cr1, r4, 5
  cmpwi cr6, r4, 5
  cmpwi cr7, r4, 5
  bl test_cr_persistence_callee_volatile
  mfcr r3
  mtspr lr, r11
  blr
  #_ REGISTER_OUT r3 0x88000088

test_cr_fields_2_to_5_survive_a_callee_that_saves_and_restores_them:
  mfspr r11, lr
  li r4, 5
  cmpwi cr0, r4, 5
  cmpwi cr1, r4, 1
  cmpwi cr2, r4, 9
  cmpwi cr3, r4, 5
  cmpwi cr4, r4, 1
  cmpwi cr5, r4, 9
  cmpwi cr6, r4, 5
  cmpwi cr7, r4, 1
  bl test_cr_persistence_callee_saves
  mfcr r3
  mtspr lr, r11
  blr
  #_ REGISTER_OUT r3 0x24824824

test_cr_persistence_callee_saves:
  mfcr r10
  li r5, -1
  cmpwi cr2, r5, 0
  cmpwi cr3, r5, 0
  cmpwi cr4, r5, 0
  cmpwi cr5, r5, 0
  mtcrf 0x3C, r10
  blr
