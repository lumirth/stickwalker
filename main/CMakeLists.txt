idf_component_register(
    SRCS "main.c" "management.c" "board.c" "display.c" "audio.c"
         "infrared/diode.c" "infrared/infrared_esp32.c" "infrared/infrared_decoder.c" "infrared/infrared_measure.S" "save.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_app_format esp_adc esp_driver_touch_sens pokewalker nvs_flash esp_timer esp_hw_support esp_partition esp_pm
                  esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_rmt
                  esp_driver_spi esp_driver_ledc esp_driver_usb_serial_jtag
    EMBED_FILES "bmi270_config.bin")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
