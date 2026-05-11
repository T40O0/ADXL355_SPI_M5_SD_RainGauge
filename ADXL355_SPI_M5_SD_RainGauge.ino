#include <SD.h>
#include <M5Unified.h>
#include <M5_ADXL355.h>

// Self-contained minimal FTP server for the Data Dump mode.
// Uses only WiFi.h (WiFiServer/WiFiClient) - no external library.

//==============================================================================
// for RTC
// M5Unified Sample
// https://github.com/m5stack/M5Unified/blob/master/examples/Basic/Rtc/Rtc.ino

#if defined ( ARDUINO )
#define NTP_TIMEZONE  "JST-9"
#define NTP_SERVER1   "ntp2.jst.mfeed.ad.jp"
#define NTP_SERVER2   "ntp1.jst.mfeed.ad.jp"
#define NTP_SERVER3   "ntp.nict.jp"

#include <WiFi.h>

#if __has_include (<esp_sntp.h>)
  #include <esp_sntp.h>
  #define SNTP_ENABLED 1
#elif __has_include (<sntp.h>)
  #include <sntp.h>
  #define SNTP_ENABLED 1
#endif

#endif

#ifndef SNTP_ENABLED
#define SNTP_ENABLED 0
#endif
//==============================================================================

// Per-100Hz-tick Serial output in TaskRead (debug only).
// Set to 1 to print the >=5 gal exceedance count to the serial monitor.
#define ENABLE_SERIAL_SAMPLES 0

// Outer loop 200 Hz (5 ms drain interval, FIFO 32-triplet limit = 8 ms).
// 1 kHz tick: every 4th FIFO sample (inside drain). 100 Hz tick: every
// SLOW_DIV-th outer iteration on the last drained sample.
unsigned int hz = 200;
const unsigned int SLOW_DIV = 2;   // 200 / 100
unsigned int dtWrite = 1000 / hz;
unsigned int SDWriteTime = 60;

// dt is shared between TaskRead and TaskSave; protect with portMUX.
auto dt = M5.Rtc.getDateTime();
portMUX_TYPE dtMux = portMUX_INITIALIZER_UNLOCKED;

char hhmm[10];
char yyyymmdd[12];
char fileName[30];
int fileDate = 0;
File f;

// Total FIFO triplets drained per minute. Expected ~= 4000 * 60 = 240000;
// deviation flags FIFO overflow or scheduler slip.
uint32_t samp4kCount = 0;
double AccThres0 = 5.;
// double AccCount0 = 0.;  // sum disabled
int binaryCount0 = 0;     // 100 Hz exceed count (decimated last sample, no LPF)
int binary1k0     = 0;    // 1000 Hz exceed count (every 4th FIFO sample)
int binary4k0     = 0;    // 4000 Hz exceed count (every FIFO sample, sensor LPF only)
double AccThres1 = 10.0;
// double AccCount1 = 0.;  // sum disabled
int binaryCount1 = 0;
int binary1k1     = 0;
int binary4k1     = 0;
double AccThres2 = 20.0;
// double AccCount2 = 0.;  // sum disabled
int binaryCount2 = 0;
int binary1k2     = 0;
int binary4k2     = 0;
double AccThres3 = 30.;
// double AccCount3 = 0.;  // sum disabled
int binaryCount3 = 0;
int binary1k3     = 0;
int binary4k3     = 0;

// Pre-computed squared thresholds. Comparing magSq = x^2+y^2+z^2 against
// these avoids a sqrt() call per FIFO sample in the 4 kHz hot path.
const double AccThres0Sq = AccThres0 * AccThres0;
const double AccThres1Sq = AccThres1 * AccThres1;
const double AccThres2Sq = AccThres2 * AccThres2;
const double AccThres3Sq = AccThres3 * AccThres3;

// CSV header carrying the threshold values for each column.
String accHeader;

// Initial capacity for accData (single-line write per minute).
const size_t ACCDATA_RESERVE = 256;

// Drop count (missed batches when the SD queue is full) .
volatile uint32_t dropCount = 0;
volatile uint32_t batchOkCount = 0;

// Display snapshot: TaskRead copies counters here before reset; TaskSave reads.
// double   dispSum0 = 0., dispSum1 = 0., dispSum2 = 0., dispSum3 = 0.;  // sum disabled
uint32_t dispBin0 = 0,  dispBin1 = 0,  dispBin2 = 0,  dispBin3 = 0;   // 100 Hz
uint32_t disp1k0  = 0,  disp1k1  = 0,  disp1k2  = 0,  disp1k3  = 0;   // 1000 Hz
uint32_t disp4k0  = 0,  disp4k1  = 0,  disp4k2  = 0,  disp4k3  = 0;   // 4000 Hz
uint32_t dispSamp4k = 0;                                              // total drained samples
portMUX_TYPE dispMux = portMUX_INITIALIZER_UNLOCKED;

//==============================================================================

// Create an instance of ADXL355
//PL::ADXL355(SCK, MISO, MOSI, SS)
PL::ADXL355 adxl355(26, 36, 32, 33);
auto range = PL::ADXL355_Range::range2g;

auto ODR = PL::ADXL355_OutputDataRate::odr4000;
auto HPF = PL::ADXL355_HpfFrequency::hpf0_0954;
auto syncTime = PL::ADXL355_Synchronization::internal;

//==============================================================================

void TaskRead( void *pvParameters );
void TaskSave( void *pvParameters );
xQueueHandle xQueue;

// FTP server state (Data Dump mode). See MiniFTP definitions below.
WiFiServer ftpCtrlSrv(21);
WiFiServer ftpDataSrv(50000);
static const uint16_t FTP_PASV_PORT = 50000;

//==============================================================================

void txtWrite(const char *string, uint16_t color) {
    Serial.println(string);
    M5.Lcd.fillScreen(color);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.setTextDatum(0);
    M5.Lcd.drawString(string, 0, 2, 4);
}
//==============================================================================

void getDate() {
  sprintf(yyyymmdd, "%04d-%02d-%02d"
    , dt.date.year
    , dt.date.month
    , dt.date.date
  );
}
//==============================================================================

