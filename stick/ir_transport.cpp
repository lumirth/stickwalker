#include "ir_transport.h"
#include "stick_build.h"

#include "board_hal.h"
#include "diode_status.h"
#include "ir_gate.h"
#include "ir_rx_core.h"
#include "ir_tx.h"
#include "sound_bridge.h"

#include <Arduino.h>
#include <driver/dedic_gpio.h>
#include <driver/gpio.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_private/systimer.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <hal/gpio_ll.h>
#include <hal/systimer_ll.h>
#include <hal/wdt_hal.h>
#include <soc/systimer_struct.h>
#include <string.h>

namespace {

constexpr uint32_t kRingSize = 32768;
constexpr uint32_t kRingMask = kRingSize - 1;
constexpr uint32_t kPublishGates = 32;
#if !CONFIG_IDF_TARGET_ESP32S3 || !CONFIG_ESP_INT_WDT || \
    !CONFIG_ESP_INT_WDT_CHECK_CPU1 || !CONFIG_ESP_TASK_WDT_INIT || \
    !CONFIG_ESP_TASK_WDT_PANIC || !CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 || \
    CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1 || CONFIG_FREERTOS_SMP || \
    !CONFIG_FREERTOS_SYSTICK_USES_SYSTIMER || !CONFIG_ESP_TIMER_IMPL_SYSTIMER
#error "Review the IR watchdog ownership contract for this SDK configuration"
#endif
static_assert(SYSTIMER_COUNTER_OS_TICK == 1 && SYSTIMER_COUNTER_ESPTIMER == 0,
              "Capture pauses only the dedicated RTOS clock, never wall time");
constexpr int64_t kProgressPeriodUs = 50000;
constexpr int64_t kProgressStallUs = 100000;
static_assert(CONFIG_ESP_INT_WDT_TIMEOUT_MS * 1000 > kProgressStallUs * 2,
              "IR progress checks require interrupt watchdog headroom");

// Faults are acquisition failures, distinct from rejected optical candidates.
enum Fault : uint32_t {
  kSetupFault = 1, kWorkerFault = 2, kRingOverrun = 4,
  kProgressStall = 8, kTransmitFault = 16, kTurnaroundFault = 32,
  kReplyDeadline = 64, kWatchdogFault = 128,
};
enum Command : uint32_t { kPark, kListen, kStop };
enum WorkerState : uint32_t { kStarting, kParked, kCapturing, kDone };

uint8_t *ring = nullptr;
TaskHandle_t worker = nullptr;
// These words are shared only through the acquire/release operations below.
uint32_t command = kPark;
uint32_t worker_state = kDone;
uint32_t faults = 0;
uint32_t produced = 0;
uint32_t consumed = 0;
bool session = false;
bool passive = false;
uint32_t epoch_first = 0;
int64_t epoch_us = 0;
// Foreground-owned total; unsigned ring ages recover timestamps across a
// 32-bit sample-index wrap without sharing a 64-bit counter with the producer.
uint64_t consumed_epoch = 0;
uint32_t ignore_before = 0;
bool ignoring_tx = false;
pw_stick::WireReceiver receiver;
pw_stick::GateHealth health;
uint32_t delivered = 0, rejected = 0, gaps = 0;
uint32_t checksum_failures = 0, malformed_packets = 0, rejected_session = 0;
unsigned maximum_reply_us = 0, maximum_roundtrip_us = 0;
unsigned maximum_tx_uncertainty_us = 0;
int64_t pending_request_us = 0, peer_request_us = 0, previous_reply_us = 0;
portMUX_TYPE watchdog_lock = portMUX_INITIALIZER_UNLOCKED;
bool capture_watchdog_owned = false;
bool task_watchdog_capture = false;
esp_task_wdt_user_handle_t progress_user = nullptr;
uint32_t progress_produced = 0, progress_consumed = 0, watchdog_feeds = 0;
int64_t progress_us = 0;
// The pinned SDK uses MWDT1 for IWDT. This context only feeds its existing
// configuration; it does not initialize, disable, or reconfigure MWDT1.
wdt_hal_context_t interrupt_watchdog = {WDT_MWDT1, {&TIMERG1}};
#ifdef PW_STICK_BENCH_CONTROL
bool bench_trace = false;
#endif

void IRAM_ATTR fail(Fault reason) {
  __atomic_fetch_or(&faults, uint32_t(reason), __ATOMIC_RELEASE);
}

void core1_tick(bool enabled) {
  systimer_ll_enable_alarm_int(&SYSTIMER, SYSTIMER_ALARM_OS_TICK_CORE1, enabled);
}

uint16_t ticks_from_us(int64_t us) {
  return uint16_t(uint64_t(us) * 32768u / 1000000u);
}

int64_t sample_time(uint64_t ordinal) {
  return epoch_us + int64_t(ordinal * 625 / 216);
}

int64_t gate_time(uint32_t gate, uint32_t next_read) {
  return sample_time(consumed_epoch - uint32_t(next_read - gate));
}

bool watch_idle(bool enabled) {
  if (task_watchdog_capture == !enabled) return true;
  esp_task_wdt_config_t config = {};
  config.timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000;
  config.idle_core_mask = enabled ? 1 : 0;
  config.trigger_panic = true;
  // Reconfigure also removes/restores IDF's idle hook; deleting the idle task
  // subscription alone would leave that hook resetting a nonexistent entry.
  if (esp_task_wdt_reconfigure(&config) != ESP_OK) return false;
  task_watchdog_capture = !enabled;
  return true;
}

void watch_progress() {
  const int64_t now = esp_timer_get_time();
  if (now - progress_us < kProgressPeriodUs) return;
  const uint32_t written = __atomic_load_n(&produced, __ATOMIC_ACQUIRE);
  const uint32_t read = __atomic_load_n(&consumed, __ATOMIC_ACQUIRE);
  if (written == progress_produced || read == progress_consumed) {
    if (now - progress_us >= kProgressStallUs) {
      fail(kProgressStall);
      __atomic_store_n(&command, kPark, __ATOMIC_RELEASE);
    }
    return;
  }
  bool fed = false;
  // Capture relinquishes this ownership before restoring either tick. The
  // normal IDF tick hook and this substitute therefore never feed together.
  portENTER_CRITICAL(&watchdog_lock);
  if (capture_watchdog_owned && !__atomic_load_n(&faults, __ATOMIC_ACQUIRE)) {
    wdt_hal_write_protect_disable(&interrupt_watchdog);
    wdt_hal_feed(&interrupt_watchdog);
    wdt_hal_write_protect_enable(&interrupt_watchdog);
    fed = true;
  }
  portEXIT_CRITICAL(&watchdog_lock);
  if (!fed) return;
  if (esp_task_wdt_reset_user(progress_user) != ESP_OK) fail(kWatchdogFault);
  progress_produced = written;
  progress_consumed = read;
  progress_us = now;
  ++watchdog_feeds;
}

// The only job in this loop is acquiring raw evidence. No optical trigger,
// packet decoder, event queue, logging, or storage work can change its cadence.
// Publication/control every 32 gates bounds park latency to about 93 us.
void IRAM_ATTR __attribute__((noinline)) capture(uint32_t input_mask) {
  uint32_t gate = __atomic_load_n(&produced, __ATOMIC_RELAXED);
  core1_tick(false);
  portDISABLE_INTERRUPTS();
  // Exclusive capture pauses RTOS virtual time. The pinned SysTick ISR would
  // otherwise replay every missed millisecond on return, making handoff grow
  // with the peer's hold duration. Counter 1 belongs only to the OS ticks;
  // counter 0 (esp_timer/native deadlines) and both watchdog clocks stay live.
  systimer_ll_enable_counter(&SYSTIMER, SYSTIMER_COUNTER_OS_TICK, false);
  portENTER_CRITICAL(&watchdog_lock);
  capture_watchdog_owned = true;
  portEXIT_CRITICAL(&watchdog_lock);
  const int64_t now = esp_timer_get_time();
  uint32_t next = esp_cpu_get_cycle_count() + 800;
  uint32_t fraction = 0;
  epoch_first = gate;
  // A lower time bound on the first scheduled gate, within one gate's timing
  // quantization. TX exclusion rounds toward the following complete gate.
  epoch_us = now + 3;
  gpio_ll_output_enable(&GPIO, 5);
  __atomic_store_n(&worker_state, kCapturing, __ATOMIC_RELEASE);
  for (;; ++gate) {
    if (!(gate & (kPublishGates - 1))) {
      __atomic_store_n(&produced, gate, __ATOMIC_RELEASE);
      if (__atomic_load_n(&command, __ATOMIC_ACQUIRE) != kListen) break;
      if (gate - __atomic_load_n(&consumed, __ATOMIC_ACQUIRE) >
          kRingSize - kPublishGates) {
        fail(kRingOverrun);
        break;
      }
    }
    const uint32_t t0 = next;
    fraction += 4;
    next += 694;
    if (fraction >= 9) { fraction -= 9; ++next; }
    pw_stick::measure_ir_gate(t0, next, input_mask, health,
                              ring + (gate & kRingMask));
    // measure_ir_gate waits for its own scheduled release/readout. Enter the
    // next iteration during recharge instead of burning that time and then
    // squeezing bookkeeping into the following 65-cycle release margin.
  }
  __atomic_store_n(&produced, gate, __ATOMIC_RELEASE);
  gpio_ll_output_disable(&GPIO, 5);
  portENTER_CRITICAL(&watchdog_lock);
  capture_watchdog_owned = false;
  portEXIT_CRITICAL(&watchdog_lock);
  systimer_ll_enable_counter(&SYSTIMER, SYSTIMER_COUNTER_OS_TICK, true);
  core1_tick(true);
  portENABLE_INTERRUPTS();
}

void sampler(void *) {
  dedic_gpio_bundle_handle_t bundle = nullptr;
  uint32_t input_mask = 0;
  const int pins[] = {5};
  dedic_gpio_bundle_config_t config = {};
  config.gpio_array = pins;
  config.array_size = 1;
  config.flags.in_en = 1;
  if (dedic_gpio_new_bundle(&config, &bundle) != ESP_OK ||
      dedic_gpio_get_in_mask(bundle, &input_mask) != ESP_OK) {
    fail(kSetupFault);
  } else {
    __atomic_store_n(&worker_state, kParked, __ATOMIC_RELEASE);
    while (__atomic_load_n(&command, __ATOMIC_ACQUIRE) != kStop) {
      if (__atomic_load_n(&command, __ATOMIC_ACQUIRE) != kListen ||
          __atomic_load_n(&faults, __ATOMIC_ACQUIRE)) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        continue;
      }
      capture(input_mask);
      // A service window must give the scheduler/idle watchdog an actual
      // opportunity before flash coordination and the next receive turn.
      vTaskDelay(1);
      __atomic_store_n(&worker_state, kParked, __ATOMIC_RELEASE);
    }
  }
  core1_tick(true);
  if (bundle) dedic_gpio_del_bundle(bundle);
  __atomic_store_n(&worker_state, kDone, __ATOMIC_RELEASE);
  vTaskSuspend(nullptr);
}

