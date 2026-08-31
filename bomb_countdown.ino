#include <Wire.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <MFRC522.h>

#define BUZZER_PIN   16
#define LED_PIN      17
#define SDA_PIN      21
#define SCL_PIN      22
#define RFID_SS_PIN  5
#define RFID_RST_PIN 27

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, SCL_PIN, SDA_PIN);
MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN);

enum State { IDLE, COUNTING, DEFUSED, EXPLODED };
State state = IDLE;

const unsigned long TOTAL_TIME_MS = 20000UL;
unsigned long countdownStartMillis = 0;
int lastWholeSecondShown = -1;
unsigned long lastDisplayMillis = 0;

unsigned long lastBeepMillis = 0;
unsigned long ledOffAt = 0;

unsigned long lastCardScan = 0;
const unsigned long cardDebounce = 1000;

unsigned long defusedAt = 0;
const unsigned long defusedScreenDuration = 5000;

const unsigned long HOLD_REQUIRED_MS = 5000;
const unsigned long HOLD_GRACE_MS = 400;
bool isHolding = false;
unsigned long holdStartTime = 0;
unsigned long lastCardPresentMillis = 0;
int lastHoldSecondShown = -1;

void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  SPI.begin();
  mfrc522.PCD_Init();

  u8g2.begin();
  showIdleScreen();
}

void loop() {
  if (state == IDLE || state == EXPLODED) {
    if (checkForTap()) {
      if (millis() - lastCardScan > cardDebounce) {
        lastCardScan = millis();
        if (state == IDLE) {
          startCountdown();
        } else {
          resetBomb();
        }
      }
    }
  }

  if (state == COUNTING) {
    updateHoldToDefuse();
    if (state == COUNTING) {
      runCountdownTick();
    }
  }

  turnOffLedIfDue();

  if (state == EXPLODED) {
    tone(BUZZER_PIN, 2000);
    digitalWrite(LED_PIN, HIGH);
  }

  if (state == DEFUSED && millis() - defusedAt >= defusedScreenDuration) {
    resetBomb();
  }
}

bool checkForTap() {
  if (!detectCard(false)) return false;
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  return true;
}

bool detectCard(bool wakingFromHalt) {
  byte bufferATQA[2];
  byte bufferSize = sizeof(bufferATQA);
  MFRC522::StatusCode result = wakingFromHalt
      ? mfrc522.PICC_WakeupA(bufferATQA, &bufferSize)
      : mfrc522.PICC_RequestA(bufferATQA, &bufferSize);

  if (result != MFRC522::STATUS_OK) return false;
  return mfrc522.PICC_ReadCardSerial();
}

void updateHoldToDefuse() {
  bool present = detectCard(isHolding);

  if (present) {
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    lastCardPresentMillis = millis();

    if (!isHolding) {
      isHolding = true;
      holdStartTime = millis();
      lastHoldSecondShown = -1;
    }

    unsigned long heldFor = millis() - holdStartTime;
    int wholeSecondsHeld = heldFor / 1000;

    if (wholeSecondsHeld != lastHoldSecondShown) {
      lastHoldSecondShown = wholeSecondsHeld;
      showLoadingBar(wholeSecondsHeld);
    }

    unsigned long elapsed = millis() - countdownStartMillis;
    long remainingMs = (long)TOTAL_TIME_MS - (long)elapsed;
    if (remainingMs < 0) remainingMs = 0;

    unsigned long beepInterval = getBeepInterval(remainingMs);
    unsigned long beepDuration = constrain(beepInterval / 2, 50, 250);
    if (millis() - lastBeepMillis >= beepInterval) {
      lastBeepMillis = millis();
      triggerTick(2200, beepDuration);
    }

    if (heldFor >= HOLD_REQUIRED_MS) {
      defuseBomb();
    }
  } else if (isHolding) {
    if (millis() - lastCardPresentMillis > HOLD_GRACE_MS) {
      isHolding = false;
      lastWholeSecondShown = -1;
    }
  }
}

void showIdleScreen() {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_helvB14_tr);
  u8g2.drawStr(2, 38, "SCAN CARD");
  u8g2.sendBuffer();
}

void showCountNumber(int value) {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_fub30_tf);
  int xPos = (value >= 10) ? 15 : 40;
  u8g2.setCursor(xPos, 50);
  u8g2.print(value);
  u8g2.sendBuffer();
}

