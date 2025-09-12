/*
Side scrolling LED matrix display ("subway sign")
7 rows x 60 cols x 7 modules (was 8 modules, but one's been removed)

Connector pinout:
1 - 5V      - 5V supply from the display boards
2 - clk     - clock next data bit into shift register
3 - latch   - load shifted data from shift register to output latch
4 - R0      - row select bit 0
5 - R1      - row select bit 1
6 - R2      - row select bit 2
7 - /OE     - output enable (drive LEDs) - uses 300us ON per row per frame
9 - data    - data to go in the shift register
10 - GND    - common ground / 0V

To display something, the row data is shifted in, latch, output enabled, repeat.
The R0..2 bits and the /OE pin enable drive for LEDs of that row. When measured from working device, OE was asserted for 300us on per row per frame (60fps).
Sample board used 20MHz clock, which is marginal (low pin drive current results in sawtooth waveform). We will use 2MHz instead.

This program:
- boots into AP mode, which allows configuring; will stay in this mode for 2 minutes if we can connect to wifi station, or as long as there's a client connection.
- if we can connect to wifi LAN (i.e. it's configured and available), we'll do that
- if we can't connect, we can just run the last animation that was saved
- can configure via http at scrollingdisplay.local (either AP or STA modes), or via 192.168.0.1 for AP mode.

technical:
- use a 300us timer interrupt as the timebase for all output, wake hi prio task
    - hi prio task manages:
        - updating the bitmap to be displayed, and
            - use adafruit graphics library for drawing to the bitmap
        - control signals and queuing SPI transfers
- low prio task does the logic for the network, and forwards changes of the string to the high prio task
    - web interface have index.html for setting the string via UI, and single API endpoint for setting the text and scroll rate
*/

// #define DEBUG // << enable serial print statements

#include <Arduino.h>
#include <LittleFS.h>
#include "esp_heap_caps.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoJson.h>

#include "ScrollingDisplay.h"

#define LED_PIN 8
#define MAX_TEXT_LENGTH 4096

#ifdef DEBUG
#define DEBUG_PRINTF(...) DEBUG_PRINTF(__VA_ARGS__)
#define DEBUG_PRINTLN(...) DEBUG_PRINTLN(__VA_ARGS__)
#else
#define DEBUG_PRINTF(...)
#define DEBUG_PRINTLN(...)
#endif

// wifi
String ssid;
String pass;
String apSsid = "ScrollingDisplay"; // defaults
String apPass = "12345678";
String mdnsHostName = "scrollingdisplay";
String text;
int scrollDelay = 50;

#define WIFI_RECONNECT_INTERVAL 60000 // 1 min
#define AP_TIMEOUT (5 * 60 * 1000)    // AP will close 5 minutes after boot

WebServer server(80);
IPAddress apIP(192, 168, 0, 1);

void setupWiFi();
void handleWiFiConnection();

bool saveSettings();
bool loadSettings();

// Files we use
#define INDEX_HTML_FILENAME "/web/index.html"
#define SETTINGS_FILENAME "/message.txt"

String systemInfo()
{
    // LittleFS info
    size_t totalBytes = LittleFS.totalBytes();
    size_t usedBytes = LittleFS.usedBytes();
    size_t freeBytes = totalBytes - usedBytes;

    // RAM info
    size_t freeHeap = ESP.getFreeHeap();
    size_t totalHeap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t usedHeap = totalHeap - freeHeap;

    // Build single-line string
    String info = "FS: total=" + String(totalBytes * 0.001, 1) + "kB, used=" + String(usedBytes * 0.001, 1) +
                  "kB, free=" + String(freeBytes * 0.001, 1) + "kB; RAM: total=" + String(totalHeap * 0.001, 1) +
                  "kB, used=" + String(usedHeap * 0.001, 1) + "kB, free=" + String(freeHeap * 0.001, 1) + "kB..   ";
    return info;
}

