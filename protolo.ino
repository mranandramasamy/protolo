#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <vector>
#include <utility>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "user_config.h"
#include "epaper_driver_bsp.h"
#include "board_power_bsp.h"
#include "adc_bsp.h"
#include "lvgl.h"
#include "lockscreen_img.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"


static const char *TAG = "LVGL";
static SemaphoreHandle_t lvgl_mux = NULL;

epaper_driver_display *driver = NULL;
board_power_bsp_t power(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);

// Forward Declarations
static bool example_lvgl_lock(int timeout_ms);
static void example_lvgl_unlock(void);
bool fetchDataFromAPI();
int getBatteryPercent();
int getMemoryUsagePercent();
void navigateTo(int nextScreen);
void showHomeScreen();
void showMenuScreen();
void refreshMenu();
void showNotesList();
void refreshNotesList();
void showNotesView();
void showFlashList();
void refreshFlashList();
void showFlashView();
void showMemorizeList();
void refreshMemorizeList();
void showMemorizeView();
void showStoriesList();
void refreshStoriesList();
void showStoriesView();
void showConnectScreen();
void startWiFiAP();
void stopWiFiAP();
void setupWebServer();
bool loadDataFromJSON();
bool saveDataToJSON();
String customSleepQuote = ""; // Holds the dynamic quote from JSON
// Forward Declarations
void prepareMemorizeSequence(String rawContent);
std::vector<int> generate_hybrid_sequence(int n);

// --- Screen View System States ---
enum ScreenType {
    SCREEN_HOME,
    SCREEN_MENU,
    SCREEN_NOTES_LIST,
    SCREEN_NOTES_VIEW,
    SCREEN_FLASH_LIST,
    SCREEN_FLASH_VIEW,
    SCREEN_STORIES_LIST,
    SCREEN_STORIES_VIEW,
    SCREEN_MEMORIZE_LIST,
    SCREEN_MEMORIZE_VIEW,
    SCREEN_CONNECT,
    SCREEN_CONNECT_WIFI_LIST,
    SCREEN_SLEEP
};

ScreenType currentScreen = SCREEN_HOME;
ScreenType previousScreen = SCREEN_HOME;

// Global pointers for memory storage and connection routing
WebServer* server = nullptr;
DynamicJsonDocument* doc = nullptr; 

// UI Components
static lv_style_t styleTitle;
lv_obj_t *menuLabel = nullptr;
lv_obj_t *contentLabel = nullptr;
lv_obj_t *headerLabel = nullptr; 

int selectedIndex = 0;
const int MENU_ITEMS = 7;

unsigned long bootClickTime = 0;
int bootClickCount = 0;

unsigned long powerClickTime = 0;
int powerClickCount = 0;          

const unsigned long DOUBLE_CLICK_WINDOW = 300; // ms time window
// Button Debounce History State
bool lastBoot = HIGH;

bool lastPower = HIGH;
unsigned long wifiStartTime = 0;
const unsigned long WIFI_TIMEOUT_MS = 300000; 

unsigned long homeScreenActiveStart = 0;
const unsigned long SLEEP_TIMEOUT_MS = 4000; // Time to wait on Home screen before deep sleep (4 seconds)

// Global Models
struct ContentItem { String title; String content; };
struct FlashCard { String front; String back; };
struct FlashcardDeck { String title; std::vector<FlashCard> cards; };
struct StoryItem { String title; String text; };
struct MemorizeItem { String title; String content; };

std::vector<ContentItem> items;
std::vector<FlashcardDeck> flashDecks;
std::vector<StoryItem> stories;
std::vector<MemorizeItem> memorizeItems;

int selectedItemIndex = 0;
int noteScrollLine = 0; 
int selectedFlashDeckIndex = 0;
int selectedFlashcardIndex = 0; 
std::vector<int> flashcardOrder;
bool showingFlashcardFront = true;
int selectedStoryIndex = 0;
int storyScrollY = 0; 

// --- Global Models for Memorize Module ---
std::vector<String> memorizeLines;
std::vector<int> memorizeSequence;
int currentMemorizeIndex = 0;

const char* ap_ssid = "Protolo_AP";
const char* ap_password = "password123";
bool wifiRunning = false;

// --- Global Models for Multi-WiFi Architecture ---
struct DiscoveredNetwork {
    String ssid;
    int32_t rssi;
};
std::vector<DiscoveredNetwork> foundNetworks;
int selectedNetworkIndex = 0;

struct KnownNetwork {
    const char* ssid;
    const char* password;
    const char* displayName; // Clean name to show on screen
};

const KnownNetwork myNetworks[] = {
    {"Wifi name 1", "passxxx", "home wifi"},
    {"Wifi name 2", "passxxx", "mobile wifi"}
};
const int myNetworksCount = sizeof(myNetworks) / sizeof(myNetworks[0]);

void handleRoot();    // Forward declaration for the web portal UI
void handleSave();    // Forward declaration for the database save action


// System Status Engines
int getBatteryPercent() {
    float voltageSum = 0.0;
    const int SAMPLES = 10;
    static float filteredPercent = -1.0; // Keeps track of the filtered value across calls
    
    // 1. Take multiple readings over a brief period to smooth out electrical ripples
    for (int i = 0; i < SAMPLES; i++) {
        float sampleVoltage = 0.0;
        adc_get_value(&sampleVoltage, NULL);
        voltageSum += sampleVoltage;
        delay(5); 
    }
    
    float voltage = voltageSum / SAMPLES;
    
    // 2. Guardrail Boundaries
    if (voltage >= 4.15) {
        voltage = 4.15;
    }
    if (voltage <= 3.60) {
        if (voltage <= 3.40) return 0;
        return (int)((voltage - 3.40) / (3.60 - 3.40) * 5.0);
    }
    
    // 3. The Active Operating Zone Map
    float rawPercent = ((voltage - 3.60) / (4.15 - 3.60)) * 95.0 + 5.0;
    
    // 4. Exponential Moving Average (EMA) Filter
    // If it's the first run after boot, initialize the filter with the raw reading
    if (filteredPercent < 0) {
        filteredPercent = rawPercent;
    } else {
        // Blend 92% of the old reading with 8% of the new reading.
        // This completely absorbs momentary voltage dips caused by screen refreshes!
        filteredPercent = (filteredPercent * 0.92) + (rawPercent * 0.08);
    }
    
    int finalPercent = (int)(filteredPercent + 0.5); // Round to nearest whole integer
    if (finalPercent > 100) finalPercent = 100;
    if (finalPercent < 0) finalPercent = 0;
    
    return finalPercent;
}

