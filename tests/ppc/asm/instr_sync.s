# sync, lwsync and eieio are memory barriers: they order accesses between
# threads and must leave every register as it was.

test_sync:
  #_ REGISTER_IN r3 0x0123456789ABCDEF
  sync
  blr
  #_ REGISTER_OUT r3 0x0123456789ABCDEF

test_lwsync:
  #_ REGISTER_IN r3 0x0123456789ABCDEF
  lwsync
  blr
  #_ REGISTER_OUT r3 0x0123456789ABCDEF

test_eieio:
  #_ REGISTER_IN r3 0x0123456789ABCDEF
  eieio
  blr
  #_ REGISTER_OUT r3 0x0123456789ABCDEF