// Safe TF-open helper: retry every 1 s, remount every 30 s.
static File openSDFileSafe(const char *path, const char *mode) {
  File f;
  uint32_t retry = 0;
  while (true) {
    f = SD.open(path, mode);
    if (f) {
      if (retry > 0) {
        Serial.printf("SD recovered after %u retries\n", (unsigned)retry);
      }
      return f;
    }
    retry++;
    Serial.printf("SD open '%s' failed, retry %u\n", path, (unsigned)retry);
    M5.Lcd.fillRect(0, 200, 320, 40, RED);
    M5.Lcd.setTextFont(2);
    M5.Lcd.setTextColor(WHITE, RED);
    M5.Lcd.setCursor(0, 200);
    M5.Lcd.printf("SD ERROR retry=%u", (unsigned)retry);
    M5.Lcd.setCursor(0, 220);
    M5.Lcd.print("Check SD card!");
    delay(1000);
    if (retry % 30 == 0) {
      Serial.println("Remounting SD...");
      SD.end();
      delay(500);
      SD.begin(GPIO_NUM_4, SPI, 10000000);
    }
  }
}
//==============================================================================

void createFile() {
  fileDate = dt.date.date;
  getDate();
  sprintf(fileName, "/%04d%02d%02d.csv",
          dt.date.year,
          dt.date.month,
          fileDate);
  bool isNew = !SD.exists(fileName);
  f = openSDFileSafe(fileName, FILE_APPEND);
  if (isNew) {
    f.println(accHeader);
  }
}
//==============================================================================

// Post-set confirmation screen, shown for 10 seconds.
static void rtcConfirmScreen() {
  static constexpr const char* const wd[7] = {"Sun","Mon","Tue","Wed","Thr","Fri","Sat"};

  M5.Lcd.fillScreen(WHITE);
  M5.Lcd.setTextColor(BLACK, WHITE);
  M5.Lcd.setTextFont(2);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.print("RTC time:");
  M5.Lcd.setCursor(0, 90);
  M5.Lcd.print("ESP32 time:");

  int prevSec = -1;
  for (int i = 100; i > 0; --i) {
    delay(100);
    auto rtcDt = M5.Rtc.getDateTime();
    auto sysT  = time(nullptr);
    auto sysTm = localtime(&sysT);

    M5.Lcd.setTextFont(4);
    M5.Lcd.setTextColor(BLACK, WHITE);
    M5.Lcd.setCursor(0, 18);
    M5.Lcd.printf("%04d/%02d/%02d (%s)",
                  rtcDt.date.year, rtcDt.date.month, rtcDt.date.date, wd[rtcDt.date.weekDay]);
    M5.Lcd.setCursor(0, 50);
    M5.Lcd.printf("%02d:%02d:%02d",
                  rtcDt.time.hours, rtcDt.time.minutes, rtcDt.time.seconds);

    M5.Lcd.setCursor(0, 108);
    M5.Lcd.printf("%04d/%02d/%02d (%s)",
                  sysTm->tm_year+1900, sysTm->tm_mon+1, sysTm->tm_mday, wd[sysTm->tm_wday]);
    M5.Lcd.setCursor(0, 140);
    M5.Lcd.printf("%02d:%02d:%02d",
                  sysTm->tm_hour, sysTm->tm_min, sysTm->tm_sec);

    int sec = i / 10;
    if (sec != prevSec) {
      M5.Lcd.setTextFont(2);
      M5.Lcd.setCursor(0, 200);
      M5.Lcd.printf("Measurement starts in %2d sec", sec);
      prevSec = sec;
    }
  }
}

//==============================================================================

// Common tail for every RTC-setting path (Set_RTC / Manual_Set / menu
// timeout): mirror the just-written M5 RTC into the system clock and
// show the 10-second confirmation screen.
static void applyRtcAndConfirm() {
  auto rtcDt = M5.Rtc.getDateTime();
  struct tm tmRtc = {};
  tmRtc.tm_year = rtcDt.date.year - 1900;
  tmRtc.tm_mon  = rtcDt.date.month - 1;
  tmRtc.tm_mday = rtcDt.date.date;
  tmRtc.tm_hour = rtcDt.time.hours;
  tmRtc.tm_min  = rtcDt.time.minutes;
  tmRtc.tm_sec  = rtcDt.time.seconds;
  time_t tt = mktime(&tmRtc);
  struct timeval tv = { tt, 0 };
  settimeofday(&tv, nullptr);
  rtcConfirmScreen();
}

//==============================================================================
void Set_RTC() {
  M5.Lcd.fillScreen(WHITE);
  M5.Lcd.setCursor(0,0);
  M5.Lcd.setTextColor(BLACK, WHITE);
  M5.Lcd.setTextFont(2);
  if (!M5.Rtc.isEnabled())
  {
    M5.Lcd.println("RTC not found.");
    delay(500);
  }
  M5.Lcd.fillScreen(WHITE);
  M5.Lcd.setCursor(0,0);
  M5.Lcd.println("RTC found.");
  delay(1000);

  M5.Lcd.setCursor(0,0);
  M5.Lcd.print("WiFi: Searching.");
  WiFi.begin();
  while (WiFi.status() != WL_CONNECTED) {
    M5.Lcd.print('.');
    delay(1000);
  }
  M5.Lcd.setCursor(0,0);
  M5.Lcd.print("Wifi: Connected.");

  configTzTime(NTP_TIMEZONE, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);

  #if SNTP_ENABLED
    while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
      delay(1000);
    }
  #else
    delay(1600);
    struct tm timeInfo;
    while (!getLocalTime(&timeInfo, 1000)) {
    };
  #endif

  time_t t = time(nullptr) + 1;
  while (t > time(nullptr));
  M5.Rtc.setDateTime(localtime(&t));

  applyRtcAndConfirm();
  WiFi.disconnect(true);
}
//==============================================================================
void Set_WiFi(){
  M5.Lcd.fillScreen(WHITE);
  M5.Lcd.setTextColor(BLACK, WHITE);
  M5.Lcd.setTextFont(4);

  M5.Lcd.setCursor(0, 5);
  M5.Lcd.println("Use your phone");
  M5.Lcd.setCursor(0, 35);
  M5.Lcd.println("to select Wi-Fi.");

  WiFi.mode(WIFI_AP_STA);
  WiFi.beginSmartConfig();

  M5.Lcd.setCursor(0, 90);
  M5.Lcd.println("Waiting for");
  M5.Lcd.setCursor(0, 120);
  M5.Lcd.println("SmartConfig...");

  while (!WiFi.smartConfigDone()) {
    delay(500);
  }
  Set_RTC();
}
//==============================================================================