int getMemoryUsagePercent() {
    size_t totalHeap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (totalHeap == 0) return 0;
    return (int)(((float)(totalHeap - freeHeap) / totalHeap) * 100.0);
}

void saveToFlashMemory() {
    if (doc == nullptr) return;
    
    // Open the file in write mode ("w")
    File file = LittleFS.open("/config.json", "w");
    if (!file) {
        Serial.println("Failed to open LittleFS file for writing!");
        return;
    }
    
    // Write the live JSON document directly to the flash chip
    serializeJson(*doc, file);
    file.close();
    Serial.println("Data successfully written to permanent flash memory.");
}

bool loadFromFlashMemory() {
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS Mount Failed");
        return false;
    }

    File file = LittleFS.open("/config.json", "r");
    if (!file || file.size() == 0) {
        Serial.println("No saved configuration found in flash memory. Using defaults.");
        return false;
    }

    String payload = file.readString();
    file.close();

    DynamicJsonDocument incomingDoc(24576);
    DeserializationError error = deserializeJson(incomingDoc, payload);

    if (!error) {
        if (doc != nullptr) delete doc;
        doc = new DynamicJsonDocument(24576);
        *doc = incomingDoc;

        // Restore global strings
        customSleepQuote = doc->containsKey("sleep_quote") ? (*doc)["sleep_quote"].as<String>() : "";

        // Restore Notes
        items.clear();
        if (doc->containsKey("notes")) {
            for (JsonVariant v : (*doc)["notes"].as<JsonArray>()) {
                items.push_back({v["title"].as<String>(), v["content"].as<String>()});
            }
        }

        // Restore Memorize
        memorizeItems.clear();
        if (doc->containsKey("memorize")) {
            for (JsonVariant v : (*doc)["memorize"].as<JsonArray>()) {
                memorizeItems.push_back({v["title"].as<String>(), v["content"].as<String>()});
            }
        }

        // Restore Flashcards
        flashDecks.clear();
        if (doc->containsKey("flashcards")) {
            for (JsonVariant d : (*doc)["flashcards"].as<JsonArray>()) {
                FlashcardDeck deck;
                deck.title = d["title"].as<String>();
                for (JsonVariant c : d["cards"].as<JsonArray>()) {
                    deck.cards.push_back({c["front"].as<String>(), c["back"].as<String>()});
                }
                flashDecks.push_back(deck);
            }
        }

        // Restore Stories
        stories.clear();
        if (doc->containsKey("stories")) {
            for (JsonVariant v : (*doc)["stories"].as<JsonArray>()) {
                stories.push_back({v["title"].as<String>(), v["text"].as<String>()});
            }
        }

        Serial.println("Successfully loaded all configurations from permanent flash memory.");
        return true;
    }
    return false;
}

void handleRoot() {
    // 1. Initialize Chunked Transfer
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "text/html", "");

    // 2. Send Header and CSS
    String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<style>body{font-family:-apple-system,BlinkMacSystemFont,Arial,sans-serif;background:#f4f4f7;padding:20px;margin:0;color:#333;}";
    html += ".container{max-width:850px;margin:0 auto;} .card{background:white;padding:25px;border-radius:12px;box-shadow:0 4px 12px rgba(0,0,0,0.06);margin-bottom:25px;}";
    html += "h1{text-align:center;color:#111;margin-bottom:5px;} .subtitle{text-align:center;color:#666;margin-bottom:30px;font-size:14px;}";
    html += "h2{margin-top:0;color:#007aff;border-bottom:2px solid #f0f0f5;padding-bottom:8px;}";
    html += "label{display:block;margin:15px 0 6px;font-weight:bold;color:#444;}";
    html += "textarea{width:100%;height:160px;border:1px solid #ccc;border-radius:6px;padding:12px;box-sizing:border-box;font-family:monospace;font-size:13px;background:#fafafa;}";
    html += "textarea:focus{border-color:#007aff;outline:none;background:#fff;}";
    html += ".btn{background:#007aff;color:white;border:none;padding:16px;border-radius:6px;cursor:pointer;font-size:16px;font-weight:bold;width:100%;margin-top:15px;transition:0.2s;}";
    html += ".btn:hover{background:#005ecb;} .hint{font-size:12px;color:#777;margin-top:4px;background:#edf2f7;padding:8px;border-radius:4px;font-family:monospace;}</style></head><body>";
    html += "<div class='container'><h1>Protolo Local Dashboard</h1><div class='subtitle'>Modify, Add, or Delete Contents Directly Inside Individual Section Fields</div><form action='/save' method='POST'>";
    
    server->sendContent(html);

    // 3. Sleep Quote Section
    server->sendContent("<div class='card'><h2>1. Lockscreen Sleep Quote</h2><textarea name='sleep_quote'>");
    server->sendContent(customSleepQuote);
    server->sendContent("</textarea></div>");
    
    delay(2); // Let the processor breathe!

    // 4. Notes Section (Isolated Memory Scope)
    server->sendContent("<div class='card'><h2>2. Notes Scratchpad Directory</h2><textarea name='notes'>");
    {
        DynamicJsonDocument docNotes(8192);
        JsonArray arr = docNotes.to<JsonArray>();
        for (const auto& item : items) {
            JsonObject obj = arr.createNestedObject();
            obj["title"] = item.title;
            obj["content"] = item.content;
        }
        String out; serializeJsonPretty(arr, out);
        server->sendContent(out);
    } // Memory is instantly freed here!
    server->sendContent("</textarea></div>");
    
    delay(2);

    // 5. Memorize Section
    server->sendContent("<div class='card'><h2>3. Memorize Pegs & Recall Sequences</h2><textarea name='memorize'>");
    {
        DynamicJsonDocument docMem(8192);
        JsonArray arr = docMem.to<JsonArray>();
        for (const auto& item : memorizeItems) {
            JsonObject obj = arr.createNestedObject();
            obj["title"] = item.title;
            obj["content"] = item.content;
        }
        String out; serializeJsonPretty(arr, out);
        server->sendContent(out);
    }
    server->sendContent("</textarea></div>");
    
    delay(2);

    // 6. Flashcards Section
    server->sendContent("<div class='card'><h2>4. Flash Decks</h2><textarea name='flashcards'>");
    {
        DynamicJsonDocument docFC(16384); // Slightly larger allowance for decks
        JsonArray arr = docFC.to<JsonArray>();
        for (const auto& deck : flashDecks) {
            JsonObject dObj = arr.createNestedObject();
            dObj["title"] = deck.title;
            JsonArray cardsArr = dObj.createNestedArray("cards");
            for (const auto& card : deck.cards) {
                JsonObject cObj = cardsArr.createNestedObject();
                cObj["front"] = card.front;
                cObj["back"] = card.back;
            }
        }
        String out; serializeJsonPretty(arr, out);
        server->sendContent(out);
    }
    server->sendContent("</textarea></div>");
    
    delay(2);

    // 7. Stories Section
    server->sendContent("<div class='card'><h2>5. Long Stories</h2><textarea name='stories'>");
    {
        DynamicJsonDocument docSt(16384);
        JsonArray arr = docSt.to<JsonArray>();
        for (const auto& item : stories) {
            JsonObject obj = arr.createNestedObject();
            obj["title"] = item.title;
            obj["text"] = item.text;
        }
        String out; serializeJsonPretty(arr, out);
        server->sendContent(out);
    }
    server->sendContent("</textarea></div>");

    // 8. Send the Footer and Button
    server->sendContent("<button type='submit' class='btn'>Save Configurations & Disconnect Device</button></form></div></body></html>");

    // 9. Send Empty Chunk to Signal End of Connection
    server->sendContent("");
}

