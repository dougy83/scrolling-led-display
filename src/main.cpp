/*
Side scrolling LED matrix display ("subway sign")
7 rows x 60 cols x 7 modules (was 8 modules, but one's been removed)

TODO: get actual pin number
Connector pinout:
5V      - 5V supply from the display boards
GND     - common ground
R2      - row select bit 2
R1      - row select bit 1
R0      - row select bit 0
/OE     - output enable (drive LEDs)
latch   - load shifted data from shift register to output latch
clk     - clock next data bit into shift register
data    - data to go in the shift register

To display something, the row data is shifted in, latch, output enabled (measured as roughly: 300us ON, 600us off for each of the 7 rows, then 10ms gap between frames).
The R0..2 bits and the /OE pin probably enable drive for LEDs of that row
Sample board used 20MHz clock, which worked, but it's bearly enough drive and waveform was sawtooth; also there was 2us of gap between bytes, so could simply go 1/3 the bitrate and have same average transfer speed.


This program:
- boots into AP mode, which allows configuring; will stay in this mode as long as there's a connection. SSID might be HSBNE-display, and password might be helloyellow
- if we can connect to wifi LAN (i.e. it's configured and available), we'll do that
- if we can't connect, we can just run the last animation that was saved
- can configure via http at hsbne-display.local (either AP or STA modes)

technical:
- use a 100us timer interrupt as the timebase for all output, e.g. control signals and queuing SPI transfers
- high priority task manages updating the bitmap to be displayed; signal via flag
    - or just do all scrolling in the timer interrupt; it's not going to take long (maybe it will)
- use adafruit graphics library for drawing to the bitmap
- allow config via the web interface using strings of the format:
    [start scroll][font arial][scrolldelay 10]HELLO ALL!
    [start scroll][font arial][scrolldelay 10]Welcome to HSBNE
    - this should scroll "HELLO ALL!" completely, then scroll "Welcome to HSBNE" completely, then repeat

*/


#include <Arduino.h>

#define LC_INCLUDE "lc-addrlabels.h"
#include "pt-1.4/pt.h"
#include "Adafruit_GFX.h"

#define LINES   7
#define MODULE_COLUMNS  60  // 60 LED columns, but 64 shift register outputs - data may need padding
#define MODULES 7           // was 8 display modules, but ones been removed
#define COLUMNS (MODULE_COLUMNS * MODULES)
#define MODULE_PADDING 4
#define COLUMNS_PADDED (COLUMNS + MODULE_PADDING * MODULES)

uint8_t *pixelData[2][LINES][COLUMNS_PADDED] = {};  // double buffered, ready for shifting (i.e. prepadded, etc.). Data must have minimum col count
bool displaySecondBuffer = 0;       // display from second buffer

// scroll the bitmap one pixel, left or right, wrapping around. For normal scroll, it makes sense to make the bitmap content width + display width
void scrollBitmap(GFXcanvas1 *canvas, bool left)
{
    int16_t width, height;
    canvas->getSize(width, height);
    
    uint8_t *buffer;
    canvas->getBuffer();

    for (int y = 0; y < height; y++)
    {
        // data is stored MSb-first
        int blocks = (width + 7) / 8;       // 8 bit blocks
        uint8_t *ptr = &buffer[y * blocks]; // row ptr

        bool bit = left ? (ptr[0] & 0x80) : (ptr[0] & 0x01);
        for (int xb = 0; xb < blocks; xb++)
        {
            if (left)
            {
                uint8_t *p = &ptr[blocks - xb - 1];
                bool temp = *p & 0x80;
                *p = (*p << 1) | bit;
                bit = temp; 
            }
            else
            {
                uint8_t *p = &ptr[xb];
                bool temp = *p & 1;
                *p = (*p >> 1) | (bit << 7);
                bit = temp; 
            }
        }
    }
}

// put function declarations here:
int myFunction(int, int);

void setup() {
  // put your setup code here, to run once:
  int result = myFunction(2, 3);
}

void loop() {
  // put your main code here, to run repeatedly:
}

// put function definitions here:
int myFunction(int x, int y) {
  return x + y;
}