// Manual RTC setting UI.
void Manual_Set() {
  int year   = 2026;
  int month  = 1;
  int day    = 1;
  int hour   = 0;
  int minute = 0;
  int second = 0;

  // Button layout (320x240)
  const int xs[6]   = {   5,  80, 132, 184, 236, 288 };
  const int colW[6] = {  70,  48,  48,  48,  48,  30 };
  const int upY  = 30;
  const int dnY  = 120;
  const int btnH = 40;

  auto drawAll = [&](){
    M5.Lcd.fillScreen(WHITE);
    M5.Lcd.setTextColor(BLACK, WHITE);
    M5.Lcd.setTextFont(2);

    for (int i = 0; i < 6; i++) {
      M5.Lcd.fillRoundRect(xs[i], upY, colW[i], btnH, 5, ORANGE);
      M5.Lcd.setCursor(xs[i] + colW[i]/2 - 5, upY + btnH/2 - 8);
      M5.Lcd.setTextColor(BLACK, ORANGE);
      M5.Lcd.print("+");
    }
    for (int i = 0; i < 6; i++) {
      M5.Lcd.fillRoundRect(xs[i], dnY, colW[i], btnH, 5, ORANGE);
      M5.Lcd.setCursor(xs[i] + colW[i]/2 - 5, dnY + btnH/2 - 8);
      M5.Lcd.setTextColor(BLACK, ORANGE);
      M5.Lcd.print("-");
    }
    M5.Lcd.setTextColor(BLACK, WHITE);
    const char *hdr[6] = {"Year", "Mon", "Day", "Hour", "Min", "Sec"};
    for (int i = 0; i < 6; i++) {
      M5.Lcd.setCursor(xs[i] + 4, 5);
      M5.Lcd.print(hdr[i]);
    }
    M5.Lcd.setTextFont(4);
    char buf[8];
    int vals[6] = {year, month, day, hour, minute, second};
    for (int i = 0; i < 6; i++) {
      M5.Lcd.fillRect(xs[i], 75, colW[i], 40, WHITE);
      M5.Lcd.setCursor(xs[i] + 2, 80);
      if (i == 0) sprintf(buf, "%04d", vals[i]);
      else        sprintf(buf, "%02d", vals[i]);
      M5.Lcd.print(buf);
    }
    M5.Lcd.fillRoundRect(20,  195, 130, 40, 8, GREEN);
    M5.Lcd.fillRoundRect(170, 195, 130, 40, 8, RED);
    M5.Lcd.setTextFont(4);
    M5.Lcd.setTextColor(BLACK, GREEN);
    M5.Lcd.setCursor(60, 200);
    M5.Lcd.print("SET");
    M5.Lcd.setTextColor(WHITE, RED);
    M5.Lcd.setCursor(190, 200);
    M5.Lcd.print("CANCEL");
    M5.Lcd.setTextColor(BLACK, WHITE);
  };

  auto adjust = [&](int idx, int delta){
    switch (idx) {
      case 0: year   = constrain(year + delta, 2024, 2099); break;
      case 1: month  = ((month - 1 + delta + 12) % 12) + 1; break;
      case 2: day    = ((day   - 1 + delta + 31) % 31) + 1; break;
      case 3: hour   = (hour   + delta + 24) % 24; break;
      case 4: minute = (minute + delta + 60) % 60; break;
      case 5: second = (second + delta + 60) % 60; break;
    }
  };

  // Partial redraw of one digit cell only (avoids fillScreen flicker
  // during long-press auto-repeat).
  auto drawValue = [&](int idx){
    M5.Lcd.fillRect(xs[idx], 75, colW[idx], 40, WHITE);
    M5.Lcd.setTextFont(4);
    M5.Lcd.setTextColor(BLACK, WHITE);
    M5.Lcd.setCursor(xs[idx] + 2, 80);
    char buf[8];
    int vals[6] = {year, month, day, hour, minute, second};
    if (idx == 0) sprintf(buf, "%04d", vals[idx]);
    else          sprintf(buf, "%02d", vals[idx]);
    M5.Lcd.print(buf);
  };

  drawAll();

  // Long-press auto-repeat: HOLD_DELAY -> REPEAT_SLOW -> REPEAT_FAST after ACCEL_AFTER.
  const uint32_t HOLD_DELAY_MS    = 400;
  const uint32_t REPEAT_SLOW_MS   = 150;
  const uint32_t REPEAT_FAST_MS   = 50;
  const uint32_t ACCEL_AFTER_MS   = 2000;
  int      holdBtn   = -1;          // adjust() idx (0..5), -1 = no hold
  int      holdDelta = 0;
  int      holdRow   = 0;           // upY or dnY (for in-button check)
  uint32_t holdStart = 0;
  uint32_t lastRepeat = 0;

  while (true) {
    M5.update();
    auto t = M5.Touch.getDetail();
    uint32_t now = millis();

    if (t.wasPressed()) {
      int xt = t.x;
      int yt = t.y;
      bool handled = false;

      for (int i = 0; i < 6 && !handled; i++) {
        if (xt >= xs[i] && xt <= xs[i] + colW[i] &&
            yt >= upY  && yt <= upY  + btnH) {
          adjust(i, +1); drawValue(i);
          holdBtn = i; holdDelta = +1; holdRow = upY;
          holdStart = lastRepeat = now;
          handled = true;
        }
      }
      for (int i = 0; i < 6 && !handled; i++) {
        if (xt >= xs[i] && xt <= xs[i] + colW[i] &&
            yt >= dnY  && yt <= dnY  + btnH) {
          adjust(i, -1); drawValue(i);
          holdBtn = i; holdDelta = -1; holdRow = dnY;
          holdStart = lastRepeat = now;
          handled = true;
        }
      }
      if (!handled && xt >= 20 && xt <= 150 && yt >= 195 && yt <= 235) {
        auto newdt = M5.Rtc.getDateTime();
        newdt.date.year   = year;
        newdt.date.month  = month;
        newdt.date.date   = day;
        newdt.time.hours  = hour;
        newdt.time.minutes= minute;
        newdt.time.seconds= second;
        // PCF8563 stores weekday in a separate register; compute it from the date.
        {
          static const int sakamoto_t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
          int yy = year - (month < 3 ? 1 : 0);
          newdt.date.weekDay = (yy + yy/4 - yy/100 + yy/400
                                + sakamoto_t[month - 1] + day) % 7;
        }
        M5.Rtc.setDateTime(&newdt);

        // Mirror the new RTC value into the system clock + show 10 s confirmation.
        applyRtcAndConfirm();
        return;
      }
      if (!handled && xt >= 170 && xt <= 300 && yt >= 195 && yt <= 235) {
        return;
      }
    }

    // While a +/- button is held, fire repeats after the initial delay, then accelerate.
    if (holdBtn >= 0) {
      if (!t.isPressed()) {
        holdBtn = -1;
      } else {
        int xt = t.x, yt = t.y;
        bool stillIn = (xt >= xs[holdBtn] && xt <= xs[holdBtn] + colW[holdBtn] &&
                        yt >= holdRow    && yt <= holdRow    + btnH);
        if (!stillIn) {
          holdBtn = -1;
        } else {
          uint32_t held = now - holdStart;
          if (held >= HOLD_DELAY_MS) {
            uint32_t interval = (held >= ACCEL_AFTER_MS) ? REPEAT_FAST_MS
                                                         : REPEAT_SLOW_MS;
            if (now - lastRepeat >= interval) {
              adjust(holdBtn, holdDelta); drawValue(holdBtn);
              lastRepeat = now;
            }
          }
        }
      }
    }

    delay(20);
  }
}

