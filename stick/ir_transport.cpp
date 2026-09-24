#include "ir_transport.h"

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
#include <esp_timer.h>
#include <hal/gpio_ll.h>
#include <hal/systimer_ll.h>
#include <soc/systimer_struct.h>

namespace {

constexpr uint32_t kRingSize = 32768;
constexpr uint32_t kRingMask = kRingSize - 1;
constexpr unsigned kEventSlots = 8;
constexpr unsigned kGateHz = 345600;
constexpr unsigned kLowCutoff = 124;
constexpr unsigned kPacketGapGates = 120;
// An entry exchange is shorter than 30 seconds. Do not insert a one-tick
// blind spot every 125 ms: measured failures lost 5-9 opening UART bytes.
constexpr unsigned kSegmentGates = kGateHz * 30;

struct OpticalEvent {
  uint32_t first;
  uint32_t last;
  uint32_t end_gate;
  int64_t end_us;
};

uint8_t *ring = nullptr;
uint8_t *packet_bins = nullptr;
OpticalEvent events[kEventSlots];
volatile uint32_t produced = 0;
volatile uint32_t event_write = 0;
volatile uint32_t event_read = 0;
volatile bool running = false;
volatile bool stop_requested = false;
volatile bool worker_ready = false;
volatile bool worker_done = false;
volatile bool tx_busy = false;
volatile bool failed = false;
volatile bool passive_diagnostic = false;
#ifdef PW_STICK_BENCH_CONTROL
volatile bool bench_trace = false;
bool trace_enabled() {
  return __atomic_load_n(&bench_trace, __ATOMIC_ACQUIRE);
}
#endif
uint32_t overlong_bursts = 0;
uint32_t raw_events = 0;
uint32_t valid_bursts = 0;
uint32_t invalid_bursts = 0;
uint32_t self_echo_bursts = 0;
uint32_t short_bursts = 0;
pw_stick::GateHealth final_health;
uint32_t final_samples = 0;
uint32_t final_input_mask = 0;
uint8_t diagnostic_bins[pw_stick::kMaxBurstGates];
unsigned diagnostic_count = 0;
unsigned diagnostic_first = 0;
unsigned diagnostic_last = 0;
#ifdef PW_STICK_BENCH_CONTROL
// Retain only the opening three UART bytes of the most recently delivered
// burst. Checksum rejection occurs in the foreground immediately afterward;
// this small copy lets a failed opening byte be examined without changing the
// sampler's instruction stream or buffering an entire session.
uint8_t checksum_opening[96];
unsigned checksum_opening_count = 0;
unsigned checksum_opening_first = 0;
unsigned checksum_opening_gates = 0;
#endif
struct BurstDiagnostic {
  unsigned gates;
  unsigned decoded;
  unsigned credible;
  unsigned bytes;
  unsigned starts;
  unsigned stops;
  uint8_t wire[16];
};
BurstDiagnostic burst_diagnostics[32];
unsigned burst_diagnostic_count = 0;
TaskHandle_t worker_handle = nullptr;
TaskHandle_t protocol_handle = nullptr;

void core1_tick(bool enabled) {
  systimer_ll_enable_alarm_int(&SYSTIMER, SYSTIMER_ALARM_OS_TICK_CORE1,
                               enabled);
}

uint32_t IRAM_ATTR __attribute__((noinline)) aligned_next(uint16_t align_us) {
  const uint32_t cycle = esp_cpu_get_cycle_count();
  if (align_us >= 1000) return cycle;
  const int64_t now_us = esp_timer_get_time();
  uint32_t wait_us = uint32_t((int64_t(align_us) + 1000 - now_us % 1000) % 1000);
  if (wait_us < 20) wait_us += 1000;
  return esp_cpu_get_cycle_count() + wait_us * 240;
}

// Keep this producer separate from setup, decoding, and application code.
// Its per-gate control flow follows the frozen .137 stream producer.
uint32_t IRAM_ATTR __attribute__((noinline)) run_gate_segment(
    uint8_t *bins, uint32_t first, uint32_t limit,
    pw_stick::GateHealth &health, uint32_t input_mask,
    bool detect_events, bool wake_events, bool session_idle_abort,
    uint16_t align_us, TaskHandle_t caller) {
  uint32_t next = aligned_next(align_us), fraction = 0;
  uint32_t first_low = 0, last_low = 0, last_service_gate = first;
  bool in_packet = false, self_echo = false;
  gpio_ll_output_enable(&GPIO, 5);
  uint32_t gate = first;
  for (; (gate < limit || in_packet) && gate < limit + pw_stick::kMaxBurstGates &&
         !__atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE);
       ++gate) {
    if (session_idle_abort && !in_packet &&
        gate - last_service_gate >= kGateHz / 10) break;
    const uint32_t t0 = next;
    fraction += 4;
    next += 694;
    if (fraction >= 9) { fraction -= 9; ++next; }
    const uint8_t value = pw_stick::measure_ir_gate(
        t0, next, input_mask, health, bins + (gate & kRingMask));
    if ((gate & 31u) == 31u)
      __atomic_store_n(&produced, gate + 1, __ATOMIC_RELEASE);
    if (detect_events) {
      if (value < kLowCutoff) {
        if (!in_packet) {
          first_low = gate;
          in_packet = true;
          self_echo = __atomic_load_n(&tx_busy, __ATOMIC_ACQUIRE);
          // Quiet the other core only for the optical burst. A whole-session
          // tick mask trips its interrupt watchdog, while a live OS tick can
          // disturb the GPIO5 charge measurement within a UART data cell.
          core1_tick(false);
        }
        last_low = gate;
      } else if (in_packet && gate - last_low >= kPacketGapGates) {
        if (self_echo) ++self_echo_bursts;
        else if (last_low - first_low < 6) ++short_bursts;
        else {
          const uint32_t written = __atomic_load_n(&event_write, __ATOMIC_RELAXED);
          const uint32_t read = __atomic_load_n(&event_read, __ATOMIC_ACQUIRE);
          if (written - read < kEventSlots) {
            events[written & (kEventSlots - 1)] = {
                first_low, last_low, gate, esp_timer_get_time()};
            __atomic_store_n(&event_write, written + 1, __ATOMIC_RELEASE);
            ++raw_events;
          } else {
            __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
          }
        }
        in_packet = false;
        core1_tick(true);
        if (wake_events && !self_echo) {
          __atomic_store_n(&produced, gate + 1, __ATOMIC_RELEASE);
          portENABLE_INTERRUPTS();
          if (caller) xTaskNotifyGive(caller);
          // This is a causal peer packet boundary: the peer waits for the
          // Stick's answer, so service the idle task and watchdog here.
          vTaskDelay(1);
          portDISABLE_INTERRUPTS();
          next = aligned_next(align_us);
          last_service_gate = gate;
        }
      }
    }
    while (int32_t(esp_cpu_get_cycle_count() - next) < 0) {}
  }
  if (in_packet) {
    core1_tick(true);
    ++overlong_bursts;
    // A packet that reaches the bounded extension has no valid optical gap.
    // The protocol must see an error, never a truncated but plausible frame.
    if (gate >= limit + pw_stick::kMaxBurstGates)
      __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
  }
  __atomic_store_n(&produced, gate, __ATOMIC_RELEASE);
  gpio_ll_output_disable(&GPIO, 5);
  return gate;
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
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    goto finish;
  }
  final_input_mask = input_mask;
  __atomic_store_n(&worker_ready, true, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&running, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE))
    vTaskDelay(1);
  if (__atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE)) goto finish;

  {
    uint32_t next;
    uint32_t fraction = 0;
    uint32_t gate = 0;
    pw_stick::GateHealth health;
#ifdef PW_STICK_BENCH_CONTROL
    if (trace_enabled() &&
        !__atomic_load_n(&passive_diagnostic, __ATOMIC_ACQUIRE))
      Serial.println("PW_STICK_SEGMENT_BEGIN");
#endif
    next = esp_cpu_get_cycle_count() + 800;
    portDISABLE_INTERRUPTS();
    if (__atomic_load_n(&passive_diagnostic, __ATOMIC_ACQUIRE)) {
      gpio_ll_output_enable(&GPIO, 5);
      for (; gate < kGateHz / 5; ++gate) {
        const uint32_t t0 = next;
        fraction += 4;
        next += 694;
        if (fraction >= 9) { fraction -= 9; ++next; }
        pw_stick::measure_ir_gate(t0, next, input_mask, health,
                                  ring + (gate & kRingMask));
        if ((gate & 31u) == 31u)
          __atomic_store_n(&produced, gate + 1, __ATOMIC_RELEASE);
        while (int32_t(esp_cpu_get_cycle_count() - next) < 0) {}
      }
    } else {
      while (!__atomic_load_n(&stop_requested, __ATOMIC_ACQUIRE)) {
        gate = run_gate_segment(ring, gate, gate + kSegmentGates, health,
                                input_mask, true, true, true, 250,
                                protocol_handle);
        portENABLE_INTERRUPTS();
#ifdef PW_STICK_BENCH_CONTROL
        if (trace_enabled() && gate <= kSegmentGates)
          Serial.printf("PW_STICK_SEGMENT_END gate=%lu\n",
                        (unsigned long)gate);
#endif
        if (protocol_handle) xTaskNotifyGive(protocol_handle);
        vTaskDelay(1);
        portDISABLE_INTERRUPTS();
      }
    }
    portENABLE_INTERRUPTS();
    gpio_ll_output_disable(&GPIO, 5);
    __atomic_store_n(&produced, gate, __ATOMIC_RELEASE);
    final_health = health;
    final_samples = gate;
  }