void showCountDecimal(float value) {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_fub25_tf);
  u8g2.setCursor(8, 45);
  u8g2.print(value, 2);
  u8g2.sendBuffer();
}

void showLoadingBar(int secondsHeld) {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(18, 14, "DEFUSING...");

  int x = 10, y = 30, barWidth = 108, barHeight = 20;
  u8g2.drawFrame(x, y, barWidth, barHeight);

  int fillWidth = (barWidth - 4) * secondsHeld / (HOLD_REQUIRED_MS / 1000);
  if (fillWidth > 0) {
    u8g2.drawBox(x + 2, y + 2, fillWidth, barHeight - 4);
  }

  u8g2.setFont(u8g2_font_6x10_tr);
  char buf[8];
  sprintf(buf, "%d/5s", secondsHeld);
  u8g2.drawStr(48, 60, buf);

  u8g2.sendBuffer();
}

void showDefusedScreen() {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_helvB14_tr);
  u8g2.drawStr(5, 38, "DEFUSED");
  u8g2.sendBuffer();
}

void startCountdown() {
  state = COUNTING;
  countdownStartMillis = millis();
  lastWholeSecondShown = -1;
  lastBeepMillis = 0;
  lastDisplayMillis = 0;
  isHolding = false;
  showCountNumber(20);

  triggerTick(2200, 250);
}

void triggerTick(int freq, int durationMs) {
  tone(BUZZER_PIN, freq, durationMs);
  digitalWrite(LED_PIN, HIGH);
  ledOffAt = millis() + durationMs;
}

void turnOffLedIfDue() {
  if (ledOffAt != 0 && millis() >= ledOffAt && state != EXPLODED) {
    digitalWrite(LED_PIN, LOW);
    ledOffAt = 0;
  }
}

unsigned long getBeepInterval(long remainingMs) {
  if (remainingMs > 12000) {
    return 900;
  } else if (remainingMs > 10000) {
    return 600;
  } else {
    return map(remainingMs, 10000, 0, 500, 60);
  }
}

void runCountdownTick() {
  unsigned long elapsed = millis() - countdownStartMillis;
  long remainingMs = (long)TOTAL_TIME_MS - (long)elapsed;

  if (remainingMs <= 0) {
    explode();
    return;
  }

  if (isHolding) return;

  if (remainingMs > 10000) {
    int wholeSecond = (remainingMs + 999) / 1000;
    if (wholeSecond != lastWholeSecondShown) {
      lastWholeSecondShown = wholeSecond;
      showCountNumber(wholeSecond);

      unsigned long beepDuration = constrain(getBeepInterval(remainingMs) / 2, 50, 250);
      triggerTick(2200, beepDuration);
      lastBeepMillis = millis();
    }
  } else {
    if (millis() - lastDisplayMillis >= 30) {
      lastDisplayMillis = millis();
      showCountDecimal(remainingMs / 1000.0);
    }

    unsigned long beepInterval = getBeepInterval(remainingMs);
    unsigned long beepDuration = constrain(beepInterval / 2, 50, 250);
    if (millis() - lastBeepMillis >= beepInterval) {
      lastBeepMillis = millis();
      triggerTick(2200, beepDuration);
    }
  }
}

void defuseBomb() {
  state = DEFUSED;
  isHolding = false;
  noTone(BUZZER_PIN);
  digitalWrite(LED_PIN, LOW);
  ledOffAt = 0;
  showDefusedScreen();
  defusedAt = millis();

  beep(120, 2500);
  delay(80);
  beep(120, 2500);
}

void explode() {
  state = EXPLODED;
  isHolding = false;

  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFont(u8g2_font_fub30_tf);
  u8g2.setCursor(5, 48);
  u8g2.print("BOOM!");
  u8g2.sendBuffer();

  digitalWrite(LED_PIN, HIGH);
}

void resetBomb() {
  state = IDLE;
  isHolding = false;
  noTone(BUZZER_PIN);
  digitalWrite(LED_PIN, LOW);
  ledOffAt = 0;
  showIdleScreen();
}

void beep(int durationMs, int freq) {
  tone(BUZZER_PIN, freq);
  delay(durationMs);
  noTone(BUZZER_PIN);
}
