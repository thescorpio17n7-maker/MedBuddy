

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Servo.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>

// ---------------- PIN DEFINITIONS ----------------
#define BUTTON_PIN     7
#define BUZZER_PIN     5
#define SERVO_PIN      12
#define RED_LED_PIN    6
#define GREEN_LED_PIN  13
#define WHITE_LED_PIN  A0

#define STEPPER_IN1    8
#define STEPPER_IN2    9
#define STEPPER_IN3    10
#define STEPPER_IN4    11

#define DFPLAYER_RX    9   // Arduino pin, connects to DFPlayer TX
#define DFPLAYER_TX    11  // Arduino pin, connects to DFPlayer RX

// ---------------- OLED SETUP ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------------- SERVO SETUP ----------------
Servo pusherServo;
const int SERVO_REST_ANGLE = 0;
const int SERVO_PUSH_ANGLE = 90;

// ---------------- DFPLAYER SETUP ----------------
SoftwareSerial dfSerial(DFPLAYER_RX, DFPLAYER_TX);
DFRobotDFPlayerMini dfPlayer;
const uint8_t TRACK_TIME_ALERT = 1;   // 0001.mp3
const uint8_t TRACK_TAKE_MED   = 2;   // 0002.mp3

// ---------------- STEPPER SETTINGS ----------------
const int stepSequence[8][4] = {
  {1,0,0,0},
  {1,1,0,0},
  {0,1,0,0},
  {0,1,1,0},
  {0,0,1,0},
  {0,0,1,1},
  {0,0,0,1},
  {1,0,0,1}
};
int stepIndex = 0;
const int STEPS_PER_90 = 1024;      // quarter turn for 28BYJ-48 half-step mode
const int STEP_DELAY_MS = 2;

// ---------------- SCHEDULE SETTINGS (millis-based) ----------------
const uint32_t START_DELAY_MS   = 15000UL;  // first dose 15s after power-on
const uint32_t SCHEDULE_GAP_MS  = 15000UL;  // then every 15s after that (demo speed)

const uint32_t PUSH_GAP_MS        = 2000UL; // between 2 pushes at same cylinder
const uint32_t POST_PUSH_WAIT_MS  = 1000UL; // wait after last push, before rotating
const uint32_t POSITION_SETTLE_MS = 1000UL; // wait after stepper stops, before next push starts
const uint32_t ALERT_BUZZ_MS      = 1500UL; // buzzer+red LED duration at alert
const uint32_t DISPENSE_BUZZ_MS   = 1500UL; // buzzer+green LED duration at dispense-complete

uint32_t scheduleTimeMs[4];   // millis() timestamps for the 4 doses
bool     scheduleDone[4]     = {false, false, false, false};
uint8_t  currentSchedule      = 0;

// doseCounts[schedule][cylinder position 0..3] = how many pills to push
const uint8_t doseCounts[4][4] = {
  {2, 1, 0, 0},   // Schedule 1: 2 from cyl0 (0deg), 1 from cyl1 (90deg)
  {0, 0, 1, 1},   // Schedule 2: 1 from cyl2 (180deg), 1 from cyl3 (270deg)
  {2, 1, 0, 0},   // Schedule 3: same as schedule 1
  {0, 0, 1, 1}    // Schedule 4: same as schedule 2
};

// ===========================================================================
void setup() {
  Serial.begin(9600);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(WHITE_LED_PIN, OUTPUT);

  pinMode(STEPPER_IN1, OUTPUT);
  pinMode(STEPPER_IN2, OUTPUT);
  pinMode(STEPPER_IN3, OUTPUT);
  pinMode(STEPPER_IN4, OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(WHITE_LED_PIN, HIGH); // "system powered on" indicator

  pusherServo.attach(SERVO_PIN);
  pusherServo.write(SERVO_REST_ANGLE);

  // --- OLED init ---
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED init failed"));
  }
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("MedBuddy"));
  display.setTextSize(1);
  display.println(F("Starting..."));
  display.display();

  // --- DFPlayer init ---
  dfSerial.begin(9600);
  if (!dfPlayer.begin(dfSerial, false, true)) { // isACK=false prevents freezing on repeated play() calls
    Serial.println(F("DFPlayer not found"));
  } else {
    dfPlayer.volume(22); // 0-30
  }

  // --- Build schedule times based on millis() ---
  uint32_t startMs = millis();
  scheduleTimeMs[0] = startMs + START_DELAY_MS;
  scheduleTimeMs[1] = scheduleTimeMs[0] + SCHEDULE_GAP_MS;
  scheduleTimeMs[2] = scheduleTimeMs[1] + SCHEDULE_GAP_MS;
  scheduleTimeMs[3] = scheduleTimeMs[2] + SCHEDULE_GAP_MS;

  showIdleScreen();
}

