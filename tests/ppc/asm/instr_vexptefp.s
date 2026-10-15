test_vexptefp_1:
  #_ REGISTER_IN v3 [40000000, 40400000, 40800000, 40A00000]
  vexptefp v4, v3
  blr
  #_ REGISTER_OUT v3 [40000000, 40400000, 40800000, 40A00000]
  #_ REGISTER_OUT v4 [40800000, 41000000, 41800000, 42000000]

test_vexptefp_underflow:
  # -inf, -1000, -127, -126: below 2^-126 flushes to 0, 2^-126 itself is exact
  #_ REGISTER_IN v3 [FF800000, C47A0000, C2FE0000, C2FC0000]
  vexptefp v4, v3
  blr
  #_ REGISTER_OUT v4 [00000000, 00000000, 00000000, 00800000]

test_vexptefp_overflow:
  # 127, 128, 1000, +inf: 2^127 is exact, from 2^128 up +inf
  #_ REGISTER_IN v3 [42FE0000, 43000000, 447A0000, 7F800000]
  vexptefp v4, v3
  blr
  #_ REGISTER_OUT v4 [7F000000, 7F800000, 7F800000, 7F800000]

test_vexptefp_nan:
  # NaN passes through, the other lanes are unaffected
  #_ REGISTER_IN v3 [7FC00000, 3F800000, 00000000, BF800000]
  vexptefp v4, v3
  blr
  #_ REGISTER_OUT v4 [7FC00000, 40000000, 3F800000, 3F000000]
