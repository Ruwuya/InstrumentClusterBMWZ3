# BMW Z3 Instrument Cluster

An ESP32 drives a real 1997 BMW Z3 (E36/7) VDO instrument cluster on the bench. The firmware simulates a car with an engine, a clutch and a gearbox, and turns that into the pulse signals that move the cluster's tachometer and speedometer. You control everything from a PC over UART (the serial monitor).

School project for the subject Embedded Controller.

Cluster part number: 62.11-8 371 580 (mph with an inner km/h scale).

## Features

- Ignition on/off. The engine idles at 800 RPM.
- Throttle in 10 levels (keys `0`–`9`). The RPM moves smoothly towards the target.
- Clutch and gearbox: R, N and gears 1–6 with real ratios and final drive.
- Shifting only works with the clutch in. Otherwise you get a `*grind*`.
- Releasing the clutch in gear forces the RPM to match the road speed.
- Over-rev: if a downshift would put the RPM at 7000 or above, the engine dies.
- Coasting: in neutral or with the clutch in, the car slowly loses speed.
- Reverse can only be selected when the car is stopped.
- Manual mode: `:rpm` and `:speed` set the gauges directly (for testing and calibration).
- A status line over serial once per second.

## Hardware

| Part | Purpose |
|---|---|
| WEMOS LOLIN32 Lite (ESP32) | Microcontroller |
| BMW Z3 instrument cluster (1997, VDO) | The gauges being driven |
| 2× 4N35 optocoupler | Isolate the ESP32 from the cluster's 12 V side |
| 2× 220 Ω resistor | Current limit for the 4N35 LEDs |
| 1× 1 kΩ resistor | Pull-up to +12 V on the tachometer signal |
| 12 V bench power supply (0.5–1 A limit) | Powers the cluster |

## Safety

- **12 V must never reach the ESP32 side.**
- The ESP32 ground and the PSU ground are kept **separate on purpose**. The 4N35 optocouplers are the only link between the two sides.
- Do a continuity check before every power-up (see [docs/Wiring.txt](docs/Wiring.txt)).
- Power-up order: PSU first, then USB. The PSU current must **not** change when USB is plugged in. If it does, the isolation is broken.

## Wiring (short version)

Full step-by-step wiring is in [docs/Wiring.txt](docs/Wiring.txt), and all known cluster pins are in [docs/Pinout.txt](docs/Pinout.txt).

**Cluster power**

| Cluster pin | Connect to |
|---|---|
| X16 pin 22, X16 pin 23, X17 pin 4, X17 pin 16 | +12 V |
| X16 pin 24, X17 pin 3 | PSU GND |
| X16 pin 13 ↔ X16 pin 25 | Bridge (CAN jumper) |

The cluster needs both X16 **and** X17 connected, or it draws 0 A and stays dark.

**Signals**

| Signal | ESP32 | 4N35 (12 V side) | Cluster pin |
|---|---|---|---|
| Tachometer | GPIO 18 → 220 Ω → pin 1 | pin 4 → PSU GND, pin 5 → 1 kΩ to +12 V | X16 pin 20 |
| Speedometer | GPIO 19 → 220 Ω → pin 1 | pin 4 → PSU GND, pin 5 (no pull-up) | X271 pin 4 |

4N35 pin 2 goes to ESP32 GND on both. The speedometer needs no pull-up because the cluster has one inside.

## Calibration

Measured on the bench:

| Gauge | Formula | Example |
|---|---|---|
| Tachometer | f = RPM / 30 | 100 Hz = 3000 RPM |
| Speedometer | f = km/h × 1.25 (= mph × 2) | 50 Hz = 40 km/h / 25 mph |

Road speed from engine RPM:

```
km/h = RPM × tyre circumference × 60 / (gear ratio × final drive × 1000)
```

| Value | |
|---|---|
| Gear ratios 1–6 | 4.23, 2.52, 1.66, 1.22, 1.00, 0.80 |
| Reverse | 4.0 |
| Final drive | 3.45 |
| Tyre circumference | 1.97 m |

## Controls

Serial monitor at **115200 baud**. Single keys act straight away. Commands start with `:` and end with Enter.

| Key | Action |
|---|---|
| `i` | Ignition on/off |
| `c` | Clutch in/out |
| `e` | Gear up (clutch in) |
| `q` | Gear down (clutch in) |
| `0`–`9` | Throttle (0 = off, 9 = full) |
| `,` | Left turn signal on/off |
| `.` | Right turn signal on/off |
| `h` | Hazards on/off |
| `?` | Help |

| Command | Action |
|---|---|
| `:rpm 3000` | Set the tachometer directly |
| `:speed 80` | Set the speedometer directly (km/h) |
| `:auto` | Back to the simulation |
| `:help` | Show the help |

Example: `i` → `c` → `e` → `c` → `5` starts the engine, puts it in 1st gear and accelerates.

## How the code works

- **LEDC** (the ESP32's PWM hardware) generates the square waves with `ledcWriteTone()`. The tachometer uses channel 0 and the speedometer uses channel 2, because channels 0 and 1 share a timer.
- `ledcWriteTone()` is only called when the frequency actually changes. Calling it all the time restarts the timer and makes the tachometer read too low.
- Everything runs on non-blocking `millis()` timers (no `delay()`): the simulation and gauges update every 50 ms, and the status line prints every 1 s.
- The serial parser collects characters after `:` into a buffer (with echo and backspace) and reads the command with `sscanf()`.
- A serial monitor only sends key presses, not key releases, so "hold to accelerate" isn't possible. That's why the throttle uses levels.

## Status

| Function | Status |
|---|---|
| Tachometer | Working, calibrated |
| Speedometer | Working, calibrated |
| Engine / gearbox simulation | Working |
| UART keys and commands | Working |
| Turn signals, high beam, fog | Pins found and tested with +12 V by hand. Waiting for parts. |
| ASC, park brake (P) | Pins from the manual. Waiting for parts. |
| Fuel gauge | **Not working.** See below. |
| Temperature gauge | Not tested |

## Known issues

- Higher gears accelerate faster than lower gears, because the throttle drives the RPM and the speed is calculated from it.
- Some warning lights have no bulb in this cluster (ABS, oil pressure, check battery).

## Next steps

- Lights when the parts arrive: 4N35 + IRF9540 (P-MOSFET) for lights switched to +12 V (turn signals, high beam, fog), and 4N35 + ULN2803A for lights switched to ground (ASC, park brake).
- Turn signal and hazard logic in the code.
- Test the fuel gauge between 1 and 70 Ω (with a 100 Ω potentiometer).
- Better physics (acceleration that depends on the gear).

## Documents

| File | Content |
|---|---|
| [docs/Pinout.txt](docs/Pinout.txt) | All known cluster pins (tested / from the manual) |
| [docs/Wiring.txt](docs/Wiring.txt) | Bench wiring, continuity checks and power-up order |
| [docs/e50110.pdf](docs/e50110.pdf) | BMW Z3 Electrical Troubleshooting Manual (cluster: PDF pages 197–208) |
