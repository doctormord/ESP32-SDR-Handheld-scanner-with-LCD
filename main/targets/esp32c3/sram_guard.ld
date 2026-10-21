ASSERT(_iram_end <= 0x403a0000, "C3 RF dump overlaps IRAM")
ASSERT(_data_end <= 0x3fca0000, "C3 RF dump overlaps data")
ASSERT(_bss_end <= 0x3fca0000, "C3 RF dump overlaps BSS")
