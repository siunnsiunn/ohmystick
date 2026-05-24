idf_component_register(
    SRCS "main.c" "usb_descriptors.c" "usb_recovery.c" "vox_config.c"
    PRIV_REQUIRES driver
                  esp_driver_i2c
                  esp_driver_spi
                  esp_codec_dev
                  esp_lcd
                  espressif__tinyusb
                  espressif2022__bmi270
                  espressif__i2c_bus
                  nvs_flash
    INCLUDE_DIRS "."
)

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
