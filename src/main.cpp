#include <Arduino.h>

// ============================================================
//  Pins and channels
// ============================================================

// Tachometer signal output pin and channel
const int PIN_RPM = 18;
const int RPM_CHANNEL = 0;

// Speedometer signal output pin and channel
const int PIN_SPEED = 19;
const int SPEED_CHANNEL = 2;

// Turn signals. 16 and 17 aren't wired yet (waiting for the 4N35 + IRF9540),
// so the built-in LED shows the blinking until then
const int PIN_LEFT_SIGNAL = 16;
const int PIN_RIGHT_SIGNAL = 17;
const int PIN_LED = 22;  // built-in LED on the LOLIN32 Lite, lights up when the pin is LOW

// Other lights switched to +12 V (4N35 + IRF9540)
const int PIN_HIGH_BEAM = 4;   // -> X16 pin 1 (the LOLIN32 Lite has no GPIO 21)
const int PIN_FOG = 13;        // -> X17 pin 8

// Warning lights switched to ground (4N35 + ULN2803A)
const int PIN_PARK_BRAKE = 25;  // -> X16 pin 17
const int PIN_ASC = 27;         // -> X17 pin 12

// ============================================================
//  Calibration and car model
// ============================================================

// Gauge calibration (measured on the bench)
const float RPM_PER_HZ = 30.0f;        // 100 Hz = 3000 RPM
const float SPEED_HZ_PER_KMH = 1.25f;  // 50 Hz = 40 km/h, 100 Hz ≈ 80 km/h

// Car model
const float FINAL_DRIVE = 3.45f;
const float TYRE_CIRCUMFERENCE = 1.97f;  // metres

// Gearbox: -1 = reverse, 0 = neutral, 1-6 = gears
const int MIN_GEAR = -1;
const int MAX_GEAR = 6;
const float GEAR_RATIOS[] = {0, 4.23f, 2.52f, 1.66f, 1.22f, 1.00f, 0.80f};  // index = gear
const float REVERSE_RATIO = 4.0f;

const float COAST_DECEL = 0.1f;  // km/h lost per update while rolling freely (2 km/h per second)

// ============================================================
//  Engine
// ============================================================

const float IDLE_RPM = 800.0f;
const float MAX_RPM = 6000.0f;     // soft limit: the throttle can't push past this
const float HARD_LIMIT = 7000.0f;  // engine dies at or above this (only by downshifting)
const float RPM_RISE = 40.0f;      // max RPM gained per update when the target is higher
const float RPM_FALL = 25.0f;      // max RPM lost per update when the target is lower

bool engineRunning = false;     // toggled with 'i'
int throttle = 0;               // 0 = off ... 9 = full, set with the number keys
bool clutchPressed = false;     // toggled with 'c'
int gear = 0;                   // starts in neutral
float rpm = 0.0f;
float kmh = 0.0f;

// Manual mode: :rpm / :speed commands drive the gauges directly
bool manualMode = false;

// ============================================================
//  Lights
// ============================================================

const unsigned long BLINK_INTERVAL_MS = 333;  // on 333 ms, off 333 ms = 90 flashes per minute

enum TurnSignal { SIGNAL_OFF, SIGNAL_LEFT, SIGNAL_RIGHT };
TurnSignal turnSignal = SIGNAL_OFF;  // the stalk: off, left or right
bool hazards = false;                // the hazard button, works on top of the stalk
bool blinkOn = false;                // the current half of the blink: lit or dark
unsigned long lastBlink = 0;

bool parkBrake = true;               // the car starts parked, toggled with 'p'
bool highBeam = false;               // toggled with 'b'
bool fogLights = false;              // toggled with 'f'

// ASC (traction control)
const int ASC_SPIN_THROTTLE = 7;     // this much throttle in 1st or reverse spins the wheels
const int ASC_MAX_THROTTLE = 5;      // ASC cuts the engine back to this while it's working
const unsigned long ASC_FLASH_MS = 150;  // fast flash while ASC is working
bool ascEnabled = true;              // the ASC button, toggled with 'a'
bool ascWorking = false;             // true while ASC is catching a wheelspin

