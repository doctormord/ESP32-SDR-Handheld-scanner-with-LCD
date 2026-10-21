ASSERT(_iram_end <= 0x40060000, "S2 RF dump overlaps IRAM")
ASSERT(_data_end <= 0x3fff0000, "S2 RF dump overlaps data")
ASSERT(_bss_end <= 0x3fff0000, "S2 RF dump overlaps BSS")
