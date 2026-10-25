test_tail_chain_runs_a_million_hops:
  lis r3, 0xF
  ori r3, r3, 0x4240
  li r4, 0
  b test_tail_chain_ping
  #_ REGISTER_OUT r3 0
  #_ REGISTER_OUT r4 0x16E360

test_tail_chain_ping:
  cmplwi r3, 0
  beqlr
  addi r3, r3, -1
  addi r4, r4, 1
  b test_tail_chain_pong

test_tail_chain_pong:
  cmplwi r3, 0
  beqlr
  addi r3, r3, -1
  addi r4, r4, 2
  b test_tail_chain_ping
