idf_component_register(
    SRCS "main.cpp" "hostfile_sd.cpp" "platform_stubs.cpp" "lcd_mipi.cpp" "panel_tab5.cpp" "usb_kbd.cpp" "audio_codec.cpp" "menu_disk.cpp" "fdd_ext.cpp" "touch_mouse.cpp" "rtc_tab5.cpp" "screenshot.cpp" "bt_hid.cpp" "usb_msc.cpp" "usb_image.cpp" "sd_tab5.cpp"
    INCLUDE_DIRS "."
    # The compatible BIOS and font, built into the image rather than read off
    # the SD card. The card is still searched first when the menu names a file,
    # but a stock unit needs no card contents at all - which is what makes an
    # M5Burner image work out of the box, since that ships a firmware image and
    # nothing else. 96KB + 282KB of flash, memory-mapped, costing no RAM.
    EMBED_FILES "rom/PC98N.ROM" "rom/FONT_ESP.ROM" "rom/HDDROM.BIN"
    # No arduino-esp32, no bt/esp_hid: the P4 has no on-die radio (Bluetooth
    # would go through the companion ESP32-C6 over esp-hosted, which is a later
    # phase). espressif__m5stack_tab5 provides the board support.
    REQUIRES pc98core fatfs sdmmc esp_lcd usb driver nvs_flash esp_app_format
             espressif__m5stack_tab5 espressif__usb_host_hid espressif__tinyusb espressif__esp_lcd_st7121 espressif__esp_lcd_st7123 espressif__esp_lcd_ili9881c espressif__esp_lcd_touch
             esp_driver_i2s esp_psram esp_driver_ppa bt esp_hid esp_hosted
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=incompatible-pointer-types -Wno-incompatible-pointer-types
    -Wno-error=int-conversion -Wno-error=implicit-function-declaration
    -Wno-error=format -Wno-error=format-truncation -Wno-error=return-type -fno-strict-aliasing)
if(PC98_NEWCPU)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PC98_NEWCPU=${PC98_NEWCPU})
endif()
