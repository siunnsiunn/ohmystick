idf_component_register(
    SRCS "main.c" "lcd.c" "statusbar.c" "screens.c" "display.c" "download.c" "pmic.c" "audio.c" "imu.c"
         "usb_audio.c" "buttons.c" "apps.c" "timer.c" "beep.c" "diag.c" "monitor.c" "usb_descriptors.c" "usb_recovery.c" "vox_config.c"
         "ble_presenter.c" "esp_hid_gap.c" "btmic.c" "inbox.c" "zh_font.c" "wifimic.c"
    PRIV_REQUIRES driver
                  esp_system
                  esp_driver_i2c
                  esp_driver_spi
                  esp_codec_dev
                  esp_lcd
                  espressif__tinyusb
                  espressif2022__bmi270
                  espressif__i2c_bus
                  nvs_flash
                  esp_hid
                  bt
                  esp_wifi
                  esp_netif
                  esp_event
                  esp_http_server
    INCLUDE_DIRS "."
)

option(VOX_ENABLE_BLE_PRESENTER
       "BLE HID for presenter and BT dictation modes (menu); OFF builds a USB-only image" ON)
option(VOX_DEBUG_NO_UAC "Keep USB-Serial/JTAG alive by skipping UAC/HID init" OFF)
option(VOX_DIAG_CODEC_FAIL_TONE
       "Diagnostic only: emit a tone over UAC when codec init fails" OFF)
set(VOX_AUTO_DOWNLOAD_AFTER_SEC "0" CACHE STRING
    "If >0, firmware reboots itself into ROM download mode after this many seconds")
set(VOX_CODEC_FAIL_DOWNLOAD_AFTER_SEC "0" CACHE STRING
    "If >0, codec init failure reboots into ROM download mode after this many seconds")
set(VOX_DEBUG_NO_UAC_AUTO_DOWNLOAD_SEC "180" CACHE STRING
    "NO_UAC diagnostic safety timer; set to 0 only when another recovery path is available")

if(VOX_DEBUG_NO_UAC AND
   VOX_AUTO_DOWNLOAD_AFTER_SEC STREQUAL "0" AND
   NOT VOX_DEBUG_NO_UAC_AUTO_DOWNLOAD_SEC STREQUAL "0")
    message(STATUS
        "VOX_DEBUG_NO_UAC safety: auto-download after "
        "${VOX_DEBUG_NO_UAC_AUTO_DOWNLOAD_SEC}s")
    set(VOX_AUTO_DOWNLOAD_AFTER_SEC "${VOX_DEBUG_NO_UAC_AUTO_DOWNLOAD_SEC}")
endif()

set(VOX_BLE_VARIANT "" CACHE STRING "Overnight test variant number (empty = normal build)")
set(VOX_BLE_JUST_WORKS "0" CACHE STRING "1 = Just Works, 0 = passkey on LCD")
if(NOT VOX_BLE_VARIANT STREQUAL "")
    math(EXPR VOX_BLE_PID_DEC "18944 + ${VOX_BLE_VARIANT}")   # 0x4A00 + n
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        VOX_BLE_VARIANT=${VOX_BLE_VARIANT}
        VOX_BLE_PID=${VOX_BLE_PID_DEC}
        "VOX_BLE_NAME=\"SS3-V${VOX_BLE_VARIANT}\"")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE VOX_BLE_JUST_WORKS=${VOX_BLE_JUST_WORKS})

if(VOX_ENABLE_BLE_PRESENTER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VOX_ENABLE_BLE_PRESENTER=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VOX_ENABLE_BLE_PRESENTER=0)
endif()

if(VOX_DEBUG_NO_UAC)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VOX_DEBUG_NO_UAC=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VOX_DEBUG_NO_UAC=0)
endif()

if(VOX_DIAG_CODEC_FAIL_TONE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VOX_DIAG_CODEC_FAIL_TONE=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE VOX_DIAG_CODEC_FAIL_TONE=0)
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    VOX_AUTO_DOWNLOAD_AFTER_SEC=${VOX_AUTO_DOWNLOAD_AFTER_SEC}
    VOX_CODEC_FAIL_DOWNLOAD_AFTER_SEC=${VOX_CODEC_FAIL_DOWNLOAD_AFTER_SEC}
)

# CONFIG_USB_DEVICE_UAC_AS_PART=y means usb_device_uac no longer feeds tinyusb
# its own tusb_config.h / usb_descriptors.c — we provide both from main/. The
# tusb_uac/ helper headers (uac_config.h, uac_descriptors.h, tusb_config_uac.h)
# still hold the EP-size / channel-count macros we want to reuse, so wire both
# directories into tinyusb's public includes.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
idf_component_get_property(uac_dir  espressif__usb_device_uac COMPONENT_DIR)
if(tusb_lib AND uac_dir)
    target_include_directories(${tusb_lib} BEFORE PUBLIC
        "${COMPONENT_DIR}"
        "${uac_dir}/tusb_uac")
endif()

# Record the crash address in RTC memory (see __wrap_esp_panic_handler in main.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
