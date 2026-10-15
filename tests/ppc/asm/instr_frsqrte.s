test_frsqrte_1:
  #_ REGISTER_IN f1 1.0
  frsqrte f1, f1
  blr
  #_ REGISTER_OUT f1 0.97070312500000000

test_frsqrte_2:
  #_ REGISTER_IN f1 64.0
  frsqrte f1, f1
  blr
  #_ REGISTER_OUT f1 0.12133789062500000

test_frsqrte_3:
  #_ REGISTER_IN f1 0.5
  frsqrte f1, f1
  blr
  #_ REGISTER_OUT f1 1.37500000000000000
