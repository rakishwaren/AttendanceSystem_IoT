#include "WiFi.h"
#include "qrcode.h"
#include <esp_int_wdt.h>
#include <esp_task_wdt.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Keypad.h>
#include <ArduinoJson.h>

// Constants: Device Identity
const String dev_name     = "NFC Punch Card Device";
const String dev_serial   = "nfc_vendor0001&dev00001";
const String dev_version  = "1.0 - alpha";

// Constants: Keypad Specification
const byte KEY_ROWS = 4, KEY_COLS = 4;
const char keys[KEY_ROWS][KEY_COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[KEY_ROWS] = {13, 12, 14, 27}; 
byte colPins[KEY_COLS] = {16, 17, 25, 26}; 

// Constants: LCD Size
const int LCD_COLS      = 16;
const int LCD_ROWS      = 2;

// Keypad Declaration
Keypad keypad = Keypad( makeKeymap(keys), rowPins, colPins, KEY_ROWS, KEY_COLS);

// LCD Declaration (CS | DC | MOSI | SCK | RST)
Adafruit_ST7735 tft = Adafruit_ST7735(5, 2, 23, 18, 4);

// I/O Sensors and Switches
#define DOOR_RELAY      0

// Command Light
#define LED_GREEN       22
#define LED_YELLOW      21
#define LED_RED         19

// Multi-Threading Task Handles & Properties
TaskHandle_t handle_Task1, handle_Task2;
const int StackSize           = 10000;

// HTTP and Wifi Task
const String ServerAddr       = "192.168.0.147:3200";
const String headers[]        = { "Content-Type", "application/json" };
const char* wifi_ssid         = "Loh's Level 1(2.4GHz)";
const char* wifi_pwd          = "Lbh72257465";
int reconnect_tries           = 0;

// Challenge and response variables
String challenge_obj          = "";
String challenge_id           = "";
String hashed_challenge       = "";
String response_pw            = "";

// QR-Code System Declaration
QRCode qrcode;
uint8_t qr_version                = 13;

// State Controlling
boolean wifi_connected        = false;
boolean switch_state          = LOW;
boolean led_red_on            = false;
boolean led_yellow_on         = false;
boolean led_green_on          = false;
boolean door_relay_on         = false;
int LED_State_Red             = LOW;
int LED_State_Yellow          = LOW;
int LED_State_Green           = LOW;
unsigned int interruptTimer;

// Interrupts
unsigned int wifi_stat_refresh;

// Result Declaration
struct VerifyResult {
  bool result;
  String fullName = "";
  bool punch_mode;
};

// Beginning Executiion
void setup() {
  Serial.begin(115200);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(DOOR_RELAY, OUTPUT);

  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_RED, LOW);
  digitalWrite(DOOR_RELAY, LOW);
  
  keypad.addEventListener(keyPressEvent);

  tft.initR(INITR_144GREENTAB);

  tft.fillScreen(ST77XX_BLACK);

  tft.setTextWrap(false);
  tft.setCursor(5, 10);
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(2);
  tft.println("Welcome To");
  tft.setCursor(10, 30);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.println("Door Access System");
  tft.setCursor(25, 60);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setTextSize(1);
  tft.println(dev_version);

  delay(2000);

  wifi_mgr();

  xTaskCreate(Task_1, "Task_01", StackSize, NULL, 3, &handle_Task1);
  xTaskCreate(Task_2, "Task_02", StackSize, NULL, 2, &handle_Task2);
  configASSERT(handle_Task1);
  configASSERT(handle_Task2);
}

// Root Loop Execution (Send to Task 0 Function)
void loop() { Task_0(); delay(50); }

// Flashing Stuff
void Task_0(){
  if(millis() - interruptTimer >= 100){
    if(led_red_on == true)
      LED_State_Red = !LED_State_Red;
    else
      LED_State_Red = LOW;

    if(led_yellow_on == true)
      LED_State_Yellow = !LED_State_Yellow;
    else
      LED_State_Yellow = LOW;

    if(led_green_on == true)
      LED_State_Green = !LED_State_Green;
    else
      LED_State_Green = LOW;
      
    interruptTimer = millis();
  }

  if(door_relay_on == true) 
    digitalWrite(DOOR_RELAY, HIGH);
  else
    digitalWrite(DOOR_RELAY, LOW);
    
  digitalWrite(LED_GREEN, LED_State_Green);
  digitalWrite(LED_YELLOW, LED_State_Yellow);
  digitalWrite(LED_RED, LED_State_Red);
}