// ============================================================
//  Timers
// ============================================================

const unsigned long UPDATE_INTERVAL_MS = 50;    // simulation + gauges
const unsigned long STATUS_INTERVAL_MS = 1000;  // status line
unsigned long lastUpdate = 0;
unsigned long lastStatus = 0;

// Last frequency sent to each gauge. ledcWriteTone() restarts the timer every
// time it's called, so only call it when the value actually changes
int lastRpmHz = -1;
int lastSpeedHz = -1;

// ============================================================
//  Serial command line
// ============================================================

char cmdBuffer[32];     // the command being typed, e.g. "rpm 3000"
int cmdLength = 0;      // how many characters are in cmdBuffer
bool inCommand = false; // true between ':' and Enter

// ============================================================
//  Gearbox helpers
// ============================================================

// Ratio for any gear. Reverse needs its own case: GEAR_RATIOS[-1] would read
// memory outside the array.
float ratioFor(int g) {
  if (g == -1) {
    return REVERSE_RATIO;
  }
  return GEAR_RATIOS[g];
}

// "R", "N", "1" ... "6" for printing
const char* gearName(int g) {
  static const char* names[] = {"R", "N", "1", "2", "3", "4", "5", "6"};
  return names[g + 1];  // shift by one so reverse (-1) lands on index 0
}

// The same formula both ways: engine RPM <-> road speed in the current gear
float speedFromRpm(float r, int g) {
  return r * TYRE_CIRCUMFERENCE * 60 / (ratioFor(g) * FINAL_DRIVE * 1000);
}

float rpmFromSpeed(float speed, int g) {
  return speed * ratioFor(g) * FINAL_DRIVE * 1000 / (TYRE_CIRCUMFERENCE * 60);
}

// ============================================================
//  Driver actions (called from the keys)
// ============================================================

void toggleIgnition() {
  if (engineRunning) {
    engineRunning = false;
    rpm = 0;
    Serial.println("Engine off");
    return;
  }
  // Like a real car: starting in gear with the clutch out would make it lurch
  if (gear != 0 && !clutchPressed) {
    Serial.println("Press the clutch (c) or select neutral before starting");
    return;
  }
  engineRunning = true;
  rpm = IDLE_RPM;
  Serial.println("Engine started");
}

void shiftUp() {
  if (!clutchPressed) {
    Serial.println("*grind* - press the clutch (c) to shift");
    return;
  }
  if (gear < MAX_GEAR) {
    gear++;
    Serial.printf("Gear %s\n", gearName(gear));
  }
}

void shiftDown() {
  if (!clutchPressed) {
    Serial.println("*grind* - press the clutch (c) to shift");
    return;
  }
  if (gear == 0 && kmh >= 1) {
    Serial.println("*grind* - stop the car before selecting reverse");
    return;
  }
  if (gear > MIN_GEAR) {
    gear--;
    Serial.printf("Gear %s\n", gearName(gear));
  }
}

void toggleClutch() {
  clutchPressed = !clutchPressed;
  Serial.println(clutchPressed ? "Clutch in" : "Clutch out");

  // Releasing the clutch in gear: engine and wheels lock together,
  // so the engine is forced to the RPM that matches the road speed
  if (!clutchPressed && engineRunning && gear != 0) {
    rpm = rpmFromSpeed(kmh, gear);

    if (rpm >= HARD_LIMIT) {
      engineRunning = false;
      rpm = 0;
      Serial.println("Over-rev! Engine died (downshifted too far)");
    }
    else if (rpm < IDLE_RPM) {
      rpm = IDLE_RPM;  // too slow for this gear: the engine pulls the car up to idle speed
    }
  }
}

