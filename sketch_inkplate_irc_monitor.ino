#include <Inkplate.h>
#include <SdFat.h>
#include <WiFi.h>
#include <IRCClient.h>
#include <ArduinoJson.h>
#include <ezTime.h>

#define SERIAL_BAUDRATE 115200
#define FULL_REFRESH_MIN_INTERVAL_MS 5000
#define PARTIAL_REFRESH_MIN_INTERVAL_MS 500

#define CURSOR_SIZE_SCALE 4
#define CURSOR_SIZE_X (6 * CURSOR_SIZE_SCALE)
#define CURSOR_SIZE_Y (8 * CURSOR_SIZE_SCALE)

#define COLOR_3B_BLACK 0
#define COLOR_3B_DARK_GREY 4
#define COLOR_3B_LIGHT_GREY 6
#define COLOR_3B_WHITE 7

#define COLOR_1B_BLACK 1
#define COLOR_1B_WHITE 0

#define CONFIG_FILENAME "/libera.json"
#define CONFIG_DOC_SIZE 2048
#define CONFIG_STRING_SIZE 32

#define WIFI_CONNECTION_TIMEOUT_MS 30000
#define WIFI_CONNECTION_CHECK_INTERVAL_MS 500
#define TIME_SYNC_TIMEOUT_S 10
#define TIME_CLOCK_FORMAT "m/d H:i"

/*** CONFIG SCHEMA (sd-card:/config.json)
{
    "display": {
        "text-size": int,
        "invert-status-bar": bool,
        "log-to-display": bool
    },
    "wifi": {
        "auto-connect": bool,
        "ssid": string,
        "password": string
    },
    "irc": {
        "auto-connect": bool,
        "server": string,
        "port": int,
        "nickname": string,
        "username": string,
        "password": string,
        "channel": string
    },
    "time": {
      "ntp-server": string,
      "timezone": string,
      "use-rtc": bool
    }
}
***/

struct _DisplayConfig {
  int   text_size;
  bool  invert_status_bar;
  bool  log_to_display;
};
struct _WifiConfig {
  bool  auto_connect;
  char  ssid[CONFIG_STRING_SIZE];
  char  password[CONFIG_STRING_SIZE];
};
struct _IRCConfig {
  bool  auto_connect;
  char  server[CONFIG_STRING_SIZE];
  int   port;
  char  nickname[CONFIG_STRING_SIZE];
  char  username[CONFIG_STRING_SIZE];
  char  password[CONFIG_STRING_SIZE];
  char  channel[CONFIG_STRING_SIZE];
};
struct _TimeConfig {
  char  ntp_server[CONFIG_STRING_SIZE];
  char  timezone[CONFIG_STRING_SIZE];
  bool  use_rtc;
};
struct _Config {
  _DisplayConfig display;
  _WifiConfig wifi;
  _IRCConfig irc;
  _TimeConfig time;
};

Inkplate display(INKPLATE_1BIT);
WiFiClient wifiClient;
IRCClient* ircClient = NULL;

unsigned long _lastFullRefresh;
unsigned long _lastPartialRefresh;
void updateDisplay(bool wait=true) {
  auto now = millis();
  auto lastFullDiff = now - _lastFullRefresh;
  auto lastPartialDiff = now - _lastPartialRefresh;

  if(lastFullDiff > (FULL_REFRESH_MIN_INTERVAL_MS * 18)) {
    // do a full refresh every 90s
    display.display(true);
    _lastFullRefresh = _lastPartialRefresh = millis();
  } else {
    // do a partial refresh
    if(lastPartialDiff < PARTIAL_REFRESH_MIN_INTERVAL_MS) {
      if(wait) {
        delay(PARTIAL_REFRESH_MIN_INTERVAL_MS - lastPartialDiff);
        display.partialUpdate(true, true);
      } else {
        display.partialUpdate(false, true);
      }
    } else {
      display.partialUpdate(true, true);
    }
    _lastPartialRefresh = millis();    
  }
}

int willWrap(int charCount) {
  auto width_px = display.width();
  auto width_chars = width_px / CURSOR_SIZE_X;
  return (charCount - 1) / width_chars;
}

