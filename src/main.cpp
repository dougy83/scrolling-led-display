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

To display something, the row data is shifted in, latch, output enabled (measured as roughly: 300us ON, 600us off for each of the 7 rows, then 10ms gap between frames). 60Hz frame rate
The R0..2 bits and the /OE pin probably enable drive for LEDs of that row
Sample board used 20MHz clock, which worked, but it's bearly enough drive and waveform was sawtooth; also there was 2us of gap between bytes, so could simply go 1/3 the bitrate and have same average transfer speed.


This program:
- boots into AP mode, which allows configuring; will stay in this mode as long as there's a connection. SSID might be HSBNE-display, and password might be helloyellow
- if we can connect to wifi LAN (i.e. it's configured and available), we'll do that
- if we can't connect, we can just run the last animation that was saved
- can configure via http at hsbne-display.local (either AP or STA modes)

technical:
- use a 300us timer interrupt as the timebase for all output, wake hi prio task
    - hi prio task manages:
        - updating the bitmap to be displayed, and 
        - control signals and queuing SPI transfers
- low prio task does the logic for the network, and forwards changes of the string to the high prio task
    - web interface have index.html for setting the string via UI, and single API endpoint for setting the text and scroll rate
- use adafruit graphics library for drawing to the bitmap
*/

#include <Arduino.h>
#include "ScrollingDisplay.h"

void setup()
{
    ScrollingDisplay.begin();
    ScrollingDisplay.setScrollRate(50);
    ScrollingDisplay.setText("Wrote 273984 bytes (153433 compressed) at 0x00010000 in 2.0 seconds (effective 1084.5 kbit/s)...");

    pinMode(8, OUTPUT);
}

void loop()
{
    digitalWrite(8, LOW);
    delay(300);
    digitalWrite(8, HIGH);    
    delay(300); 
}