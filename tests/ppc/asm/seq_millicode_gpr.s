test_millicode_gpr_save_and_restore_from_the_first_register:
  #_ REGISTER_IN r1 0x10002000
  #_ REGISTER_IN r12 0x1234
  #_ REGISTER_IN r14 0x110E
  #_ REGISTER_IN r15 0x110F
  #_ REGISTER_IN r16 0x1110
  #_ REGISTER_IN r17 0x1111
  #_ REGISTER_IN r18 0x1112
  #_ REGISTER_IN r19 0x1113
  #_ REGISTER_IN r20 0x1114
  #_ REGISTER_IN r21 0x1115
  #_ REGISTER_IN r22 0x1116
  #_ REGISTER_IN r23 0x1117
  #_ REGISTER_IN r24 0x1118
  #_ REGISTER_IN r25 0x1119
  #_ REGISTER_IN r26 0x111A
  #_ REGISTER_IN r27 0x111B
  #_ REGISTER_IN r28 0x111C
  #_ REGISTER_IN r29 0x111D
  #_ REGISTER_IN r30 0x111E
  #_ REGISTER_IN r31 0x111F
  bl test_millicode_gpr_save_14
  li r12, 0
  li r14, 0
  li r15, 0
  li r16, 0
  li r17, 0
  li r18, 0
  li r19, 0
  li r20, 0
  li r21, 0
  li r22, 0
  li r23, 0
  li r24, 0
  li r25, 0
  li r26, 0
  li r27, 0
  li r28, 0
  li r29, 0
  li r30, 0
  li r31, 0
  bl test_millicode_gpr_restore_14
  blr
  #_ REGISTER_OUT r12 0x1234
  #_ REGISTER_OUT r14 0x110E
  #_ REGISTER_OUT r15 0x110F
  #_ REGISTER_OUT r16 0x1110
  #_ REGISTER_OUT r17 0x1111
  #_ REGISTER_OUT r18 0x1112
  #_ REGISTER_OUT r19 0x1113
  #_ REGISTER_OUT r20 0x1114
  #_ REGISTER_OUT r21 0x1115
  #_ REGISTER_OUT r22 0x1116
  #_ REGISTER_OUT r23 0x1117
  #_ REGISTER_OUT r24 0x1118
  #_ REGISTER_OUT r25 0x1119
  #_ REGISTER_OUT r26 0x111A
  #_ REGISTER_OUT r27 0x111B
  #_ REGISTER_OUT r28 0x111C
  #_ REGISTER_OUT r29 0x111D
  #_ REGISTER_OUT r30 0x111E
  #_ REGISTER_OUT r31 0x111F

test_millicode_gpr_save_and_restore_at_the_last_register:
  #_ REGISTER_IN r1 0x10002000
  #_ REGISTER_IN r12 0x1234
  #_ REGISTER_IN r14 0x110E
  #_ REGISTER_IN r15 0x110F
  #_ REGISTER_IN r16 0x1110
  #_ REGISTER_IN r17 0x1111
  #_ REGISTER_IN r18 0x1112
  #_ REGISTER_IN r19 0x1113
  #_ REGISTER_IN r20 0x1114
  #_ REGISTER_IN r21 0x1115
  #_ REGISTER_IN r22 0x1116
  #_ REGISTER_IN r23 0x1117
  #_ REGISTER_IN r24 0x1118
  #_ REGISTER_IN r25 0x1119
  #_ REGISTER_IN r26 0x111A
  #_ REGISTER_IN r27 0x111B
  #_ REGISTER_IN r28 0x111C
  #_ REGISTER_IN r29 0x111D
  #_ REGISTER_IN r30 0x111E
  #_ REGISTER_IN r31 0x111F
  bl test_millicode_gpr_save_31
  li r12, 0
  li r14, 0
  li r15, 0
  li r16, 0
  li r17, 0
  li r18, 0
  li r19, 0
  li r20, 0
  li r21, 0
  li r22, 0
  li r23, 0
  li r24, 0
  li r25, 0
  li r26, 0
  li r27, 0
  li r28, 0
  li r29, 0
  li r30, 0
  li r31, 0
  bl test_millicode_gpr_restore_31
  blr
  #_ REGISTER_OUT r12 0x1234
  #_ REGISTER_OUT r14 0x0
  #_ REGISTER_OUT r15 0x0
  #_ REGISTER_OUT r16 0x0
  #_ REGISTER_OUT r17 0x0
  #_ REGISTER_OUT r18 0x0
  #_ REGISTER_OUT r19 0x0
  #_ REGISTER_OUT r20 0x0
  #_ REGISTER_OUT r21 0x0
  #_ REGISTER_OUT r22 0x0
  #_ REGISTER_OUT r23 0x0
  #_ REGISTER_OUT r24 0x0
  #_ REGISTER_OUT r25 0x0
  #_ REGISTER_OUT r26 0x0
  #_ REGISTER_OUT r27 0x0
  #_ REGISTER_OUT r28 0x0
  #_ REGISTER_OUT r29 0x0
  #_ REGISTER_OUT r30 0x0
  #_ REGISTER_OUT r31 0x111F

test_millicode_gpr_save_14:
  std r14, -152(r1)
  std r15, -144(r1)
  std r16, -136(r1)
  std r17, -128(r1)
  std r18, -120(r1)
  std r19, -112(r1)
  std r20, -104(r1)
  std r21, -96(r1)
  std r22, -88(r1)
  std r23, -80(r1)
  std r24, -72(r1)
  std r25, -64(r1)
  std r26, -56(r1)
  std r27, -48(r1)
  std r28, -40(r1)
  std r29, -32(r1)
  std r30, -24(r1)
test_millicode_gpr_save_31:
  std r31, -16(r1)
  stw r12, -8(r1)
  blr

test_millicode_gpr_restore_14:
  ld r14, -152(r1)
  ld r15, -144(r1)
  ld r16, -136(r1)
  ld r17, -128(r1)
  ld r18, -120(r1)
  ld r19, -112(r1)
  ld r20, -104(r1)
  ld r21, -96(r1)
  ld r22, -88(r1)
  ld r23, -80(r1)
  ld r24, -72(r1)
  ld r25, -64(r1)
  ld r26, -56(r1)
  ld r27, -48(r1)
  ld r28, -40(r1)
  ld r29, -32(r1)
  ld r30, -24(r1)
test_millicode_gpr_restore_31:
  ld r31, -16(r1)
  lwz r12, -8(r1)
  mtspr lr, r12
  blr