int _lastUsedLine;
void ircCallback(IRCMessage ircMessage) {
  Serial.println("R< " + ircMessage.original);
  if (ircMessage.command == "PRIVMSG" && ircMessage.text[0] != '\001') {
    String message("<" + ircMessage.nick + "> " + ircMessage.text);
    display.setTextWrap(true);
    display.setTextColor(COLOR_1B_BLACK);
    Serial.println(message);

    if(((_lastUsedLine + willWrap(message.length()) + 1) * CURSOR_SIZE_Y) > display.height()) {
      // wrap back to top of screen
      _lastUsedLine = 1;
      display.fillRect(0, CURSOR_SIZE_Y, display.width(), display.height() - CURSOR_SIZE_Y, COLOR_1B_WHITE);
    }
    display.setCursor(1 * CURSOR_SIZE_X, _lastUsedLine++ * CURSOR_SIZE_Y);
    display.println(message);
    _lastUsedLine += willWrap(message.length());
    updateDisplay(false);
  }
}
void ircSentCallback(String message) {
  Serial.println("S> " + message);
}

_Config CFG;
void loadConfig() {
  SdFile cfg_file;
  StaticJsonDocument<CONFIG_DOC_SIZE> doc;

  cfg_file.open(CONFIG_FILENAME, O_RDONLY);
  DeserializationError err = deserializeJson(doc, cfg_file);
  cfg_file.close();
  if(err) {
    // panic
    Serial.println("Failed to open config file: " + String(CONFIG_FILENAME));
    return;
  }

  CFG.display.text_size = doc["display"]["text-size"];
  CFG.display.invert_status_bar = doc["display"]["invert-status-bar"];
  CFG.display.log_to_display = doc["display"]["log-to-display"];

  CFG.wifi.auto_connect = doc["wifi"]["auto-connect"];
  memset(&(CFG.wifi.ssid), 0, sizeof(CFG.wifi.ssid));
  strlcpy(CFG.wifi.ssid,
    doc["wifi"]["ssid"] | "",
    sizeof(CFG.wifi.ssid));
  memset(&(CFG.wifi.password), 0, sizeof(CFG.wifi.password));
  strlcpy(CFG.wifi.password,
    doc["wifi"]["password"] | "",
    sizeof(CFG.wifi.password));

  CFG.irc.auto_connect = doc["irc"]["auto-connect"];
  memset(&(CFG.irc.server), 0, sizeof(CFG.irc.server));
  strlcpy(CFG.irc.server,
    doc["irc"]["server"] | "irc.libera.chat",
    sizeof(CFG.irc.server));
  CFG.irc.port = doc["irc"]["port"];
  memset(&(CFG.irc.username), 0, sizeof(CFG.irc.username));
  strlcpy(CFG.irc.username,
    doc["irc"]["username"] | "inkplate",
    sizeof(CFG.irc.username));
  memset(&(CFG.irc.nickname), 0, sizeof(CFG.irc.nickname));
  strlcpy(CFG.irc.nickname,
    doc["irc"]["nickname"] | "inkplate",
    sizeof(CFG.irc.nickname));
  memset(&(CFG.irc.password), 0, sizeof(CFG.irc.password));
  strlcpy(CFG.irc.password,
    doc["irc"]["password"] | "",
    sizeof(CFG.irc.password));
  memset(&(CFG.irc.channel), 0, sizeof(CFG.irc.channel));
  strlcpy(CFG.irc.channel,
    doc["irc"]["channel"] | "##inkplate",
    sizeof(CFG.irc.channel));

  memset(&(CFG.time.ntp_server), 0, sizeof(CFG.time.ntp_server));
  strlcpy(CFG.time.ntp_server,
    doc["time"]["ntp-server"] | "pool.ntp.org",
    sizeof(CFG.time.ntp_server));
  memset(&(CFG.time.timezone), 0, sizeof(CFG.time.timezone));
  strlcpy(CFG.time.timezone,
    doc["time"]["timezone"] | "UTC",
    sizeof(CFG.time.timezone));
  CFG.time.use_rtc = doc["time"]["use-rtc"];

  display.setTextColor(COLOR_1B_BLACK);
  display.setCursor(1 * CURSOR_SIZE_X, _lastUsedLine++ * CURSOR_SIZE_Y);
  display.println("Config File: sd:" + String(CONFIG_FILENAME));
  updateDisplay();
}

void connectToWifi() {
  Serial.print("Using wifi credentials: ");
  Serial.print(CFG.wifi.ssid);
  Serial.print(":");
  Serial.println(CFG.wifi.password);

  Serial.print("Connecting to wifi");
  auto wifi_conn_start = millis();

  WiFi.mode(WIFI_MODE_STA);
  WiFi.begin(CFG.wifi.ssid, CFG.wifi.password);
  while(!WiFi.isConnected()) {
    if((millis() - wifi_conn_start) > WIFI_CONNECTION_TIMEOUT_MS) {
      Serial.println("TIMEOUT");
      WiFi.disconnect();
      return;
    }
    Serial.print(".");
    delay(WIFI_CONNECTION_CHECK_INTERVAL_MS);
  }
  Serial.println("DONE");
  Serial.println("IP Address: " + WiFi.localIP().toString());

  display.setTextColor(COLOR_1B_BLACK);
  display.setCursor(1 * CURSOR_SIZE_X, _lastUsedLine++ * CURSOR_SIZE_Y);
  display.println("IP Address: " + WiFi.localIP().toString());
  updateDisplay();
}

