# S2 ROM CDC compatibility component

Copied from ESP-IDF commit 25fe69f946311abdaf9ad56591f25fedbc20ac98.
Only ESP32-S2 selects this override. The single SDK change calls
`rom_usb_cdc_set_descriptor_patch()` during ROM cleanup, ensuring descriptors
point into application memory rather than reclaimed bootloader RAM.
See https://github.com/espressif/esp-idf/issues/18841. Preserve the upstream
licenses. Reassess the override when updating the S2 SDK pin.