finish:
  core1_tick(true);
  if (bundle) dedic_gpio_del_bundle(bundle);
  if (protocol_handle) xTaskNotifyGive(protocol_handle);
  __atomic_store_n(&worker_done, true, __ATOMIC_RELEASE);
  // Deleting the current worker queues its TCB for the small IDLE0 stack to
  // free. Defer that cleanup to the protocol task, which has more stack.
  vTaskSuspend(nullptr);
}

uint16_t ticks_from_us(int64_t us) {
  return uint16_t((uint64_t(us) * 32768u) / 1000000u);
}

}  // namespace

#ifdef PW_STICK_BENCH_CONTROL
extern "C" void StickIrBenchTrace(int enabled) {
  __atomic_store_n(&bench_trace, enabled != 0, __ATOMIC_RELEASE);
}
extern "C" int StickIrBenchTraceEnabled(void) {
  return trace_enabled();
}
#endif

extern "C" void StickIrInitPins(void) {
  gpio_reset_pin(GPIO_NUM_5);
  gpio_set_direction(GPIO_NUM_5, GPIO_MODE_INPUT);
  gpio_pullup_dis(GPIO_NUM_5);
  gpio_pulldown_dis(GPIO_NUM_5);
}

extern "C" void StickIrConfigure(void) {
  __atomic_store_n(&failed, !pw_stick::prepare_ir_tx(), __ATOMIC_RELEASE);
}