void handleSave() {
    DynamicJsonDocument incomingDoc(24576);
    
    // Clean and fix line breaks by stripping out hidden text area carriage returns
    String cleanQuote = server->arg("sleep_quote");
    cleanQuote.replace("\r", ""); 
    incomingDoc["sleep_quote"] = cleanQuote;

    DynamicJsonDocument dNotes(4096), dMem(4096), dCards(8192), dStories(4096);
    deserializeJson(dNotes, server->arg("notes"));
    deserializeJson(dMem, server->arg("memorize"));
    deserializeJson(dCards, server->arg("flashcards"));
    deserializeJson(dStories, server->arg("stories"));

    incomingDoc["notes"] = dNotes.as<JsonArray>();
    incomingDoc["memorize"] = dMem.as<JsonArray>();
    incomingDoc["flashcards"] = dCards.as<JsonArray>();
    incomingDoc["stories"] = dStories.as<JsonArray>();

    if (doc != nullptr) delete doc;
    doc = new DynamicJsonDocument(24576);
    *doc = incomingDoc;

    customSleepQuote = (*doc)["sleep_quote"].as<String>();

    // Sync Notes Vector structures 
    items.clear();
    JsonArray notesArr = (*doc)["notes"].as<JsonArray>();
    for (JsonVariant v : notesArr) {
        items.push_back({v["title"].as<String>(), v["content"].as<String>()});
    }

    // Sync Memorize Vector structures
    memorizeItems.clear();
    JsonArray memArr = (*doc)["memorize"].as<JsonArray>();
    for (JsonVariant v : memArr) {
        memorizeItems.push_back({v["title"].as<String>(), v["content"].as<String>()});
    }

    // Changed structure type initialization name to FlashCardDeck
    flashDecks.clear();
    JsonArray fcArr = (*doc)["flashcards"].as<JsonArray>();
    for (JsonVariant d : fcArr) {
        FlashcardDeck deck; 
        deck.title = d["title"].as<String>();
        JsonArray cards = d["cards"].as<JsonArray>();
        for (JsonVariant c : cards) {
            deck.cards.push_back({c["front"].as<String>(), c["back"].as<String>()});
        }
        flashDecks.push_back(deck);
    }

    // Sync Stories Vector structures
    stories.clear();
    JsonArray storiesArr = (*doc)["stories"].as<JsonArray>();
    for (JsonVariant v : storiesArr) {
        stories.push_back({v["title"].as<String>(), v["text"].as<String>()});
    }

    saveToFlashMemory(); 

    server->send(200, "text/html", "<h1>Configurations successfully updated! Disconnecting...</h1>");
    delay(1000);
    
    // Close the connection engine infrastructure down cleanly
    server->stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiRunning = false;
    
    currentScreen = SCREEN_MENU; 
    showMenuScreen();
}

// Spaced Repetition Sequence Generator Engine
std::vector<int> generate_hybrid_sequence(int n) {
    std::vector<int> result;
    if (n <= 0) return result;

    // PHASE 1 — Incremental Learning
    for (int end = 1; end <= n; ++end) {
        for (int i = 1; i <= end; ++i) {
            result.push_back(i);
        }
    }

    // PHASE 2 — Sliding Reinforcement
    const int WINDOW_SIZE = 4;
    if (n >= WINDOW_SIZE) {
        for (int start = 1; start <= n - WINDOW_SIZE + 1; ++start) {
            for (int i = start; i < start + WINDOW_SIZE; ++i) {
                result.push_back(i);
            }
        }
    }

    // PHASE 3 — Sparse Recall
    const int GAP = 3;
    for (int offset = 0; offset < GAP; ++offset) {
        int i = offset + 1;
        while (i <= n) {
            result.push_back(i);
            i += GAP;
        }
    }

    return result;
}

// Data parser for converting text into separate tokens for memorize module
void prepareMemorizeSequence(String rawContent) {
    memorizeLines.clear();
    memorizeSequence.clear();
    currentMemorizeIndex = 0;
    
    int startIdx = 0;
    int endIdx = rawContent.indexOf('\n');
    
    while (endIdx >= 0) {
        String line = rawContent.substring(startIdx, endIdx);
        line.trim();
        if (line.length() > 0) {
            memorizeLines.push_back(line);
        }
        startIdx = endIdx + 1;
        endIdx = rawContent.indexOf('\n', startIdx);
    }
    
    if (startIdx < rawContent.length()) {
        String lastLine = rawContent.substring(startIdx);
        lastLine.trim();
        if (lastLine.length() > 0) {
            memorizeLines.push_back(lastLine);
        }
    }

    int totalLines = memorizeLines.size();
    if (totalLines > 0) {
        // Now this will execute perfectly!
        memorizeSequence = generate_hybrid_sequence(totalLines);
    }
}

