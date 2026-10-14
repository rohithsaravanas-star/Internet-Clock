#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"
#define WEATHER_API_KEY "YOUR_OPENWEATHERMAP_API_KEY"
#endif

// =====================================================
// Wi-Fi and OpenWeatherMap
// =====================================================
#define WEATHER_CITY    "Coimbatore"
#define WEATHER_COUNTRY "IN"

const unsigned long WEATHER_INTERVAL = 600000UL;

// India Standard Time
#define TZ_STRING "IST-5:30"

// =====================================================
// ST7735 TFT - NORMAL ESP32
// =====================================================
#define TFT_CS    5
#define TFT_RST   4
#define TFT_DC    2
#define TFT_MOSI  23
#define TFT_SCLK  18
#define TFT_BL    27

Adafruit_ST7735 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);

// =====================================================
// Display dimensions
// =====================================================
#define SCREEN_W 128
#define SCREEN_H 160

// =====================================================
// RGB565 colors
// =====================================================
#define RGB565(r,g,b) ((uint16_t)((((r)&0xF8)<<8) | \
                         (((g)&0xFC)<<3) | ((b)>>3)))

#define COLOR_BG          RGB565(0,0,0)
#define COLOR_PANEL       RGB565(18,18,20)
#define COLOR_BORDER      RGB565(70,70,76)
#define COLOR_DIVIDER     RGB565(40,40,44)
#define COLOR_WHITE       RGB565(255,255,255)
#define COLOR_GREY        RGB565(130,130,136)
#define COLOR_RED         RGB565(230,40,40)
#define COLOR_SUN         RGB565(255,200,70)
#define COLOR_CLOUD       RGB565(150,160,175)
#define COLOR_RAIN        RGB565(90,150,220)

// =====================================================
// Layout
// =====================================================
const int STATUS_Y = 3;
const int CLOCK_Y = 20;
const int CLOCK_SIZE = 4;
const int DATE_Y = 60;
const int DIVIDER_Y = 74;

const int CARD_X = 6;
const int CARD_Y = 82;
const int CARD_W = 116;
const int CARD_H = 72;

// =====================================================
// Global variables
// =====================================================
struct tm currentTime;

bool timeAvailable = false;
bool wifiConnected = false;
bool weatherAvailable = false;

int clockHour = 0;
int clockMinute = 0;
int dayOfWeek = 0;
int dateDay = 1;
int dateMonth = 1;

float outdoorTempC = 0.0;
String weatherMain = "Clouds";

unsigned long lastClockTick = 0;
unsigned long lastWeatherFetch = 0;
unsigned long lastTimeRetry = 0;
unsigned long lastReconnect = 0;

const char *DAY_NAMES[] = {
  "SUN", "MON", "TUE", "WED",
  "THU", "FRI", "SAT"
};

const char *MONTH_NAMES[] = {
  "JAN", "FEB", "MAR", "APR",
  "MAY", "JUN", "JUL", "AUG",
  "SEP", "OCT", "NOV", "DEC"
};

// =====================================================
// Function declarations
// =====================================================
void setupTFT();
void connectWiFi();
void setupTimeNTP();
void setSystemTimeUTC(time_t epoch);
void syncClockFromRTC();
void fetchWeather();

void drawStaticFrame();
void drawStatusBar();
void drawWifiBars(int x, int y, bool connected);
void drawClock(bool force);
void drawDateRow(bool force);
void drawDivider();

void drawWeatherCard();
void drawWeatherIcon(const String &condition, int x, int y);
void drawSunCloudIcon(int x, int y);
void drawRainIcon(int x, int y);
void drawTempValue(int x, int y, float temp, bool valid);
void drawBoldChar(int x, int y, char c, int size, uint16_t color);

// =====================================================
// TFT initialization
// =====================================================
void setupTFT()
{
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(0);
  tft.fillScreen(COLOR_BG);
}

// =====================================================
// Wi-Fi connection
// =====================================================
void connectWiFi()
{
  Serial.println("\nConnecting to Wi-Fi...");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED &&
         attempts < 30)
  {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  Serial.println();

  wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiConnected)
  {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println("Wi-Fi connection failed.");
  }
}

