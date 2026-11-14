test_fadd_1:
  #_ REGISTER_IN f1 1.0
  #_ REGISTER_IN f2 2.0
  fadd f3, f1, f2
  blr
  #_ REGISTER_OUT f3 3.0

test_fadd_2:
  #_ REGISTER_IN f1 0.0
  #_ REGISTER_IN f2 0.0
  fadd f3, f1, f2
  blr
  #_ REGISTER_OUT f3 0.0

test_fadd_3:
  #_ REGISTER_IN f1 -200.0
  #_ REGISTER_IN f2 200.0
  fadd f3, f1, f2
  blr
  #_ REGISTER_OUT f3 0.0

test_fadd_fpu_keeps_ieee_denormal_result:
  # 2^-1022 + -(2^-1022 - 2^-1074) = 2^-1074, the smallest denormal
  #_ REGISTER_IN f1 0x0010000000000000
  #_ REGISTER_IN f2 0x800FFFFFFFFFFFFF
  fadd f3, f1, f2
  blr
  #_ REGISTER_OUT f3 0x0000000000000001

test_fadd_fpu_keeps_ieee_denormal_result_negative:
  #_ REGISTER_IN f1 0x8010000000000000
  #_ REGISTER_IN f2 0x000FFFFFFFFFFFFF
  fadd f3, f1, f2
  blr
  #_ REGISTER_OUT f3 0x8000000000000001

test_fadd_fpu_keeps_ieee_denormal_input:
  #_ REGISTER_IN f1 0x0000000000000001
  #_ REGISTER_IN f2 0x0000000000000001
  fadd f3, f1, f2
  blr
  #_ REGISTER_OUT f3 0x0000000000000002
