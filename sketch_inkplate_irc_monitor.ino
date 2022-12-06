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

#define STATUS_BAR_LEFT_SIZE 20
#define STATUS_BAR_RIGHT_SIZE 16

#define COLOR_3B_BLACK 0
#define COLOR_3B_DARK_GREY 4
#define COLOR_3B_LIGHT_GREY 6
#define COLOR_3B_WHITE 7

#define COLOR_1B_BLACK 1
#define COLOR_1B_WHITE 0

#define CONFIG_FILENAME "/config.json"
#define CONFIG_DOC_SIZE 2048
#define CONFIG_STRING_SIZE 32

#define WIFI_CONNECTION_TIMEOUT 30000
#define TIME_SYNC_TIMEOUT 10

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
WiFiClient wificlient;
IRCClient* ircclient = NULL;

char StatusBarLeftText[STATUS_BAR_LEFT_SIZE];
char StatusBarRightText[STATUS_BAR_RIGHT_SIZE];

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
    display.setCursor(1 * CURSOR_SIZE_X, _lastUsedLine++ * CURSOR_SIZE_Y);
    display.setTextWrap(true);
    display.setTextColor(COLOR_1B_BLACK);

    Serial.println(message);
    display.println(message);

    updateDisplay(false);

    _lastUsedLine += willWrap(message.length());
    if(((1 + _lastUsedLine) * CURSOR_SIZE_Y) > display.height()) {
      _lastUsedLine = 1;
      display.clearDisplay();
    }
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
  strlcpy(CFG.wifi.ssid,
    doc["wifi"]["ssid"] | "",
    sizeof(CFG.wifi.ssid));
  strlcpy(CFG.wifi.password,
    doc["wifi"]["password"] | "",
    sizeof(CFG.wifi.password));

  CFG.irc.auto_connect = doc["irc"]["auto-connect"];
  strlcpy(CFG.irc.server,
    doc["irc"]["server"] | "irc.libera.chat",
    sizeof(CFG.irc.server));
  CFG.irc.port = doc["irc"]["port"];
  strlcpy(CFG.irc.username,
    doc["irc"]["username"] | "inkplate",
    sizeof(CFG.irc.username));
  strlcpy(CFG.irc.nickname,
    doc["irc"]["nickname"] | "inkplate",
    sizeof(CFG.irc.nickname));
  strlcpy(CFG.irc.password,
    doc["irc"]["password"] | "",
    sizeof(CFG.irc.password));
  strlcpy(CFG.irc.channel,
    doc["irc"]["channel"] | "",
    sizeof(CFG.irc.channel));

  strlcpy(CFG.time.ntp_server,
    doc["time"]["ntp-server"] | "pool.ntp.org",
    sizeof(CFG.time.ntp_server));
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
  WiFi.begin(CFG.wifi.ssid, CFG.wifi.password);
  while(!WiFi.isConnected()) {
    if((millis() - wifi_conn_start) > WIFI_CONNECTION_TIMEOUT) {
      Serial.println("TIMEOUT");
      WiFi.disconnect();
      return;
    }
    Serial.print(".");
    delay(500);
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
  if(ircclient->connect(CFG.irc.nickname, CFG.irc.username, CFG.irc.password)) {
    Serial.println(" DONE");
    if(CFG.irc.channel) {
      ircclient->sendRaw("JOIN " + String(CFG.irc.channel));
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
  ezt::waitForSync(TIME_SYNC_TIMEOUT);
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

  if(ircclient == NULL) {
    ircclient = new IRCClient(CFG.irc.server, CFG.irc.port, wificlient);
  }

  ircclient->setCallback(ircCallback);
  ircclient->setSentCallback(ircSentCallback);

  setupTime();
}

void drawStatusBar() {
  auto _now = _TZ.dateTime("m/d H:i").c_str();
  int screen_width = display.width();
  int screen_height = display.height();
  int temp_C = display.readTemperature();
  float batt_V = display.readBattery();
  int status_bar_right_offset = ((screen_width / CURSOR_SIZE_X) - strlen(StatusBarRightText)) * CURSOR_SIZE_X;

  strlcpy(StatusBarLeftText, _now, strlen(_now)+1);
  StatusBarLeftText[strlen(_now)+1] = '\0';
  snprintf(StatusBarRightText, STATUS_BAR_RIGHT_SIZE, "%1.2fV %iC", batt_V, temp_C);

  if(CFG.display.invert_status_bar) {
    display.setTextColor(COLOR_1B_WHITE);
    display.fillRect(0, 0, screen_width, CURSOR_SIZE_Y, COLOR_1B_BLACK);
  } else {
    display.setTextColor(COLOR_1B_BLACK);
    display.fillRect(0, 0, screen_width, CURSOR_SIZE_Y, COLOR_1B_WHITE);
  }
  display.setCursor(0, 0);
  display.print(StatusBarLeftText);
  display.setCursor(status_bar_right_offset, 0);
  display.print(StatusBarRightText);

  updateDisplay();
}

void loop() {
  drawStatusBar();

  if(CFG.wifi.auto_connect && !WiFi.isConnected()) {
    connectToWifi();
  }

  ezt::events();

  if(CFG.irc.auto_connect && !ircclient->connected()) {
    connectToIrc();
  }
  ircclient->loop();
}