//==============================================================================

// MiniFTP state (control connection, current cwd, auth flag, login arg).
static WiFiClient ftpCli;
static String    ftpCwd     = "/";
static bool      ftpAuthed  = false;
static String    ftpUserArg;
static IPAddress ftpLocalIP;

static const char *FTP_USER = "m5";
static const char *FTP_PASS = "m5";

// Resolve an FTP path argument relative to ftpCwd.
static String ftpResolve(const String &arg) {
  if (arg.length() == 0) return ftpCwd;
  if (arg.startsWith("/")) return arg;
  if (ftpCwd == "/") return "/" + arg;
  return ftpCwd + "/" + arg;
}

// Send a code+message line on the FTP control connection.
static void ftpReply(int code, const char *msg) {
  ftpCli.printf("%d %s\r\n", code, msg);
}

// Wait for a passive-mode data client to connect (timeout in ms).
static WiFiClient ftpAcceptData(uint32_t timeoutMs = 5000) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    WiFiClient c = ftpDataSrv.accept();
    if (c) return c;
    delay(10);
  }
  return WiFiClient();
}
//==============================================================================

// Handle a single command line received on the FTP control connection.
static void ftpHandleCmd(const String &line) {
  int sp = line.indexOf(' ');
  String cmd = (sp == -1) ? line : line.substring(0, sp);
  String arg = (sp == -1) ? ""   : line.substring(sp + 1);
  cmd.toUpperCase();
  arg.trim();
  Serial.printf("FTP> %s %s\n", cmd.c_str(), arg.c_str());

  if (cmd == "USER") {
    ftpUserArg = arg;
    ftpReply(331, "Need password");
  } else if (cmd == "PASS") {
    if (ftpUserArg == FTP_USER && arg == FTP_PASS) {
      ftpAuthed = true;
      ftpReply(230, "Logged in");
    } else {
      ftpReply(530, "Login incorrect");
    }
  } else if (!ftpAuthed) {
    ftpReply(530, "Not logged in");
  } else if (cmd == "SYST") {
    ftpReply(215, "UNIX Type: L8");
  } else if (cmd == "FEAT") {
    ftpCli.println("211-Features:");
    ftpCli.println(" PASV");
    ftpCli.println(" SIZE");
    ftpCli.println(" UTF8");
    ftpCli.println("211 End");
  } else if (cmd == "OPTS") {
    ftpReply(200, "OK");
  } else if (cmd == "TYPE") {
    ftpReply(200, "Type set");
  } else if (cmd == "PWD" || cmd == "XPWD") {
    ftpCli.printf("257 \"%s\" is current directory\r\n", ftpCwd.c_str());
  } else if (cmd == "CWD") {
    String np = ftpResolve(arg);
    File f = SD.open(np);
    if (f && f.isDirectory()) {
      ftpCwd = np;
      ftpReply(250, "Directory changed");
    } else {
      ftpReply(550, "Directory not found");
    }
    if (f) f.close();
  } else if (cmd == "CDUP") {
    int sl = ftpCwd.lastIndexOf('/');
    if (sl > 0) ftpCwd = ftpCwd.substring(0, sl);
    else        ftpCwd = "/";
    ftpReply(200, "Up");
  } else if (cmd == "PASV") {
    ftpDataSrv.begin();
    uint8_t pH = (FTP_PASV_PORT >> 8) & 0xff;
    uint8_t pL = FTP_PASV_PORT & 0xff;
    ftpCli.printf("227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n",
                  ftpLocalIP[0], ftpLocalIP[1], ftpLocalIP[2], ftpLocalIP[3],
                  pH, pL);
  } else if (cmd == "LIST" || cmd == "NLST") {
    ftpReply(150, "Opening data connection");
    WiFiClient dc = ftpAcceptData();
    if (!dc) { ftpReply(425, "Can't open data connection"); return; }
    bool namesOnly = (cmd == "NLST");
    File dir = SD.open(ftpCwd);
    if (dir && dir.isDirectory()) {
      while (true) {
        File entry = dir.openNextFile();
        if (!entry) break;
        String name = entry.name();
        int sl = name.lastIndexOf('/');
        if (sl >= 0) name = name.substring(sl + 1);
        if (namesOnly) {
          dc.printf("%s\r\n", name.c_str());
        } else {
          const char *mode = entry.isDirectory() ? "drwxr-xr-x" : "-rw-r--r--";
          dc.printf("%s 1 m5 m5 %u Jan 01  2026 %s\r\n",
                    mode, (unsigned)entry.size(), name.c_str());
        }
        entry.close();
      }
      dir.close();
    }
    dc.stop();
    ftpReply(226, "Transfer complete");
  } else if (cmd == "RETR") {
    String path = ftpResolve(arg);
    File f = SD.open(path);
    if (!f || f.isDirectory()) {
      ftpReply(550, "File not found");
      if (f) f.close();
      return;
    }
    ftpReply(150, "Opening data connection");
    WiFiClient dc = ftpAcceptData();
    if (!dc) { ftpReply(425, "Can't open data connection"); f.close(); return; }
    uint8_t buf[1460];
    while (f.available()) {
      int n = f.read(buf, sizeof(buf));
      if (n <= 0) break;
      dc.write(buf, n);
    }
    f.close();
    dc.stop();
    ftpReply(226, "Transfer complete");
  } else if (cmd == "SIZE") {
    String path = ftpResolve(arg);
    File f = SD.open(path);
    if (f && !f.isDirectory()) {
      ftpCli.printf("213 %u\r\n", (unsigned)f.size());
    } else {
      ftpReply(550, "Not found");
    }
    if (f) f.close();
  } else if (cmd == "NOOP") {
    ftpReply(200, "OK");
  } else if (cmd == "QUIT") {
    ftpReply(221, "Bye");
    ftpCli.stop();
  } else {
    ftpReply(502, "Not implemented");
  }
}
//==============================================================================

