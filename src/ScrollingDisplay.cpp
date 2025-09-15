#include "ScrollingDisplay.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/timer.h"

#include "Adafruit_GFX.h"
#include "Font5x7Fixed.h"

#include <driver/spi_master.h>
#include "esp_attr.h"

#include <atomic>
#include <memory>

// display characteristics
#define ROWS 7
#define MODULE_COLUMNS 60 // 60 LED columns, but 64 shift register outputs
#define MODULES 7         // was 8 display modules, but ones been removed
#define COLUMNS (MODULE_COLUMNS * MODULES)

// timer resource alloc stuff
#define TIMER_GROUP TIMER_GROUP_0
#define TIMER_IDX TIMER_0
#define TIMER_DIVIDER 80 // 80 MHz / 80 = 1 MHz (1 tick = 1 µs)
#define TIMER_INTERVAL_US 300   // this is what I'm referring to as "ticks"

#define FRAME_RATE 60
//#define TICKS_PER_ROW ((1000000 / FRAME_RATE / TIMER_INTERVAL_US + ROWS / 2) / ROWS) 
//#define TICKS_PER_TRANSACTION (((COLUMNS * 1000000UL + SPI_SPEED - 1) / SPI_SPEED + TIMER_INTERVAL_US - 1) / TIMER_INTERVAL_US)
#define TICKS_PER_TRANSACTION 3
#define TICKS_PER_FRAME (1000000 / FRAME_RATE / TIMER_INTERVAL_US)

// SPI
#define SPI_HOST SPI2_HOST  // use HSPI
#define SPI_SPEED 2000000
static void *dmaBuff = nullptr;
static spi_transaction_t spiTrans = {};

// stuff for our task
static String text("Hello");
static std::atomic<bool> updateText(true);
static std::atomic<int> scrollDelay(50);
static std::atomic<uint32_t> tickCount(0);

static spi_device_handle_t spi = nullptr;
static TaskHandle_t highPrioTaskHandle = nullptr;

// forward refs
void scrollBitmap(GFXcanvas1 *canvas, bool left);
void initSPI();
void transmitSPI(void *data, size_t length);
int getTextWidth(const GFXfont *f, const String &text);

// periodic timer wakes our high prio task every 300us to allow for shorter non-blocking delays
bool IRAM_ATTR onTimer(void *arg)
{
    BaseType_t needToYield = pdFALSE;
    if (highPrioTaskHandle)
    {
        vTaskNotifyGiveFromISR(highPrioTaskHandle, &needToYield);
    }

    tickCount++;

    return needToYield;
}


