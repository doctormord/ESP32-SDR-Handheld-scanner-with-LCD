ASSERT(_iram_end <= 0x40820000, "C6 dump overlaps IRAM")
ASSERT(_data_end <= 0x40820000, "C6 dump overlaps data")
ASSERT(_bss_end <= 0x40820000, "C6 dump overlaps BSS")