extern "C" void StickIrPassiveDiagnostic(int enabled) {
  __atomic_store_n(&passive_diagnostic, enabled != 0, __ATOMIC_RELEASE);
}

extern "C" void StickIrStart(void) {
  if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE)) return;
  StickSoundQuiesceForIr();
  if (!ring) ring = static_cast<uint8_t *>(heap_caps_malloc(
      kRingSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!packet_bins) packet_bins = static_cast<uint8_t *>(heap_caps_malloc(
      pw_stick::kMaxBurstGates, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!ring || !packet_bins || getCpuFrequencyMhz() != 240) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return;
  }
  gpio_reset_pin(GPIO_NUM_5);
  gpio_config_t config = {};
  config.pin_bit_mask = 1ull << 5;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  if (gpio_config(&config) != ESP_OK) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return;
  }
  gpio_ll_set_level(&GPIO, 5, 1);
  protocol_handle = xTaskGetCurrentTaskHandle();
  __atomic_store_n(&produced, 0, __ATOMIC_RELEASE);
  raw_events = valid_bursts = invalid_bursts = self_echo_bursts =
      overlong_bursts = 0;
  short_bursts = 0;
  final_health = {};
  final_samples = final_input_mask = 0;
  diagnostic_count = 0;
  burst_diagnostic_count = 0;