void initServer()
{
    // Serve index.html from LittleFS
    server.on("/", HTTP_GET, []()
              {
        File f;
        if (f = LittleFS.open(INDEX_HTML_FILENAME, "r")){
            server.streamFile(f, "text/html");
            f.close();
        } else {
            server.send(404, "text/plain", "index.html not found");
        } });

    // /settext?text=<sometext>&delay=<somenumber>
    server.on("/settext", HTTP_GET, []()
              {
        bool save = false;

        if (server.hasArg("text")) {
            text = server.arg("text");
            if (text.length() > MAX_TEXT_LENGTH) {
                text = text.substring(0, MAX_TEXT_LENGTH);
            }
            ScrollingDisplay.setText(text);
            save = true;
        }
        if (server.hasArg("delay")) {
            scrollDelay = server.arg("delay").toInt();
            ScrollingDisplay.setScrollDelay(scrollDelay);
            save = true;
        }

        if (save) {
            saveSettings();
        }
        server.send(200, "text/plain", ""); });

    // /setwifi?ssid=<ssid>&pass=<pass>
    server.on("/setwifi", HTTP_GET, []()
              {
        bool doConnect = false;
        String temp = server.arg("ssid");
        if (temp.length() && temp.length() < 32)
        {
            ssid = temp;
            doConnect = true;
        }

        temp = server.arg("pass");
        if (temp.length() && temp.length() < 32)
        {
            pass = temp;
            doConnect = true;
        }

        temp = server.arg("apSsid");
        if (temp.length() && temp.length() < 32)
        {
            apSsid = temp;
        }

        temp = server.arg("apPass");
        if (temp.length() && temp.length() < 32)
        {
            apPass = temp;
        }

        temp = server.arg("hostname");
        if (temp.length() && temp.length() < 32)
        {
            mdnsHostName = temp;
            MDNS.end();
            MDNS.begin(mdnsHostName);
        }

        saveSettings();

        String response = "Wi-Fi set to: " + ssid;
        DEBUG_PRINTLN(response);
        server.send(200, "text/plain", response);

        // Attempt connection asynchronously
        if (doConnect)
        {
            WiFi.begin(ssid.c_str(), pass.c_str());
        } });

    // OTA firmware upload
    server.on("/setota", HTTP_POST, []()
              {
                  // Called when upload is finished
                  if (Update.hasError())
                  {
                      server.send(500, "text/plain", "OTA Update Failed");
                  }
                  else
                  {
                      server.send(200, "text/plain", "OTA Update Successful! Rebooting...");
                  }

                  auto start = millis();
                  while (millis() - start < 500)    // delay for response to be sent to client before rebooting
                  { 
                      server.handleClient();
                      delay(1);
                  }
                  ESP.restart(); // reboot after successful update
              },
              []()
              {
        // Called during file upload
        HTTPUpload &upload = server.upload();
        if (upload.status == UPLOAD_FILE_START)
        {
            ScrollingDisplay.setText(""); // it goes funky during update

            DEBUG_PRINTF("OTA Start: %s\n", upload.filename.c_str());
            int command = upload.name == "fs" ? U_SPIFFS : U_FLASH;
            if (!Update.begin(UPDATE_SIZE_UNKNOWN, command))
            { // Start with unknown size
                Update.printError(Serial);
            }
        }
        else if (upload.status == UPLOAD_FILE_WRITE)
        {
            // Write chunk to flash
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
            {
                Update.printError(Serial);
            }
        }
        else if (upload.status == UPLOAD_FILE_END)
        {
            if (Update.end(true))
            { // true = final check CRC
                DEBUG_PRINTF("OTA Success: %u bytes\n", upload.totalSize);
            }
            else
            {
                Update.printError(Serial);
            }
        } });

    server.begin();
    DEBUG_PRINTLN("HTTP server started");
}

void setup()
{
    Serial.begin(115200);
    ScrollingDisplay.begin();
    delay(100);

    // init FS, and load saved settings
    if (LittleFS.begin(true))
    {
        if (loadSettings())
        {
            ScrollingDisplay.setText(text);
            ScrollingDisplay.setScrollDelay(scrollDelay);
        }
        else
        {
            ScrollingDisplay.setText(systemInfo());
        }
    }

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(8, LOW);

    setupWiFi();
}

void setupWiFi()
{
    // Always start in AP mode
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(apSsid, apPass);
    DEBUG_PRINTF("AP started: %s @ %s\n", apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

void handleWiFiConnection()
{
    // Check after timeout whether to stop AP
    if (WiFi.getMode() == WIFI_AP_STA && millis() > AP_TIMEOUT)
    {
        int clients = WiFi.softAPgetStationNum();
        if (clients == 0)
        {
            DEBUG_PRINTLN("Disabling AP (STA connected, no AP clients).");
            WiFi.mode(WIFI_STA);
        }
    }

    // attempt initial connection, or reconnect STA
    if (!ssid.isEmpty() && !pass.isEmpty() && WiFi.status() != WL_CONNECTED)
    {
        static unsigned long lastRetry = 0;
        if (lastRetry == 0 || millis() - lastRetry > 30000)
        { // retry every 30s
            DEBUG_PRINTF("Retrying STA connection to %s...\n", ssid.c_str());
            WiFi.begin(ssid.c_str(), pass.c_str());
            lastRetry = millis();
        }
    }

    // init MDNS after wifi initialised
    static bool mdnsInit = false;
    if (!mdnsInit && WiFi.status() == WL_CONNECTED)
    {
        MDNS.begin(mdnsHostName);
        mdnsInit = true;
    }
}

void loop()
{
    handleWiFiConnection();

    if (WiFi.isConnected() || WiFi.softAPgetStationNum() > 0)
    {
        static bool serverInited = false;
        if (!serverInited)
        {
            initServer();
            serverInited = true;
        }

        server.handleClient();
    }
}

bool loadSettings()
{
    File file = LittleFS.open(SETTINGS_FILENAME, "r");
    if (!file)
    {
        DEBUG_PRINTLN("Failed to open settings.json for reading");
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error)
    {
        DEBUG_PRINTF("Failed to parse settings.json: %s\n", error.c_str());
        return false;
    }

    if (doc.containsKey("ssid"))
        ssid = doc["ssid"].as<String>();
    if (doc.containsKey("pass"))
        pass = doc["pass"].as<String>();
    if (doc.containsKey("apSsid"))
        apSsid = doc["apSsid"].as<String>();
    if (doc.containsKey("apPass"))
        apPass = doc["apPass"].as<String>();

    text = doc["text"].as<String>(); // text can be blank
    if (doc.containsKey("delay"))
        scrollDelay = doc["delay"].as<int>();
    if (doc.containsKey("hostname"))
        mdnsHostName = doc["hostname"].as<String>();

    return true;
}

bool saveSettings()
{
    JsonDocument doc;
    doc["ssid"] = ssid;
    doc["pass"] = pass;
    doc["apSsid"] = apSsid;
    doc["apPass"] = apPass;
    doc["text"] = text;
    doc["delay"] = scrollDelay;
    doc["hostname"] = mdnsHostName;

    File file = LittleFS.open(SETTINGS_FILENAME, "w");
    if (!file)
    {
        DEBUG_PRINTLN("Failed to open settings.json for writing");
        return false;
    }

    if (serializeJsonPretty(doc, file) == 0)
    {
        DEBUG_PRINTLN("Failed to write settings.json");
        file.close();
        return false;
    }

    file.close();
    return true;
}