void print_session() {
  const auto &attempt = *StickLastConnection();
  if (!attempt.started) { Serial.println("PW_STICK_LINK none"); return; }
  Serial.printf("PW_STICK_LINK build=%s result=%u preflight=%u battery_mv=%u "
                "checked=%u safe=%u packets=%u command=%02x phase=%u bulk=%u "
                "frames=%lu framing=%lu checksum=%lu malformed=%lu session=%lu "
                "gaps=%lu late=%lu low=%lu timeout=%lu faults=%lu "
                "reply_us=%u roundtrip_us=%u tx_uncertainty_us=%u watchdog_feeds=%lu\n",
      PW_STICK_BUILD_ID, attempt.result, attempt.preflight, attempt.battery_mv,
      unsigned(attempt.checked), unsigned(attempt.receive_safe), attempt.packets,
      attempt.command, attempt.phase, attempt.bulk_phase,
      (unsigned long)delivered, (unsigned long)rejected,
      (unsigned long)checksum_failures, (unsigned long)malformed_packets,
      (unsigned long)rejected_session, (unsigned long)gaps,
      (unsigned long)health.phase_late, (unsigned long)health.already_low,
      (unsigned long)health.timed_out,
      (unsigned long)__atomic_load_n(&faults, __ATOMIC_ACQUIRE),
      maximum_reply_us, maximum_roundtrip_us, maximum_tx_uncertainty_us,
      (unsigned long)watchdog_feeds);
}

