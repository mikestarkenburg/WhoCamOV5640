// whocam5640 2022-10-08
// mike@starkenburg.com
// NOTE: FTP port 2121 hardcoded into the ESP32_FTPClient Library!!
// Left to do:
//     flip camera
//     get max 5640 resolution
//     kill unnecessary webserver crap
//     possibly fix gain problems?
//     possibly check on 5640 autofocus
//     possibly check on 5640 digital zoom 
//     battery level
//     logging
//     ota


// ++++++++++++++++++++++++++++++++++ INCLUDES
#include "Arduino.h"
#include "WiFi.h"
#include "WiFiClient.h"
#include "ESP32_FTPClient.h"
#include "esp_camera.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_bt.h"
#include "soc/soc.h"           // Disable brownout problems
#include "soc/rtc_cntl_reg.h"  // Disable brownout problems
#include "driver/rtc_io.h"
#include "time.h"
#include "ESPAsyncWebServer.h"
#include "StringArray.h"
#include "SPIFFS.h"
#include "FS.h"

// ++++++++++++++++++++++++++++++++ DECLARATIONS

// Functions

void print_wakeup_reason();
void goToDeepSleep();
bool checkPhoto();
void capturePhotoSaveSpiffs();
void uploadFTP();
void FindLocalTime();

// Deep Sleep Variables
RTC_DATA_ATTR int cycleCount = 0;
#define SLEEPTIME 14.75 // time in minutes for each deep sleep cycle

// NTP variables
const char* ntpServer = "pool.ntp.org";  // possible optimization here, setup local NTP and use IP addr.
const long  gmtOffset_sec = -28800;
const int   daylightOffset_sec = 3600;

// Wifi variables
#define WIFI_TIMEOUT 10000 // 10seconds in milliseconds
const char* ssid = "starkhome";
const char* password = "starkenburg";

// FTP variables
char ftp_server[] = "45.56.87.104";  // hardcoded IP for pride.starkenburg.com
// NOTE: FTP port 2121 hardcoded into the ESP32_FTPClient Library!!
char ftp_user[]   = "who";
char ftp_pass[]   = "whocam";
char ftp_dir[] = "/mnt/platform/images";

// Camera variables
camera_fb_t * fb = NULL; // pointer
int fileSize = 0;
char ftpPhoto[25] = "yyyy-mm-dd_hh-mm-ss.jpg";
#define FILE_PHOTO "/photo.jpg"

// PINS For ESP32-CAM
//#define PWDN_GPIO_NUM     32
//#define RESET_GPIO_NUM    -1
//#define XCLK_GPIO_NUM      0
//#define SIOD_GPIO_NUM     26
//#define SIOC_GPIO_NUM     27

//#define Y9_GPIO_NUM       35
//#define Y8_GPIO_NUM       34
//#define Y7_GPIO_NUM       39
//#define Y6_GPIO_NUM       36
//#define Y5_GPIO_NUM       21
//#define Y4_GPIO_NUM       19
//#define Y3_GPIO_NUM       18
//#define Y2_GPIO_NUM        5
//#define VSYNC_GPIO_NUM    25
//#define HREF_GPIO_NUM     23
//#define PCLK_GPIO_NUM     22

// PINS for TTGO-Camera
#define PWDN_GPIO_NUM     26
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     32
#define SIOD_GPIO_NUM     13
#define SIOC_GPIO_NUM     12
#define Y9_GPIO_NUM       39
#define Y8_GPIO_NUM       36
#define Y7_GPIO_NUM       23
#define Y6_GPIO_NUM       18
#define Y5_GPIO_NUM       15
#define Y4_GPIO_NUM       4
#define Y3_GPIO_NUM       14
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    27
#define HREF_GPIO_NUM     25
#define PCLK_GPIO_NUM     19

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { text-align:center; }
    .vert { margin-bottom: 10%; }
    .hori{ margin-bottom: 0%; }
  </style>
</head>
<body>
  <div id="container">
    <h2>WHOISINMYSPOT!!!!</h2>
    <p></p>
    <p>
      <button onclick="location.reload();">REFRESH PAGE</button>
    </p>
  </div>
  <div><img src="saved-photo" id="photo" width="70%"></div>
</body>
</html>)rawliteral";


// ++++++++++++++++++++++++++++++++++ Create Servers

// Create FTP Server, last 2 args are timeout and debug mode
ESP32_FTPClient ftp (ftp_server, ftp_user, ftp_pass, 5000, 2);

// Create Webserver
AsyncWebServer server(80);


// ++++++++++++++++++++++++++++++++++ SETUP

