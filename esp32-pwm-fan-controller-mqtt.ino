/*
 * ESP32 PWM Fan Speed Controller (MQTT)
 * -------------------------------------
 * Subscribes to an MQTT command topic and sets the speed of a PWM fan using
 * the ESP32's LEDC peripheral. Commands are plain integers 0..100 (percent).
 * The percent actually applied is echoed to a retained state topic, and if
 * the fan has a tachometer wire its RPM is published every few seconds.
 *
 * Why this is more than "analogWrite on a pin":
 *   - 4-pin PC fans expect a 25 kHz PWM control signal (Intel spec). The
 *     default Arduino PWM frequency is far too low and makes them whine or
 *     ignore the signal; LEDC lets us pick the frequency explicitly.
 *   - The command topic is published RETAINED by the controller side, so a
 *     board that reboots or reconnects immediately gets its last setpoint.
 *   - If the broker goes away for longer than FAILSAFE_MS, the fan falls back
 *     to a safe default speed instead of staying wherever it was last told.
 *     For a ventilation / enclosure-cooling prototype "fan stuck at 0 %" is
 *     the failure you most want to avoid.
 *
 * Required libraries (install via Library Manager):
 *   - "PubSubClient" by Nick O'Leary
 *   (WiFi + LEDC PWM are built into the ESP32 Arduino core.)
 *
 * Board: any ESP32 dev board (ESP32 core for Arduino installed via Boards Mgr).
 *        Works on both the 2.x and 3.x ESP32 cores -- the LEDC API changed
 *        between them and this sketch handles both.
 *
 * Wiring, option A -- 4-pin PWM fan (12 V PC case fan):
 *   Fan pin 1 (GND,   black)  -> 12 V supply GND *and* ESP32 GND (common ground)
 *   Fan pin 2 (+12 V, yellow) -> 12 V supply
 *   Fan pin 3 (TACH,  green)  -> ESP32 GPIO26  (open-collector; internal pull-up used)
 *   Fan pin 4 (PWM,   blue)   -> ESP32 GPIO25  (the fan reads a 3.3 V PWM fine)
 *
 * Wiring, option B -- 2-wire DC fan through a logic-level N-MOSFET (IRLZ44N etc.):
 *   ESP32 GPIO25 -> 220R -> MOSFET gate, 10k gate-to-GND pull-down
 *   MOSFET drain -> fan (-),   fan (+) -> supply V+
 *   MOSFET source -> supply GND -> ESP32 GND
 *   Flyback diode (1N5819) across the fan, cathode to V+.
 *   Set FAN_HAS_TACH to 0 and consider lowering PWM_FREQ_HZ to ~1-5 kHz.
 *
 * Setup:
 *   1. Copy config.h.example to config.h
 *   2. Edit config.h with your WiFi creds + MQTT broker + topics
 *   3. Flash. Open Serial Monitor at 115200.
 *
 * Try it with mosquitto from a PC (note -r = retained):
 *   mosquitto_pub -h broker.hivemq.com -t "fluidwire/demo/fan/cmd" -r -m "60"
 *   mosquitto_sub -h broker.hivemq.com -t "fluidwire/demo/fan/#" -v
 *
 * (c) 2026 Fluidwire. Released under the MIT License.
 * https://github.com/fluidwire
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include "config.h"

// --- Hardware ---------------------------------------------------------------
#define FAN_PWM_PIN       25
#define FAN_TACH_PIN      26
#define FAN_HAS_TACH      1       // 0 for a 2-wire fan on a MOSFET
#define TACH_PULSES_PER_REV 2     // most PC fans: 2 pulses per revolution

#define PWM_FREQ_HZ       25000   // Intel 4-wire fan spec; MOSFET + DC fan: try 1000-5000
#define PWM_RESOLUTION    8       // 0..255 duty
#define PWM_CHANNEL       0       // only used by the 2.x core API

// --- Behaviour --------------------------------------------------------------
#define DEFAULT_PERCENT   50      // speed at boot and in failsafe
#define MIN_RUN_PERCENT   20      // below this most fans stall; 0 still means "off"
#define FAILSAFE_MS       60000   // broker unreachable this long -> DEFAULT_PERCENT
#define RPM_PERIOD_MS     5000

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

int           currentPercent = DEFAULT_PERCENT;
bool          failsafeActive = false;
unsigned long lastMqttOkMs   = 0;
unsigned long lastRpmMs      = 0;

#if FAN_HAS_TACH
volatile uint32_t tachPulses = 0;
void IRAM_ATTR onTachPulse() { tachPulses = tachPulses + 1; }
#endif

// --- PWM helpers (2.x vs 3.x core) -----------------------------------------
void pwmBegin() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcAttach(FAN_PWM_PIN, PWM_FREQ_HZ, PWM_RESOLUTION);
#else
  ledcSetup(PWM_CHANNEL, PWM_FREQ_HZ, PWM_RESOLUTION);
  ledcAttachPin(FAN_PWM_PIN, PWM_CHANNEL);
#endif
}

void pwmWriteDuty(uint32_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  ledcWrite(FAN_PWM_PIN, duty);
#else
  ledcWrite(PWM_CHANNEL, duty);
#endif
}

uint32_t percentToDuty(int percent) {
  const uint32_t maxDuty = (1UL << PWM_RESOLUTION) - 1;
  return (uint32_t)((percent * maxDuty + 50) / 100);   // rounded
}

// --- Fan control ------------------------------------------------------------
void publishState() {
  char msg[8];
  snprintf(msg, sizeof(msg), "%d", currentPercent);
  mqtt.publish(MQTT_STATE_TOPIC, msg, true);   // retained: late subscribers see the truth
  Serial.printf("State -> %d %% (published to %s)\n", currentPercent, MQTT_STATE_TOPIC);
}

void setFanPercent(int percent, const char *why) {
  if (percent < 0)   percent = 0;
  if (percent > 100) percent = 100;
  // A fan told to run at 5 % usually just sits there and overheats its coil.
  // Treat anything between 0 and MIN_RUN_PERCENT as MIN_RUN_PERCENT.
  if (percent > 0 && percent < MIN_RUN_PERCENT) {
    Serial.printf("Raised %d -> %d %% (MIN_RUN_PERCENT)\n", percent, MIN_RUN_PERCENT);
    percent = MIN_RUN_PERCENT;
  }
  currentPercent = percent;
  uint32_t duty = percentToDuty(percent);
  pwmWriteDuty(duty);
  Serial.printf("Fan -> %d %% (duty %u/%u) [%s]\n",
                percent, duty, (1U << PWM_RESOLUTION) - 1, why);
  if (mqtt.connected()) publishState();
}

void onMessage(char *topic, byte *payload, unsigned int len) {
  char cmd[16];
  unsigned int n = len < sizeof(cmd) - 1 ? len : sizeof(cmd) - 1;
  memcpy(cmd, payload, n);
  cmd[n] = '\0';
  Serial.printf("Cmd on %s: '%s'\n", topic, cmd);

  // Validate before parsing -- strtol() returns 0 for "fast", which would
  // silently switch the fan OFF.
  char *end = nullptr;
  long pct = strtol(cmd, &end, 10);
  if (end == cmd || *end != '\0') {
    Serial.printf("Ignored '%s' -- expected an integer percent 0..100\n", cmd);
    return;
  }
  failsafeActive = false;
  setFanPercent((int)pct, "mqtt");
}

#if FAN_HAS_TACH
void publishRpm() {
  noInterrupts();
  uint32_t pulses = tachPulses;
  tachPulses = 0;
  interrupts();
  unsigned long now = millis();
  unsigned long dt  = now - lastRpmMs;
  lastRpmMs = now;
  if (dt == 0) return;
  uint32_t rpm = (uint32_t)((uint64_t)pulses * 60000UL / (dt * TACH_PULSES_PER_REV));
  char msg[12];
  snprintf(msg, sizeof(msg), "%u", rpm);
  mqtt.publish(MQTT_RPM_TOPIC, msg, false);
  Serial.printf("RPM -> %u (published to %s)\n", rpm, MQTT_RPM_TOPIC);
}
#endif

// --- Connectivity -----------------------------------------------------------
void connectWiFi() {
  Serial.printf("WiFi: connecting to %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - start > 20000) {     // don't block forever: let failsafe run
      Serial.println(" timeout");
      return;
    }
  }
  Serial.printf(" OK, ip=%s\n", WiFi.localIP().toString().c_str());
}

bool connectMQTT() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  Serial.printf("MQTT: connecting to %s:%u as %s ... ", MQTT_HOST, MQTT_PORT, MQTT_CLIENT_ID);
  bool ok = strlen(MQTT_USER) > 0
    ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)
    : mqtt.connect(MQTT_CLIENT_ID);
  if (!ok) {
    Serial.printf("FAIL rc=%d\n", mqtt.state());
    return false;
  }
  Serial.println("OK");
  // Subscribing to a retained command topic replays the last setpoint at once,
  // so a rebooted board snaps back to what the dashboard last asked for.
  mqtt.subscribe(MQTT_CMD_TOPIC);
  Serial.printf("Subscribed to %s (retained setpoint, if any, arrives now)\n", MQTT_CMD_TOPIC);
  publishState();
  return true;
}

void checkFailsafe() {
  if (mqtt.connected()) {
    lastMqttOkMs = millis();
    return;
  }
  if (!failsafeActive && millis() - lastMqttOkMs > FAILSAFE_MS) {
    failsafeActive = true;
    Serial.printf("FAILSAFE: no broker for %lu ms -> %d %%\n",
                  (unsigned long)FAILSAFE_MS, DEFAULT_PERCENT);
    setFanPercent(DEFAULT_PERCENT, "failsafe");
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  // Fan first, network second: the enclosure gets airflow even if WiFi is down.
  pwmBegin();
  setFanPercent(DEFAULT_PERCENT, "boot default");
  Serial.printf("PWM on GPIO%d @ %d Hz, %d-bit\n", FAN_PWM_PIN, PWM_FREQ_HZ, PWM_RESOLUTION);

#if FAN_HAS_TACH
  pinMode(FAN_TACH_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN), onTachPulse, FALLING);
  Serial.printf("Tach on GPIO%d (%d pulses/rev)\n", FAN_TACH_PIN, TACH_PULSES_PER_REV);
#endif

  lastMqttOkMs = millis();
  lastRpmMs    = millis();
  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) connectMQTT();
}

void loop() {
  static unsigned long lastRetryMs = 0;

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastRetryMs > 5000) { lastRetryMs = millis(); connectWiFi(); }
  } else if (!mqtt.connected()) {
    if (millis() - lastRetryMs > 5000) { lastRetryMs = millis(); connectMQTT(); }
  } else {
    mqtt.loop();
  }

  checkFailsafe();

#if FAN_HAS_TACH
  if (mqtt.connected() && millis() - lastRpmMs >= RPM_PERIOD_MS) publishRpm();
#endif
}
