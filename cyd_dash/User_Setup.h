// Copiare in TFT_eSPI/User_Setup.h prima di compilare lo sketch.
// Configurazione testata con ESP32-2432S028R, display 2.8", touch XPT2046.
#define USER_SETUP_INFO "CYD ESP32-2432S028R"
#define ILI9341_2_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
#define TFT_INVERSION_ON
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
#define SPI_FREQUENCY       55000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000
#define USE_HSPI_PORT
