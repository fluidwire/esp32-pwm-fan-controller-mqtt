# ESP32-PWM-FAN-CONTROLLER-MQTT — control a PWM fan speed from an ESP32 using MQTT commands

IoT example by [Fluidwire](https://fluidwire.com/).

## What it does

Connects an ESP32 to WiFi and an MQTT broker, subscribes to a command topic,
and sets a fan's speed with the ESP32's LEDC PWM peripheral. Payloads are plain
integers (`0`–`100`, percent). Anything that isn't a valid number is rejected;
anything out of range is clamped; anything between `0` and `MIN_RUN_PERCENT` is
raised to the minimum speed the fan can actually spin at. The percent the
firmware *actually applied* goes back out on a **retained** state topic, and if
the fan has a tachometer wire its RPM is published every 5 seconds.

Three details make this a controller rather than a blinky with a fan on it:

- **25 kHz PWM.** 4-pin PC fans follow the Intel spec and expect a 25 kHz
  control signal. The LEDC peripheral lets the sketch pick that frequency
  explicitly instead of inheriting a sub-kHz default that makes fans whine.
- **Retained command topic.** You publish the setpoint with the retain flag, so
  a board that reboots (brownout, OTA, someone kicked the cable) subscribes and
  snaps straight back to the last requested speed.
- **Failsafe on broker loss.** If the ESP32 can't reach the broker for
  `FAILSAFE_MS` (60 s) it drops to `DEFAULT_PERCENT` (50 %). For enclosure
  cooling, grow tents, server-closet ventilation or a chicken-house fan, "stuck
  at 0 % because the Raspberry Pi running Mosquitto rebooted" is the failure
  you most want designed out.

Pairs with any Fluidwire sensor publisher (e.g. a DHT22 or BME280 node) and a
rule in Node-RED or Home Assistant: temperature goes up, this node spins up.

## Required hardware

- An ESP32 dev board (any variant with WiFi)
- **Either** a 4-pin PWM fan (12 V PC case fan — Arctic, Noctua, be quiet!,
  any brand with a blue PWM wire) **and** a 12 V supply,
- **or** a plain 2-wire DC fan plus a logic-level N-channel MOSFET (IRLZ44N,
  IRL540N, AO3400 breakout), a 220 Ω gate resistor, a 10 kΩ pull-down and a
  1N5819 flyback diode
- Jumper wires

## Required libraries

Install via the Arduino IDE Library Manager:

- **PubSubClient** by Nick O'Leary

`WiFi.h` and the LEDC PWM functions ship with the ESP32 Arduino core, so no
extra install is needed for them. The sketch compiles on both the 2.x and 3.x
ESP32 cores — the LEDC API changed between them and both paths are handled.

## Wiring / setup

### Option A — 4-pin PWM fan

| Fan wire (pin)        | Connect to                                   |
|-----------------------|----------------------------------------------|
| GND (1, black)        | 12 V supply GND **and** ESP32 GND            |
| +12 V (2, yellow/red) | 12 V supply                                  |
| TACH (3, green)       | ESP32 **GPIO26** (internal pull-up is used)  |
| PWM (4, blue)         | ESP32 **GPIO25**                             |

The fan's PWM input is open-collector and reads a 3.3 V signal just fine — no
level shifter needed. The TACH output is also open-collector, which is why the
sketch enables the internal pull-up. The common ground is the part people skip;
without it neither signal has a reference.

### Option B — 2-wire DC fan via MOSFET

```
ESP32 GPIO25 ──220Ω──┬── MOSFET gate
                     └── 10kΩ ── GND
MOSFET drain ──────────── fan (−)      fan (+) ── supply V+
MOSFET source ─────────── supply GND ── ESP32 GND
1N5819 across the fan, cathode (stripe) to V+
```

Set `FAN_HAS_TACH` to `0` in the sketch. Many brushed DC fans are happier at a
lower PWM frequency — try `PWM_FREQ_HZ` of 1000–5000 if yours buzzes or won't
start at low duty.

### Then

1. Copy `config.h.example` to `config.h`.
2. Fill in your WiFi credentials, MQTT broker, and topics.
3. Flash the board and open the Serial Monitor at **115200** baud.

Drive it from any MQTT client. Note the `-r` — that's the retained setpoint:

```
mosquitto_pub -h broker.hivemq.com -t "fluidwire/demo/fan/cmd" -r -m "60"
mosquitto_pub -h broker.hivemq.com -t "fluidwire/demo/fan/cmd" -r -m "0"
mosquitto_pub -h broker.hivemq.com -t "fluidwire/demo/fan/cmd" -r -m "5"
mosquitto_sub -h broker.hivemq.com -t "fluidwire/demo/fan/#" -v
```

Power-cycle the ESP32 after the first command and watch it come back up at
60 % without you re-sending anything.

## Expected serial output

```
Fan -> 50 % (duty 128/255) [boot default]
PWM on GPIO25 @ 25000 Hz, 8-bit
Tach on GPIO26 (2 pulses/rev)
WiFi: connecting to your-wifi-ssid .... OK, ip=192.168.1.42
MQTT: connecting to broker.hivemq.com:1883 as fluidwire-esp32-fan ... OK
Subscribed to fluidwire/demo/fan/cmd (retained setpoint, if any, arrives now)
State -> 50 % (published to fluidwire/demo/fan/state)
Cmd on fluidwire/demo/fan/cmd: '60'
Fan -> 60 % (duty 153/255) [mqtt]
State -> 60 % (published to fluidwire/demo/fan/state)
RPM -> 1140 (published to fluidwire/demo/fan/rpm)
Cmd on fluidwire/demo/fan/cmd: '5'
Raised 5 -> 20 % (MIN_RUN_PERCENT)
Fan -> 20 % (duty 51/255) [mqtt]
State -> 20 % (published to fluidwire/demo/fan/state)
Cmd on fluidwire/demo/fan/cmd: 'fast'
Ignored 'fast' -- expected an integer percent 0..100
```

If you unplug the broker, after a minute you'll see:

```
FAILSAFE: no broker for 60000 ms -> 50 %
Fan -> 50 % (duty 128/255) [failsafe]
```

## Troubleshooting

- **Fan runs at full speed no matter what you publish.** Most 4-pin fans spin at
  100 % when the PWM wire is floating or sees no valid signal. Check GPIO25 is on
  the blue wire and that grounds are common.
- **Fan whines or ticks at low speeds.** PWM frequency is wrong for that fan.
  4-pin fans want 25 kHz; brushed DC fans on a MOSFET usually prefer 1–5 kHz.
- **Fan won't start below ~30 %.** Normal — raise `MIN_RUN_PERCENT`. Startup
  torque needs more than the running duty; some fans also need a brief kick at
  100 % before settling, which you can add in `setFanPercent()`.
- **RPM reads 0.** Confirm `FAN_HAS_TACH` is `1`, the green wire is on GPIO26,
  and the fan is actually turning. 3-pin fans report tach but have no PWM wire.
- **RPM is exactly double or half what the fan's spec says.** Change
  `TACH_PULSES_PER_REV` (2 is standard, a few fans use 1 or 4).
- **`MQTT: ... FAIL rc=-2`.** Can't reach the broker. Check host/port and that
  your network allows outbound port 1883.
- **Setpoint lost after reboot.** You published without `-r`. Retain it, or make
  your dashboard/rule engine retain it.
- **ESP32 reboots when the fan starts.** The fan's inrush is pulling down a
  shared rail. Give the fan its own supply; don't feed 12 V anywhere near the
  ESP32's 3.3 V/5 V pins.

## Security note

This sample uses a plaintext MQTT connection (port 1883) on a public test broker
so it runs out of the box — fine for a demo, but **anyone** can command a public
topic, and this one controls airflow something may depend on. For anything real:

- Use a private broker with username/password auth (fill `MQTT_USER` /
  `MQTT_PASSWORD`).
- Use TLS — see the companion sample
  [`esp32-mqtt-tls-secure`](https://github.com/fluidwire/esp32-mqtt-tls-secure).
- Choose hard-to-guess topic names, and keep `MIN_RUN_PERCENT` / the failsafe
  tuned so a malicious or garbage command can't leave the enclosure without
  airflow.
- Keep your WiFi and broker credentials in `config.h`, which is git-ignored.

## License

MIT — see [LICENSE](LICENSE). Use it, fork it, ship it.

## Got stuck?

We build IoT and embedded hardware for a living. If you're prototyping something
and hit a wall, reach out at [fluidwire.com](https://fluidwire.com/).