// System JSON Management
bool loadDataFromJSON() {
    if (!LittleFS.exists("/data.json")) return false;
    File file = LittleFS.open("/data.json", "r");
    if (!file) return false;

    DynamicJsonDocument* doc = new DynamicJsonDocument(32768);
    DeserializationError error = deserializeJson(*doc, file);
    file.close();
    if (error) { 
        delete doc;
        return false; 
    }

    // 1. Parse Notes
    items.clear();
    JsonArray notesArr = (*doc)["notes"];
    for (JsonObject note : notesArr) {
        items.push_back({note["title"].as<String>(), note["content"].as<String>()});
    }

    if (doc->containsKey("sleep_quote") && !(*doc)["sleep_quote"].isNull()) {
        customSleepQuote = (*doc)["sleep_quote"].as<String>();
    } else {
        customSleepQuote = ""; // Fallback state handled in triggerDeepSleep
    }

    // 2. Parse Flashcards
    flashDecks.clear();
    JsonArray decksArr = (*doc)["flashcards"];
    for (JsonObject deck : decksArr) {
        FlashcardDeck d;
        d.title = deck["title"].as<String>();
        JsonArray cardsArr = deck["cards"];
        for (JsonObject card : cardsArr) {
            d.cards.push_back({card["front"].as<String>(), card["back"].as<String>()});
        }
        flashDecks.push_back(d);
    }

    // 3. Parse Stories
    stories.clear();
    JsonArray storiesArr = (*doc)["stories"];
    for (JsonObject story : storiesArr) {
        stories.push_back({story["title"].as<String>(), story["text"].as<String>()});
    }

    // 4. Parse Your Brand New Memorize Array
    memorizeItems.clear();
    if ((*doc).containsKey("memorize")) {
        JsonArray memorizeArr = (*doc)["memorize"];
        for (JsonObject mem : memorizeArr) {
            memorizeItems.push_back({mem["title"].as<String>(), mem["content"].as<String>()});
        }
    }

    delete doc;
    return true;
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    uint16_t *buffer = (uint16_t *)color_map;
    driver->EPD_Clear();
    for(int y = area->y1; y <= area->y2; y++) {
        for(int x = area->x1; x <= area->x2; x++) {
            uint8_t color = (*buffer < 0x7fff) ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE;
            driver->EPD_DrawColorPixel(x, y, color);
            buffer++;
        }
    }
    driver->EPD_DisplayPart();
    lv_disp_flush_ready(drv);
}

static void example_increase_lvgl_tick(void *arg) {
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static bool example_lvgl_lock(int timeout_ms) {
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;       
}

static void example_lvgl_unlock(void) { xSemaphoreGive(lvgl_mux); }

static void example_lvgl_port_task(void *arg) {
    uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    for(;;) {
        if (example_lvgl_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            example_lvgl_unlock();
        }
        if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

void lvgl_port(void) {
    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t disp_drv;
    
    lv_init();
    lv_color_t *buffer_1 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
    lv_color_t *buffer_2 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
    assert(buffer_1); assert(buffer_2);
    lv_disp_draw_buf_init(&disp_buf, buffer_1, buffer_2, EPD_WIDTH * EPD_HEIGHT);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EPD_WIDTH;
    disp_drv.ver_res = EPD_HEIGHT;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.full_refresh = 1;
    lv_disp_drv_register(&disp_drv);
    esp_timer_create_args_t lvgl_tick_timer_args = {};
    lvgl_tick_timer_args.callback = &example_increase_lvgl_tick;
    lvgl_tick_timer_args.name = "lvgl_tick";
    esp_timer_handle_t lvgl_tick_timer = NULL;
    esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer);
    esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000);
    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);
    xTaskCreatePinnedToCore(example_lvgl_port_task, "LVGL", 8 * 1024, NULL, 4, NULL, 1);
}

void navigateTo(int nextScreen) {
    if (currentScreen == SCREEN_CONNECT && nextScreen != SCREEN_CONNECT) {
        stopWiFiAP();
    }
    previousScreen = currentScreen;
    currentScreen = (ScreenType)nextScreen;
}

// Screen Presentations
void showHomeScreen() {
    if(!example_lvgl_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);

    lv_obj_t *lblHome = lv_label_create(scr);
    char homeLayout[128];
    sprintf(homeLayout, "Protolo\n\nAnand\n\n\nBattery %d%%\nMemory %d%%", getBatteryPercent(), getMemoryUsagePercent());
    
    lv_label_set_text(lblHome, homeLayout);
    lv_obj_align(lblHome, LV_ALIGN_CENTER, 0, 0);

    navigateTo(SCREEN_HOME);
    
    lv_timer_handler(); 
    example_lvgl_unlock();
}