void print_capture() {
  const uint32_t end = __atomic_load_n(&produced, __ATOMIC_ACQUIRE);
  const uint64_t available = consumed_epoch +
      uint32_t(end - __atomic_load_n(&consumed, __ATOMIC_ACQUIRE));
  const uint32_t count = available < kRingSize ? uint32_t(available) : kRingSize;
  Serial.printf("PW_STICK_RX_CAPTURE gates=%lu first=%lu epoch=%lu epoch_us=%lld "
                "first_us=%lld ignore_before=%lu\n",
                (unsigned long)count, (unsigned long)(end - count),
                (unsigned long)epoch_first, static_cast<long long>(epoch_us),
                static_cast<long long>(sample_time(available - count)),
                (unsigned long)ignore_before);
  for (uint32_t offset = 0; ring && offset < count; ++offset) {
    const uint32_t gate = end - count + offset;
    Serial.printf("%02x", ring[gate & kRingMask]);
    if ((gate & 63u) == 63u) Serial.println();
  }
  Serial.println("\nPW_STICK_RX_CAPTURE_END");
}

}  // namespace

extern "C" void StickIrInitPins(void) {
  gpio_reset_pin(GPIO_NUM_5);
  gpio_set_direction(GPIO_NUM_5, GPIO_MODE_INPUT);
  gpio_pullup_dis(GPIO_NUM_5);
  gpio_pulldown_dis(GPIO_NUM_5);
}

