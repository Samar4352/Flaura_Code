#include <Arduino.h>
#include <Button.h>
#include <Battery.h>
#include <Moisture.h>
#include <WaterPump.h>
#include <WaterLevel.h>
#include <myconfig.h>
#include <Preferences.h>

// ------------------------ Blynk import ------------------------ //

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

// ------------------------ Bluetooth ------------------------ //


// ------------------------ Deep sleep config ---------------------- //

#define WAKEUP_LEVEL  HIGH  // Trigger wakeup when button is HIGH
#define uS_TO_S_FACTOR 1000000LL  // Conversion factor for micro seconds to seconds
#define TIME_TO_SLEEP  86400 * uS_TO_S_FACTOR
#define TIME_TO_SLEEP_FOREVER  31536000 * uS_TO_S_FACTOR

Preferences preferences;

// ------------------------ Pins ---------------------- //

const gpio_num_t PIN_BUTTON = GPIO_NUM_13;

const gpio_num_t PIN_PUMP_POWER = GPIO_NUM_17;

const gpio_num_t PIN_MOISTURE_POWER = GPIO_NUM_19;
const gpio_num_t PIN_MOISTURE_SIGNAL = GPIO_NUM_34;
const uint16_t MOISTURE_CALIBRATION_WATER = 821; // hardcoded for now, TODO claibrate directly from app
const uint16_t MOISTURE_CALIBRATION_AIR = 2535; // hardcoded for now, TODO claibrate directly from app

const gpio_num_t PIN_WATER_POWER_LEVEL_100 = GPIO_NUM_14;
const gpio_num_t PIN_WATER_POWER_LEVEL_70 = GPIO_NUM_27;
const gpio_num_t PIN_WATER_POWER_LEVEL_30 = GPIO_NUM_26;
const gpio_num_t PIN_WATER_POWER_LEVEL_10 = GPIO_NUM_25;
const gpio_num_t PIN_WATER_SIGNAL = GPIO_NUM_35;

const gpio_num_t PIN_BATTERY_LEVEL = GPIO_NUM_32;
const float BATTERY_MIN_VOLTAGE = 2.5;
const float BATTERY_MAX_VOLTAGE = 3.10;
const float BATTERY_THRESHOLD = 2.7;

const gpio_num_t PIN_BUILTIN_LED = GPIO_NUM_5;

// ------------------------ Constants ---------------------- //

const uint16_t MEASURE_WAITING_TIME = 250; // time to wait between measurements

// ------------------------ Cycle controller ---------------------- //

const uint16_t LOOP_FREQUENCY = 25; // Hz
const uint16_t WAIT_PERIOD = 1000 / LOOP_FREQUENCY; // ms
struct Timer {
    uint32_t laptime;
    uint32_t ticks;
};
Timer cycleTimer;
BlynkTimer pumpTimer;
BlynkTimer hibernationTimer;
BlynkTimer countdownTimer;

// ------------------------ Declarations ---------------------- //
Button button(PIN_BUTTON);
WaterPump waterPump(PIN_PUMP_POWER);
Moisture moisture(PIN_MOISTURE_POWER, PIN_MOISTURE_SIGNAL, MEASURE_WAITING_TIME, MOISTURE_CALIBRATION_WATER, MOISTURE_CALIBRATION_AIR);
Battery battery(PIN_BATTERY_LEVEL, MEASURE_WAITING_TIME, BATTERY_MIN_VOLTAGE, BATTERY_MAX_VOLTAGE);
WaterLevel waterLevel(PIN_WATER_SIGNAL, PIN_WATER_POWER_LEVEL_10, PIN_WATER_POWER_LEVEL_30, PIN_WATER_POWER_LEVEL_70, PIN_WATER_POWER_LEVEL_100, MEASURE_WAITING_TIME);

uint8_t moistureThreshold;
uint8_t pumpDurationSec;
long countdownBeforeSleep;

// ------------------------ Blynk Config ---------------------- //

// called every time the device is connected to Blynk.Cloud
BLYNK_CONNECTED() {
    Blynk.syncVirtual(V4, V7);
}

// called every time the V4 state changes
BLYNK_WRITE(V4) {
    int v4Value = param.asInt();

    if (v4Value < 0) {
        moistureThreshold = 0;
    } else if (v4Value > 100) {
        moistureThreshold = 100;
    } else {
        moistureThreshold = v4Value;
    }

}

// called every time the V7 state changes
BLYNK_WRITE(V7) {
    // V7 received is volume to water in millimeter
    // I measured that roughly 2ml of water is pushed per second
    // so pumpDurationSec = V7 / 2
    int v7Value = param.asInt();

    if (v7Value < 0) {
        pumpDurationSec = 1;
    } else if (v7Value > 100) { // 100ml max
        pumpDurationSec = 100;
    } else {
        pumpDurationSec = v7Value;
    }

    pumpDurationSec = pumpDurationSec / 2;

}

void turnOffBlynkPumpSwitch() {
    Blynk.virtualWrite(V5, 0);
}

BLYNK_WRITE(V5) {
    int v5Value = param.asInt();

    if (v5Value == 1) {
        waterPump.startPumping(pumpDurationSec);
        Blynk.virtualWrite(V6, 1); // log pump action
        pumpTimer.setTimeout(pumpDurationSec * 1000, turnOffBlynkPumpSwitch);
    } else {
        waterPump.stopPumping();
    }

}