// Main Task
static void Task_1(void * pvParameters){
  while(true){
    // 1st Touch Down Display
    any_key_start:{
      led_yellow_on = false;
      tft.fillScreen(ST77XX_BLACK);

      tft.setTextWrap(false);
      tft.setCursor(0, 10);
      tft.setTextColor(ST77XX_GREEN);
      tft.setTextSize(1);
      tft.println("Door Access System");
      tft.setCursor(0, 30);
      tft.setTextColor(ST77XX_WHITE);
      tft.setTextSize(1);
      tft.println("Press A - Start as");
      tft.println("Challenge & Response");
      tft.println(" ");
      tft.println("Press B - Start as");
      tft.println("QR Code (30s max)");
      
      tft.setCursor(0, 75);
      tft.setTextColor(ST77XX_YELLOW);
      tft.setTextSize(1);
      tft.println("A: SELECT");
      tft.println("B: CANCEL");
      tft.println("C: BACKSPACE");
      tft.println("D: FORCE RESTART");

      char keypress = NO_KEY;

      while(true){
        keypress = keypad.getKey();
        
        if(keypress == 'A')
          break;

        if(keypress == 'B')
          break;
      }

      challenge_id = "";
      response_pw = "";

      if(keypress == 'B'){
        QRMode_Punch();

        goto any_key_start;
      }
    }

    // Display Challenge Code
    challenge_disp:{
      int timeout = 30;
      
      if(getNewChallenge() == false){
        goto any_key_start;
      }
      
      led_yellow_on = true;
      tft.fillScreen(ST77XX_BLACK);

      tft.setTextWrap(false);
      
      tft.setCursor(0, 10);
      tft.setTextColor(ST77XX_GREEN);
      tft.setTextSize(1);
      tft.println("Door Access System");
      
      tft.setCursor(0, 30);
      tft.setTextColor(ST77XX_WHITE);
      tft.setTextSize(1);
      tft.println("Challenge Code: ");
      tft.println(String(challenge_id));
      
      tft.setCursor(0, 50);
      tft.setTextColor(ST77XX_WHITE);
      tft.setTextSize(1);
      tft.println("Timeout: " + String(30));

      while(checkTarget() == false){
        for(uint16_t x=50; x<58; x++){
          tft.drawFastHLine(0, x, 128,ST77XX_BLACK);
        }
        
        tft.setCursor(0, 50);
        
        if(timeout > 20)
          tft.setTextColor(ST77XX_GREEN);
        else if(timeout > 10)
          tft.setTextColor(ST77XX_YELLOW);
        else
          tft.setTextColor(ST77XX_RED);
        
        tft.setTextSize(1);
        tft.println("Timeout: " + String(timeout));
        
        if(timeout <= 0) {deleteChallenge(); led_yellow_on = false; goto any_key_start;}
        else timeout--;
      }
    }
    
    String input_chars = "";
    int retries = 0;
    led_yellow_on = false;

    // Input Respond Code
    input_response:{
      tft.fillScreen(ST77XX_BLACK);

      tft.setTextWrap(false);
      
      tft.setCursor(0, 10);
      tft.setTextColor(ST77XX_GREEN);
      tft.setTextSize(1);
      tft.println("Door Access System");
      
      tft.setCursor(0, 30);
      tft.setTextColor(ST77XX_WHITE);
      tft.setTextSize(1);
      tft.println("Input Response Code: ");
      
      tft.setCursor(0, 40);
      tft.setTextColor(ST77XX_YELLOW);
      tft.setTextSize(1);
      tft.println("");

      char keypress = NO_KEY;
      int press_state = LOW;

      while(true) {
        keypress = keypad.getKey();
        bool br = false;

        switch(keypress){
          case 'A':
            press_state = HIGH;
            
            if(input_chars.length() == 8){
              br = true;
            }
            else{
              for(uint16_t x=40; x<48; x++){
                tft.drawFastHLine(0, x, 128,ST77XX_BLACK);
              }
              
              tft.setCursor(0, 40);
              tft.setTextColor(ST77XX_RED);
              tft.setTextSize(1);
              tft.println(input_chars);
            }
            break;

          case 'B':
            if(press_state == LOW){
              press_state = HIGH;
              deleteChallenge(); 
              goto any_key_start;
            }

          case 'C':
            if(input_chars.length() > 0 && press_state == LOW){

              int lastInd = input_chars.length() - 1;
              input_chars.remove(lastInd);

              for(uint16_t x=40; x<48; x++){
                tft.drawFastHLine(0, x, 128,ST77XX_BLACK);
              }
              
              tft.setCursor(0, 40);
              tft.setTextColor(ST77XX_YELLOW);
              tft.setTextSize(1);
              tft.println(input_chars);

              press_state = HIGH;
            }
            break;

          case '*':
          case '#':
          case 'D':
            press_state = HIGH;
            break;
            
          case NO_KEY:
            press_state = LOW;
            break;

          default:
            if(input_chars.length() < 8 && press_state == LOW){
              Serial.println("Key pressed: " + keypress);
              input_chars += keypress;

              for(uint16_t x=40; x<48; x++){
                tft.drawFastHLine(0, x, 128,ST77XX_BLACK);
              }
              
              tft.setCursor(0, 40);
              tft.setTextColor(ST77XX_YELLOW);
              tft.setTextSize(1);
              tft.println(input_chars);
              
              press_state = HIGH;
            }
            break;
        }

        if(br == true) break;
      }
    }

    // Authenticate Challenge and Response to access
    verify_result:{
      led_yellow_on = true;
      
      if(retries >= 3){
        tft.fillScreen(ST77XX_BLACK);

        tft.setTextWrap(false);
        
        tft.setCursor(0, 10);
        tft.setTextColor(ST77XX_GREEN);
        tft.setTextSize(1);
        tft.println("Door Access System");
        
        tft.setCursor(0, 30);
        tft.setTextColor(ST77XX_RED);
        tft.setTextSize(1);
        tft.println("Maximum retries reached");
        tft.println("Please try again!");

        vTaskDelay(3000 / portTICK_PERIOD_MS);
        deleteChallenge(); 
        led_yellow_on = false; 
        goto any_key_start;
      }
      else{
         VerifyResult res = checkResponse(input_chars);
        
         if(res.result == true){
            led_yellow_on = false; 
            led_green_on = true;
            int timeouts = 8;
    
            tft.fillScreen(ST77XX_BLACK);
    
            tft.setTextWrap(false);
            
            tft.setCursor(0, 10);
            tft.setTextColor(ST77XX_GREEN);
            tft.setTextSize(1);
            tft.println("Door Access System");
            
            tft.setCursor(0, 30);
            tft.setTextColor(ST77XX_GREEN);
            tft.setTextSize(1);
            tft.println("Access Granted!");
            if(res.punch_mode == true)
              tft.println("Welcome, ");
            else
              tft.println("Goodbye, ");
            tft.setTextColor(ST77XX_YELLOW);
            tft.println(res.fullName);
            
            tft.setCursor(0, 60);
            tft.setTextColor(ST77XX_YELLOW);
            tft.setTextSize(1);
            tft.println("Timeout: " + String(8));

            door_relay_on = true;
            
            while(timeouts >= 0){
              for(uint16_t x=60; x<68; x++){
                tft.drawFastHLine(0, x, 128,ST77XX_BLACK);
              }

              tft.setCursor(0, 60);
              tft.setTextColor(ST77XX_YELLOW);
              tft.setTextSize(1);
              tft.println("Timeout: " + String(timeouts));
              timeouts--;

              vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
            
            door_relay_on = false;
            led_green_on = false;
            goto any_key_start;
          }
          else{
            retries++;
            led_yellow_on = false;
            led_red_on = true;

            tft.fillScreen(ST77XX_BLACK);
    
            tft.setTextWrap(false);
            tft.setCursor(0, 10);
            tft.setTextColor(ST77XX_GREEN);
            tft.setTextSize(1);
            tft.println("Door Access System");
            
            tft.setCursor(0, 30);
            tft.setTextColor(ST77XX_RED);
            tft.setTextSize(1);
            tft.println("Verification Failed!");
            tft.println("Retries: " + String(retries) + " of 3");
            
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            led_red_on = false;
            input_chars = "";
            goto input_response;
          }
      }
    }
    
    reset_wdog();
  }
}

// Wi-Fi State Check
static void Task_2(void * pvParameters){
  while(true){
    if(WiFi.status() == WL_CONNECTED){
      wifi_connected = true;

      if(reconnect_tries > 0){
        reconnect_tries = 0;
        vTaskResume(handle_Task1);
      }
    }
    else{
      wifi_connected = false;

      if(reconnect_tries == 0){
        vTaskSuspend(handle_Task1);
      }
      
      if(reconnect_tries >= 10){
        tft.fillScreen(ST77XX_BLACK);

        tft.setTextWrap(false);
        tft.setCursor(0, 10);
        tft.setTextColor(ST77XX_GREEN);
        tft.setTextSize(1);
        tft.println("Door Access System");
        tft.setCursor(0, 30);
        tft.setTextSize(1);
        tft.setTextColor(ST77XX_WHITE);
        tft.println("Failed to Reconnect");
        
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        ESP.restart();
      }
      else{
        reconnect_tries++;
        WiFi.begin(wifi_ssid, wifi_pwd);
        
        tft.fillScreen(ST77XX_BLACK);

        tft.setTextWrap(false);
        tft.setCursor(0, 10);
        tft.setTextColor(ST77XX_GREEN);
        tft.setTextSize(1);
        tft.println("Door Access System");
        tft.setCursor(0, 30);
        tft.setTextSize(1);
        tft.setTextColor(ST77XX_WHITE);
        tft.println("Reconnecting: " + String(reconnect_tries) + " of 10");
      }
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    reset_wdog();
  }
}

// Authenticate Result (Challenge)
VerifyResult checkResponse(String payload){
  HTTPClient http;
  String con_str = "http://" + ServerAddr + "/dev_req/verify_response";
  http.begin(con_str.c_str());
  http.addHeader(headers[0], headers[1]);

  StaticJsonDocument<256> doc, doc2;
  doc["pid"] = challenge_obj;
  doc["responseIdentity"] = payload;

  char postMsg[255];
  serializeJson(doc, postMsg);

  int httpCode = http.POST(postMsg);

  if(httpCode > 0){
    switch(httpCode){
      case 200:
        String payload = http.getString();
      
        DeserializationError error = deserializeJson(doc2, payload);
        
        if(!error){
          http.end();
          VerifyResult res;
          const char* name_user = doc2["fullname"];
          Serial.println(name_user);
          res.result = (bool)doc2["verified"];
          res.fullName = String(name_user);
          res.punch_mode = (bool)doc2["punch_mode"];
          return res;
        }
    }
  }

  http.end();
  
  VerifyResult res;
  res.result = false;
  res.fullName = "";
  res.punch_mode = false;
  return res;
}


// Authenticate Result (QR Code)
VerifyResult verifyQR(){
  HTTPClient http;
  String con_str = "http://" + ServerAddr + "/dev_req/verify_qr";
  http.begin(con_str.c_str());
  http.addHeader(headers[0], headers[1]);

  StaticJsonDocument<256> doc, doc2;
  doc["pid"] = challenge_obj;

  char postMsg[255];
  serializeJson(doc, postMsg);

  int httpCode = http.POST(postMsg);

  if(httpCode > 0){
    switch(httpCode){
      case 200:
        String payload = http.getString();
      
        DeserializationError error = deserializeJson(doc2, payload);
        
        if(!error){
          http.end();
          VerifyResult res;
          const char* name_user = doc2["fullname"];
          Serial.println(name_user);
          res.result = (bool)doc2["verified"];
          res.fullName = String(name_user);
          res.punch_mode = (bool)doc2["punch_mode"];
          return res;
        }
    }
  }

  http.end();
  
  VerifyResult res;
  res.result = false;
  res.fullName = "";
  res.punch_mode = false;
  return res;
}

// QR-Code Type Door Access
void QRMode_Punch(){
  if(!getNewChallenge())return;
  
  uint8_t qrcodeData[qrcode_getBufferSize(qr_version)];

  String binds = dev_serial + "@" + hashed_challenge;
  
  qrcode_initText(&qrcode, qrcodeData, qr_version, ECC_QUARTILE, binds.c_str());
  
  tft.fillScreen(ST77XX_WHITE);
  
  int screen_W = 128;
  int screen_H = 128;
  int timeout = 30;

  uint8_t x0 = (screen_W - qrcode.size) / 2;
  uint8_t y0 = (screen_H - qrcode.size) / 2;

  for(uint8_t y=0; y<qrcode.size; y++){
    for(uint8_t x=0; x<qrcode.size; x++){
      if(qrcode_getModule(&qrcode, x, y)){
        tft.drawPixel(x0+x, y0+y, ST77XX_BLACK);
      }
      else{
        tft.drawPixel(x0+x, y0+y, ST77XX_WHITE);
      }
    }
  }

  tft.setCursor(13, 10);
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(1);
  tft.println("Scan Here to Punch");

  led_yellow_on = true;

  bool targetInitiated = false;
  
  while(timeout >= 0){
    for(uint16_t x=110; x<118; x++){
      tft.drawFastHLine(0, x, 128,ST77XX_WHITE);
    }
    
    tft.setCursor(30, 110);
    tft.setTextColor(ST77XX_RED);
    tft.setTextSize(1);
    tft.println("Timeout: " + String(timeout));

    if(checkTarget()){
      targetInitiated = true;
      break;
    }

    vTaskDelay(200 / portTICK_PERIOD_MS);
    timeout--;
  }
  
  led_yellow_on = false;

  if(targetInitiated){
    led_yellow_on = true;
    VerifyResult resultSet = verifyQR();
    
    if(resultSet.result == true){
      led_yellow_on = false; 
      led_green_on = true;
      int timeouts = 8;

      tft.fillScreen(ST77XX_BLACK);

      tft.setTextWrap(false);
      
      tft.setCursor(0, 10);
      tft.setTextColor(ST77XX_GREEN);
      tft.setTextSize(1);
      tft.println("Door Access System");
      
      tft.setCursor(0, 30);
      tft.setTextColor(ST77XX_GREEN);
      tft.setTextSize(1);
      tft.println("Access Granted!");
      if(resultSet.punch_mode == true)
        tft.println("Welcome, ");
      else
        tft.println("Goodbye, ");
      tft.setTextColor(ST77XX_YELLOW);
      tft.println(resultSet.fullName);
      
      tft.setCursor(0, 60);
      tft.setTextColor(ST77XX_YELLOW);
      tft.setTextSize(1);
      tft.println("Timeout: " + String(8));

      door_relay_on = true;
      
      while(timeouts >= 0){
        for(uint16_t x=60; x<68; x++){
          tft.drawFastHLine(0, x, 128,ST77XX_BLACK);
        }

        tft.setCursor(0, 60);
        tft.setTextColor(ST77XX_YELLOW);
        tft.setTextSize(1);
        tft.println("Timeout: " + String(timeouts));
        timeouts--;

        vTaskDelay(1000 / portTICK_PERIOD_MS);
      }
      
      door_relay_on = false;
    }
    
    led_green_on = false;
  }
  else{
    deleteChallenge();
  }
}

// Request New Challenge Code
bool getNewChallenge(){
  HTTPClient http;
  String con_str = "http://" + ServerAddr + "/dev_req/create_challenge";
  http.begin(con_str.c_str());
  http.addHeader(headers[0], headers[1]);

  StaticJsonDocument<256> doc, doc2;
  doc["module_id"] = dev_serial;

  char postMsg[255];
  serializeJson(doc, postMsg);

  int httpCode = http.POST(postMsg);

  if(httpCode > 0){
    switch(httpCode){
      case 200:
        String payload = http.getString();
      
        DeserializationError error = deserializeJson(doc2, payload);
        
        if(!error){
          const char* pid = doc2["data"]["pid"];
          const char* challenge = doc2["data"]["challenge_id"];
          const char* hashed_ch = doc2["data"]["hashed"];

          challenge_obj = String(pid);
          challenge_id = String(challenge);
          hashed_challenge = String(hashed_ch);
          http.end();
          return true;
        }
    }
  }

  http.end();
  return false;
}

// Remove Challenge Code
void deleteChallenge(){
  HTTPClient http;
  String con_str = "http://" + ServerAddr + "/dev_req/remove_challenge";
  http.begin(con_str.c_str());
  http.addHeader(headers[0], headers[1]);

  StaticJsonDocument<256> doc;
  doc["pid"] = challenge_obj;

  char postMsg[255];
  serializeJson(doc, postMsg);

  int httpCode = http.POST(postMsg);

  if(httpCode > 0){
    switch(httpCode){
      case 200:
        String payload = http.getString();

        break;
    }
  }

  http.end();
}

// Check Challenge Initiated
bool checkTarget(){
  vTaskDelay(800 / portTICK_PERIOD_MS);
  HTTPClient http;
  String con_str = "http://" + ServerAddr + "/dev_req/have_target";
  http.begin(con_str.c_str());
  http.addHeader(headers[0], headers[1]);

  StaticJsonDocument<256> doc, doc2;
  doc["pid"] = challenge_obj;

  char postMsg[255];
  serializeJson(doc, postMsg);

  int httpCode = http.POST(postMsg);
  bool responses = false;

  if(httpCode > 0){
    switch(httpCode){
      case 200:
        String payload = http.getString();
      
        DeserializationError error = deserializeJson(doc2, payload);
        
        if(!error){
          responses = (bool)doc2["isTarget"];
        }
        break;
    }
  }

  http.end();

  return responses;
}

// Keypress event
void keyPressEvent(KeypadEvent key){
  switch(keypad.getState()){
    case PRESSED:
      break;

    case RELEASED:
      break;

    case HOLD:
      if(key == 'D'){
        tft.fillScreen(ST77XX_BLACK);
    
        tft.setTextWrap(false);
        
        tft.setCursor(0, 10);
        tft.setTextColor(ST77XX_GREEN);
        tft.setTextSize(1);
        tft.println("Restarting Device");
        
        tft.setCursor(0, 30);
        tft.setTextColor(ST77XX_RED);
        tft.setTextSize(1);
        tft.println("Restarting in 5 secs...");

        vTaskDelay(5000 / portTICK_PERIOD_MS);
            
        ESP.restart();
      }
      break;
  }
}

// Initialization: Wi-Fi Set up
void wifi_mgr(){
  tft.fillScreen(ST77XX_BLACK);

  tft.setTextWrap(false);
  tft.setCursor(0, 10);
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(1);
  tft.println("Door Access System");
  
  tft.setCursor(0, 30);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.println("Connecting to WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid, wifi_pwd);

  uint8_t i = 0;
  while(WiFi.status() != WL_CONNECTED){
    delay(500);

    if((++i) >= 10){
      tft.fillScreen(ST77XX_BLACK);

      tft.setTextWrap(false);
      tft.setCursor(0, 10);
      tft.setTextColor(ST77XX_GREEN);
      tft.setTextSize(1);
      tft.println("Door Access System");
      
      tft.setCursor(0, 30);
      tft.setTextColor(ST77XX_RED);
      tft.setTextSize(1);
      tft.println("Connect Error!");
      
      delay(2000);
      ESP.restart();
    }
  }

  wifi_connected = true;
  
  tft.fillScreen(ST77XX_BLACK);

  tft.setTextWrap(false);
  
  tft.setCursor(0, 10);
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(1);
  tft.println("Door Access System");
  
  tft.setCursor(0, 30);
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(1);
  tft.println("Connected to WiFi");
  tft.setTextColor(ST77XX_WHITE);
  tft.print("IP: ");
  tft.println(WiFi.localIP());
  
  delay(3000);
}

// ESP-32 only: Reset WatchDog Timer
void reset_wdog(){
  vTaskDelay(10 / portTICK_PERIOD_MS);
  esp_task_wdt_reset();
}

// ESP-32 only: Restart device
void Restart_Dev(){
  if(handle_Task1 != NULL) vTaskDelete(handle_Task1);
  if(handle_Task2 != NULL) vTaskDelete(handle_Task2);
  
  ESP.restart();
}