extern "C" void StickIrConfigure(void) {
  if (worker || progress_user) { fail(kWorkerFault); return; }
  __atomic_store_n(&faults, 0, __ATOMIC_RELEASE);
  delivered = rejected = gaps = 0;
  checksum_failures = malformed_packets = rejected_session = 0;
  maximum_reply_us = maximum_roundtrip_us = maximum_tx_uncertainty_us = 0;
  pending_request_us = peer_request_us = previous_reply_us = 0;
  health = {};
  __atomic_store_n(&produced, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&consumed, 0, __ATOMIC_RELEASE);
  epoch_first = ignore_before = 0;
  ignoring_tx = false;
  consumed_epoch = 0;
  epoch_us = 0;
  watchdog_feeds = 0;
  if (!setCpuFrequencyMhz(240) || !StickDiodePreflight() ||
      !pw_stick::prepare_ir_tx()) fail(kSetupFault);
}

extern "C" void StickIrPassiveDiagnostic(int enabled) { passive = enabled != 0; }

extern "C" void StickIrStart(void) {
  if (worker) { fail(kWorkerFault); return; }
  if (StickIrFailed()) return;
  StickSoundQuiesceForIr();
  if (!ring) ring = static_cast<uint8_t *>(heap_caps_malloc(
      kRingSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!ring || getCpuFrequencyMhz() != 240) { fail(kSetupFault); return; }
  gpio_config_t config = {};
  config.pin_bit_mask = 1ull << 5;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&config) != ESP_OK) { fail(kSetupFault); return; }
  gpio_ll_set_level(&GPIO, 5, 1);
  __atomic_store_n(&produced, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&consumed, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&command, kPark, __ATOMIC_RELEASE);
  __atomic_store_n(&worker_state, kStarting, __ATOMIC_RELEASE);
  if (xTaskCreatePinnedToCore(sampler, "pw-ir-g5", 4096, nullptr, 18,
                              &worker, 0) != pdPASS) {
    worker = nullptr;
    fail(kSetupFault);
    return;
  }
  const int64_t deadline = esp_timer_get_time() + 100000;
  while (__atomic_load_n(&worker_state, __ATOMIC_ACQUIRE) == kStarting &&
         esp_timer_get_time() < deadline) delayMicroseconds(20);
  if (__atomic_load_n(&worker_state, __ATOMIC_ACQUIRE) != kParked) {
    fail(kWorkerFault);
    return;
  }
  if (esp_task_wdt_status(xTaskGetIdleTaskHandleForCore(0)) != ESP_OK ||
      esp_task_wdt_add_user("Stick IR progress", &progress_user) != ESP_OK) {
    fail(kWatchdogFault);
    return;
  }
  session = true;
  StickIrEndService();
}

extern "C" int StickIrBeginService(void) {
  if (!worker || !session) return !StickIrFailed();
  __atomic_store_n(&command, kPark, __ATOMIC_RELEASE);
  const int64_t deadline = esp_timer_get_time() + 10000;
  while (__atomic_load_n(&worker_state, __ATOMIC_ACQUIRE) == kCapturing &&
         esp_timer_get_time() < deadline) delayMicroseconds(5);
  if (__atomic_load_n(&worker_state, __ATOMIC_ACQUIRE) != kParked) {
    fail(kWorkerFault);
    return 0;
  }
  if (!watch_idle(true) || esp_task_wdt_reset_user(progress_user) != ESP_OK) {
    fail(kWatchdogFault);
    return 0;
  }
  return !StickIrFailed();
}