// Process pending FTP control activity. Call repeatedly from the main loop.
static void ftpHandle() {
  if (!ftpCli || !ftpCli.connected()) {
    WiFiClient incoming = ftpCtrlSrv.accept();
    if (incoming) {
      ftpCli = incoming;
      ftpAuthed = false;
      ftpCwd    = "/";
      ftpReply(220, "M5 FTP Ready");
    }
    return;
  }
  if (ftpCli.available()) {
    String line = ftpCli.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      ftpHandleCmd(line);
    }
  }
}
//==============================================================================

// Data Dump mode: SoftAP + minimal FTP server.
// Connect Wi-Fi to "M5-SEISMO" (m5seismo) and open
// ftp://192.168.4.1 in Windows Explorer (user m5 / pass m5).
void Data_Dump_FTP() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextColor(WHITE, BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.setTextFont(2);
  M5.Lcd.println("Data Dump (AP+FTP)");
  M5.Lcd.println();

  M5.Lcd.print("Init SD...");
  uint32_t t0 = millis();
  while (!SD.begin(GPIO_NUM_4, SPI, 10000000)) {
    if (millis() - t0 > 5000) {
      M5.Lcd.fillScreen(RED);
      M5.Lcd.setCursor(0, 0);
      M5.Lcd.setTextColor(WHITE, RED);
      M5.Lcd.println("SD INIT FAILED.");
      M5.Lcd.println("Reset to retry.");
      while (true) delay(1000);
    }
    delay(100);
  }
  M5.Lcd.println(" OK");

  WiFi.mode(WIFI_AP);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP("M5-SEISMO", "m5seismo");

  ftpLocalIP = apIP;
  ftpCtrlSrv.begin();
  ftpDataSrv.begin();
  ftpAuthed = false;
  ftpCwd    = "/";

  M5.Lcd.printf("SSID: M5-SEISMO\n");
  M5.Lcd.printf("PASS: m5seismo\n");
  M5.Lcd.println();
  M5.Lcd.printf("URL : ftp://192.168.4.1\n");
  M5.Lcd.printf("User: m5 / Pass: m5\n");
  M5.Lcd.println();
  M5.Lcd.println("Open in Explorer:");
  M5.Lcd.println("ftp://m5:m5@192.168.4.1");

  const int rbX = 60, rbY = 190, rbW = 200, rbH = 40;
  M5.Lcd.fillRoundRect(rbX,     rbY,     rbW,     rbH,     8, RED);
  M5.Lcd.fillRoundRect(rbX + 4, rbY + 4, rbW - 8, rbH - 8, 8, ORANGE);
  M5.Lcd.setTextFont(4);
  M5.Lcd.setTextColor(BLACK, ORANGE);
  M5.Lcd.setCursor(rbX + 70, rbY + 8);
  M5.Lcd.print("Reset");

  while (true) {
    ftpHandle();
    M5.update();
    auto td = M5.Touch.getDetail();
    if (td.wasPressed()) {
      if (td.x >= rbX && td.x <= rbX + rbW &&
          td.y >= rbY && td.y <= rbY + rbH) {
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setTextFont(4);
        M5.Lcd.setTextColor(WHITE, BLACK);
        M5.Lcd.setCursor(0, 100);
        M5.Lcd.println("Restarting...");
        delay(500);
        ESP.restart();
      }
    }
    delay(1);
  }
}

