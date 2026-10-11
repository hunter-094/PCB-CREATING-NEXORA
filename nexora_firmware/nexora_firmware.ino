/*
  NEXORA - firmware ESP32-S3 (WROOM-1 N16R8)
  ------------------------------------------
  - 9 touches de raccourci  -> envoient F13 a F21 (a associer a des actions
    sur le PC avec PowerToys, AutoHotkey, Steam, OBS, etc.)
  - Touches PAUSE / PLAY    -> controle media (lecture/pause, piste suivante)
  - Encodeur rotatif        -> volume +/- ; clic = mute
  - Ecran OLED SSD1309 128x64 (I2C) -> affiche la derniere action et le volume

  Branchement (voir le schema KiCad) :
    Touches : IO4, IO5, IO6, IO7, IO15, IO16, IO17, IO18, IO8
    PAUSE IO9, PLAY IO10
    Encodeur : A IO11, B IO12, bouton IO13
    OLED : SDA IO1, SCL IO2, RES IO14

  Reglages dans l'IDE Arduino (core "esp32" d'Espressif) :
    Carte       : ESP32S3 Dev Module
    USB Mode    : USB-OTG (TinyUSB)
    PSRAM       : OPI PSRAM
    Flash Size  : 16MB
  Bibliotheque a installer : U8g2 (par olikraus)

  Pour flasher : brancher le port USB natif en maintenant le bouton BOOT,
  appuyer sur RESET, puis relacher BOOT.
*/

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDConsumerControl.h"

// ---------- Broches ----------
#define PIN_OLED_SDA 1
#define PIN_OLED_SCL 2
#define PIN_OLED_RES 14

#define PIN_ENC_A  11
#define PIN_ENC_B  12
#define PIN_ENC_SW 13

// ---------- Objets ----------
USBHIDKeyboard        Keyboard;
USBHIDConsumerControl Consumer;
U8G2_SSD1309_128X64_NONAME0_F_HW_I2C u8g2(U8G2_R0, PIN_OLED_RES);

// ---------- Description des touches ----------
enum ActionType { ACT_KEY, ACT_MEDIA };

struct Btn {
  uint8_t       pin;
  const char*   name;     // texte affiche sur l'ecran
  ActionType    type;
  uint16_t      code;     // code HID (clavier) ou code media
  bool          pressed;
  unsigned long lastChange;
};

// Codes HID bruts : F13 = 0x68 ... F21 = 0x70
// Pour changer une action, modifie simplement la ligne correspondante.
Btn buttons[] = {
  {4,  "Touche 1", ACT_KEY,   0x68, false, 0},  // F13
  {5,  "Touche 2", ACT_KEY,   0x69, false, 0},  // F14
  {6,  "Touche 3", ACT_KEY,   0x6A, false, 0},  // F15
  {7,  "Touche 4", ACT_KEY,   0x6B, false, 0},  // F16
  {15, "Touche 5", ACT_KEY,   0x6C, false, 0},  // F17
  {16, "Touche 6", ACT_KEY,   0x6D, false, 0},  // F18
  {17, "Touche 7", ACT_KEY,   0x6E, false, 0},  // F19
  {18, "Touche 8", ACT_KEY,   0x6F, false, 0},  // F20
  {8,  "Touche 9", ACT_KEY,   0x70, false, 0},  // F21
  {9,  "Lecture/Pause", ACT_MEDIA, CONSUMER_CONTROL_PLAY_PAUSE, false, 0},
  {10, "Piste suivante", ACT_MEDIA, CONSUMER_CONTROL_SCAN_NEXT,  false, 0},
};
const uint8_t NB_BTN = sizeof(buttons) / sizeof(buttons[0]);
const unsigned long DEBOUNCE_MS = 20;

// ---------- Encodeur ----------
volatile int16_t encAcc = 0;
volatile uint8_t encState = 0;

void IRAM_ATTR encISR() {
  static const int8_t table[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  encState = ((encState << 2) | (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B)) & 0x0F;
  encAcc += table[encState];
}

bool encPressed = false;
unsigned long encLastChange = 0;

// ---------- Affichage ----------
String lastAction = "NEXORA pret";
int    volumeLevel = 50;          // estimation locale, juste pour l'affichage
bool   muted = false;
bool   dirty = true;
unsigned long lastDraw = 0;

void draw() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB10_tr);
  u8g2.drawStr(0, 12, "NEXORA");
  u8g2.drawHLine(0, 16, 128);

  u8g2.setFont(u8g2_font_helvR10_tr);
  u8g2.drawStr(0, 36, lastAction.c_str());

  // Barre de volume
  u8g2.drawFrame(0, 48, 100, 12);
  if (!muted) u8g2.drawBox(2, 50, map(volumeLevel, 0, 100, 0, 96), 8);
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(104, 58, muted ? "MUT" : String(volumeLevel).c_str());
  u8g2.sendBuffer();
}

void showAction(const String& text) {
  lastAction = text;
  dirty = true;
}

// ---------- Setup ----------
void setup() {
  for (uint8_t i = 0; i < NB_BTN; i++) pinMode(buttons[i].pin, INPUT_PULLUP);
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);

  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  u8g2.begin();
  u8g2.setBusClock(400000);

  Keyboard.begin();
  Consumer.begin();
  USB.begin();

  draw();
}

// ---------- Boucle principale ----------
void loop() {
  unsigned long now = millis();

  // 1) Touches (anti-rebond)
  for (uint8_t i = 0; i < NB_BTN; i++) {
    bool down = (digitalRead(buttons[i].pin) == LOW);
    if (down != buttons[i].pressed && now - buttons[i].lastChange > DEBOUNCE_MS) {
      buttons[i].pressed = down;
      buttons[i].lastChange = now;

      if (buttons[i].type == ACT_KEY) {
        if (down) Keyboard.pressRaw(buttons[i].code);
        else      Keyboard.releaseRaw(buttons[i].code);
      } else {
        if (down) Consumer.press(buttons[i].code);
        else      Consumer.release();
      }
      if (down) showAction(buttons[i].name);
    }
  }

  // 2) Encodeur : 4 transitions = 1 cran
  int16_t acc;
  noInterrupts();
  acc = encAcc;
  if (abs(acc) >= 4) encAcc = 0;
  interrupts();

  if (abs(acc) >= 4) {
    if (acc > 0) {
      Consumer.press(CONSUMER_CONTROL_VOLUME_INCREMENT);
      volumeLevel = min(100, volumeLevel + 2);
    } else {
      Consumer.press(CONSUMER_CONTROL_VOLUME_DECREMENT);
      volumeLevel = max(0, volumeLevel - 2);
    }
    Consumer.release();
    muted = false;
    showAction("Volume");
  }

  // 3) Bouton de l'encodeur : mute
  bool encDown = (digitalRead(PIN_ENC_SW) == LOW);
  if (encDown != encPressed && now - encLastChange > DEBOUNCE_MS) {
    encPressed = encDown;
    encLastChange = now;
    if (encDown) {
      Consumer.press(CONSUMER_CONTROL_MUTE);
      Consumer.release();
      muted = !muted;
      showAction(muted ? "Muet" : "Son actif");
    }
  }

  // 4) Rafraichissement de l'ecran (au plus toutes les 50 ms)
  if (dirty && now - lastDraw > 50) {
    draw();
    dirty = false;
    lastDraw = now;
  }
}