#ifdef PW_STICK_BENCH_CONTROL
  checksum_opening_count = 0;
#endif
  __atomic_store_n(&event_write, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&event_read, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&stop_requested, false, __ATOMIC_RELEASE);
  __atomic_store_n(&worker_ready, false, __ATOMIC_RELEASE);
  __atomic_store_n(&worker_done, false, __ATOMIC_RELEASE);
  __atomic_store_n(&running, false, __ATOMIC_RELEASE);
  if (xTaskCreatePinnedToCore(sampler, "pw-ir-g5", 4096, nullptr, 18,
                              &worker_handle, 0) != pdPASS) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return;
  }
#ifdef PW_STICK_BENCH_CONTROL
  if (trace_enabled())
    Serial.printf("PW_STICK_WORKER_START handle=%p heap_ok=%u\n",
                  worker_handle, unsigned(heap_caps_check_integrity_all(false)));
#endif
  const int64_t deadline = esp_timer_get_time() + 100000;
  while (!__atomic_load_n(&worker_ready, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE) &&
         esp_timer_get_time() < deadline)
    delayMicroseconds(20);
  if (!__atomic_load_n(&worker_ready, __ATOMIC_ACQUIRE)) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    __atomic_store_n(&stop_requested, true, __ATOMIC_RELEASE);
    return;
  }
  core1_tick(true);
  __atomic_store_n(&running, true, __ATOMIC_RELEASE);
}

extern "C" void StickIrStop(void) {
  __atomic_store_n(&stop_requested, true, __ATOMIC_RELEASE);
  const int64_t deadline = esp_timer_get_time() + 500000;
  while (worker_handle && !__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE) &&
         esp_timer_get_time() < deadline)
    delay(1);
  if (worker_handle && !__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE))
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
#ifdef PW_STICK_BENCH_CONTROL
  if (trace_enabled())
    Serial.printf("PW_STICK_WORKER_STOP handle=%p done=%u heap_ok=%u\n",
                  worker_handle,
                  unsigned(__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE)),
                  unsigned(heap_caps_check_integrity_all(false)));
#endif
  if (worker_handle && __atomic_load_n(&worker_done, __ATOMIC_ACQUIRE))
    vTaskDelete(worker_handle);
  worker_handle = nullptr;
  __atomic_store_n(&running, false, __ATOMIC_RELEASE);
  gpio_reset_pin(GPIO_NUM_5);
  gpio_set_direction(GPIO_NUM_5, GPIO_MODE_INPUT);
  gpio_pullup_dis(GPIO_NUM_5);
  gpio_pulldown_dis(GPIO_NUM_5);