// =====================================================
// NTP time synchronization
// =====================================================
void setupTimeNTP()
{
  Serial.println("Synchronizing time...");

  configTzTime(
    TZ_STRING,
    "pool.ntp.org",
    "time.nist.gov",
    "time.google.com"
  );

  for (int i = 0; i < 15; i++)
  {
    if (getLocalTime(&currentTime, 1000))
    {
      timeAvailable = true;
      Serial.println("Time synchronized.");
      return;
    }
  }

  timeAvailable = false;
  Serial.println("NTP synchronization failed.");
}

// =====================================================
// Set time using UTC timestamp from weather API
// =====================================================
void setSystemTimeUTC(time_t epoch)
{
  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;

  settimeofday(&tv, nullptr);

  setenv("TZ", TZ_STRING, 1);
  tzset();

  timeAvailable = true;

  Serial.println("Time set using weather API.");
}

// =====================================================
// Fetch weather from OpenWeatherMap
// =====================================================
void fetchWeather()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    weatherAvailable = false;
    return;
  }

  Serial.println("\nFetching weather...");

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;

  String url =
    "https://api.openweathermap.org/data/2.5/weather?q=" +
    String(WEATHER_CITY) + "," +
    String(WEATHER_COUNTRY) +
    "&appid=" + String(WEATHER_API_KEY) +
    "&units=metric";

  if (!https.begin(client, url))
  {
    Serial.println("HTTPS initialization failed.");
    weatherAvailable = false;
    return;
  }

  int httpCode = https.GET();

  if (httpCode == HTTP_CODE_OK)
  {
    String payload = https.getString();

    JsonDocument doc;
    DeserializationError error =
      deserializeJson(doc, payload);

    if (!error)
    {
      outdoorTempC = doc["main"]["temp"] | 0.0f;

      weatherMain = String(
        (const char *)(doc["weather"][0]["main"] | "Clouds")
      );

      weatherAvailable = true;

      Serial.printf(
        "Temperature: %.1f C, Condition: %s\n",
        outdoorTempC,
        weatherMain.c_str()
      );

      if (!timeAvailable && !doc["dt"].isNull())
      {
        time_t epoch = doc["dt"].as<time_t>();
        setSystemTimeUTC(epoch);
      }
    }
    else
    {
      Serial.println("JSON parsing failed.");
      weatherAvailable = false;
    }
  }
  else
  {
    Serial.printf("Weather HTTP error: %d\n", httpCode);
    weatherAvailable = false;
  }

  https.end();
  drawWeatherCard();
}

// =====================================================
// Read time and date
// =====================================================
void syncClockFromRTC()
{
  if (!getLocalTime(&currentTime, 50))
  {
    timeAvailable = false;
    return;
  }

  timeAvailable = true;

  clockHour = currentTime.tm_hour;
  clockMinute = currentTime.tm_min;

  dayOfWeek = currentTime.tm_wday;
  dateDay = currentTime.tm_mday;
  dateMonth = currentTime.tm_mon + 1;
}

// =====================================================
// Draw bold character
// =====================================================
void drawBoldChar(
  int x, int y, char c,
  int size, uint16_t color)
{
  tft.setTextSize(size);
  tft.setTextColor(color);

  tft.setCursor(x, y);
  tft.print(c);

  tft.setCursor(x + 1, y);
  tft.print(c);
}

// =====================================================
// Static screen
// =====================================================
void drawStaticFrame()
{
  tft.fillScreen(COLOR_BG);
  drawDivider();
}

// =====================================================
// Wi-Fi status icon
// =====================================================
void drawWifiBars(int x, int y, bool connected)
{
  for (int i = 0; i < 3; i++)
  {
    int barH = 3 + i * 3;

    uint16_t color =
      connected ? COLOR_WHITE : COLOR_GREY;

    tft.fillRect(
      x + i * 5,
      y + (9 - barH),
      3,
      barH,
      color
    );
  }
}

// =====================================================
// Status bar
// =====================================================
void drawStatusBar()
{
  tft.fillRect(0, 0, SCREEN_W, 15, COLOR_BG);

  drawWifiBars(6, STATUS_Y, wifiConnected);

  tft.fillCircle(
    SCREEN_W - 8,
    STATUS_Y + 4,
    2,
    timeAvailable ? COLOR_RED : COLOR_GREY
  );
}

