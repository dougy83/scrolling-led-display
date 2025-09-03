#ifndef __ScrollingDisplay_h__
#define __ScrollingDisplay_h__

#include <Arduino.h>

class ScrollingDisplayIntf
{
public:
    void begin();
    void setText(const String &s);
    void setScrollRate(int pixelShiftDelayMillis);

    // IO definitions
    struct PinDefs
    {
        static const int
            cs = 7,   // Chip Select pin
            r0 = 1,   // row sel 0
            r1 = 2,   // row sel 0
            r2 = 3,   // row sel 0
            clk = 4,  // SPI SCK
            data = 6, // SPI MOSI
            oe = 0;   // enable display output
    };
};

extern ScrollingDisplayIntf ScrollingDisplay;
#endif // __ScrollingDisplay_h__
