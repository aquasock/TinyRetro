// Target definition: Arduino Nano ESP32 + Adafruit 2.2" PiTFT HAT (ILI9340)
//
// The same hardware the TinyDesk LCD port drives, so the pin choices here are
// deliberately identical to that port's ili9340.h. See its README for the two
// wiring mistakes that cost time: D/C is Pi header pin 22 (GPIO25), not 18,
// and 5 V for the backlight comes from VBUS because VIN is a 6-21 V buck input.
#define RG_TARGET_NAME             "NANO-ESP32"

// Storage: no SD card on this rig, so ROMs live in a FAT partition in flash.
// rg_tool.py adds it with --fatsize, under the label below.
#define RG_STORAGE_ROOT             "/fs"
#define RG_STORAGE_FLASH_PARTITION  "vfs"

// Audio: the ESP32-S3 has no internal DAC, and nothing is wired to I2S here.
// Silent until a DAC is added; the emulators run either way.
#define RG_AUDIO_USE_INT_DAC        0   // 0 = Disable, 1 = GPIO25, 2 = GPIO26, 3 = Both
#define RG_AUDIO_USE_EXT_DAC        0   // 0 = Disable, 1 = Enable

// Video: ILI9340, 320x240, on the ESP32-S3's hardware SPI pins.
// The HAT is designed for 32 MHz (see Adafruit's pitft22 device-tree overlay),
// so the clock is held there rather than the 40 MHz the S3 devkit target uses.
#define RG_SCREEN_DRIVER            0   // 0 = ILI9341/ST7789 (compatible command set)
#define RG_SCREEN_HOST              SPI2_HOST
#define RG_SCREEN_SPEED             32000000
#define RG_SCREEN_BACKLIGHT         0   // the HAT's NPN is pulled up; no GPIO needed
#define RG_SCREEN_WIDTH             320
#define RG_SCREEN_HEIGHT            240
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0}
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}
// The ILI9340 shares its command set with the ILI9341 for everything the
// launcher and emulators use. Power and gamma registers differ, and this panel
// was proven with the short sequence below rather than the ILI9341 full one.
#define RG_SCREEN_INIT()                                                                                         \
    ILI9341_CMD(0x3A, 0x55);                 /* 16 bits per pixel */                                            \
    ILI9341_CMD(0x36, 0x28);                 /* Memory Access Control (MV|BGR): landscape */


// Input: the HAT's four tactile switches, each closing to GND.
//
// They are on the HAT's PI GPIO numbers, which are not ESP32 GPIOs -- the
// S3 has no GPIO22-25 at all. Wire the HAT's 2x20 header pins to free Nano
// pins and use the ESP32 numbers here:
//
//   HAT pin 11  Pi GPIO17  (SW4)  ->  Nano D5  GPIO 8   = B
//   HAT pin 13  Pi GPIO27  (SW1)  ->  Nano D2  GPIO 5   = UP
//   HAT pin 15  Pi GPIO22  (SW3)  ->  Nano D4  GPIO 7   = A
//   HAT pin 16  Pi GPIO23  (SW2)  ->  Nano D3  GPIO 6   = DOWN
//
// The four on-board switches are unused in favour of the Wii adapter below,
// so this map is kept only as a fallback and is not defined.

// I2C: the Wii Nunchuck Breakout Adapter, address 0x52. Hardware I2C on the
// Nano's A4/A5. The adapter is level shifted with its own pull-ups, and is
// powered from 3V3 so the host lines stay at 3.3 V.
#define RG_GPIO_I2C_SDA             GPIO_NUM_11   /* A4 */
#define RG_GPIO_I2C_SCL             GPIO_NUM_12   /* A5 */

// Input source: a Wii extension controller (Classic Controller or nunchuck).
// Defined only to enable the driver in rg_input.c; the button mapping lives
// in rg_wiimote.c because the protocol, not a pin table, decides the buttons.
#define RG_GAMEPAD_WII_MAP
// The raw report is logged on every change when RG_WIIMOTE_TRACE is defined.
// It is off by default: the mapping below is confirmed against real presses on
// a genuine Classic Controller (A, B and both d-pad directions), and the trace
// writes 60+ lines a second, which swamps the console.
// #define RG_WIIMOTE_TRACE
// The four on-board switches are left unmapped on purpose: RG_GAMEPAD_GPIO_MAP
// would read Nano pins D2..D5, which are free and floating here, so defining it
// would inject phantom presses.

// Battery: nothing is wired to a divider, so reporting is disabled.
#define RG_BATTERY_DRIVER           0

// Status LED: the Nano's RGB LED is on GPIO 48/46/45; 48 doubles as the SPI
// clock here, so no status LED is defined.
// #define RG_GPIO_LED              GPIO_NUM_46

// SPI display, matching ports/nanoesp32 in the TinyDesk tree.
#define RG_GPIO_LCD_MISO            GPIO_NUM_47
#define RG_GPIO_LCD_MOSI            GPIO_NUM_38
#define RG_GPIO_LCD_CLK             GPIO_NUM_48
#define RG_GPIO_LCD_CS              GPIO_NUM_21
#define RG_GPIO_LCD_DC              GPIO_NUM_18
// No RG_GPIO_LCD_RST: the HAT drives RESET from its own supervisor (IC1).
// No RG_GPIO_LCD_BCKL: the HAT's backlight NPN is pulled up, on with 5 V.
