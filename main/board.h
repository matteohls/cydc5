#pragma once
/*
 * NM-CYD-C5 (RockBase) pin map.
 * Source: pin map attributed to the NM-CYD-C5 v1.0 schematic (via bradleyeckert/LiteForth PR #272),
 * cross-checked against wiki.rockbaseiot.com for MOSI/SCLK/MISO/CS/backlight/SD.
 * DC (GPIO24) appears only in the first source -> verify against the schematic if the screen stays black.
 */
#define PIN_SPI_MOSI   7
#define PIN_SPI_SCLK   6
#define PIN_SPI_MISO   2   /* shared by SD (and touch); also a strapping pin */

#define PIN_LCD_CS     23
#define PIN_LCD_DC     24
#define PIN_LCD_BL     25
#define PIN_SD_CS      10
#define PIN_TOUCH_CS   1   /* must be held HIGH, otherwise XPT2046 fights for MISO */
#define PIN_TOUCH_IRQ  3   /* low while touched (from one source only - verify if touch never responds) */

#define LCD_HOST_ID    SPI2_HOST
#define LCD_W          320
#define LCD_H          240
/* 80 MHz is needed for a scaled picture (128 KB/frame). If the screen shows noise, try 40 MHz (then expect ~39 fps) */
#define LCD_PCLK_HZ    (80 * 1000 * 1000)

/* Size of the game picture on screen. 267x240 = full height, correct aspect ratio.
 * If it is too slow, use 240x216 (1.5x). Max is 320x240. */
#define GAME_OUT_W     200   /* 1.25x, leaves 60 px left/right and 60 px at the bottom for big touch buttons */
#define GAME_OUT_H     180

/*
 * Panel orientation / colour. These are NOT verified on real hardware.
 * If the picture is mirrored, flip MIRROR_X / MIRROR_Y.
 * If colours look inverted (negative), flip LCD_INVERT.
 * If red and blue are swapped, flip LCD_BGR.
 */
#define LCD_SWAP_XY    1
#define LCD_MIRROR_X   0
#define LCD_MIRROR_Y   1
#define LCD_INVERT     1
#define LCD_BGR        0

#define SHOW_FPS       0   /* the picture now covers the margins; fps is still logged on the UART console */
#define STREAM_EVERY   2   /* send every Nth frame to the browser (2 => ~30 fps) */