idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_lcd fatfs sdmmc esp_timer vfs)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-function -Wno-format-truncation)