// Starts a new blink cycle with the light ON, so it reacts straight away
void restartBlink() {
  blinkOn = true;
  lastBlink = millis();
}

void toggleTurnSignal(TurnSignal side) {
  if (turnSignal == side) {
    turnSignal = SIGNAL_OFF;
    Serial.println("Turn signal off");
    return;
  }
  // Switching straight from left to right works too, like flicking the stalk
  turnSignal = side;
  Serial.println(side == SIGNAL_LEFT ? "Left turn signal" : "Right turn signal");
  restartBlink();
}

void toggleHazards() {
  hazards = !hazards;
  Serial.println(hazards ? "Hazards on" : "Hazards off");
  if (hazards) {
    restartBlink();
  }
}

void toggleParkBrake() {
  parkBrake = !parkBrake;
  Serial.println(parkBrake ? "Park brake on" : "Park brake off");
}

void toggleHighBeam() {
  highBeam = !highBeam;
  Serial.println(highBeam ? "High beam on" : "High beam off");
}

void toggleFog() {
  fogLights = !fogLights;
  Serial.println(fogLights ? "Fog lights on" : "Fog lights off");
}

void toggleAsc() {
  ascEnabled = !ascEnabled;
  Serial.println(ascEnabled ? "ASC on" : "ASC off - no traction control");
}

// Too much throttle in a low gear with the clutch out: the rear wheels spin
bool wheelsSpinning() {
  bool lowGear = gear == 1 || gear == -1;
  return engineRunning && !clutchPressed && lowGear && throttle >= ASC_SPIN_THROTTLE;
}

// ============================================================
//  Serial input
// ============================================================

void printHelp() {
  Serial.println();
  Serial.println("Keys:      i = ignition    c = clutch in/out");
  Serial.println("           e = gear up     q = gear down (clutch must be in)");
  Serial.println("           0-9 = throttle (0 off, 9 full)");
  Serial.println("           , = left signal . = right signal   h = hazards");
  Serial.println("           p = park brake  a = ASC on/off");
  Serial.println("           b = high beam   f = fog lights");
  Serial.println("Commands:  :rpm 3000    set the tacho directly");
  Serial.println("           :speed 80    set the speedometer directly (km/h)");
  Serial.println("           :auto        back to the simulation");
  Serial.println("           :help        this list");
  Serial.println();
}

// Runs a finished command line, e.g. "rpm 3000"
void handleCommand(char* line) {
  char name[16] = "";
  int value = 0;
  int count = sscanf(line, "%15s %d", name, &value);

  if (strcmp(name, "rpm") == 0 && count == 2) {
    manualMode = true;
    rpm = constrain(value, 0, 8000);
    Serial.printf("Manual: tacho at %.0f RPM\n", rpm);
  }
  else if (strcmp(name, "speed") == 0 && count == 2) {
    manualMode = true;
    kmh = constrain(value, 0, 260);
    Serial.printf("Manual: speedometer at %.0f km/h\n", kmh);
  }
  else if (strcmp(name, "auto") == 0) {
    manualMode = false;
    Serial.println("Back to the simulation");
  }
  else if (strcmp(name, "help") == 0) {
    printHelp();
  }
  else {
    Serial.printf("Unknown command: %s (type :help)\n", line);
  }
}

