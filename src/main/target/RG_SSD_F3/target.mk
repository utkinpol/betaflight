F3_TARGETS  += $(TARGET)
FEATURES    = VCP SDCARD_SPI

TARGET_SRC = \
            drivers/accgyro/accgyro_mpu.c \
            drivers/accgyro/accgyro_spi_mpu6000.c 

HSE_VALUE      = 12000000