//==============================================================================

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 921600;
  cfg.clear_display = true;
  cfg.output_power = false;
  cfg.internal_imu = false;
  cfg.internal_rtc = true;
  cfg.internal_spk = false;
  cfg.external_imu = false;
  cfg.external_rtc = false;
  cfg.external_spk = false;
  cfg.led_brightness = 0;
  M5.begin(cfg);
  M5.Lcd.setBrightness(100);

  accHeader  = "datetime";
  // accHeader += ",sum>=";  accHeader += AccThres0; accHeader += "gal,n>="; accHeader += AccThres0; accHeader += "gal";  // sum disabled
  // accHeader += ",sum>=";  accHeader += AccThres1; accHeader += "gal,n>="; accHeader += AccThres1; accHeader += "gal";  // sum disabled
  // accHeader += ",sum>=";  accHeader += AccThres2; accHeader += "gal,n>="; accHeader += AccThres2; accHeader += "gal";  // sum disabled
  // accHeader += ",sum>=";  accHeader += AccThres3; accHeader += "gal,n>="; accHeader += AccThres3; accHeader += "gal";  // sum disabled
  accHeader += ",n100Hz>=";  accHeader += AccThres0; accHeader += "gal";
  accHeader += ",n100Hz>=";  accHeader += AccThres1; accHeader += "gal";
  accHeader += ",n100Hz>=";  accHeader += AccThres2; accHeader += "gal";
  accHeader += ",n100Hz>=";  accHeader += AccThres3; accHeader += "gal";
  accHeader += ",n1kHz>=";   accHeader += AccThres0; accHeader += "gal";
  accHeader += ",n1kHz>=";   accHeader += AccThres1; accHeader += "gal";
  accHeader += ",n1kHz>=";   accHeader += AccThres2; accHeader += "gal";
  accHeader += ",n1kHz>=";   accHeader += AccThres3; accHeader += "gal";
  accHeader += ",n4kHz>=";   accHeader += AccThres0; accHeader += "gal";
  accHeader += ",n4kHz>=";   accHeader += AccThres1; accHeader += "gal";
  accHeader += ",n4kHz>=";   accHeader += AccThres2; accHeader += "gal";
  accHeader += ",n4kHz>=";   accHeader += AccThres3; accHeader += "gal";

  //==============================================================================
  M5.Lcd.fillScreen(WHITE);
  const int btnX = 60;
  const int btnW = 200;
  const int btnH = 40;
  const int btnGap = 5;
  const int btnR = 8;
  const int btnY0 = 40;

  struct Btn { const char *label; int y; };
  Btn btns[4] = {
    {"Wi-Fi Setting", btnY0 + 0 * (btnH + btnGap)},
    {"Reset RTC",     btnY0 + 1 * (btnH + btnGap)},
    {"Manual Set",    btnY0 + 2 * (btnH + btnGap)},
    {"Data Dump",     btnY0 + 3 * (btnH + btnGap)},
  };
  for (int i = 0; i < 4; i++) {
    M5.Lcd.fillRoundRect(btnX,     btns[i].y,     btnW,     btnH,     btnR, RED);
    M5.Lcd.fillRoundRect(btnX + 4, btns[i].y + 4, btnW - 8, btnH - 8, btnR, ORANGE);
    M5.Lcd.setTextFont(4);
    M5.Lcd.setTextColor(BLACK, ORANGE);
    M5.Lcd.setCursor(btnX + 25, btns[i].y + 8);
    M5.Lcd.print(btns[i].label);
  }
  M5.Lcd.setTextColor(BLACK, WHITE);

  M5.Lcd.setTextFont(4);
  M5.Lcd.setTextColor(BLACK, WHITE);
  M5.Lcd.setCursor(0, 5);
  M5.Lcd.print("Tap a button:");

  int prevSec = -1;
  // dispatched is visible to the post-menu code so the timeout path can
  // run applyRtcAndConfirm() (handlers already do it themselves).
  bool dispatched = false;
  for (int i = 300; i > 0; --i) {
    M5.update();
    auto td = M5.Touch.getDetail();
    int xt = td.x, yt = td.y;
    if (td.wasPressed()) {
      for (int b = 0; b < 4; b++) {
        if (xt >= btnX && xt <= btnX + btnW &&
            yt >= btns[b].y && yt <= btns[b].y + btnH) {
          switch (b) {
            case 0: Set_WiFi();      break;
            case 1: Set_RTC();       break;
            case 2: Manual_Set();    break;
            case 3: Data_Dump_FTP(); break;
          }
          dispatched = true;
          break;
        }
      }
    }
    if (dispatched) break;

    int sec = i / 10;
    if (sec != prevSec) {
      M5.Lcd.setTextFont(4);
      M5.Lcd.setTextColor(BLACK, WHITE);
      M5.Lcd.setCursor(0, 215);
      M5.Lcd.printf("Wait... %2d sec", sec);
      prevSec = sec;
    }
    delay(100);
  }
  //==============================================================================

  adxl355.begin();
  adxl355.setRange(range);
  adxl355.setOutputDataRate(ODR);
  adxl355.setHpfFrequency(HPF);
  adxl355.setSynchronization(syncTime);
  adxl355.enableMeasurement();
  adxl355.clearFifo();   // start TaskRead with an empty FIFO

  while (!SD.begin(GPIO_NUM_4, SPI, 10000000)) {
    txtWrite("ERROR: SD CARD", BLACK);
    delay(100);
  }

  dt = M5.Rtc.getDateTime();
  txtWrite("Start RTC. waiting...", BLACK);
  while ((dt.date.year < 2026) || (dt.date.year > 2099)) {
    Serial.println(dt.date.year);
    delay(10);
    dt = M5.Rtc.getDateTime();
  }

  if (!dispatched) {
    applyRtcAndConfirm();
  }

  M5.Lcd.fillScreen(BLACK);

  // Queue holds up to 3 String pointers to absorb transient SD delays.
  xQueue = xQueueCreate(3, sizeof(String*));
  if (xQueue != NULL){
    xTaskCreatePinnedToCore(
      TaskRead
      ,  "ReadADXL355"
      ,  8192
      ,  NULL
      ,  3
      ,  NULL
      ,  PRO_CPU_NUM);
    xTaskCreatePinnedToCore(
      TaskSave
      ,  "SaveData2SDCard"
      ,  8192
      ,  NULL
      ,  3
      ,  NULL
      ,  APP_CPU_NUM);
  }
  else {
    while(1){
      Serial.println("Failed to create queue.");
    }
  }
}
//==============================================================================