void sendDataToBlynk() {
    Blynk.virtualWrite(V0, battery.getBatteryVoltage());
    Blynk.virtualWrite(V1, battery.getBatteryPercentage());
    Blynk.virtualWrite(V2, moisture.getMoisturePercentage());
    Blynk.virtualWrite(V3, waterLevel.getWaterPercentage());
}

// ------------------------ Storage preferences functions ---------------------- //

void clearWifiCredentials() {
    preferences.begin("wifi", false); // Use the same namespace that you used for these preferences
    preferences.remove("ssid");
    preferences.remove("password");
    preferences.end();
}

void saveCredentials(const String &ssid, const String &password) {
    preferences.begin("wifi", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.end();
}

void loadCredentials(char* ssid, char* password) {
    preferences.begin("wifi", false);
    String ssidStr = preferences.getString("ssid", WIFI_SSID); // load from config if empty
    String passwordStr = preferences.getString("password", WIFI_PASSWORD); // load from config if empty
    preferences.end();

    // load the strings into the char*
    ssidStr.toCharArray(ssid, ssidStr.length() + 1);
    passwordStr.toCharArray(password, passwordStr.length() + 1);
}

void saveData() {
    preferences.begin("data", false);
    preferences.putUInt("moisture", moistureThreshold);
    preferences.putUInt("duration", pumpDurationSec);
    preferences.end();
}

void loadData() {
    preferences.begin("data", false);
    moistureThreshold = preferences.getUInt("moisture", 0);
    pumpDurationSec = preferences.getUInt("duration", 0);
    preferences.end();

}

// ------------------------ Deep sleep functions ---------------------- //

void deepSleep(long long int timeToSleep) {
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BUTTON, WAKEUP_LEVEL);
    esp_sleep_enable_timer_wakeup(timeToSleep);
    esp_deep_sleep_start();
}


void hibernate() {
    saveData();

   esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL,         ESP_PD_OPTION_OFF);
    deepSleep(TIME_TO_SLEEP);
}

void sleepForeverIfNeeded() {
    if (battery.getBatteryVoltage() <= BATTERY_THRESHOLD) {
        Blynk.virtualWrite(V1, 0);
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
        esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
        esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL,         ESP_PD_OPTION_OFF);

        deepSleep(TIME_TO_SLEEP_FOREVER);
    } else {
    }
}

void updateCountdown() {
    Blynk.virtualWrite(V8, countdownBeforeSleep - 2);
    countdownBeforeSleep--;
}

void prepareForHibernation() {

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        // was woken up by user action
        // maybe user wants to do something
        // let's not go to sleep immediately
        countdownBeforeSleep = 60 + pumpDurationSec;
        hibernationTimer.setTimeout(countdownBeforeSleep * 1000L, hibernate);
    } else {
        countdownBeforeSleep = 2 + pumpDurationSec;
        hibernationTimer.setTimeout(countdownBeforeSleep * 1000L, hibernate);
    }

    countdownTimer.setInterval(1000L, updateCountdown);
}


// ------------------------ Methods ---------------------- //

void waitForNextCycle() {
    uint32_t now;
    do { now = millis(); } while (now - cycleTimer.laptime < WAIT_PERIOD);
    cycleTimer.laptime = now;
    cycleTimer.ticks++;
}

void startMeasures() {
    battery.startMeasure();
    moisture.startMeasure();
    waterLevel.startMesure();
}

bool isMeasuringCompleted() {
    return battery.getBatteryVoltage() != -1
        && moisture.getMoisturePercentage() != -1
        && waterLevel.getWaterPercentage() != -1;
}

void waterPlantIfNeeded() {
    if (waterLevel.getWaterPercentage() > 0 && moisture.getMoisturePercentage() < moistureThreshold) {
        waterPump.startPumping(pumpDurationSec);
        Blynk.virtualWrite(V6, 1); // log pump action
    } else {
    }
}

// ------------------------ Core ---------------------- //

bool needToStartMeasure = true;
bool needToSendDataBlynk = true;
bool needToCheckWatering = true;
bool needToCheckBatteryHealth = true;

void startTasks() {
    if (needToStartMeasure) {
        needToStartMeasure = false;
        startMeasures();
    }

    if (isMeasuringCompleted()) {
        if (needToCheckBatteryHealth) {
            needToCheckBatteryHealth = false;
            sleepForeverIfNeeded();
        }

        if (needToCheckWatering) {
            needToCheckWatering = false;
            waterPlantIfNeeded();
        }
        

        if (needToSendDataBlynk) {
            needToSendDataBlynk = false;
            sendDataToBlynk();
            prepareForHibernation();
        }   
    }
}

void setup() {
    Serial.begin(115200);

    pinMode(PIN_BUILTIN_LED, OUTPUT);
    digitalWrite(PIN_BUILTIN_LED, LOW);

    char ssid[32];
    char password[32];

    loadCredentials(ssid, password);
    loadData();

    WiFi.begin(ssid, password);
    
    Blynk.config(BLYNK_AUTH_TOKEN); // Configure Blynk to use WiFi, but don't connect yet
}

void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        // Only try to connect Blynk if WiFi is connected
        if (!Blynk.connected()) {
            Blynk.connect();
        }
    }

    Blynk.run();
    pumpTimer.run();
    hibernationTimer.run();
    countdownTimer.run();

    button.loopRoutine();
    battery.loopRoutine();
    moisture.loopRoutine();
    waterPump.loopRoutine();
    waterLevel.loopRoutine();

    startTasks();

    waitForNextCycle();
}