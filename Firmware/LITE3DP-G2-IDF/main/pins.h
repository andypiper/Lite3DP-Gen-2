#pragma once
#include "driver/gpio.h"

// Stepper motor (DRV8825)
#define PIN_DIR     GPIO_NUM_26
#define PIN_STEP    GPIO_NUM_27
#define PIN_EN      GPIO_NUM_25

// Endstop
#define PIN_ENDSTOP GPIO_NUM_36

// UV LED (PWM via LEDC)
#define PIN_UV      16

// SD card (SPI, shares bus with TFT)
#define PIN_SD_CS   5

// SD card detect (LOW = card present)
#define PIN_SD_DET  GPIO_NUM_34

// Play/Pause button (LOW = pressed)
#define PIN_BTN_PLAY GPIO_NUM_39

// Touchscreen interrupt (XPT2046)
#define PIN_T_CS    17
#define PIN_T_IRQ   35

// TFT SPI pins are defined in components/tft_espi/User_Setup.h