void loop() {
  delay(1000);
}
//==============================================================================

void TaskRead(void *pvParameters) {
  auto accelerations = adxl355.getAccelerations();
  unsigned int i = 1;
  unsigned int slowTick = 0;       // 100 Hz tick: fires every SLOW_DIV outer iterations.
  unsigned int oneKTick = 0;       // 1 kHz tick: fires every 4th FIFO sample (4 kHz / 4).
  // Squared magnitude (gal^2). The 100 Hz tick reuses the last value
  // from the most recent FIFO drain (kept across outer iterations).
  double magSq = 0.;
  String accData;
  accData.reserve(ACCDATA_RESERVE);

  auto localDt = M5.Rtc.getDateTime();
  txtWrite("waiting start...", BLACK);
  while (localDt.time.seconds != 0) {
    delay(1);
    localDt = M5.Rtc.getDateTime();
  }
  portENTER_CRITICAL(&dtMux);
  dt = localDt;
  portEXIT_CRITICAL(&dtMux);
  getDate();

  TickType_t xLastWakeTimeSend = xTaskGetTickCount();

  for (;;) {
    portENTER_CRITICAL(&dtMux);
    int curHour = dt.time.hours;
    int curMin  = dt.time.minutes;
    portEXIT_CRITICAL(&dtMux);
    sprintf(hhmm, "%02d:%02d", curHour, curMin);

    // Drain FIFO (~20 triplet/5ms): 4 kHz = each, 1 kHz = every 4th.
    // 100 Hz uses the last magSq on SLOW_DIV-th outer iter (below).
    uint8_t fifoEntries = adxl355.getNumberOfFifoSamples();
    uint8_t nTriplets   = fifoEntries / 3;
    samp4kCount += nTriplets;
    for (uint8_t k = 0; k < nTriplets; k++) {
      accelerations = adxl355.getAccelerationsFromFifo();
      magSq =
        accelerations.x * accelerations.x +
        accelerations.y * accelerations.y +
        accelerations.z * accelerations.z;
      // 4000 Hz threshold counts (every FIFO sample, no decimation).
      if (magSq >= AccThres0Sq) binary4k0 += 1;
      if (magSq >= AccThres1Sq) binary4k1 += 1;
      if (magSq >= AccThres2Sq) binary4k2 += 1;
      if (magSq >= AccThres3Sq) binary4k3 += 1;
      // 1000 Hz threshold counts (every 4th FIFO sample = 4 kHz / 4).
      oneKTick++;
      if (oneKTick >= 4) {
        oneKTick = 0;
        if (magSq >= AccThres0Sq) binary1k0 += 1;
        if (magSq >= AccThres1Sq) binary1k1 += 1;
        if (magSq >= AccThres2Sq) binary1k2 += 1;
        if (magSq >= AccThres3Sq) binary1k3 += 1;
      }
    }
    // (nTriplets == 0 rare; magSq retains previous value, acceptable for 100 Hz.)

    // 100 Hz threshold counts (every SLOW_DIV-th outer iter on last magSq).
    slowTick++;
    if (slowTick >= SLOW_DIV) {
      slowTick = 0;
      if (magSq >= AccThres0Sq) { /* AccCount0 += sqrt(magSq); */ binaryCount0 += 1; }  // sum disabled
      if (magSq >= AccThres1Sq) { /* AccCount1 += sqrt(magSq); */ binaryCount1 += 1; }  // sum disabled
      if (magSq >= AccThres2Sq) { /* AccCount2 += sqrt(magSq); */ binaryCount2 += 1; }  // sum disabled
      if (magSq >= AccThres3Sq) { /* AccCount3 += sqrt(magSq); */ binaryCount3 += 1; }  // sum disabled
#if ENABLE_SERIAL_SAMPLES
      // Serial.print(AccCount0); Serial.print(", ");  // sum disabled
      Serial.println(binaryCount0);
#endif
    }

    if (i >= hz * SDWriteTime) {
      accData += '\n';
      accData += yyyymmdd;
      accData += ' ';
      accData += hhmm;
      // 100 Hz exceed counts
      accData += ',';
      // accData += AccCount0;  accData += ',';  // sum disabled
      accData += binaryCount0;
      accData += ',';
      // accData += AccCount1;  accData += ',';  // sum disabled
      accData += binaryCount1;
      accData += ',';
      // accData += AccCount2;  accData += ',';  // sum disabled
      accData += binaryCount2;
      accData += ',';
      // accData += AccCount3;  accData += ',';  // sum disabled
      accData += binaryCount3;
      // 1000 Hz exceed counts
      accData += ',';
      accData += binary1k0;
      accData += ',';
      accData += binary1k1;
      accData += ',';
      accData += binary1k2;
      accData += ',';
      accData += binary1k3;
      // 4000 Hz exceed counts
      accData += ',';
      accData += binary4k0;
      accData += ',';
      accData += binary4k1;
      accData += ',';
      accData += binary4k2;
      accData += ',';
      accData += binary4k3;

      portENTER_CRITICAL(&dispMux);
      // dispSum0 = AccCount0;  // sum disabled
      dispBin0 = (uint32_t)binaryCount0;
      disp1k0  = (uint32_t)binary1k0;
      disp4k0  = (uint32_t)binary4k0;
      // dispSum1 = AccCount1;  // sum disabled
      dispBin1 = (uint32_t)binaryCount1;
      disp1k1  = (uint32_t)binary1k1;
      disp4k1  = (uint32_t)binary4k1;
      // dispSum2 = AccCount2;  // sum disabled
      dispBin2 = (uint32_t)binaryCount2;
      disp1k2  = (uint32_t)binary1k2;
      disp4k2  = (uint32_t)binary4k2;
      // dispSum3 = AccCount3;  // sum disabled
      dispBin3 = (uint32_t)binaryCount3;
      disp1k3  = (uint32_t)binary1k3;
      disp4k3  = (uint32_t)binary4k3;
      dispSamp4k = samp4kCount;
      portEXIT_CRITICAL(&dispMux);
      String *snapshot = new String(accData);
      if (xQueueSendToBack(xQueue, &snapshot, 0) != pdTRUE) {
        delete snapshot;
        dropCount = dropCount + 1;
      }
      accData = "";
      /* AccCount0 = 0.; */ binaryCount0 = 0; binary1k0 = 0; binary4k0 = 0;  // sum disabled
      /* AccCount1 = 0.; */ binaryCount1 = 0; binary1k1 = 0; binary4k1 = 0;  // sum disabled
      /* AccCount2 = 0.; */ binaryCount2 = 0; binary1k2 = 0; binary4k2 = 0;  // sum disabled
      /* AccCount3 = 0.; */ binaryCount3 = 0; binary1k3 = 0; binary4k3 = 0;  // sum disabled
      samp4kCount = 0;
      slowTick = 0;
      oneKTick = 0;

      i = 1;
    }
    else {
      i++;
    }
    vTaskDelayUntil(&xLastWakeTimeSend, 1000 / hz / portTICK_PERIOD_MS);
  }
}
//==============================================================================