// Reads everything that has arrived over UART since last time
void readSerial() {
  while (Serial.available() > 0) {
    char c = Serial.read();

    // --- Typing a command: collect characters until Enter ---
    if (inCommand) {
      if (c == '\n' || c == '\r') {
        Serial.println();            // move to a new line after the echo
        cmdBuffer[cmdLength] = '\0'; // end the string
        if (cmdLength > 0) {
          handleCommand(cmdBuffer);
        }
        inCommand = false;
        cmdLength = 0;
      }
      else if ((c == '\b' || c == 127) && cmdLength > 0) {
        cmdLength--;                 // backspace: forget the last character
        Serial.print("\b \b");       // and erase it on screen
      }
      else if (c >= ' ' && cmdLength < (int)sizeof(cmdBuffer) - 1) {
        cmdBuffer[cmdLength] = c;
        cmdLength++;
        Serial.print(c);             // echo so you can see what you type
      }
      continue;
    }

    // --- Not in a command: single keys act straight away ---
    if (c == ':') {
      inCommand = true;
      cmdLength = 0;
      Serial.print(':');
      continue;
    }

    if (c >= '0' && c <= '9') {
      throttle = c - '0';            // '7' -> 7
      if (manualMode) {
        manualMode = false;
        Serial.println("Back to the simulation");
      }
      Serial.printf("Throttle %d\n", throttle);
      continue;
    }

    switch (tolower(c)) {
      case 'i': toggleIgnition(); break;
      case 'c': toggleClutch(); break;
      case 'e': shiftUp(); break;
      case 'q': shiftDown(); break;
      case ',': toggleTurnSignal(SIGNAL_LEFT); break;
      case '.': toggleTurnSignal(SIGNAL_RIGHT); break;
      case 'h': toggleHazards(); break;
      case 'p': toggleParkBrake(); break;
      case 'a': toggleAsc(); break;
      case 'b': toggleHighBeam(); break;
      case 'f': toggleFog(); break;
      case '?': printHelp(); break;
    }
  }
}

// ============================================================
//  Simulation and outputs
// ============================================================

// One simulation step: engine RPM and road speed
void updateEngine() {
  // Engine: moves towards the RPM the throttle asks for
  // ASC: on a wheelspin it cuts the engine power, but only when it's switched on
  bool spinning = wheelsSpinning() && ascEnabled;
  if (spinning && !ascWorking) {
    Serial.println("ASC: wheelspin - cutting power");
  }
  ascWorking = spinning;

  int effectiveThrottle = throttle;
  if (ascWorking) {
    effectiveThrottle = min(throttle, ASC_MAX_THROTTLE);
  }

  if (engineRunning) {
    float targetRpm = IDLE_RPM + (MAX_RPM - IDLE_RPM) * effectiveThrottle / 9.0f;

    if (rpm < targetRpm) {
      rpm = min(rpm + RPM_RISE, targetRpm);
    }
    else if (rpm > targetRpm) {
      rpm = max(rpm - RPM_FALL, targetRpm);
    }
  }
  else {
    rpm = 0;
  }

  // Wheels: driven by the engine only with a gear in and the clutch out
  bool engineConnected = engineRunning && gear != 0 && !clutchPressed;
  if (engineConnected) {
    kmh = speedFromRpm(rpm, gear);
  }
  else {
    // Rolling freely: the car slowly loses speed
    kmh = max(kmh - COAST_DECEL, 0.0f);
  }
}

// Sends rpm and kmh to the cluster
void updateGauges() {
  int rpmHz = rpm / RPM_PER_HZ;
  if (rpmHz != lastRpmHz) {
    ledcWriteTone(RPM_CHANNEL, rpmHz);
    lastRpmHz = rpmHz;
  }

  int speedHz = kmh * SPEED_HZ_PER_KMH;
  if (speedHz != lastSpeedHz) {
    ledcWriteTone(SPEED_CHANNEL, speedHz);
    lastSpeedHz = speedHz;
  }
}