// =====================================================
// Clock display
// =====================================================
void drawClock(bool force)
{
  static char previous[6] = "";

  char buffer[6];

  snprintf(
    buffer, sizeof(buffer),
    "%02d:%02d",
    clockHour, clockMinute
  );

  if (!force && strcmp(buffer, previous) == 0)
    return;

  int charW = 6 * CLOCK_SIZE;
  int totalW = 5 * charW;
  int x = (SCREEN_W - totalW) / 2;

  tft.fillRect(0, CLOCK_Y - 2, SCREEN_W, 36, COLOR_BG);

  drawBoldChar(x,          CLOCK_Y, buffer[0], CLOCK_SIZE, COLOR_WHITE);
  drawBoldChar(x + charW,  CLOCK_Y, buffer[1], CLOCK_SIZE, COLOR_WHITE);
  drawBoldChar(x + 2 * charW, CLOCK_Y, ':',    CLOCK_SIZE, COLOR_RED);
  drawBoldChar(x + 3 * charW, CLOCK_Y, buffer[3], CLOCK_SIZE, COLOR_WHITE);
  drawBoldChar(x + 4 * charW, CLOCK_Y, buffer[4], CLOCK_SIZE, COLOR_WHITE);

  strcpy(previous, buffer);
}

// =====================================================
// Date display
// =====================================================
void drawDateRow(bool force)
{
  static char previous[24] = "";

  char buffer[24];

  snprintf(
    buffer, sizeof(buffer),
    "%s, %02d %s",
    DAY_NAMES[dayOfWeek],
    dateDay,
    MONTH_NAMES[dateMonth - 1]
  );

  if (!force && strcmp(buffer, previous) == 0)
    return;

  tft.fillRect(0, DATE_Y - 2, SCREEN_W, 12, COLOR_BG);

  tft.setTextSize(1);
  tft.setTextColor(COLOR_GREY);

  int16_t x1, y1;
  uint16_t w, h;

  tft.getTextBounds(buffer, 0, 0, &x1, &y1, &w, &h);

  tft.setCursor((SCREEN_W - w) / 2, DATE_Y);
  tft.print(buffer);

  strcpy(previous, buffer);
}

// =====================================================
// Divider
// =====================================================
void drawDivider()
{
  tft.drawFastHLine(
    6, DIVIDER_Y,
    SCREEN_W - 12,
    COLOR_DIVIDER
  );
}

// =====================================================
// Sun and cloud icon
// =====================================================
void drawSunCloudIcon(int x, int y)
{
  int sx = x + 9;
  int sy = y + 9;

  tft.fillCircle(sx, sy, 5, COLOR_SUN);

  for (int i = 0; i < 8; i++)
  {
    float a = i * 3.14159f / 4.0f;

    int x1 = sx + cos(a) * 7;
    int y1 = sy + sin(a) * 7;
    int x2 = sx + cos(a) * 10;
    int y2 = sy + sin(a) * 10;

    tft.drawLine(x1, y1, x2, y2, COLOR_SUN);
  }

  int cx = x + 15;
  int cy = y + 17;

  tft.fillCircle(cx - 4, cy, 4, COLOR_CLOUD);
  tft.fillCircle(cx + 2, cy - 2, 5, COLOR_CLOUD);
  tft.fillCircle(cx + 7, cy, 4, COLOR_CLOUD);
  tft.fillRect(cx - 4, cy, 11, 4, COLOR_CLOUD);
}

// =====================================================
// Rain icon
// =====================================================
void drawRainIcon(int x, int y)
{
  int cx = x + 13;
  int cy = y + 10;

  tft.fillCircle(cx - 5, cy, 5, COLOR_CLOUD);
  tft.fillCircle(cx + 2, cy - 3, 6, COLOR_CLOUD);
  tft.fillCircle(cx + 8, cy, 5, COLOR_CLOUD);
  tft.fillRect(cx - 5, cy, 13, 5, COLOR_CLOUD);

  for (int i = 0; i < 3; i++)
  {
    int rx = x + 5 + i * 6;

    tft.drawLine(
      rx, y + 18,
      rx - 2, y + 24,
      COLOR_RAIN
    );
  }
}