extern "C" void StickIrEndService(void) {
  if (!worker || !session || StickIrFailed() ||
      __atomic_load_n(&worker_state, __ATOMIC_ACQUIRE) == kCapturing) return;
  if (__atomic_load_n(&worker_state, __ATOMIC_ACQUIRE) != kParked) {
    fail(kWorkerFault);
    return;
  }
  const uint32_t first = __atomic_load_n(&produced, __ATOMIC_ACQUIRE);
  if (!progress_user || !watch_idle(false)) { fail(kWatchdogFault); return; }
  __atomic_store_n(&consumed, first, __ATOMIC_RELEASE);
  consumed_epoch = 0;
  receiver.break_capture(first);
  ignore_before = first;
  ignoring_tx = false;
  progress_produced = progress_consumed = first;
  progress_us = esp_timer_get_time();
  __atomic_store_n(&command, kListen, __ATOMIC_RELEASE);
  xTaskNotifyGive(worker);
  const int64_t deadline = esp_timer_get_time() + 10000;
  while (__atomic_load_n(&produced, __ATOMIC_ACQUIRE) - first < kPublishGates &&
         !StickIrFailed() && esp_timer_get_time() < deadline)
    delayMicroseconds(5);
  if (__atomic_load_n(&worker_state, __ATOMIC_ACQUIRE) != kCapturing ||
      __atomic_load_n(&produced, __ATOMIC_ACQUIRE) - first < kPublishGates)
    fail(kWorkerFault);
}

extern "C" void StickIrStop(void) {
  __atomic_store_n(&command, kStop, __ATOMIC_RELEASE);
  if (worker) xTaskNotifyGive(worker);
  const int64_t deadline = esp_timer_get_time() + 50000;
  while (worker &&
         (__atomic_load_n(&worker_state, __ATOMIC_ACQUIRE) != kDone ||
          eTaskGetState(worker) != eSuspended) &&
         esp_timer_get_time() < deadline) delay(1);
  if (worker &&
      (__atomic_load_n(&worker_state, __ATOMIC_ACQUIRE) != kDone ||
       eTaskGetState(worker) != eSuspended)) {
    fail(kWorkerFault);
    return;
  }
  if (worker) vTaskDelete(worker);
  worker = nullptr;
  session = false;
  if (progress_user) {
    if (watch_idle(true) && esp_task_wdt_delete_user(progress_user) == ESP_OK)
      progress_user = nullptr;
    else fail(kWatchdogFault);
  }
  core1_tick(true);
  StickIrInitPins();
  if (!pw_stick::suspend_ir_tx()) fail(kTransmitFault);
  StickBoardPower().setExtOutput(false);
  setCpuFrequencyMhz(80);
#ifdef PW_STICK_BENCH_CONTROL
  if (bench_trace) {
    print_session();
    if (passive) print_capture();
    Serial.println("PW_STICK_STOP_COMPLETE");
  }
#endif
}

extern "C" u16 StickIrTicks(void) { return ticks_from_us(esp_timer_get_time()); }
extern "C" void StickIrDelayTicks(u16 ticks) {
  const u16 start = StickIrTicks();
  while (u16(StickIrTicks() - start) < ticks) {}
}

