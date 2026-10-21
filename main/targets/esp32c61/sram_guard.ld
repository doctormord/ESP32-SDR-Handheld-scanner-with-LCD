/* Reserve bank 3 for capture; leave the final bank and ROM data CPU-owned. */
ASSERT(_iram_end <= 0x40820000, "C61 RF dump overlaps IRAM")
ASSERT(_data_end <= 0x40820000, "C61 RF dump overlaps data")
ASSERT(_bss_end <= 0x40820000, "C61 RF dump overlaps BSS")
