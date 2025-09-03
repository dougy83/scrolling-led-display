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

To display something, the row data is shifted in, latch, output enabled (measured as roughly: 300us ON, 600us off for each of the 7 rows, then 10ms gap between frames). 60Hz frame rate
The R0..2 bits and the /OE pin enable drive for LEDs of that row.
Sample board used 20MHz clock, which worked, but it's bearly enough drive and waveform was sawtooth, and there's no need for that speed.

This program:
- boots into AP mode, which allows configuring; will stay in this mode as long as there's a connection.
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
#include <LittleFS.h>
#include "esp_heap_caps.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Update.h>

#include "ScrollingDisplay.h"

#define LED_PIN 8
#define MAX_TEXT_LENGTH 4096

// wifi
String ssid;
String pass;
#define AP_SSID "ScrollingDisplay"
#define AP_PASS "HelloYellow"
#define MDNS_HOSTNAME "scrollingdisplay"
#define WIFI_RECONNECT_INTERVAL 60000 // 1 min
WebServer server(80);
IPAddress apIP(192, 168, 1, 1);

// Files we use
#define INDEX_HTML_FILENAME "/web/index.html"
#define WIFI_CREDS_FILENAME "/wifi.txt"
#define MESSAGE_FILENAME    "/message.txt"

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
        String newText;
        int newDelay = 50;
        bool save = false;

        if (server.hasArg("text")) {
            newText = server.arg("text");
            if (newText.length() > MAX_TEXT_LENGTH) {
                newText = newText.substring(0, MAX_TEXT_LENGTH);
            }
            ScrollingDisplay.setText(newText);
            save = true;
        }
        if (server.hasArg("delay")) {
            newDelay = server.arg("delay").toInt();
            ScrollingDisplay.setScrollDelay(newDelay);
            save = true;
        }

        if (save) {
            File f;
            if (f = LittleFS.open(MESSAGE_FILENAME, "w")) {
                f.println(newText);
                f.println(newDelay);
                f.close();
            }
        }
        server.send(200, "text/plain", ""); });

    // /setwifi?ssid=<ssid>&pass=<pass>
    server.on("/setwifi", HTTP_GET, []()
              {
        bool save = false;
        if (server.hasArg("ssid")) {
            String temp = server.arg("ssid");
            if (temp.length() < 32) {
                ssid = temp;
                save = true;
            }
        }
        if (server.hasArg("pass")) {
            String temp = server.arg("pass");
            if (temp.length() < 32) {
                pass = temp;
                save = true;
            }
        }

        if (save) {
            File f;
            if (f = LittleFS.open(WIFI_CREDS_FILENAME, "w")) {
                f.println(ssid);
                f.println(pass);
                f.close();
            }
        }

        String response = "Wi-Fi set to: " + ssid;
        Serial.println(response);
        server.send(200, "text/plain", response);

        // Attempt connection asynchronously
        WiFi.begin(ssid.c_str(), pass.c_str()); });

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
                  ESP.restart(); // reboot after successful update
              },
              []()
              {
        // Called during file upload
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            ScrollingDisplay.setText("");   // it goes funky during update

            Serial.printf("OTA Start: %s\n", upload.filename.c_str());
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { // Start with unknown size
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            // Write chunk to flash
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) { // true = final check CRC
                Serial.printf("OTA Success: %u bytes\n", upload.totalSize);
            } else {
                Update.printError(Serial);
            }
        } });

    server.begin();
    Serial.println("HTTP server started");
}

void setup()
{
    Serial.begin(115200);
    ScrollingDisplay.begin();
    delay(100);

    // init FS, and load saved settings
    if (LittleFS.begin(true))
    {
        File f = LittleFS.open(MESSAGE_FILENAME, "r");
        if (f)
        {
            String msg = f.readStringUntil('\n');
            ScrollingDisplay.setText(msg);

            int scrollDelay = atoi(f.readStringUntil('\n').c_str());
            ScrollingDisplay.setScrollDelay(scrollDelay > 0 ? scrollDelay : 50);

            f.close();
        }
        else
        {
            ScrollingDisplay.setText(systemInfo());
        }

        f = LittleFS.open(WIFI_CREDS_FILENAME, "r");
        if (f)
        {
            ssid = f.readStringUntil('\n');
            ssid.trim();
            pass = f.readStringUntil('\n');
            pass.trim();

            f.close();
        }
    }

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(8, LOW);
}

// crappy reconnection logic with fallback to AP mode if cannot connect. Will block on connection attempt
void handleWiFiConnection()
{
    if (WiFi.isConnected())
    {
        if (millis() > 120000 && WiFi.getMode() == WIFI_AP_STA)
        {
            WiFi.mode(WIFI_STA);    // don't need AP anymore
        }
        return;
    }

    static unsigned long lastAttempt = 0;

    if (lastAttempt == 0 || millis() - lastAttempt >= WIFI_RECONNECT_INTERVAL)
    {
        lastAttempt = millis() + 1;

        int clients = WiFi.softAPgetStationNum();
        if (!ssid.isEmpty() && !pass.isEmpty() && clients == 0)
        {
            Serial.printf("Trying to connect to %s...\n", ssid.c_str());
            WiFi.mode(WIFI_AP_STA);
            WiFi.begin(ssid.c_str(), pass.c_str());

            unsigned long start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < 10000)
            {
                delay(500);
                Serial.print(".");
            }
        }

        if (WiFi.isConnected())
        {
            Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
            if (MDNS.begin(MDNS_HOSTNAME))
            {
                MDNS.addService("http", "tcp", 80);
            }
        }
        else
        {
            Serial.println("\nConnection failed. Starting AP...");
            WiFi.mode(WIFI_AP);
            WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
            WiFi.softAP(AP_SSID, AP_PASS);
        }
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