void connectToIrc() {
  Serial.print("Connecting to IRC...");
  if(ircClient->connect(CFG.irc.nickname, CFG.irc.username, CFG.irc.password)) {
    Serial.println(" DONE");
    if(CFG.irc.channel) {
      ircClient->sendRaw("JOIN " + String(CFG.irc.channel));
    }

    display.setTextColor(COLOR_1B_BLACK);
    display.setCursor(1 * CURSOR_SIZE_X, _lastUsedLine++ * CURSOR_SIZE_Y);
    display.println("Connected to IRC");
    updateDisplay();
  } else {
    Serial.println(" FAILED");
  }
}

Timezone _TZ;
void setupTime() {
  ezt::setServer(CFG.time.ntp_server);
  _TZ.setLocation(CFG.time.timezone);

  if(CFG.time.use_rtc && display.rtcIsSet()) {
    // get time from rtc
    UTC.setTime(display.rtcGetEpoch(), 0);
    Serial.println("Time from RTC: " + UTC.dateTime(ISO8601));
  }
  ezt::waitForSync(TIME_SYNC_TIMEOUT_S);
  if(CFG.time.use_rtc) {
    // set time to rtc
    display.rtcSetDate(UTC.weekday(), UTC.day(), UTC.month(), UTC.year());
    display.rtcSetTime(UTC.hour(), UTC.minute(), UTC.second());
  }
  _TZ.setDefault();

  Serial.println("Synced time to: " + _TZ.dateTime(ISO8601));

  display.setTextColor(COLOR_1B_BLACK);
  display.setCursor(1 * CURSOR_SIZE_X, _lastUsedLine++ * CURSOR_SIZE_Y);
  display.print("Time: " + _TZ.dateTime(ISO8601));
  updateDisplay();
}

void setup() {
  _lastFullRefresh = 0;
  _lastPartialRefresh = 0;
  _lastUsedLine = 1;

  display.begin();
  display.clearDisplay();
  display.display(true);
  display.setTextWrap(false);
  display.setTextSize(CURSOR_SIZE_SCALE);

  Serial.begin(SERIAL_BAUDRATE);

  Serial.println("Starting...");
  display.setTextColor(COLOR_1B_BLACK);
  display.setCursor(1 * CURSOR_SIZE_X, _lastUsedLine++ * CURSOR_SIZE_Y);
  display.println("Starting...");
  updateDisplay();

  display.sdCardInit();
  loadConfig();

  if(CFG.wifi.auto_connect) {
    connectToWifi();
  }

  if(ircClient == NULL) {
    ircClient = new IRCClient(CFG.irc.server, CFG.irc.port, wifiClient);
  }

  ircClient->setCallback(ircCallback);
  ircClient->setSentCallback(ircSentCallback);

  setupTime();
}

void drawStatusBar() {
  auto clockText = _TZ.dateTime(TIME_CLOCK_FORMAT);
  char sensorsText[16];
  auto temp_C = display.readTemperature();
  auto batt_V = display.readBattery();

  auto screen_width = display.width();

  memset(&sensorsText, 0, sizeof(sensorsText));
  snprintf(sensorsText, sizeof(sensorsText), "%1.2fV %iC", batt_V, temp_C);

  auto sensorsTextOffset_px = screen_width - (strlen(sensorsText) * CURSOR_SIZE_X);

  if(CFG.display.invert_status_bar) {
    display.setTextColor(COLOR_1B_WHITE);
    display.fillRect(0, 0, screen_width, CURSOR_SIZE_Y, COLOR_1B_BLACK);
  } else {
    display.setTextColor(COLOR_1B_BLACK);
    display.fillRect(0, 0, screen_width, CURSOR_SIZE_Y, COLOR_1B_WHITE);
  }
  display.setCursor(0, 0);
  display.print(clockText);
  display.setCursor(sensorsTextOffset_px, 0);
  display.print(sensorsText);

  updateDisplay();
}

void loop() {
  drawStatusBar();

  if(CFG.wifi.auto_connect && !WiFi.isConnected()) {
    connectToWifi();
  }

  ezt::events();

  if(CFG.irc.auto_connect && !ircClient->connected()) {
    connectToIrc();
  }
  ircClient->loop();
}