void TaskSave(void *pvParameters) {
  String *recData = nullptr;
  bool firstBatch = true;

  delay(5);

  for (;;) {
    // Check xQueueReceive return; skip write on timeout.
    if (xQueueReceive(xQueue, &recData, SDWriteTime * 1000 / portTICK_PERIOD_MS) != pdTRUE) {
      auto localDt = M5.Rtc.getDateTime();
      portENTER_CRITICAL(&dtMux);
      dt = localDt;
      portEXIT_CRITICAL(&dtMux);
      continue;
    }

    // First batch: create file with post sec=0 RTC so name matches start date.
    if (firstBatch) {
      auto firstDt = M5.Rtc.getDateTime();
      portENTER_CRITICAL(&dtMux);
      dt = firstDt;
      portEXIT_CRITICAL(&dtMux);
      createFile();
      firstBatch = false;
    }

    *recData += '\n';
    f.print(recData->substring(1));
    f.close();
    batchOkCount = batchOkCount + 1;

    auto localDt = M5.Rtc.getDateTime();
    portENTER_CRITICAL(&dtMux);
    dt = localDt;
    portEXIT_CRITICAL(&dtMux);

    // Snapshot of last minute's counters (published by TaskRead before reset).
    // double   s0, s1, s2, s3;  // sum disabled
    uint32_t b0, b1, b2, b3;          // 100 Hz exceed counts
    uint32_t f0, f1, f2, f3;          // 1000 Hz exceed counts
    uint32_t g0, g1, g2, g3;          // 4000 Hz exceed counts
    uint32_t samp4k;                  // total drained samples
    portENTER_CRITICAL(&dispMux);
    /* s0 = dispSum0; */ b0 = dispBin0; f0 = disp1k0; g0 = disp4k0;
    /* s1 = dispSum1; */ b1 = dispBin1; f1 = disp1k1; g1 = disp4k1;
    /* s2 = dispSum2; */ b2 = dispBin2; f2 = disp1k2; g2 = disp4k2;
    /* s3 = dispSum3; */ b3 = dispBin3; f3 = disp1k3; g3 = disp4k3;
    samp4k = dispSamp4k;
    portEXIT_CRITICAL(&dispMux);

    // 5 s status panel (backlight ON), then ~55 s with backlight OFF
    // until the next batch (saves ~92% of LCD power vs always-on).
    M5.Lcd.setBrightness(100);
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(WHITE, BLACK);

    M5.Lcd.setTextFont(4);
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.printf("%04d/%02d/%02d %02d:%02d",
                  localDt.date.year, localDt.date.month, localDt.date.date,
                  localDt.time.hours, localDt.time.minutes);

    M5.Lcd.setTextFont(2);
    M5.Lcd.setCursor(0, 40);
    M5.Lcd.printf("         100Hz  1kHz   4kHz");
    M5.Lcd.setCursor(0, 60);
    M5.Lcd.printf(">=%2.0f gal  %5lu %5lu  %6lu",
                  AccThres0, (unsigned long)b0, (unsigned long)f0, (unsigned long)g0);
    M5.Lcd.setCursor(0, 80);
    M5.Lcd.printf(">=%2.0f gal  %5lu %5lu  %6lu",
                  AccThres1, (unsigned long)b1, (unsigned long)f1, (unsigned long)g1);
    M5.Lcd.setCursor(0, 100);
    M5.Lcd.printf(">=%2.0f gal  %5lu %5lu  %6lu",
                  AccThres2, (unsigned long)b2, (unsigned long)f2, (unsigned long)g2);
    M5.Lcd.setCursor(0, 120);
    M5.Lcd.printf(">=%2.0f gal  %5lu %5lu  %6lu",
                  AccThres3, (unsigned long)b3, (unsigned long)f3, (unsigned long)g3);

    M5.Lcd.setCursor(0, 150);
    M5.Lcd.printf("OK  : %lu", (unsigned long)batchOkCount);
    // DROP = number of dropped batches (queue full from slow SD writes).
    M5.Lcd.setCursor(0, 170);
    M5.Lcd.setTextColor(dropCount ? RED : WHITE, BLACK);
    M5.Lcd.printf("DROP: %lu", (unsigned long)dropCount);
    M5.Lcd.setTextColor(WHITE, BLACK);
    // N4k: total FIFO triplets drained per minute (expected ~= 240000).
    M5.Lcd.setCursor(0, 190);
    M5.Lcd.printf("N4k : %lu", (unsigned long)samp4k);

    vTaskDelay(5000 / portTICK_PERIOD_MS);
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setBrightness(0); // end

    delete recData;
    recData = nullptr;

    if (localDt.date.date != fileDate) {
      portENTER_CRITICAL(&dtMux);
      dt = localDt;
      portEXIT_CRITICAL(&dtMux);
      createFile();
    }
    else {
      // Use openSDFileSafe here.
      f = openSDFileSafe(fileName, FILE_APPEND);
    }
  }
}