// ===========================================================================
void loop() {
  if (currentSchedule < 4) {
    if (!scheduleDone[currentSchedule] &&
        millis() >= scheduleTimeMs[currentSchedule]) {
      runScheduleEvent(currentSchedule);
      scheduleDone[currentSchedule] = true;
      currentSchedule++;
      if (currentSchedule < 4) {
        showIdleScreen();
      } else {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println(F("All doses"));
        display.println(F("for today"));
        display.println(F("complete."));
        display.display();
      }
    }
  }
}

// ===========================================================================
void showIdleScreen() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println(F("Ready"));
  display.setTextSize(1);
  display.print(F("Waiting: Dose "));
  display.print(currentSchedule + 1);
  display.println(F("/4"));
  display.display();
}

// ===========================================================================
void runScheduleEvent(uint8_t idx) {

  // ---- STEP 1: Alert - red LED + buzzer together for 1.5s ----
  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);

  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print(F("Dose "));
  display.println(idx + 1);
  display.setTextSize(1);
  printPillDescription(idx);
  display.display();

  dfPlayer.play(TRACK_TIME_ALERT); // isACK=false means this won't hang

  delay(ALERT_BUZZ_MS);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // ---- STEP 2: Show "press button" prompt, then wait ----
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(F("Dose "));
  display.println(idx + 1);
  printPillDescription(idx);
  display.println();
  display.setTextSize(2);
  display.println(F("PRESS"));
  display.println(F("BUTTON"));
  display.display();

  while (digitalRead(BUTTON_PIN) == HIGH) {
    // waiting for press (pull-up wiring: pressed = LOW)
  }
  delay(150); // debounce

  // ---- STEP 3: Dispense across the 4 cylinder positions ----
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println(F("Dispen-"));
  display.println(F("sing.."));
  display.display();

  for (uint8_t pos = 0; pos < 4; pos++) {
    uint8_t count = doseCounts[idx][pos];
    if (count > 0) {
      for (uint8_t i = 0; i < count; i++) {
        pushOnce();
        if (i < count - 1) {
          delay(PUSH_GAP_MS);
        }
      }
      delay(POST_PUSH_WAIT_MS);
    }
    rotateStepper90();
    delay(POSITION_SETTLE_MS); // pause after stepper stops, before next push
  }

  // ---- STEP 4: Confirm dispense - green LED + buzzer together for 1.5s ----
  digitalWrite(GREEN_LED_PIN, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  dfPlayer.play(TRACK_TAKE_MED);

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("Please take"));
  display.println(F("your medicine."));
  display.display();

  delay(DISPENSE_BUZZ_MS);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
}

// ===========================================================================
void printPillDescription(uint8_t idx) {
  if (idx % 2 == 0) {
    display.println(F("2 red+1 blue"));
  } else {
    display.println(F("1 grn+1 purp"));
  }
}

// ===========================================================================
void pushOnce() {
  pusherServo.write(SERVO_PUSH_ANGLE);
  delay(500);
  pusherServo.write(SERVO_REST_ANGLE);
  delay(500);
}

// ===========================================================================
void rotateStepper90() {
  for (int s = 0; s < STEPS_PER_90; s++) {
    stepIndex++;
    if (stepIndex > 7) stepIndex = 0;
    digitalWrite(STEPPER_IN1, stepSequence[stepIndex][0]);
    digitalWrite(STEPPER_IN2, stepSequence[stepIndex][1]);
    digitalWrite(STEPPER_IN3, stepSequence[stepIndex][2]);
    digitalWrite(STEPPER_IN4, stepSequence[stepIndex][3]);
    delay(STEP_DELAY_MS);
  }
  digitalWrite(STEPPER_IN1, LOW);
  digitalWrite(STEPPER_IN2, LOW);
  digitalWrite(STEPPER_IN3, LOW);
  digitalWrite(STEPPER_IN4, LOW);
}
