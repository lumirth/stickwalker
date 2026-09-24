#include "display_panel.h"
#include "eeprom_backend.h"
#include "foreground_bridge.h"
#include "input_bridge.h"
#include "ir_transport.h"
#include "sound_bridge.h"

#include <Arduino.h>
#include <M5Unified.h>
#include <esp_timer.h>

extern "C" void StickPortBoot(void);

namespace {

bool ready = false;
#ifdef PW_STICK_BENCH_CONTROL
bool previous_ir = false;
bool passive_capture = false;
uint64_t passive_deadline_us = 0;
#endif
uint64_t next_sample_us = 0;
uint64_t next_quarter_us = 0;
uint64_t next_second_us = 0;
uint64_t next_input_us = 0;
uint64_t next_beep_us = 0;
uint64_t next_health_us = 0;

void fatal(const char *reason) {
  Serial.printf("PW_STICK_FATAL %s\n", reason);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setCursor(8, 12);
  M5.Display.print(reason);
}

}  // namespace

extern "C" void StickPortSetup(void) {
  Serial.begin(115200);
  setCpuFrequencyMhz(240);
  auto config = M5.config();
  M5.begin(config);
  if (!StickDisplayPanelInit()) { fatal("Display buffer failed"); return; }
  if (!StickEepromMount()) { fatal("EEPROM mount failed"); return; }
  StickEepromDefer(1);
  StickPortBoot();
  StickEepromDefer(0);
  StickDisplayPresent();
  const uint64_t now = uint64_t(esp_timer_get_time());
  next_sample_us = now + 62500;
  next_quarter_us = now + 250000;
  next_second_us = now + 1000000;
  next_input_us = now + 5000;
  next_beep_us = now + 8000;
  next_health_us = now + 2000000;
  ready = true;
#ifdef PW_STICK_BENCH_CONTROL
  Serial.println("PW_STICK_READY");
#endif
}

extern "C" void StickPortLoop(void) {
  if (!ready) { delay(50); return; }
  const uint64_t now = uint64_t(esp_timer_get_time());
#ifdef PW_STICK_BENCH_CONTROL
  if (passive_capture) {
    if (now >= passive_deadline_us) {
      StickIrStop();
      StickIrPassiveDiagnostic(0);
      passive_capture = false;
      Serial.println("PW_STICK_PASSIVE_DONE");
    } else {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    return;
  }
  while (Serial.available()) {
    const int command = Serial.read();
    if (command == 'c' && !StickForegroundIsIr()) {
      StickForegroundRequestIr();
      Serial.println("PW_STICK_BENCH_CONNECT");
    }
    if (command == 'p' && !StickForegroundIsIr()) {
      StickIrConfigure();
      StickIrPassiveDiagnostic(1);
      StickIrStart();
      passive_capture = !StickIrFailed();
      passive_deadline_us = uint64_t(esp_timer_get_time()) + 200000;
      Serial.println(passive_capture ? "PW_STICK_PASSIVE_START" :
                     "PW_STICK_PASSIVE_ERROR");
      if (!passive_capture) StickIrPassiveDiagnostic(0);
    }
  }
#endif
  if (now >= next_quarter_us) {
    next_quarter_us += 250000;
    StickForegroundQuarterSecond();
  }
  if (now >= next_second_us) {
    next_second_us += 1000000;
    StickForegroundSecond();
  }

  if (StickForegroundIsIr()) {
#ifdef PW_STICK_BENCH_CONTROL
    previous_ir = true;
    if (now >= next_health_us) {
      unsigned phase, received, reference;
      StickForegroundIrDiagnostic(&phase, &received, &reference);
      Serial.printf("PW_STICK_IR_ALIVE phase=%u received=%u elapsed=%u\n",
                    phase, received, unsigned(uint16_t(StickIrTicks() - reference)));
      next_health_us = now + 2000000;
    }
#endif
    StickForegroundRun();
    if (StickForegroundIsIr())
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
    return;
  }
#ifdef PW_STICK_BENCH_CONTROL
  if (previous_ir) {
    previous_ir = false;
    Serial.printf("PW_STICK_IR_DONE result=%u\n", StickForegroundIrResult());
  }

  if (now >= next_health_us) {
    next_health_us = now + 2000000;
    Serial.printf("PW_STICK_ALIVE task=%s\n",
                  StickForegroundIsMain() ? "main" :
                  StickForegroundIsBeep() ? "beep" : "other");
  }
#endif

  StickSoundService();

  if (now >= next_input_us) {
    next_input_us += 5000;
    StickInputPoll(millis());
  }
  if (StickForegroundIsMain() && now >= next_sample_us) {
    next_sample_us += 62500;
    StickForegroundRun();
    if (!StickForegroundIsIr()) StickDisplayPresent();
    return;
  }
  if (StickForegroundIsBeep() && now >= next_beep_us) {
    next_beep_us += 8000;
    StickForegroundRun();
    StickDisplayPresent();
    return;
  }
  delay(1);
}