#ifdef PW_STICK_BENCH_CONTROL
  if (trace_enabled()) {
  unsigned histogram[8] = {};
  unsigned frequencies[256] = {};
  const unsigned retained = final_samples < kRingSize ? final_samples : kRingSize;
  for (unsigned i = 0; i < retained; ++i) {
    const uint8_t value = ring[(final_samples - retained + i) & kRingMask];
    ++frequencies[value];
    ++histogram[value < 32 ? 0 : value < 64 ? 1 : value < 96 ? 2 :
                value < 124 ? 3 : value < 160 ? 4 : value < 224 ? 5 :
                value < 240 ? 6 : 7];
  }
  Serial.printf("PW_STICK_IR_STATS samples=%lu mask=%lu low=%lu timeout=%lu "
                "late=%lu events=%lu valid=%lu invalid=%lu "
                "self_echo=%lu short=%lu overlong=%lu failed=%u\n",
                (unsigned long)final_samples, (unsigned long)final_input_mask,
                (unsigned long)final_health.already_low,
                (unsigned long)final_health.timed_out,
                (unsigned long)final_health.phase_late,
                (unsigned long)raw_events, (unsigned long)valid_bursts,
                (unsigned long)invalid_bursts, (unsigned long)self_echo_bursts,
                (unsigned long)short_bursts,
                (unsigned long)overlong_bursts,
                unsigned(__atomic_load_n(&failed, __ATOMIC_ACQUIRE)));
  Serial.printf("PW_STICK_IR_HIST retained=%u bins=%u,%u,%u,%u,%u,%u,%u,%u\n",
                retained, histogram[0], histogram[1], histogram[2],
                histogram[3], histogram[4], histogram[5], histogram[6],
                histogram[7]);
  for (unsigned rank = 0; rank < 8; ++rank) {
    unsigned winner = 0;
    for (unsigned value = 1; value < 256; ++value)
      if (frequencies[value] > frequencies[winner]) winner = value;
    Serial.printf("PW_STICK_IR_VALUE rank=%u value=%u count=%u\n",
                  rank, winner, frequencies[winner]);
    frequencies[winner] = 0;
  }
  if (diagnostic_count) {
    Serial.printf("PW_STICK_DIAG count=%u first=%u last=%u\n",
                  diagnostic_count, diagnostic_first, diagnostic_last);
    for (unsigned offset = 0; offset < diagnostic_count; offset += 64) {
      const unsigned take = diagnostic_count - offset < 64 ?
          diagnostic_count - offset : 64;
      Serial.printf("PW_STICK_DIAG_HEX offset=%u ", offset);
      for (unsigned i = 0; i < take; ++i)
        Serial.printf("%02x", diagnostic_bins[offset + i]);
      Serial.println();
    }
  }
  for (unsigned i = 0; i < burst_diagnostic_count; ++i) {
    const auto &entry = burst_diagnostics[i];
    Serial.printf("PW_STICK_BURST index=%u gates=%u decoded=%u credible=%u "
                  "bytes=%u starts=%u stops=%u wire=", i, entry.gates,
                  entry.decoded, entry.credible, entry.bytes, entry.starts,
                  entry.stops);
    for (unsigned byte = 0; byte < entry.bytes && byte < 16; ++byte)
      Serial.printf("%02x", entry.wire[byte]);
    Serial.println();
  }
  Serial.println("PW_STICK_STOP_COMPLETE");
  }
#endif
}

extern "C" u16 StickIrTicks(void) {
  return ticks_from_us(esp_timer_get_time());
}

#ifdef PW_STICK_BENCH_CONTROL
extern "C" void StickIrTraceChecksumFailure(const u8 *bytes, u8 length,
                                              u16 received, u16 computed) {
  if (!trace_enabled()) return;
  Serial.printf("PW_STICK_CHECKSUM_FAILURE length=%u received=%04x computed=%04x logical=",
                length, received, computed);
  for (unsigned i = 0; i < length; ++i) Serial.printf("%02x", bytes[i]);
  Serial.println();
  Serial.printf("PW_STICK_CHECKSUM_OPENING gates=%u first=%u count=%u bins=",
                checksum_opening_gates, checksum_opening_first,
                checksum_opening_count);
  for (unsigned i = 0; i < checksum_opening_count; ++i)
    Serial.printf("%02x", checksum_opening[i]);
  Serial.println();
}

extern "C" void StickIrTraceMalformedPage(const u8 *bytes, u8 length) {
  if (!trace_enabled()) return;
  Serial.printf("PW_STICK_MALFORMED_PAGE length=%u payload=", length);
  for (unsigned i = 0; i < length; ++i) Serial.printf("%02x", bytes[i]);
  Serial.println();
}
#endif

