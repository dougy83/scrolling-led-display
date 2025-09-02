#include "ScrollingDisplay.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/timer.h"

#include "Adafruit_GFX.h"

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
#define TIMER_INTERVAL_US 300

TaskHandle_t highPrioTaskHandle = nullptr;

// periodic timer wakes our high prio task every 300us to allow for shorter non-blocking delays
bool IRAM_ATTR onTimer(void *arg)
{
    BaseType_t needToYield = pdFALSE;
    if (highPrioTaskHandle)
    {
        vTaskNotifyGiveFromISR(highPrioTaskHandle, &needToYield);
    }

    return needToYield;
}

// stuff for our task
String text("Hello");
std::atomic<bool> updateText(true);
std::atomic<int> scrollDelay(10);

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
    std::unique_ptr<GFXcanvas1> canvas;
    
    for (;;)
    {
        if (updateText)
        {
            // TODO: measure text
            int w = 6 * text.length();  // assume 6 pixels wide for each character (6x7 font)
            canvas = std::unique_ptr<GFXcanvas1>(new GFXcanvas1(max(COLUMNS, w), ROWS));

        }
        // Block until timer ISR notifies
        tick();

        digitalWrite(8, HIGH);
        tick(500);
        digitalWrite(8, LOW);
        tick(500);


        // ---- Do your high-frequency work here ----
        // e.g., queue an SPI transaction
        // spi_device_queue_trans(...);

        // Debug (avoid in real 300µs loop):
        // gpio_set_level(GPIO_NUM_2, 1);
        // gpio_set_level(GPIO_NUM_2, 0);
    }
}

// scroll the bitmap one pixel, left or right, wrapping around. For normal scroll, it makes sense to make the bitmap content width + display width
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
        
        // Create high priority task (stack 32kB, prio 23)
        xTaskCreate(
            highPrioTask,
            "HighPrioTask",
            32 * 1024, // 32 KB stack
            nullptr,
            23, // priority
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

void ScrollingDisplayIntf::setText(String &s)
{
    text = s;
    updateText = true;
}

void ScrollingDisplayIntf::setScrollRate(int pixelShiftDelayMillis)
{
    scrollDelay = pixelShiftDelayMillis;
}

// instance for the app to use
ScrollingDisplayIntf ScrollingDisplay;