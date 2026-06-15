// Lite3DP Gen 2 — TFT_eSPI display configuration
// ST7796 480×320 display on SPI2 (VSPI)

#define USER_SETUP_INFO "Lite3DP-G2"

// Driver
#define ST7796_DRIVER
#define TFT_INVERSION_ON

// SPI pins (shared with SD card and XPT2046 touch, same bus different CS)
#define TFT_MISO 19
#define TFT_MOSI 23
#define TFT_SCLK 18
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST   4

// Fonts
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// SPI frequencies
#define SPI_FREQUENCY       27000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000