extern "C" void StickIrDelayTicks(u16 ticks) {
  const u16 start = StickIrTicks();
  while (u16(StickIrTicks() - start) < ticks) {}
}

extern "C" void StickIrSendFrame(const u8 *logical, u8 length) {
  __atomic_store_n(&tx_busy, true, __ATOMIC_RELEASE);
  if (!pw_stick::transmit_logical(logical, length))
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
  __atomic_store_n(&tx_busy, false, __ATOMIC_RELEASE);
}

extern "C" void StickIrSendByte(u8 logical) {
  StickIrSendFrame(&logical, 1);
}

extern "C" int StickIrTakeBurst(u8 *wire, u8 capacity, u8 *length,
                                 u16 *lastObservationTick) {
  if (!wire || !length || !lastObservationTick) return 0;
  const uint32_t written = __atomic_load_n(&event_write, __ATOMIC_ACQUIRE);
  const uint32_t read = __atomic_load_n(&event_read, __ATOMIC_RELAXED);
  if (read == written) return 0;
  const OpticalEvent event = events[read & (kEventSlots - 1)];
  __atomic_store_n(&event_read, read + 1, __ATOMIC_RELEASE);
  const uint32_t newest = __atomic_load_n(&produced, __ATOMIC_ACQUIRE);
  const uint32_t base = event.first >= 4 ? event.first - 4 : 0;
  const uint32_t end = event.last + 4;
  const uint32_t count = end - base + 1;
  if (newest - base > kRingSize || count > pw_stick::kMaxBurstGates) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return 0;
  }
  for (uint32_t i = 0; i < count; ++i)
    packet_bins[i] = ring[(base + i) & kRingMask];
#ifdef PW_STICK_BENCH_CONTROL
  if (trace_enabled()) {
    checksum_opening_count = count < sizeof(checksum_opening) ?
        count : sizeof(checksum_opening);
    memcpy(checksum_opening, packet_bins, checksum_opening_count);
    checksum_opening_first = event.first - base;
    checksum_opening_gates = count;
  }
  if (!diagnostic_count &&
      (count >= 1000 || burst_diagnostic_count == 2) &&
      count <= sizeof(diagnostic_bins)) {
    memcpy(diagnostic_bins, packet_bins, count);
    diagnostic_count = count;
    diagnostic_first = event.first - base;
    diagnostic_last = event.last - base;
  }
#endif
  pw_stick::WireBurst burst;
  const bool decoded = pw_stick::decode_wire_burst(packet_bins, count, burst,
                                                   kLowCutoff);
  const bool credible = decoded && burst.credible_uart();
#ifdef PW_STICK_BENCH_CONTROL
  if (!credible && count <= sizeof(diagnostic_bins)) {
    memcpy(diagnostic_bins, packet_bins, count);
    diagnostic_count = count;
    diagnostic_first = event.first - base;
    diagnostic_last = event.last - base;
  }
  {
    const unsigned slot = burst_diagnostic_count < 32 ?
        burst_diagnostic_count++ : 31;
    auto &entry = burst_diagnostics[slot];
    entry.gates = count;
    entry.decoded = unsigned(decoded);
    entry.credible = unsigned(credible);
    entry.bytes = burst.length;
    entry.starts = burst.starts;
    entry.stops = burst.stops;
    memcpy(entry.wire, burst.bytes,
           burst.length < sizeof(entry.wire) ? burst.length : sizeof(entry.wire));
  }
#endif
  if (!credible) {
    ++invalid_bursts;
    return 0;
  }
  if (burst.length > capacity) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return 0;
  }
  memcpy(wire, burst.bytes, burst.length);
  ++valid_bursts;
  *length = burst.length;
  *lastObservationTick = ticks_from_us(
      event.end_us - int64_t(event.end_gate - event.last) * 6250 / 2160);
  return 1;
}

extern "C" int StickIrFailed(void) {
  return __atomic_load_n(&failed, __ATOMIC_ACQUIRE);
}