extern "C" void StickIrSendFrame(const u8 *logical, u8 length) {
  if (!logical || !length || length > pw_stick::kMaxWireBytes) {
    fail(kTransmitFault);
    return;
  }
  StickIrEndService();
  if (StickIrFailed() || !session) return;
  const unsigned wire_us = (unsigned(length) * 10000000u + 115199) / 115200;
  int64_t deadline = 0;
  if (peer_request_us) {
    deadline = peer_request_us + 100000;
    if (esp_timer_get_time() + wire_us + 500 >= deadline) {
      fail(kReplyDeadline);
      return;
    }
  }
  // The native owner has granted this turn. Discard stale, already-consumed
  // traffic and then exclude only the bounded local TX interval below.
  const uint32_t first = __atomic_load_n(&produced, __ATOMIC_ACQUIRE);
  consumed_epoch += uint32_t(first - __atomic_load_n(&consumed, __ATOMIC_RELAXED));
  __atomic_store_n(&consumed, first, __ATOMIC_RELEASE);
  pw_stick::TxWindow tx;
  if (!pw_stick::transmit_logical(logical, length, tx)) {
    fail(kTransmitFault);
    return;
  }
  const unsigned uncertainty = unsigned(tx.latest_end_us - tx.earliest_end_us);
  if (uncertainty > maximum_tx_uncertainty_us) maximum_tx_uncertainty_us = uncertainty;
  // Bound how much turnaround this deliberately conservative interval can
  // consume. This is a qualification limit, not a universal peer minimum.
  if (uncertainty > 50) { fail(kTurnaroundFault); return; }
  ignore_before = epoch_first + uint32_t(
      ((tx.latest_end_us - epoch_us) * 216 + 624) / 625);
  ignoring_tx = true;
  receiver.break_capture(ignore_before);
  if (peer_request_us) {
    const unsigned reply = unsigned(tx.latest_end_us - peer_request_us);
    if (reply > maximum_reply_us) maximum_reply_us = reply;
    if (previous_reply_us) {
      const unsigned roundtrip = unsigned(tx.latest_end_us - previous_reply_us);
      if (roundtrip > maximum_roundtrip_us) maximum_roundtrip_us = roundtrip;
    }
    if (tx.latest_end_us >= deadline) fail(kReplyDeadline);
  }
  previous_reply_us = tx.latest_end_us;
  peer_request_us = 0;
}

extern "C" void StickIrSendByte(u8 logical) { StickIrSendFrame(&logical, 1); }

extern "C" int StickIrTakeBurst(u8 *wire, u8 capacity, u8 *length,
                                 u16 *lastObservationTick) {
  if (!wire || !length || !lastObservationTick || !session || StickIrFailed()) return 0;
  uint32_t read = __atomic_load_n(&consumed, __ATOMIC_RELAXED);
  const uint32_t first = read;
  const uint32_t available = __atomic_load_n(&produced, __ATOMIC_ACQUIRE);
  if (available - read > kRingSize) { fail(kRingOverrun); return 0; }
  const uint32_t count = available - read < 512 ? available - read : 512;
  pw_stick::WireFrame frame;
  for (uint32_t remaining = count; remaining; --remaining) {
    const uint32_t gate = read++;
    if (ignoring_tx) {
      if (int32_t(gate - ignore_before) < 0) continue;
      ignoring_tx = false;
    }
    const auto event = receiver.feed(ring[gate & kRingMask], gate, frame);
    rejected += event.rejected;
    gaps += event.gap;
    if (event.frame) {
      consumed_epoch += uint32_t(read - first);
      __atomic_store_n(&consumed, read, __ATOMIC_RELEASE);
      if (frame.length > capacity) { fail(kRingOverrun); return 0; }
      memcpy(wire, frame.bytes, frame.length);
      *length = frame.length;
      *lastObservationTick = ticks_from_us(gate_time(frame.last_gate, read));
      pending_request_us = gate_time(frame.first_gate, read);
      ++delivered;
      watch_progress();
      return 1;
    }
  }
  consumed_epoch += uint32_t(read - first);
  __atomic_store_n(&consumed, read, __ATOMIC_RELEASE);
  watch_progress();
  return 0;
}

extern "C" int StickIrFailed(void) {
  return __atomic_load_n(&faults, __ATOMIC_ACQUIRE) != 0;
}
extern "C" void StickIrAcceptPacket(unsigned command_id, unsigned phase,
                                     unsigned bulk_phase) {
  peer_request_us = pending_request_us;
  StickConnectionAcceptPacket(command_id, phase, bulk_phase);
}
extern "C" void StickIrRejectPacket(unsigned reason) {
  if (reason == 1) ++checksum_failures;
  else if (reason >= 3) ++rejected_session;
  else ++malformed_packets;
}
extern "C" void StickIrSessionResult(unsigned result) {
  StickConnectionFinish(result);
  print_session();
}
extern "C" void StickIrServiceDiagnostics(void) {
#ifndef PW_STICK_BENCH_CONTROL
  if (session) return;
  while (Serial.available()) {
    const int request = Serial.read();
    if (request == '?') {
      Serial.printf("PW_STICK_DIAGNOSTICS version=2 build=%s\n", PW_STICK_BUILD_ID);
      print_session();
    } else if (request == 'r') print_capture();
  }
#endif
}
#ifdef PW_STICK_BENCH_CONTROL
extern "C" void StickIrBenchTrace(int enabled) { bench_trace = enabled != 0; }
extern "C" int StickIrBenchTraceEnabled(void) { return bench_trace; }
#endif