// waits for a timer trigger tick
void inline tick(int count = 1)
{
    while (count--)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

// High priority task
void highPrioTask(void *pvParameters)
{
    std::unique_ptr<GFXcanvas1> canvas(new GFXcanvas1(COLUMNS, ROWS));
    uint32_t lastScroll = tickCount;
    
    for (;;)
    {
        if (updateText)
        {
            auto fontPtr = &Font5x7Fixed;
            int w = getTextWidth(fontPtr, text);
            canvas = std::unique_ptr<GFXcanvas1>(new GFXcanvas1(max(COLUMNS, w), ROWS));

            canvas->setFont(fontPtr);
            canvas->setTextColor(1);
            canvas->setCursor(0,7); // font is offset (default font is not)
            canvas->print(text.c_str());

            updateText = false;
        }

        for (int r = 0; r < ROWS; r++)
        {
            using PinDefs = ScrollingDisplayIntf::PinDefs;

            // select row
            digitalWrite(PinDefs::r0, !!(r & 1));
            digitalWrite(PinDefs::r1, !!(r & 2));
            digitalWrite(PinDefs::r2, !!(r & 4));

            // send the data
            int16_t w, h;
            canvas->getSize(w, h);
            int span = (w + 7) / 8;
            auto ptr = &canvas->getBuffer()[r * span];
            size_t count = min((COLUMNS + 7) / 8, span);   // only transfer the bit we can see
            transmitSPI(ptr, count);

            tick(TICKS_PER_TRANSACTION);    // SPI will transfer in this time

            digitalWrite(PinDefs::oe, LOW);     // LEDs on
            tick();
            digitalWrite(PinDefs::oe, HIGH);     // LEDs off
        }

        // delay for the rest of the frame
        tick(TICKS_PER_FRAME - ROWS * (TICKS_PER_TRANSACTION + 1));

        // scroll needed?
        if ((tickCount - lastScroll) * TIMER_INTERVAL_US > scrollDelay * 1000)
        {
            lastScroll += scrollDelay * 1000 / TIMER_INTERVAL_US;  // constant scrolling timebase
            scrollBitmap(canvas.get(), true);
        }
    }
}

// scroll the bitmap one pixel, left or right, wrapping around
void scrollBitmap(GFXcanvas1 *canvas, bool left)
{
    int16_t width, height;
    canvas->getSize(width, height);

    uint8_t *buffer = canvas->getBuffer();

    for (int y = 0; y < height; y++)
    {
        // data is stored MSb-first
        int blocks = (width + 7) / 8;       // 8 bit blocks
        uint8_t *ptr = &buffer[y * blocks]; // row ptr

        bool bit = left ? (ptr[0] & 0x80) : (ptr[blocks - 1] & 0x01);
        for (int xb = 0; xb < blocks; xb++)
        {
            if (left)
            {
                uint8_t *p = &ptr[blocks - xb - 1]; // reverse
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


void transmitSPI(void *data, size_t length) {
    constexpr size_t buffSize = (COLUMNS + 7) / 8;
    if (length > buffSize)
    {
        length = buffSize;
    }

    if (!dmaBuff)
    {
        dmaBuff = heap_caps_malloc(buffSize, MALLOC_CAP_DMA);
    }

    if (dmaBuff)
    {
        spi_transaction_t *result;
        spi_device_get_trans_result(spi, &result, 0);   // purge

        memset(&spiTrans, 0, sizeof(spiTrans));
        spiTrans.length = length * 8;     // bits
        spiTrans.tx_buffer = dmaBuff;
        memcpy(dmaBuff, data, length);

        esp_err_t ret = spi_device_queue_trans(spi, &spiTrans, 0);
        if (ret != ESP_OK) {
            // queue full, handle if needed
        }
    }
}

void initSPI() {
    using PinDefs = ScrollingDisplayIntf::PinDefs;

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = PinDefs::data;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = PinDefs::clk;
    buscfg.quadhd_io_num = -1;
    buscfg.quadwp_io_num = -1;
    buscfg.max_transfer_sz = 4096;

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = SPI_SPEED;
    devcfg.mode = 0;
    devcfg.spics_io_num = PinDefs::cs;
    devcfg.queue_size = 1;  // only single transaction ever

    spi_bus_initialize(SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    spi_bus_add_device(SPI_HOST, &devcfg, &spi);
}

// text helper:
int getTextWidth(const GFXfont *gfxFont, const String &text)
{
    uint8_t first = gfxFont->first;
    uint8_t last = gfxFont->last;

    int width = 0;
    int len = text.length();
    for (int i = 0; i < len; i++)
    {
        char c = text[i];
        if (c >= first && c <= last)
        {
            GFXglyph *glyph = &gfxFont->glyph[c - first];
            width += glyph->xAdvance;
        }
    }

    return width;
}

// interface here:

void ScrollingDisplayIntf::begin()
{
    static bool begun = false;

    if (!begun)
    {
        // init pins
        pinMode(PinDefs::cs, OUTPUT);
        digitalWrite(PinDefs::cs, HIGH); // Deselect slave

        pinMode(PinDefs::oe, OUTPUT);
        digitalWrite(PinDefs::oe, HIGH);

        pinMode(PinDefs::r0, OUTPUT);
        digitalWrite(PinDefs::r0, LOW);
        pinMode(PinDefs::r1, OUTPUT);
        digitalWrite(PinDefs::r1, HIGH);    // high for now
        pinMode(PinDefs::r2, OUTPUT);
        digitalWrite(PinDefs::r2, LOW);

        // init the SPI for non-blocking DMA transfers
        initSPI();

        // Create high priority task (stack 32kB, prio 23)
        xTaskCreate(
            highPrioTask,
            "HighPrioTask",
            32 * 1024, // 32 KB stack
            nullptr,
            25, // priority
            &highPrioTaskHandle
        );

        // Start timer to periodically wake up the task
        timer_config_t config = {
            .alarm_en = TIMER_ALARM_EN,
            .counter_en = TIMER_PAUSE,
            .intr_type = TIMER_INTR_LEVEL,
            .counter_dir = TIMER_COUNT_UP,
            .auto_reload = TIMER_AUTORELOAD_EN,
            .divider = TIMER_DIVIDER};
        timer_init(TIMER_GROUP, TIMER_IDX, &config);
        timer_set_counter_value(TIMER_GROUP, TIMER_IDX, 0);
        timer_set_alarm_value(TIMER_GROUP, TIMER_IDX, TIMER_INTERVAL_US);
        timer_enable_intr(TIMER_GROUP, TIMER_IDX);
        timer_isr_callback_add(TIMER_GROUP, TIMER_IDX, onTimer, nullptr, 0);
        timer_start(TIMER_GROUP, TIMER_IDX);

        begun = true;
    }
}

void ScrollingDisplayIntf::setText(const String &s)
{
    if (!updateText)
    {
        if (s.length() <= MaxTextLength)
        {
            text = s;
        }
        else
        {
            text = s.substring(0, MaxTextLength);
        }

        updateText = true;
    }
}

void ScrollingDisplayIntf::setScrollDelay(int pixelShiftDelayMillis)
{
    scrollDelay = pixelShiftDelayMillis;
}

// instance for the app to use
ScrollingDisplayIntf ScrollingDisplay;