test_mmio_eieio_store:
  #_ MEMORY_IN 10001050 CCCCCCCC
  #_ REGISTER_IN r4 0x10001050
  #_ REGISTER_IN r5 0x01020304
  stw r5, 0(r4)
  eieio
  blr
  #_ REGISTER_OUT r5 0x01020304
  #_ MEMORY_OUT 10001050 01020304

test_mmio_eieio_load:
  #_ MEMORY_IN 10001050 0A0B0C0D
  #_ REGISTER_IN r4 0x10001050
  lwz r3, 0(r4)
  eieio
  blr
  #_ REGISTER_OUT r3 0x0A0B0C0D

test_mmio_eieio_load64:
  #_ MEMORY_IN 10001050 01020304 05060708
  #_ REGISTER_IN r4 0x10001050
  ld r3, 0(r4)
  eieio
  blr
  #_ REGISTER_OUT r3 0x0102030405060708

test_mmio_offset_load:
  #_ MEMORY_IN 10001050 11223344
  #_ REGISTER_IN r3 0xE40E0414
  addis r11, r3, 0x1FF2
  slwi r11, r11, 2
  lwz r3, 0(r11)
  blr
  #_ REGISTER_OUT r3 0x11223344

test_mmio_matched_null_load:
  #_ REGISTER_IN r3 0x12345678
  lis r11, 0x7FC8
  lwz r3, 0x10(r11)
  blr
  #_ REGISTER_OUT r3 0

test_mmio_eieio_last_block_word:
  #_ REGISTER_IN r3 0x12345678
  #_ REGISTER_IN r4 0x7FC8FFFC
  lwz r3, 0(r4)
  eieio
  blr
  #_ REGISTER_OUT r3 0

test_mmio_eieio_after_block:
  #_ MEMORY_IN 7FC90000 55667788
  #_ REGISTER_IN r4 0x7FC90000
  lwz r3, 0(r4)
  eieio
  blr
  #_ REGISTER_OUT r3 0x55667788