// Blinks the turn signals. Runs every loop so the timing stays accurate
void updateLights(unsigned long now) {
  bool blinking = hazards || turnSignal != SIGNAL_OFF;

  if (!blinking) {
    blinkOn = false;
  }
  else if (now - lastBlink >= BLINK_INTERVAL_MS) {
    lastBlink = now;
    blinkOn = !blinkOn;
  }

  // Hazards light both sides, the stalk only its own side
  bool leftOn = blinkOn && (hazards || turnSignal == SIGNAL_LEFT);
  bool rightOn = blinkOn && (hazards || turnSignal == SIGNAL_RIGHT);

  digitalWrite(PIN_LEFT_SIGNAL, leftOn ? HIGH : LOW);
  digitalWrite(PIN_RIGHT_SIGNAL, rightOn ? HIGH : LOW);
  digitalWrite(PIN_LED, (leftOn || rightOn) ? LOW : HIGH);  // LOW = LED on

  // HIGH -> 4N35 on -> MOSFET on -> +12 V to the cluster pin -> light on
  digitalWrite(PIN_HIGH_BEAM, highBeam ? HIGH : LOW);
  digitalWrite(PIN_FOG, fogLights ? HIGH : LOW);

  // HIGH -> 4N35 on -> ULN2803A pulls the cluster pin to ground -> light on
  digitalWrite(PIN_PARK_BRAKE, parkBrake ? HIGH : LOW);

  // ASC light: solid when switched off, fast flash while it's working
  bool ascFlash = (now / ASC_FLASH_MS) % 2 == 0;
  bool ascLight = !ascEnabled || (ascWorking && ascFlash);
  digitalWrite(PIN_ASC, ascLight ? HIGH : LOW);
}

// "off", "left", "right" or "hazards" for the status line
const char* signalName() {
  if (hazards) return "hazards";
  if (turnSignal == SIGNAL_LEFT) return "left";
  if (turnSignal == SIGNAL_RIGHT) return "right";
  return "off";
}

void printStatus() {
  // Only shown when something is on
  const char* brake = parkBrake ? " | (P)" : "";
  const char* asc = !ascEnabled ? " | ASC off" : (ascWorking ? " | ASC working" : "");
  const char* beam = highBeam ? " | high beam" : "";
  const char* fog = fogLights ? " | fog" : "";

  if (manualMode) {
    Serial.printf("[manual] %.0f RPM | %.0f km/h | signal %s%s%s%s%s\n",
                  rpm, kmh, signalName(), brake, asc, beam, fog);
    return;
  }
  Serial.printf("%s | gear %s | clutch %s | throttle %d | %.0f RPM | %.0f km/h | signal %s%s%s%s%s\n",
                engineRunning ? "engine on" : "engine off",
                gearName(gear),
                clutchPressed ? "in" : "out",
                throttle, rpm, kmh, signalName(), brake, asc, beam, fog);
}

// ============================================================

void setup() {
  Serial.begin(115200);  // matches monitor_speed in platformio.ini

  ledcSetup(RPM_CHANNEL, 1000, 8);
  ledcAttachPin(PIN_RPM, RPM_CHANNEL);

  ledcSetup(SPEED_CHANNEL, 1000, 8);
  ledcAttachPin(PIN_SPEED, SPEED_CHANNEL);

  pinMode(PIN_LEFT_SIGNAL, OUTPUT);
  pinMode(PIN_RIGHT_SIGNAL, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LEFT_SIGNAL, LOW);
  digitalWrite(PIN_RIGHT_SIGNAL, LOW);
  digitalWrite(PIN_LED, HIGH);  // HIGH = LED off

  pinMode(PIN_PARK_BRAKE, OUTPUT);
  pinMode(PIN_ASC, OUTPUT);
  pinMode(PIN_HIGH_BEAM, OUTPUT);
  pinMode(PIN_FOG, OUTPUT);

  printHelp();
  Serial.printf("Lights: high beam GPIO %d, fog GPIO %d\n", PIN_HIGH_BEAM, PIN_FOG);
  Serial.println("Engine off - press 'i' to start");
}

void loop() {
  unsigned long now = millis();

  if (now - lastUpdate >= UPDATE_INTERVAL_MS) {
    lastUpdate = now;

    readSerial();
    if (!manualMode) {
      updateEngine();
    }
    updateGauges();
  }

  updateLights(now);

  // Status line, paused while a command is being typed so it doesn't mess up the echo
  if (!inCommand && now - lastStatus >= STATUS_INTERVAL_MS) {
    lastStatus = now;
    printStatus();
  }
}