void setup() {

  // Init Serial port
  Serial.begin(115200);
  Serial.println("WhoCam5640 2022-10-08 TTGO-Camera");

  // UNinit 'brownout detector'
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  //  Print Cycles since last hard reset & type of wakeup
  ++cycleCount;
  Serial.println("Cycles since last hard reset: " + String(cycleCount));
  print_wakeup_reason();

  // Init Wi-Fi
  Serial.println("Connecting to WiFi... ");
  WiFi.begin(ssid, password);
  // Keep track of when we started our attempt to get a WiFi connection
  unsigned long startAttemptTime = millis();
  // Keep looping while we're not connected AND haven't reached the timeout
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttemptTime < WIFI_TIMEOUT) {
    delay(10);
  }
  // Make sure that we're actually connected, otherwise go to deep sleep
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wifi Connection FAILED");
    goToDeepSleep();
  }
  Serial.println("Wifi Connected!");
  Serial.print("IP Address: http://");
  Serial.println(WiFi.localIP());

  Serial.println("FTP Up...");

  // Init NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("NTP Up...");

  // Init SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("An Error has occurred while mounting SPIFFS");
    ESP.restart();
  }
  else {
    delay(500);
    Serial.println("SPIFFS Up...");
  }

  // Init Webserver
  server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send_P(200, "text/html", index_html);
  });
  server.on("/saved-photo", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(SPIFFS, FILE_PHOTO, "image/jpg", false);
  });
  server.begin();
  Serial.println("WebServer Up...");

  // Init Camera
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_QSXGA;
  config.jpeg_quality = 10;
  config.fb_count = 2;
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    ESP.restart();
  }
  Serial.println("Camera Up...");

  // Faux Loop Starts Here

  FindLocalTime();            // get Current Date-Time
  capturePhotoSaveSpiffs();   // take photo, wait until the buffer is written to spiffs
  uploadFTP();                // upload the buffer to pride names datetime
  goToDeepSleep();            // sleep and start next cycle.
}

void loop() {
}

void print_wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();
  switch (wakeup_reason)
  {
    case ESP_SLEEP_WAKEUP_EXT0 : Serial.println("Wakeup caused by external signal using RTC_IO"); break;
    case ESP_SLEEP_WAKEUP_EXT1 : Serial.println("Wakeup caused by external signal using RTC_CNTL"); break;
    case ESP_SLEEP_WAKEUP_TIMER : Serial.println("Wakeup caused by timer"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD : Serial.println("Wakeup caused by touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP : Serial.println("Wakeup caused by ULP program"); break;
    default : Serial.printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason); break;
  }
}

void goToDeepSleep()
{
  Serial.println("Going to sleep...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();
  esp_wifi_stop();
  esp_bt_controller_disable();
  // Configure the timer to wake us up!
  esp_sleep_enable_timer_wakeup(SLEEPTIME * 60L * 1000000L);
  // Go to sleep! Zzzz
  esp_deep_sleep_start();
}

// Check if photo capture was successful
bool checkPhoto( fs::FS & fs ) {
  File f_pic = fs.open( FILE_PHOTO );
  unsigned int pic_sz = f_pic.size();
  return ( pic_sz > 100 );
}

// Capture Photo and Save it to SPIFFS
void capturePhotoSaveSpiffs() {
  bool ok = 0; // Boolean indicating if the picture has been taken correctly
  Serial.println("Taking a photo... Stand By.  This may take 5-10 seconds.");
  do {
    // Take a photo with the camera
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      return;
    }

    // Write a Buffer to File
    Serial.printf("Capturing file...");
    File file = SPIFFS.open(FILE_PHOTO, FILE_WRITE);
    // Insert the data in the photo file
    if (!file) {
      Serial.println("Failed to open file in writing mode");
    }
    else {
      Serial.println("Writing a buffer to SPIFFS");
      file.write(fb->buf, fb->len); // payload (image), payload length
      fileSize = file.size();
    }
    // Close the file
    file.close();

    // check if file has been correctly saved in SPIFFS
    ok = checkPhoto(SPIFFS);
    // if not OK clear frame buffer
    if (!ok) {
      esp_camera_fb_return(fb);
    }
  } while ( !ok );

  // Capture Complete
  Serial.print("Pic saved as ");
  Serial.print(FILE_PHOTO);
  Serial.print(" - Size: ");
  Serial.print(fileSize);
  Serial.println(" bytes");
}

// Write file to FTP with current time filename
void uploadFTP(void) {
  ftp.OpenConnection();
  // this next line is suspect!!!!!
  ftp.ChangeWorkDir(ftp_dir);
  ftp.InitFile("Type I");
  ftp.NewFile(ftpPhoto);
  ftp.WriteData(fb->buf, fb->len);
  Serial.println("Writing a buffer to FTP");
  ftp.CloseFile();
  ftp.CloseConnection();
  Serial.print("Pic uploaded as ");
  Serial.print(ftpPhoto);
  Serial.print(" - Size: ");
  Serial.print(fileSize);
  Serial.println(" bytes");
  esp_camera_fb_return(fb);
}

void FindLocalTime(void) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  strftime(ftpPhoto, sizeof(ftpPhoto), "%Y-%m-%d_%H-%M-%S.jpg", &timeinfo);
  Serial.println("Here it is!!!:");
  Serial.println(ftpPhoto);
}