void showMenuScreen() {
    if(!example_lvgl_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    menuLabel = lv_label_create(scr);
    lv_obj_align(menuLabel, LV_ALIGN_CENTER, 0, 0);
    navigateTo(SCREEN_MENU);
    example_lvgl_unlock();
    refreshMenu();
}

void refreshMenu() {
    if(menuLabel == nullptr || !example_lvgl_lock(-1)) return;
    String menuStr = "";
    const char* options[] = {"home", "notes", "Flash cards", "stories", "memorize", "connect", "sleep"};
    
    // Restored the missing loop initialization line here
    for(int i = 0; i < MENU_ITEMS; i++) {
        menuStr += (selectedIndex == i) ? "-> " : "   ";
        menuStr += options[i]; 
        menuStr += "\n";
    }

    lv_label_set_text(menuLabel, menuStr.c_str());
    example_lvgl_unlock();
}

void showNotesList() {
    if(!example_lvgl_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    contentLabel = lv_label_create(scr);
    lv_obj_align(contentLabel, LV_ALIGN_CENTER, 0, 0);
    navigateTo(SCREEN_NOTES_LIST);
    example_lvgl_unlock();
    refreshNotesList();
}

void refreshNotesList() {
    if(!example_lvgl_lock(-1)) return;
    String txt = items.empty() ? "Empty Directory" : "";
    for(size_t i = 0; i < items.size(); i++) {
        txt += ((int)i == selectedItemIndex) ? "-> " : "   ";
        txt += items[i].title + "\n";
    }
    lv_label_set_text(contentLabel, txt.c_str());
    example_lvgl_unlock();
}

void showNotesView() {
    if(items.empty() || selectedItemIndex >= (int)items.size()) return;
    if(!example_lvgl_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    contentLabel = lv_label_create(scr);
    lv_obj_set_width(contentLabel, 180);
    lv_label_set_long_mode(contentLabel, LV_LABEL_LONG_WRAP);
    lv_label_set_text(contentLabel, items[selectedItemIndex].content.c_str());
    
    lv_obj_align(contentLabel, LV_ALIGN_TOP_MID, 0, noteScrollLine);

    navigateTo(SCREEN_NOTES_VIEW);
    example_lvgl_unlock();
}

void showFlashList() {
    if(!example_lvgl_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    contentLabel = lv_label_create(scr);
    lv_obj_align(contentLabel, LV_ALIGN_CENTER, 0, 0);
    navigateTo(SCREEN_FLASH_LIST);
    example_lvgl_unlock();
    refreshFlashList();
}

void refreshFlashList() {
    if(!example_lvgl_lock(-1)) return;
    String txt = flashDecks.empty() ? "No decks found" : "";
    for(size_t i = 0; i < flashDecks.size(); i++) {
        txt += ((int)i == selectedFlashDeckIndex) ? "-> " : "   ";
        txt += flashDecks[i].title + "\n";
    }
    lv_label_set_text(contentLabel, txt.c_str());
    example_lvgl_unlock();
}

void showFlashView() {
    auto& deck = flashDecks[selectedFlashDeckIndex];
    if(flashDecks.empty() || deck.cards.empty()) { showFlashList(); return; }
    if(!example_lvgl_lock(-1)) return;
    
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);

    headerLabel = lv_label_create(scr);
    char metaHead[32];
    sprintf(metaHead, "(%d/%d %s)", selectedFlashcardIndex + 1, deck.cards.size(), showingFlashcardFront ? "front" : "back");
    lv_label_set_text(headerLabel, metaHead);
    lv_obj_align(headerLabel, LV_ALIGN_TOP_MID, 0, 10);

    contentLabel = lv_label_create(scr);
    lv_obj_set_width(contentLabel, 180);
    lv_label_set_long_mode(contentLabel, LV_LABEL_LONG_WRAP);
    
    int cardIdx = flashcardOrder[selectedFlashcardIndex];
    lv_label_set_text(contentLabel, showingFlashcardFront ? deck.cards[cardIdx].front.c_str() : deck.cards[cardIdx].back.c_str());
    lv_obj_align(contentLabel, LV_ALIGN_CENTER, 0, 15);
    
    navigateTo(SCREEN_FLASH_VIEW);
    example_lvgl_unlock();
}

void showMemorizeList() {
    if(!example_lvgl_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    contentLabel = lv_label_create(scr);
    lv_obj_align(contentLabel, LV_ALIGN_CENTER, 0, 0);
    navigateTo(SCREEN_MEMORIZE_LIST);
    example_lvgl_unlock();
    refreshMemorizeList();
}

void refreshMemorizeList() {
    if(!example_lvgl_lock(-1)) return;
    String txt = memorizeItems.empty() ? "No Content to Learn" : "";
    for(size_t i = 0; i < memorizeItems.size(); i++) {
        txt += ((int)i == selectedItemIndex) ? "-> " : "   ";
        txt += memorizeItems[i].title + "\n";
    }
    lv_label_set_text(contentLabel, txt.c_str());
    example_lvgl_unlock();
}

void showMemorizeView() {
    if(!example_lvgl_lock(-1)) return;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    currentScreen = SCREEN_MEMORIZE_VIEW;

    lv_obj_t *lblWord = lv_label_create(scr);
    lv_label_set_long_mode(lblWord, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lblWord, EPD_WIDTH - 20);
    
    // Verify our lookup limits inside the hybrid generation matrix bounds
    if (!memorizeSequence.empty() && currentMemorizeIndex < (int)memorizeSequence.size()) {
        
        // Your hybrid model returns 1-based values (1, 2, 3..). Convert to 0-indexed lookup:
        int targetLineIndex = memorizeSequence[currentMemorizeIndex] - 1;
        
        if (targetLineIndex >= 0 && targetLineIndex < (int)memorizeLines.size()) {
            String currentLine = memorizeLines[targetLineIndex];
            lv_label_set_text(lblWord, currentLine.c_str());
        } else {
            lv_label_set_text(lblWord, "Indexing bounds mismatch.");
        }
    } else {
        lv_label_set_text(lblWord, "Learning complete!");
    }

    lv_obj_align(lblWord, LV_ALIGN_CENTER, 0, 0);
    lv_timer_handler();
    example_lvgl_unlock();
}


void showStoriesList() {
    if(!example_lvgl_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    contentLabel = lv_label_create(scr);
    lv_obj_align(contentLabel, LV_ALIGN_CENTER, 0, 0);
    navigateTo(SCREEN_STORIES_LIST);
    example_lvgl_unlock();
    refreshStoriesList();
}

void refreshStoriesList() {
    if(!example_lvgl_lock(-1)) return;
    String txt = stories.empty() ? "Library Empty" : "";
    for(size_t i = 0; i < stories.size(); i++) {
        txt += ((int)i == selectedStoryIndex) ? "-> " : "   ";
        txt += stories[i].title + "\n";
    }
    lv_label_set_text(contentLabel, txt.c_str());
    example_lvgl_unlock();
}

void showStoriesView() {
    if(!example_lvgl_lock(-1)) return;
    
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    currentScreen = SCREEN_STORIES_VIEW;

    // 1. Create a scrolling container frame that matches the E-Paper display boundaries
    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, EPD_WIDTH, EPD_HEIGHT);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 0, 0);
    
    // 2. Add the long content text label inside the container
    lv_obj_t *lblStory = lv_label_create(cont);
    lv_label_set_long_mode(lblStory, LV_LABEL_LONG_WRAP); // Force crisp text-wrapping
    lv_obj_set_width(lblStory, EPD_WIDTH - 10);          // Leave a slight margin for cleanly rendered letters
    
    // 3. Extract and display the active story content
    if (!stories.empty() && selectedStoryIndex < stories.size()) {
        lv_label_set_text(lblStory, stories[selectedStoryIndex].text.c_str());
    } else {
        lv_label_set_text(lblStory, "No story content found.");
    }

    // 4. Manually offset the content text based on your scrolling button input tick
    lv_obj_set_pos(lblStory, 5, storyScrollY);

    lv_timer_handler();
    example_lvgl_unlock();
}

void startWiFiAP() {
    if (!wifiRunning) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP(ap_ssid, ap_password);
        setupWebServer();
        wifiRunning = true;
        wifiStartTime = millis();
    }
}

void stopWiFiAP() {
    if (wifiRunning) {
        if (server != nullptr) { server->stop(); delete server; server = nullptr; }
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        wifiRunning = false;
    }
}

void showConnectScreen() {
    // Zero radio initialization here! Keeping the RF core completely off.
    selectedNetworkIndex = 0;
    currentScreen = SCREEN_CONNECT_WIFI_LIST;
    refreshWiFiListView();
}

void refreshWiFiListView() {
    if(!example_lvgl_lock(-1)) return;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Select Network");
    lv_obj_set_pos(title, 10, 10);

    int yOffset = 50;
    for (int i = 0; i < myNetworksCount; i++) {
        lv_obj_t *lblNet = lv_label_create(scr);
        
        // Highlight active cursor item
        if (i == selectedNetworkIndex) {
            String highlightedText = "> " + String(myNetworks[i].displayName);
            lv_label_set_text(lblNet, highlightedText.c_str());
        } else {
            String normalText = "  " + String(myNetworks[i].displayName);
            lv_label_set_text(lblNet, normalText.c_str());
        }
        
        lv_obj_set_pos(lblNet, 15, yOffset);
        yOffset += 35;
    }

    lv_timer_handler();
    example_lvgl_unlock();
}


void connectToSelectedWiFiAndSync() {
    if (selectedNetworkIndex < 0 || selectedNetworkIndex >= myNetworksCount) return;

    const char* targetSSID = myNetworks[selectedNetworkIndex].ssid;
    const char* targetPass = myNetworks[selectedNetworkIndex].password;

    if(!example_lvgl_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    
    lv_obj_t *lblStatus = lv_label_create(scr);
    String connectingMsg = "Connecting to:\n" + String(myNetworks[selectedNetworkIndex].displayName);
    lv_label_set_text(lblStatus, connectingMsg.c_str());
    lv_obj_align(lblStatus, LV_ALIGN_CENTER, 0, 0);
    
    lv_timer_handler();
    example_lvgl_unlock();

    WiFi.mode(WIFI_STA);
    WiFi.begin(targetSSID, targetPass);

    int timeoutCount = 0;
    while (WiFi.status() != WL_CONNECTED && timeoutCount < 20) {
        delay(500);
        Serial.print(".");
        timeoutCount++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (server == nullptr) {
            server = new WebServer(80);
        }
        
        server->on("/", HTTP_GET, handleRoot);
        server->on("/save", HTTP_POST, handleSave);
        server->begin();

        if(!example_lvgl_lock(-1)) return;
        lv_obj_clean(scr);
        lv_obj_t *lblIp = lv_label_create(scr);
        String ipMsg = "Portal Active!\nhttp://" + WiFi.localIP().toString() + "\n\nDouble-click POWER\nTo Disconnect & Exit";
        lv_label_set_text(lblIp, ipMsg.c_str());
        lv_obj_align(lblIp, LV_ALIGN_CENTER, 0, 0);
        lv_timer_handler();
        example_lvgl_unlock();

        powerClickCount = 0; 
        currentScreen = SCREEN_CONNECT_WIFI_LIST; // Relies on main loop to remain responsive!
        wifiRunning = true; 
    } else {
        Serial.println("\nWiFi Connection Handshake Failed.");
        if(!example_lvgl_lock(-1)) return;
        lv_obj_clean(scr);
        lv_obj_t *lblErr = lv_label_create(scr);
        lv_label_set_text(lblErr, "Connection Failed!\nReturning to Menu...");
        lv_obj_align(lblErr, LV_ALIGN_CENTER, 0, 0);
        lv_timer_handler();
        example_lvgl_unlock();
        delay(2500);
        
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        wifiRunning = false;
        showMenuScreen();
    }
}

void setupWebServer() {
    server = new WebServer(80);
    server->on("/", HTTP_GET, []() {
        File file = LittleFS.open("/data.json", "r");
        String currentJsonString = "{}";
        if(file) { currentJsonString = file.readString(); file.close(); }

        String html = "<html><body style='font-family:sans-serif; margin:20px; max-width:600px;'>";
        html += "<h2>Protolo Control Room</h2>";
        html += "<form action='/save_json' method='POST'>";
        html += "<textarea name='json_data' style='width:100%; height:400px; font-family:monospace;'>";
        html += currentJsonString;
        html += "</textarea><br><br>";
        html += "<input type='submit' value='Save Configurations & Updates' style='padding:10px;'>";
        html += "</form></body></html>";
        server->send(200, "text/html", html);
    });
    server->on("/save_json", HTTP_POST, []() {
        if (server->hasArg("json_data")) {
            String freshData = server->arg("json_data");
            File file = LittleFS.open("/data.json", "w");
            if (file) {
                file.print(freshData);
                file.close();
                loadDataFromJSON(); 
            }
        }
        server->sendHeader("Location", "/");
        server->send(303);
    });
    server->begin();
}

// ==========================================
// SYNC ENGINE: FETCH DATA FROM CLOUD API
// ==========================================
bool fetchDataFromAPI() {
    Serial.println("Local Engine Mode: Skipping remote OpenShift URL query.");
    
    if (doc != nullptr) {
        Serial.println("Local data structures active and loaded.");
        return true;
    }
    
    Serial.println("No local content loaded yet. Use the local web dashboard panel.");
    return false; 
}

void showImageLockScreen() {
    if(!example_lvgl_lock(-1)) return;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr); 
    currentScreen = SCREEN_HOME;

    // Create a new image layout container on screen
    lv_obj_t * img_bin = lv_img_create(scr);
    
    // Matrix structural mapping config header matching your black & white layout bounds
    static lv_img_dsc_t my_image_descriptor = {
        .header = {
            .cf = LV_IMG_CF_ALPHA_1BIT, // 1-bit monochrome image mapping profile
            .always_zero = 0,
            .w = 250,                   // Change if your screen layout width differs
            .h = 122                    // Change if your screen layout height differs
        },
        .data_size = sizeof(lockscreen_bitmap),
        .data = lockscreen_bitmap
    };

    lv_img_set_src(img_bin, &my_image_descriptor);
    lv_obj_align(img_bin, LV_ALIGN_CENTER, 0, 0); // Centers the graphic layout perfectly

    lv_timer_handler();
    example_lvgl_unlock();
}

void triggerDeepSleep() {
    if(!example_lvgl_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);

    lv_obj_t *lblSleep = lv_label_create(scr);
    
    // Set explicit wrapping mode and text alignment
    lv_label_set_long_mode(lblSleep, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lblSleep, 220); // Margins to avoid edge clipping
    lv_obj_set_style_text_align(lblSleep, LV_TEXT_ALIGN_CENTER, 0);
    
    // Using the accurate LVGL style functions to prevent letter distortion
    lv_obj_set_style_text_letter_space(lblSleep, 0, 0);
    lv_obj_set_style_text_line_space(lblSleep, 4, 0); 

    if (customSleepQuote.length() > 0) {
        lv_label_set_text(lblSleep, customSleepQuote.c_str());
    } else {
        lv_label_set_text(lblSleep, "Status: 204\nNo Content\n\nAnand is braining\n\nCome back later\nOr don't\n\nWhatever");
    }
    
    lv_obj_align(lblSleep, LV_ALIGN_CENTER, 0, 0);
    
    navigateTo(SCREEN_SLEEP);
    lv_timer_handler();
    example_lvgl_unlock();
    
    delay(2000); // Allow physical e-paper panel drawing cycle to complete
    
    // Direct hardware low-power state transition
    pinMode(EPD_PWR_PIN, OUTPUT);
    pinMode(Audio_PWR_PIN, OUTPUT);
    digitalWrite(EPD_PWR_PIN, LOW);
    digitalWrite(Audio_PWR_PIN, LOW);
    
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiRunning = false;
    
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BOOT_BUTTON_PIN, 0); 
    esp_deep_sleep_start();
}

void setup() {
    Serial.begin(115200);
    
    // Cleaned up the 30-second blocking while loop!
    if (Serial) {
        Serial.println("Protolo Booting Up...");
    }

    if(!LittleFS.begin(true)){ Serial.println("LittleFS Mount Failed"); }

    power.POWEER_EPD_ON();
    power.VBAT_POWER_ON();
    adc_bsp_init();
    delay(250); // Let battery voltage settle
    WiFi.mode(WIFI_OFF);
    
    if (!loadFromFlashMemory()) {
        Serial.println("Starting with empty structures or hardcoded defaults.");
    }

    lv_style_init(&styleTitle);
    lv_style_set_text_font(&styleTitle, &lv_font_montserrat_14); 

    custom_lcd_spi_t cfg = {};
    cfg.cs         = EPD_CS_PIN;
    cfg.dc         = EPD_DC_PIN;
    cfg.rst        = EPD_RST_PIN;
    cfg.busy       = EPD_BUSY_PIN;
    cfg.mosi       = EPD_MOSI_PIN;
    cfg.scl        = EPD_SCK_PIN;
    cfg.spi_host   = EPD_SPI_NUM;
    cfg.buffer_len = 5000;

    driver = new epaper_driver_display(EPD_WIDTH, EPD_HEIGHT, cfg);
    driver->EPD_Init();
    driver->EPD_Clear();
    driver->EPD_DisplayPartBaseImage();
    driver->EPD_Init_Partial();
    
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(PWR_BUTTON_PIN, INPUT_PULLUP);

    lvgl_port();
    
    // Set the initial active time right before showing the screen
    homeScreenActiveStart = millis(); 
    showHomeScreen();
}

void loop() {
    // 1. Explicit Sleep Timer removed from here entirely! 
    // Sleep is now cleanly triggered only when selecting "sleep" from the menu.

    if (wifiRunning && server != nullptr) {
        server->handleClient();
        if (millis() - wifiStartTime > WIFI_TIMEOUT_MS) {
            stopWiFiAP();
            showHomeScreen();
        }
    }

    bool currentBoot = digitalRead(BOOT_BUTTON_PIN);
    bool currentPower = digitalRead(PWR_BUTTON_PIN);

    // ==========================================
    // 1. BOOT (UP) BUTTON: CLICK TIMING ENGINE
    // ==========================================
    if (lastBoot == HIGH && currentBoot == LOW) {
        delay(20); // Debounce
        if(digitalRead(BOOT_BUTTON_PIN) == LOW) {
            unsigned long now = millis();
            if (bootClickCount == 0) {
                bootClickTime = now;
                bootClickCount = 1;
            } else if (bootClickCount == 1 && (now - bootClickTime < DOUBLE_CLICK_WINDOW)) {
                bootClickCount = 2;
            }
        }
    }
    lastBoot = currentBoot;

    // ==========================================
    // 2. BOOT (UP) BUTTON: ACTION EXECUTOR
    // ==========================================
    if (bootClickCount > 0) {
        unsigned long now = millis();
        if (bootClickCount == 2) {
            if(currentScreen == SCREEN_MENU) {
                selectedIndex = (selectedIndex - 1 + MENU_ITEMS) % MENU_ITEMS;
                refreshMenu();
            }
            else if(currentScreen == SCREEN_NOTES_LIST || currentScreen == SCREEN_MEMORIZE_LIST) {
                if(currentScreen == SCREEN_NOTES_LIST) {
                    if (!items.empty()) selectedItemIndex = (selectedItemIndex - 1 + items.size()) % items.size();
                    refreshNotesList();
                } else {
                    // ✨ FIX: Map directly to memorizeItems bounds and functional names
                    if (!memorizeItems.empty()) selectedItemIndex = (selectedItemIndex - 1 + memorizeItems.size()) % memorizeItems.size();
                    refreshMemorizeList();
                }
            }
            else if(currentScreen == SCREEN_FLASH_LIST) {
                if (!flashDecks.empty()) selectedFlashDeckIndex = (selectedFlashDeckIndex - 1 + flashDecks.size()) % flashDecks.size();
                refreshFlashList();
            }
            else if(currentScreen == SCREEN_STORIES_LIST) {
                if (!stories.empty()) selectedStoryIndex = (selectedStoryIndex - 1 + stories.size()) % stories.size();
                refreshStoriesList();
            }
            else if(currentScreen == SCREEN_STORIES_VIEW) {
                storyScrollY += 30;
                if (storyScrollY > 0) storyScrollY = 0;
                showStoriesView();
            }
            else if(currentScreen == SCREEN_NOTES_VIEW) {
                noteScrollLine += 30;
                if (noteScrollLine > 0) noteScrollLine = 0;
                showNotesView();
            }
            else if (currentScreen == SCREEN_CONNECT_WIFI_LIST) {
                selectedNetworkIndex = (selectedNetworkIndex - 1 + myNetworksCount) % myNetworksCount;
                refreshWiFiListView();
            }
            bootClickCount = 0;
        }
        else if (bootClickCount == 1 && (now - bootClickTime >= DOUBLE_CLICK_WINDOW)) {
            if(currentScreen == SCREEN_MENU) {
                selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
                refreshMenu();
            }
            else if(currentScreen == SCREEN_NOTES_LIST || currentScreen == SCREEN_MEMORIZE_LIST) {
                if(currentScreen == SCREEN_NOTES_LIST) {
                    if (!items.empty()) selectedItemIndex = (selectedItemIndex + 1) % items.size();
                    refreshNotesList();
                } else {
                    // ✨ FIX: Map directly to memorizeItems bounds and functional names
                    if (!memorizeItems.empty()) selectedItemIndex = (selectedItemIndex + 1) % memorizeItems.size();
                    refreshMemorizeList();
                }
            }
            else if(currentScreen == SCREEN_FLASH_LIST) {
                if (!flashDecks.empty()) selectedFlashDeckIndex = (selectedFlashDeckIndex + 1) % flashDecks.size();
                refreshFlashList();
            }
            else if(currentScreen == SCREEN_STORIES_LIST) {
                if (!stories.empty()) selectedStoryIndex = (selectedStoryIndex + 1) % stories.size();
                refreshStoriesList();
            }
            else if(currentScreen == SCREEN_STORIES_VIEW) {
                storyScrollY -= 30;
                showStoriesView();
            }
            else if(currentScreen == SCREEN_NOTES_VIEW) {
                noteScrollLine -= 30;
                showNotesView();
            }
            else if(currentScreen == SCREEN_FLASH_VIEW) {
                auto& deck = flashDecks[selectedFlashDeckIndex];
                if (!deck.cards.empty()) {
                    selectedFlashcardIndex = random(0, deck.cards.size());
                    showingFlashcardFront = true;
                    showFlashView();
                }
            }
            else if (currentScreen == SCREEN_CONNECT_WIFI_LIST) {
                selectedNetworkIndex = (selectedNetworkIndex + 1) % myNetworksCount;
                refreshWiFiListView();
            }
            bootClickCount = 0;
        }
    }

    // ==========================================
    // 3. BOTTOM (POWER) BUTTON: CLICK TIMING ENGINE
    // ==========================================
    if (lastPower == HIGH && currentPower == LOW) {
        delay(20); // Debounce
        if(digitalRead(PWR_BUTTON_PIN) == LOW) {
            unsigned long now = millis();
            if (powerClickCount == 0) {
                powerClickTime = now;
                powerClickCount = 1;
            } else if (powerClickCount == 1 && (now - powerClickTime < DOUBLE_CLICK_WINDOW)) {
                powerClickCount = 2;
            }
        }
    }
    lastPower = currentPower;

    // ==========================================
    // 4. BOTTOM (POWER) BUTTON: ACTION EXECUTOR
    // ==========================================
    if (powerClickCount > 0) {
        unsigned long now = millis();
        if (powerClickCount == 2) { // DOUBLE CLICK TO GO BACK TREE
            if (currentScreen == SCREEN_MENU) {
                showHomeScreen();
            }
            else if (currentScreen == SCREEN_NOTES_LIST || currentScreen == SCREEN_FLASH_LIST || 
                     currentScreen == SCREEN_MEMORIZE_LIST || currentScreen == SCREEN_STORIES_LIST || 
                     currentScreen == SCREEN_CONNECT) {
                showMenuScreen();
            }
            else if (currentScreen == SCREEN_NOTES_VIEW) {
                noteScrollLine = 0;
                showNotesList();
            }
            else if (currentScreen == SCREEN_STORIES_VIEW) {
                storyScrollY = 0; // Reset scroll offset tracking
                showStoriesList();
            }
            else if (currentScreen == SCREEN_FLASH_VIEW) {
                showFlashList();
            }
            else if (currentScreen == SCREEN_MEMORIZE_VIEW) {
                currentMemorizeIndex = 0; // Reset line progression index
                showMemorizeList();
            }
            else if (currentScreen == SCREEN_CONNECT_WIFI_LIST) {
                Serial.println("Backing out of portal layout. Tearing down active server instances...");
                if (server != nullptr) {
                    server->stop();
                }
                
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                wifiRunning = false;
                Serial.println("Wi-Fi Core Radio powered off cleanly. Battery footprint protected.");
                
                currentScreen = SCREEN_MENU;
                showMenuScreen();
            }
            else {
                showHomeScreen();
            }
            powerClickCount = 0;
        }
        else if (powerClickCount == 1 && (now - powerClickTime >= DOUBLE_CLICK_WINDOW)) { // 🟢 SINGLE CLICK TO SELECT/ADVANCE TREE
            if (currentScreen == SCREEN_HOME) { 
                showMenuScreen();
            }
            else if (currentScreen == SCREEN_MENU) {
                if(selectedIndex == 0)      showHomeScreen();
                else if(selectedIndex == 1) showNotesList();
                else if(selectedIndex == 2) showFlashList();
                else if(selectedIndex == 3) showStoriesList();
                else if(selectedIndex == 4) showMemorizeList();
                else if(selectedIndex == 5) showConnectScreen();
                else if(selectedIndex == 6) triggerDeepSleep();
            }
            else if (currentScreen == SCREEN_NOTES_LIST) { 
                noteScrollLine = 0;
                showNotesView();
            }
            else if (currentScreen == SCREEN_STORIES_LIST) { 
                storyScrollY = 0;
                showStoriesView();
            }
            else if (currentScreen == SCREEN_MEMORIZE_LIST) {
                // ✨ FIX: Load selected memorize content dataset loop into the tokenizer engine cleanly
                if (!memorizeItems.empty() && selectedItemIndex < memorizeItems.size()) {
                    prepareMemorizeSequence(memorizeItems[selectedItemIndex].content);
                    showMemorizeView();
                }
            }
            else if (currentScreen == SCREEN_FLASH_LIST) {
                if(!flashDecks.empty() && !flashDecks[selectedFlashDeckIndex].cards.empty()) {
                    size_t cardCount = flashDecks[selectedFlashDeckIndex].cards.size();
                    flashcardOrder.clear();
                    for(size_t i = 0; i < cardCount; i++) flashcardOrder.push_back(i);
                    selectedFlashcardIndex = 0; 
                    showingFlashcardFront = true; 
                    showFlashView();
                }
            }
            else if (currentScreen == SCREEN_FLASH_VIEW) {
                showingFlashcardFront = !showingFlashcardFront;
                showFlashView();
            }
            else if (currentScreen == SCREEN_MEMORIZE_VIEW) {
                // Advance inside line item collection array bounds
                if (!memorizeSequence.empty() && currentMemorizeIndex < (int)memorizeSequence.size() - 1) {
                    currentMemorizeIndex++;
                    showMemorizeView();
                } else {
                    // Full repetition loop path finished, take user back to lists select deck
                    currentMemorizeIndex = 0;
                    showMemorizeList();
                }
            }
            else if (currentScreen == SCREEN_CONNECT_WIFI_LIST) {
                connectToSelectedWiFiAndSync(); // Kick off authentication and retrieval engine sequence
            }
            powerClickCount = 0;
        }
    }

    if (WiFi.status() == WL_CONNECTED && server != nullptr) {
        server->handleClient(); // Fixed: changed . to ->
    }

    delay(!wifiRunning ? 60 : 2);
}