// =====================================================
// Select weather icon
// =====================================================
void drawWeatherIcon(
  const String &condition,
  int x, int y)
{
  if (condition == "Rain" ||
      condition == "Drizzle" ||
      condition == "Thunderstorm")
  {
    drawRainIcon(x, y);
  }
  else
  {
    drawSunCloudIcon(x, y);
  }
}

// =====================================================
// Temperature display
// =====================================================
void drawTempValue(
  int x, int y,
  float temp,
  bool valid)
{
  tft.setTextSize(2);
  tft.setTextColor(COLOR_WHITE);

  String value =
    valid ? String((int)round(temp)) : "--";

  int16_t x1, y1;
  uint16_t w, h;

  tft.getTextBounds(value, 0, 0, &x1, &y1, &w, &h);

  tft.setCursor(x, y);
  tft.print(value);

  if (valid)
  {
    int degreeX = x + w + 3;

    tft.drawCircle(degreeX, y + 2, 2, COLOR_WHITE);

    tft.setCursor(degreeX + 5, y);
    tft.print("C");
  }
}

// =====================================================
// Weather card
// =====================================================
void drawWeatherCard()
{
  tft.fillRoundRect(
    CARD_X, CARD_Y,
    CARD_W, CARD_H,
    6, COLOR_PANEL
  );

  tft.drawRoundRect(
    CARD_X, CARD_Y,
    CARD_W, CARD_H,
    6, COLOR_BORDER
  );

  // Location title
  const char *title = "COIMBATORE";

  tft.setTextSize(1);
  tft.setTextColor(COLOR_GREY);

  int16_t x1, y1;
  uint16_t w, h;

  tft.getTextBounds(title, 0, 0, &x1, &y1, &w, &h);

  tft.setCursor((SCREEN_W - w) / 2, CARD_Y + 5);
  tft.print(title);

  // Icon
  drawWeatherIcon(
    weatherMain,
    CARD_X + 17,
    CARD_Y + 20
  );

  // Temperature
  drawTempValue(
    CARD_X + 57,
    CARD_Y + 27,
    outdoorTempC,
    weatherAvailable
  );

  // Condition
  String condition =
    weatherAvailable ? weatherMain : "NO DATA";

  tft.setTextSize(1);
  tft.setTextColor(COLOR_GREY);

  tft.getTextBounds(condition, 0, 0, &x1, &y1, &w, &h);

  tft.setCursor((SCREEN_W - w) / 2, CARD_Y + 55);
  tft.print(condition);
}

// =====================================================
// SETUP
// =====================================================
void setup()
{
  Serial.begin(115200);
  delay(500);

  setupTFT();
  drawStaticFrame();
  drawStatusBar();

  connectWiFi();

  if (wifiConnected)
  {
    setupTimeNTP();
    fetchWeather();
  }

  syncClockFromRTC();

  drawStatusBar();
  drawClock(true);
  drawDateRow(true);
  drawWeatherCard();

  lastClockTick = millis();
  lastWeatherFetch = millis();
  lastTimeRetry = millis();
}

// =====================================================
// LOOP
// =====================================================
void loop()
{
  unsigned long now = millis();

  // Update clock every second
  if (now - lastClockTick >= 1000)
  {
    lastClockTick = now;

    syncClockFromRTC();

    drawStatusBar();
    drawClock(false);
    drawDateRow(false);
  }

  // Refresh weather every 10 minutes
  if (now - lastWeatherFetch >= WEATHER_INTERVAL)
  {
    lastWeatherFetch = now;

    if (WiFi.status() != WL_CONNECTED)
    {
      connectWiFi();
      wifiConnected = (WiFi.status() == WL_CONNECTED);
    }

    if (wifiConnected)
      fetchWeather();
  }

  // Retry NTP if time is unavailable
  if (!timeAvailable &&
      now - lastTimeRetry >= 10000)
  {
    lastTimeRetry = now;

    if (WiFi.status() == WL_CONNECTED)
      setupTimeNTP();
  }

  // Reconnect Wi-Fi
  if (WiFi.status() != WL_CONNECTED &&
      now - lastReconnect >= 10000)
  {
    lastReconnect = now;
    wifiConnected = false;

    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  delay(10);
}