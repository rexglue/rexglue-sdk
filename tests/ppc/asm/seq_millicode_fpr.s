test_millicode_fpr_save_and_restore_from_the_first_register:
  #_ REGISTER_IN r12 0x10002000
  #_ REGISTER_IN f0 0.0
  #_ REGISTER_IN f14 14.5
  #_ REGISTER_IN f15 15.5
  #_ REGISTER_IN f16 16.5
  #_ REGISTER_IN f17 17.5
  #_ REGISTER_IN f18 18.5
  #_ REGISTER_IN f19 19.5
  #_ REGISTER_IN f20 20.5
  #_ REGISTER_IN f21 21.5
  #_ REGISTER_IN f22 22.5
  #_ REGISTER_IN f23 23.5
  #_ REGISTER_IN f24 24.5
  #_ REGISTER_IN f25 25.5
  #_ REGISTER_IN f26 26.5
  #_ REGISTER_IN f27 27.5
  #_ REGISTER_IN f28 28.5
  #_ REGISTER_IN f29 29.5
  #_ REGISTER_IN f30 30.5
  #_ REGISTER_IN f31 31.5
  bl test_millicode_fpr_save_14
  fmr f14, f0
  fmr f15, f0
  fmr f16, f0
  fmr f17, f0
  fmr f18, f0
  fmr f19, f0
  fmr f20, f0
  fmr f21, f0
  fmr f22, f0
  fmr f23, f0
  fmr f24, f0
  fmr f25, f0
  fmr f26, f0
  fmr f27, f0
  fmr f28, f0
  fmr f29, f0
  fmr f30, f0
  fmr f31, f0
  bl test_millicode_fpr_restore_14
  blr
  #_ REGISTER_OUT f14 14.5
  #_ REGISTER_OUT f15 15.5
  #_ REGISTER_OUT f16 16.5
  #_ REGISTER_OUT f17 17.5
  #_ REGISTER_OUT f18 18.5
  #_ REGISTER_OUT f19 19.5
  #_ REGISTER_OUT f20 20.5
  #_ REGISTER_OUT f21 21.5
  #_ REGISTER_OUT f22 22.5
  #_ REGISTER_OUT f23 23.5
  #_ REGISTER_OUT f24 24.5
  #_ REGISTER_OUT f25 25.5
  #_ REGISTER_OUT f26 26.5
  #_ REGISTER_OUT f27 27.5
  #_ REGISTER_OUT f28 28.5
  #_ REGISTER_OUT f29 29.5
  #_ REGISTER_OUT f30 30.5
  #_ REGISTER_OUT f31 31.5

test_millicode_fpr_save_and_restore_at_the_last_register:
  #_ REGISTER_IN r12 0x10002000
  #_ REGISTER_IN f0 0.0
  #_ REGISTER_IN f14 14.5
  #_ REGISTER_IN f15 15.5
  #_ REGISTER_IN f16 16.5
  #_ REGISTER_IN f17 17.5
  #_ REGISTER_IN f18 18.5
  #_ REGISTER_IN f19 19.5
  #_ REGISTER_IN f20 20.5
  #_ REGISTER_IN f21 21.5
  #_ REGISTER_IN f22 22.5
  #_ REGISTER_IN f23 23.5
  #_ REGISTER_IN f24 24.5
  #_ REGISTER_IN f25 25.5
  #_ REGISTER_IN f26 26.5
  #_ REGISTER_IN f27 27.5
  #_ REGISTER_IN f28 28.5
  #_ REGISTER_IN f29 29.5
  #_ REGISTER_IN f30 30.5
  #_ REGISTER_IN f31 31.5
  bl test_millicode_fpr_save_31
  fmr f14, f0
  fmr f15, f0
  fmr f16, f0
  fmr f17, f0
  fmr f18, f0
  fmr f19, f0
  fmr f20, f0
  fmr f21, f0
  fmr f22, f0
  fmr f23, f0
  fmr f24, f0
  fmr f25, f0
  fmr f26, f0
  fmr f27, f0
  fmr f28, f0
  fmr f29, f0
  fmr f30, f0
  fmr f31, f0
  bl test_millicode_fpr_restore_31
  blr
  #_ REGISTER_OUT f14 0.0
  #_ REGISTER_OUT f15 0.0
  #_ REGISTER_OUT f16 0.0
  #_ REGISTER_OUT f17 0.0
  #_ REGISTER_OUT f18 0.0
  #_ REGISTER_OUT f19 0.0
  #_ REGISTER_OUT f20 0.0
  #_ REGISTER_OUT f21 0.0
  #_ REGISTER_OUT f22 0.0
  #_ REGISTER_OUT f23 0.0
  #_ REGISTER_OUT f24 0.0
  #_ REGISTER_OUT f25 0.0
  #_ REGISTER_OUT f26 0.0
  #_ REGISTER_OUT f27 0.0
  #_ REGISTER_OUT f28 0.0
  #_ REGISTER_OUT f29 0.0
  #_ REGISTER_OUT f30 0.0
  #_ REGISTER_OUT f31 31.5

test_millicode_fpr_save_14:
  stfd f14, -144(r12)
  stfd f15, -136(r12)
  stfd f16, -128(r12)
  stfd f17, -120(r12)
  stfd f18, -112(r12)
  stfd f19, -104(r12)
  stfd f20, -96(r12)
  stfd f21, -88(r12)
  stfd f22, -80(r12)
  stfd f23, -72(r12)
  stfd f24, -64(r12)
  stfd f25, -56(r12)
  stfd f26, -48(r12)
  stfd f27, -40(r12)
  stfd f28, -32(r12)
  stfd f29, -24(r12)
  stfd f30, -16(r12)
test_millicode_fpr_save_31:
  stfd f31, -8(r12)
  blr

test_millicode_fpr_restore_14:
  lfd f14, -144(r12)
  lfd f15, -136(r12)
  lfd f16, -128(r12)
  lfd f17, -120(r12)
  lfd f18, -112(r12)
  lfd f19, -104(r12)
  lfd f20, -96(r12)
  lfd f21, -88(r12)
  lfd f22, -80(r12)
  lfd f23, -72(r12)
  lfd f24, -64(r12)
  lfd f25, -56(r12)
  lfd f26, -48(r12)
  lfd f27, -40(r12)
  lfd f28, -32(r12)
  lfd f29, -24(r12)
  lfd f30, -16(r12)
test_millicode_fpr_restore_31:
  lfd f31, -8(r12)
  blr
