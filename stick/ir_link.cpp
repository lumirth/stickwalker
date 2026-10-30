#include "ir_link.h"
#include "stick_build.h"

#include "board_hal.h"
#include "diode_status.h"
#include "ir_receiver.h"
#include "sound_bridge.h"

#include <Arduino.h>
#include <driver/dedic_gpio.h>
#include <driver/gpio.h>
#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_pm.h>
#include <esp_private/systimer.h>
#include <esp_timer.h>
#include <hal/dedic_gpio_cpu_ll.h>
#include <hal/gpio_ll.h>
#include <hal/rmt_ll.h>
#include <hal/systimer_ll.h>
#include <soc/io_mux_reg.h>
#include <soc/rmt_struct.h>
#include <soc/soc.h>
#include <soc/soc_caps.h>
#include <soc/systimer_struct.h>

namespace {

constexpr uint32_t kGateHz = 345600;
constexpr uint32_t kRingSize = 32768;
constexpr uint32_t kRingMask = kRingSize - 1;
// Native admission has a fixed 97.7 ms wait plus one bounded 20.9 ms grace
// for a frame already arriving. Add our maximum 11.81 ms TX and arm overhead.
// This independent ceiling prevents indefinitely masked ticks if the session
// violates its ownership contract; noise cannot extend this interval.
constexpr uint32_t kReceiveLimit = kGateHz * 140 / 1000;
constexpr uint32_t kPassiveLimit = kGateHz / 10;
#if CONFIG_ESP_INT_WDT
static_assert(CONFIG_ESP_INT_WDT_TIMEOUT_MS > 150,
              "Receive turns require interrupt-watchdog margin");
#endif
constexpr unsigned kMaxBytes = 136;
constexpr unsigned kRmtHz = 80000000;
constexpr unsigned kBaud = 115200;
constexpr unsigned kPulseTicks = 130;
// On ESP32-S3, only the last TX channel supports DMA. The pinned IDF
// rmt_tx_register_to_group() enforces this allocation, rather than choosing
// an arbitrary channel. The register-level completion fence depends on it.
static_assert(SOC_RMT_GROUPS == 1 && SOC_RMT_TX_CANDIDATES_PER_GROUP == 4 &&
              SOC_RMT_SUPPORT_DMA, "The IR completion fence requires ESP32-S3");
constexpr unsigned kTxChannel = 3;
constexpr uint32_t kTxDone = RMT_LL_EVENT_TX_DONE(kTxChannel);

enum class Request : uint32_t { Park, Receive, Exit };
volatile Request request = Request::Park;
volatile bool worker_ready = false;
volatile bool worker_active = false;
volatile bool worker_done = false;
volatile bool failed = false;
volatile bool watch_tx_end = false;
volatile bool tx_boundary_ready = false;
volatile bool tx_driver_done = false;
volatile uint32_t produced = 0;
volatile uint32_t released = 0;
volatile uint32_t tx_receive_gate = 0;
uint32_t consumed = 0;
uint16_t consumed_tick = 0;
uint32_t consumed_fraction = 0;
uint32_t epoch_first = 0;
int64_t epoch_us = 0;
uint64_t epoch_tick_fraction = 0;
uint16_t epoch_tick = 0;
bool passive = false;
bool tx_enabled = false;
unsigned previous_cpu_mhz = 80;
TaskHandle_t worker = nullptr;
pw_stick::IrObservation *ring = nullptr;
struct GateHealth {
  uint32_t phase_late = 0;
  uint32_t already_low = 0;
  uint32_t timed_out = 0;
};
GateHealth gate_health;
pw_stick::IrReceiver receiver;
rmt_channel_handle_t tx_channel = nullptr;
rmt_encoder_handle_t tx_encoder = nullptr;
DRAM_ATTR rmt_symbol_word_t tx_symbols[kMaxBytes * 10];
#if CONFIG_PM_ENABLE
esp_pm_lock_handle_t cpu_lock = nullptr;
esp_pm_lock_handle_t sleep_lock = nullptr;
bool clocks_locked = false;
#endif

uint32_t valid_frames = 0;
uint32_t checksum_failures = 0;
uint32_t malformed_packets = 0;
uint32_t rejected_session = 0;
uint32_t resync_bytes = 0;
uint32_t deadline_misses = 0;
unsigned maximum_reply_us = 0;
unsigned maximum_roundtrip_us = 0;
int64_t peer_completed_us = 0;
int64_t candidate_peer_us = 0;
int64_t previous_reply_us = 0;
#ifdef PW_STICK_BENCH_CONTROL
bool bench_trace = false;
#endif

void IRAM_ATTR core1_tick(bool enabled) {
  systimer_ll_enable_alarm_int(&SYSTIMER, SYSTIMER_ALARM_OS_TICK_CORE1, enabled);
}

uint16_t ticks_from_us(int64_t us) {
  return uint16_t((uint64_t(us) * 32768) / 1000000);
}

void consume_from(uint32_t gate) {
  // 32768/345600 = 64/675. Preserve the fractional Timer W phase at
  // the epoch origin, including when the 16-bit native clock wraps. Wide
  // arithmetic occurs only at a turn boundary, never for every raw gate.
  const uint64_t elapsed = epoch_tick_fraction +
      uint64_t(gate - epoch_first) * 64000000;
  consumed = gate;
  consumed_tick = uint16_t(epoch_tick + elapsed / 675000000);
  consumed_fraction = uint32_t(elapsed % 675000000);
  __atomic_store_n(&released, gate, __ATOMIC_RELEASE);
}

int64_t observation_us(uint32_t gate) {
  return epoch_us + int64_t(gate - epoch_first) * 6250 / 2160;
}

bool IRAM_ATTR tx_done_callback(rmt_channel_handle_t,
                                const rmt_tx_done_event_data_t *, void *) {
  __atomic_store_n(&tx_driver_done, true, __ATOMIC_RELEASE);
  return false;
}

bool prepare_transmitter() {
  auto &power = StickBoardPower();
  const bool was_off = !power.getExtOutput();
  if (!power.setExtOutput(true)) return false;
  if (was_off) delay(20);
  if (tx_enabled) return true;
  if (!tx_channel) {
    rmt_tx_channel_config_t config = {};
    config.gpio_num = GPIO_NUM_46;
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = kRmtHz;
    config.mem_block_symbols = 1536;
    config.trans_queue_depth = 1;
    config.flags.with_dma = true;
    rmt_copy_encoder_config_t encoder_config = {};
    if (rmt_new_tx_channel(&config, &tx_channel) != ESP_OK) return false;
    if (rmt_new_copy_encoder(&encoder_config, &tx_encoder) != ESP_OK) {
      rmt_del_channel(tx_channel);
      tx_channel = nullptr;
      return false;
    }
    rmt_tx_event_callbacks_t callbacks = {};
    callbacks.on_trans_done = tx_done_callback;
    if (rmt_tx_register_event_callbacks(tx_channel, &callbacks, nullptr) != ESP_OK) {
      rmt_del_encoder(tx_encoder);
      rmt_del_channel(tx_channel);
      tx_encoder = nullptr;
      tx_channel = nullptr;
      return false;
    }
  }
  tx_enabled = rmt_enable(tx_channel) == ESP_OK;
  return tx_enabled;
}

// One owner schedules reset, integration, readout and the raw handoff. The
// integration interval is electrically passive, but it is not wasted CPU
// time: bounded publication and TX-completion work happen there. Nothing
// decides whether to retain a measurement based on its optical amplitude.
uint32_t IRAM_ATTR __attribute__((noinline, noclone)) sample_receive_turn(
    uint32_t first, uint32_t next, uint32_t input_mask, uint32_t limit) {
  const uint32_t wall_deadline = next + limit * 6250 / 9;
  uint32_t fraction = 0;
  uint32_t gate = first;
  gpio_ll_output_enable(&GPIO, 5);
  for (; gate - first < limit &&
         __atomic_load_n(&request, __ATOMIC_ACQUIRE) == Request::Receive;
       ++gate) {
    const uint32_t origin = next;
    next += 694;
    fraction += 4;
    if (fraction >= 9) { fraction -= 9; ++next; }

    bool late = false;
    const uint32_t release = origin + 65;
    if (int32_t(esp_cpu_get_cycle_count() - release) >= 0) late = true;
    while (int32_t(esp_cpu_get_cycle_count() - release) < 0) {}
    gpio_ll_output_disable(&GPIO, 5);

    // Gate-count limits alone do not bound elapsed time when a measurement
    // runs late. Check the independent wall deadline once per gate, during
    // integration, and never silently start a new receive window.
    if (int32_t(esp_cpu_get_cycle_count() - wall_deadline) >= 0) {
      if (!passive) __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
      break;
    }

    // This gate is integrating and has not produced an observation yet.
    // Only earlier gates may be published. Reserve the complete next block
    // before writing any of it; the producer never overwrites unread data.
    if ((gate & 31) == 0) {
      __atomic_store_n(&produced, gate, __ATOMIC_RELEASE);
      if (!passive && gate - __atomic_load_n(&released, __ATOMIC_ACQUIRE) >
                          kRingSize - 32) {
        __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
        break;
      }
    }
    if (__atomic_load_n(&watch_tx_end, __ATOMIC_RELAXED) &&
        (rmt_ll_tx_get_interrupt_status_raw(&RMT, kTxChannel) & kTxDone)) {
      // The two retained completed gates are after our last possible SIR
      // pulse. The gate currently integrating can contain the peer's first
      // pulse, even if it begins immediately after our physical TX end.
      __atomic_store_n(&tx_receive_gate,
                        gate - first >= 2 ? gate - 2 : first, __ATOMIC_RELAXED);
      __atomic_store_n(&produced, gate, __ATOMIC_RELEASE);
      __atomic_store_n(&tx_boundary_ready, true, __ATOMIC_RELEASE);
      __atomic_store_n(&watch_tx_end, false, __ATOMIC_RELAXED);
      rmt_ll_enable_interrupt(&RMT, kTxDone, true);
    }

    const uint32_t readout = origin + 450;
    if (int32_t(esp_cpu_get_cycle_count() - readout) >= 0) late = true;
    while (int32_t(esp_cpu_get_cycle_count() - readout) < 0) {}
    pw_stick::IrObservation observation = 0;
    if (!(dedic_gpio_cpu_ll_read_in() & input_mask)) {
      ++gate_health.already_low;
    } else {
      REG_SET_BIT(IO_MUX_GPIO5_REG, FUN_PD);
      const uint32_t ramp = esp_cpu_get_cycle_count();
      const uint32_t read_limit = next - 70;
      uint32_t high_until = 0;
      bool crossed = false;
      if (int32_t(ramp - read_limit) >= 0) late = true;
      for (;;) {
        const uint32_t probe_start = esp_cpu_get_cycle_count();
        if (int32_t(probe_start - read_limit) >= 0) break;
        if (!(dedic_gpio_cpu_ll_read_in() & input_mask)) {
          crossed = true;
          break;
        }
        high_until = probe_start - ramp;
      }
      if (crossed) {
        const uint32_t elapsed = esp_cpu_get_cycle_count() - ramp;
        observation = elapsed <= pw_stick::kObservationCyclesMask
            ? pw_stick::IrObservation(elapsed) : pw_stick::kObservationInvalid;
      } else {
        observation = pw_stick::IrObservation(pw_stick::kObservationCensored | high_until);
        ++gate_health.timed_out;
      }
      REG_CLR_BIT(IO_MUX_GPIO5_REG, FUN_PD);
    }
    gpio_ll_output_enable(&GPIO, 5);
    if (late || int32_t(esp_cpu_get_cycle_count() - next) >= 0) {
      observation = pw_stick::kObservationInvalid;
      ++gate_health.phase_late;
    }
    // Raw captures can span service turns. An offline decoder must not join
    // observations across the unobserved interval between those turns.
    if (gate == first) observation |= pw_stick::kObservationInvalid;
    ring[gate & kRingMask] = observation;
    // The next iteration uses the reset tail for its bookkeeping; its release
    // and readout still occur at their absolute scheduled cycle deadlines.
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
  } else {
    __atomic_store_n(&worker_ready, true, __ATOMIC_RELEASE);
    uint32_t gate = 0;
    while (__atomic_load_n(&request, __ATOMIC_ACQUIRE) != Request::Exit) {
      if (__atomic_load_n(&request, __ATOMIC_ACQUIRE) != Request::Receive) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        continue;
      }
      // Correlate the independent clocks while interrupts cannot separate
      // their reads. Publish the time origin before publishing active.
      core1_tick(false);
      portDISABLE_INTERRUPTS();
      const int64_t now = esp_timer_get_time();
      const uint32_t origin = esp_cpu_get_cycle_count() + 4800;
      epoch_first = gate;
      epoch_us = now + 20;
      const uint64_t watch = uint64_t(epoch_us) * 32768;
      epoch_tick = uint16_t(watch / 1000000);
      epoch_tick_fraction = (watch % 1000000) * 675;
      __atomic_store_n(&worker_active, true, __ATOMIC_RELEASE);
      const uint32_t limit = passive ? kPassiveLimit : kReceiveLimit;
      const uint32_t end = sample_receive_turn(gate, origin, input_mask, limit);
      portENABLE_INTERRUPTS();
      core1_tick(true);
      if (!passive && end - gate >= limit)
        __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
      gate = end;
      // An explicit deadline never silently opens another receive window.
      if (__atomic_load_n(&request, __ATOMIC_ACQUIRE) == Request::Receive)
        __atomic_store_n(&request, Request::Park, __ATOMIC_RELEASE);
      __atomic_store_n(&worker_active, false, __ATOMIC_RELEASE);
    }
  }
  core1_tick(true);
  if (bundle) dedic_gpio_del_bundle(bundle);
  __atomic_store_n(&worker_done, true, __ATOMIC_RELEASE);
  vTaskSuspend(nullptr);
}

bool receive() {
  if (__atomic_load_n(&failed, __ATOMIC_ACQUIRE) || !worker) return false;
  if (__atomic_load_n(&worker_active, __ATOMIC_ACQUIRE)) return true;
  // The session has finished with the previous receive turn's observations.
  __atomic_store_n(&released, __atomic_load_n(&produced, __ATOMIC_ACQUIRE),
                    __ATOMIC_RELEASE);
  __atomic_store_n(&request, Request::Receive, __ATOMIC_RELEASE);
  xTaskNotifyGive(worker);
  const int64_t deadline = esp_timer_get_time() + 10000;
  while (!__atomic_load_n(&worker_active, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE) &&
         esp_timer_get_time() < deadline) delayMicroseconds(5);
  if (!__atomic_load_n(&worker_active, __ATOMIC_ACQUIRE)) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return false;
  }
  receiver.reset();
  consume_from(epoch_first);
  // Acquisition is armed only after it has actually published observations.
  // These samples do not tune any decoder or electrical parameter.
  while (__atomic_load_n(&produced, __ATOMIC_ACQUIRE) == consumed &&
         __atomic_load_n(&worker_active, __ATOMIC_ACQUIRE) &&
         esp_timer_get_time() < deadline) delayMicroseconds(5);
  if (__atomic_load_n(&produced, __ATOMIC_ACQUIRE) == consumed) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return false;
  }
  return true;
}

void print_session() {
  const auto &attempt = *StickLastConnection();
  if (!attempt.started) { Serial.println("PW_STICK_LINK none"); return; }
  Serial.printf("PW_STICK_LINK build=%s result=%u preflight=%u battery_mv=%u "
                "checked=%u safe=%u packets=%lu command=%02x phase=%u bulk=%u "
                "framing=%lu erased=%lu checksum=%lu malformed=%lu session=%lu late=%lu low=%lu "
                "resync=%lu reply_us=%u roundtrip_us=%u deadline_misses=%lu\n",
      PW_STICK_BUILD_ID, attempt.result, attempt.preflight, attempt.battery_mv,
      unsigned(attempt.checked), unsigned(attempt.receive_safe),
      (unsigned long)attempt.packets, attempt.command, attempt.phase, attempt.bulk_phase,
      (unsigned long)receiver.stats().framing_errors,
      (unsigned long)receiver.stats().erasures,
      (unsigned long)checksum_failures, (unsigned long)malformed_packets,
      (unsigned long)rejected_session, (unsigned long)gate_health.phase_late,
      (unsigned long)gate_health.already_low, (unsigned long)resync_bytes,
      maximum_reply_us, maximum_roundtrip_us, (unsigned long)deadline_misses);
}

void print_capture() {
  const uint32_t end = __atomic_load_n(&produced, __ATOMIC_ACQUIRE);
  const uint32_t count = end < kRingSize ? end : kRingSize;
  Serial.printf("PW_STICK_RX_CAPTURE gates=%lu format=u16 first_gate=%lu "
                "end_gate=%lu epoch_first=%lu epoch_us=%lld\n",
      (unsigned long)count, (unsigned long)(end - count), (unsigned long)end,
      (unsigned long)epoch_first, static_cast<long long>(epoch_us));
  for (uint32_t offset = 0; offset < count; ++offset) {
    Serial.printf("%04x", ring[(end - count + offset) & kRingMask]);
    if ((offset & 31) == 31) Serial.println();
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
  if (worker) { __atomic_store_n(&failed, true, __ATOMIC_RELEASE); return; }
  __atomic_store_n(&failed, false, __ATOMIC_RELEASE);
  valid_frames = checksum_failures = malformed_packets = rejected_session = 0;
  resync_bytes = deadline_misses = maximum_reply_us = maximum_roundtrip_us = 0;
  peer_completed_us = candidate_peer_us = previous_reply_us = 0;
  receiver = pw_stick::IrReceiver{};
  gate_health = {};
  __atomic_store_n(&produced, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&released, 0, __ATOMIC_RELEASE);
  epoch_first = 0;
  epoch_us = 0;
  epoch_tick = 0;
  epoch_tick_fraction = 0;
  consumed = 0;
  consumed_tick = 0;
  consumed_fraction = 0;
  previous_cpu_mhz = getCpuFrequencyMhz();
  if (!setCpuFrequencyMhz(240)) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return;
  }
#if CONFIG_PM_ENABLE
  if ((!cpu_lock && esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ir-cpu", &cpu_lock) != ESP_OK) ||
      (!sleep_lock && esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ir-awake", &sleep_lock) != ESP_OK) ||
      esp_pm_lock_acquire(cpu_lock) != ESP_OK) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return;
  }
  if (esp_pm_lock_acquire(sleep_lock) != ESP_OK) {
    esp_pm_lock_release(cpu_lock);
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return;
  }
  clocks_locked = true;
#endif
  if (!StickDiodePreflight() || !prepare_transmitter())
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
}

extern "C" void StickIrPassiveDiagnostic(int enabled) { passive = enabled != 0; }

extern "C" void StickIrStart(void) {
  if (worker || __atomic_load_n(&failed, __ATOMIC_ACQUIRE)) return;
  StickSoundQuiesceForIr();
  if (!ring) ring = static_cast<pw_stick::IrObservation *>(heap_caps_malloc(
      kRingSize * sizeof(*ring), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!ring || getCpuFrequencyMhz() != 240) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return;
  }
  StickIrInitPins();
  gpio_ll_set_level(&GPIO, 5, 1);
  receiver.reset();
  consumed = 0;
  __atomic_store_n(&produced, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&released, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&request, Request::Park, __ATOMIC_RELEASE);
  __atomic_store_n(&worker_ready, false, __ATOMIC_RELEASE);
  __atomic_store_n(&worker_active, false, __ATOMIC_RELEASE);
  __atomic_store_n(&worker_done, false, __ATOMIC_RELEASE);
  __atomic_store_n(&watch_tx_end, false, __ATOMIC_RELEASE);
  if (xTaskCreatePinnedToCore(sampler, "pw-ir", 4096, nullptr, 18, &worker, 0) != pdPASS) {
    worker = nullptr;
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return;
  }
  const int64_t deadline = esp_timer_get_time() + 100000;
  while (!__atomic_load_n(&worker_ready, __ATOMIC_ACQUIRE) &&
         !__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE) &&
         esp_timer_get_time() < deadline) delay(1);
  if (!__atomic_load_n(&worker_ready, __ATOMIC_ACQUIRE)) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return;
  }
  receive();
}

extern "C" int StickIrBeginService(void) {
  if (!worker) return !__atomic_load_n(&failed, __ATOMIC_ACQUIRE);
  __atomic_store_n(&request, Request::Park, __ATOMIC_RELEASE);
  xTaskNotifyGive(worker);
  const int64_t deadline = esp_timer_get_time() + 10000;
  while (__atomic_load_n(&worker_active, __ATOMIC_ACQUIRE) &&
         esp_timer_get_time() < deadline) {}
  if (__atomic_load_n(&worker_active, __ATOMIC_ACQUIRE))
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
  return !__atomic_load_n(&failed, __ATOMIC_ACQUIRE);
}

extern "C" void StickIrResumeReception(void) { receive(); }

extern "C" int StickIrStorageAllowed(void) {
  return !worker ||
      (!__atomic_load_n(&worker_active, __ATOMIC_ACQUIRE) &&
       __atomic_load_n(&request, __ATOMIC_ACQUIRE) == Request::Park);
}

extern "C" int StickIrReceivingFrame(void) { return receiver.active(); }

extern "C" void StickIrStop(void) {
  if (worker) {
    __atomic_store_n(&request, Request::Exit, __ATOMIC_RELEASE);
    xTaskNotifyGive(worker);
    const int64_t deadline = esp_timer_get_time() + 100000;
    while ((!__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE) ||
            eTaskGetState(worker) != eSuspended) &&
           esp_timer_get_time() < deadline) delayMicroseconds(20);
    if (!__atomic_load_n(&worker_done, __ATOMIC_ACQUIRE) ||
        eTaskGetState(worker) != eSuspended) {
      __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
      return;
    }
    vTaskDelete(worker);
    worker = nullptr;
  }
  if (tx_enabled) {
    if (rmt_disable(tx_channel) != ESP_OK)
      __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    tx_enabled = false;
  }
  StickIrInitPins();
  StickBoardPower().setExtOutput(false);
#if CONFIG_PM_ENABLE
  if (clocks_locked) {
    esp_pm_lock_release(sleep_lock);
    esp_pm_lock_release(cpu_lock);
    clocks_locked = false;
  }
#endif
  setCpuFrequencyMhz(previous_cpu_mhz);
#ifdef PW_STICK_BENCH_CONTROL
  if (bench_trace) {
    Serial.printf("PW_STICK_IR_STATS samples=%lu low=%lu timeout=%lu late=%lu "
                  "valid=%lu failed=%u\n",
        (unsigned long)__atomic_load_n(&produced, __ATOMIC_ACQUIRE),
        (unsigned long)gate_health.already_low, (unsigned long)gate_health.timed_out,
        (unsigned long)gate_health.phase_late, (unsigned long)valid_frames,
        unsigned(__atomic_load_n(&failed, __ATOMIC_ACQUIRE)));
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
  if (!logical || !length || length > kMaxBytes || !tx_enabled ||
      !StickIrBeginService()) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return;
  }
  unsigned used = 0, fraction = 0;
  for (unsigned index = 0; index < length; ++index) {
    const uint8_t wire = logical[index] ^ 0xaa;
    for (unsigned bit = 0; bit < 10; ++bit) {
      const bool mark = bit == 9 || (bit && (wire & (1u << (bit - 1))));
      unsigned cell = kRmtHz / kBaud;
      fraction += kRmtHz % kBaud;
      if (fraction >= kBaud) { fraction -= kBaud; ++cell; }
      auto &symbol = tx_symbols[used++];
      symbol.val = 0;
      symbol.level0 = mark ? 0 : 1;
      symbol.duration0 = kPulseTicks;
      symbol.level1 = 0;
      symbol.duration1 = cell - kPulseTicks;
    }
  }
  if (!receive()) return;
  __atomic_store_n(&tx_driver_done, false, __ATOMIC_RELEASE);
  __atomic_store_n(&tx_boundary_ready, false, __ATOMIC_RELEASE);
  rmt_transmit_config_t config = {};
  config.flags.queue_nonblocking = true;
  config.flags.eot_level = 0;
  // The driver synchronously encodes this complete transaction and starts
  // the hardware before returning. Prevent its ISR from clearing TX_END
  // between that start and handing the sticky completion flag to core 0.
  portDISABLE_INTERRUPTS();
  rmt_ll_clear_interrupt_status(&RMT, kTxDone);
  const esp_err_t result = rmt_transmit(tx_channel, tx_encoder, tx_symbols,
                                       used * sizeof(*tx_symbols), &config);
  if (result == ESP_OK) {
    rmt_ll_enable_interrupt(&RMT, kTxDone, false);
    __atomic_store_n(&watch_tx_end, true, __ATOMIC_RELEASE);
  }
  portENABLE_INTERRUPTS();
  const int64_t deadline = esp_timer_get_time() + 20000;
  while (result == ESP_OK &&
         (!__atomic_load_n(&tx_boundary_ready, __ATOMIC_ACQUIRE) ||
          !__atomic_load_n(&tx_driver_done, __ATOMIC_ACQUIRE)) &&
         !__atomic_load_n(&failed, __ATOMIC_ACQUIRE) &&
         esp_timer_get_time() < deadline) {}
  if (result != ESP_OK || !__atomic_load_n(&tx_boundary_ready, __ATOMIC_ACQUIRE) ||
      !__atomic_load_n(&tx_driver_done, __ATOMIC_ACQUIRE) ||
      rmt_tx_wait_all_done(tx_channel, 0) != ESP_OK) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return;
  }
  // Read the raw samples retained across physical completion, regardless of
  // ISR or task wakeup latency. There is no post-TX quiet-time discard.
  receiver.reset();
  consume_from(__atomic_load_n(&tx_receive_gate, __ATOMIC_ACQUIRE));
  const int64_t completed = observation_us(consumed + 2);
  if (peer_completed_us) {
    const unsigned elapsed = unsigned(completed - peer_completed_us);
    if (elapsed > maximum_reply_us) maximum_reply_us = elapsed;
    if (previous_reply_us) {
      const unsigned interval = unsigned(completed - previous_reply_us);
      if (interval > maximum_roundtrip_us) maximum_roundtrip_us = interval;
      if (interval >= 100000) ++deadline_misses;
    }
    previous_reply_us = completed;
    peer_completed_us = 0;
  }
}

extern "C" void StickIrSendByte(u8 logical) { StickIrSendFrame(&logical, 1); }

extern "C" int StickIrTakeBurst(u8 *wire, u8 capacity, u8 *length,
                                 u16 *lastObservationTick) {
  if (!wire || !length || !lastObservationTick || !ring || passive ||
      __atomic_load_n(&failed, __ATOMIC_ACQUIRE)) return 0;
  const uint32_t end = __atomic_load_n(&produced, __ATOMIC_ACQUIRE);
  if (end - consumed > kRingSize) {
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
    return 0;
  }
  // Bound one native poll. Noise can use CPU time, but cannot park acquisition
  // or postpone the native timeout checks indefinitely.
  const uint32_t first = consumed;
  const uint32_t limit = end - consumed > 1024 ? consumed + 1024 : end;
  pw_stick::IrFrame frame;
  while (consumed != limit) {
    const uint32_t gate = consumed++;
    const uint16_t tick = consumed_tick;
    consumed_fraction += 64000000;
    if (consumed_fraction >= 675000000) {
      consumed_fraction -= 675000000;
      ++consumed_tick;
    }
    if (!receiver.push(ring[gate & kRingMask], gate, tick, frame))
      continue;
    if (frame.length > capacity ||
        __atomic_load_n(&failed, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&produced, __ATOMIC_ACQUIRE) - first > kRingSize) {
      __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
      return 0;
    }
    memcpy(wire, frame.bytes, frame.length);
    *length = frame.length;
    *lastObservationTick = frame.end_tick;
    candidate_peer_us = observation_us(frame.end_gate);
    resync_bytes += frame.discarded_prefix;
    ++valid_frames;
    __atomic_store_n(&released, consumed, __ATOMIC_RELEASE);
    return 1;
  }
  if (__atomic_load_n(&produced, __ATOMIC_ACQUIRE) - first > kRingSize)
    __atomic_store_n(&failed, true, __ATOMIC_RELEASE);
  __atomic_store_n(&released, consumed, __ATOMIC_RELEASE);
  return 0;
}

extern "C" int StickIrFailed(void) {
  return __atomic_load_n(&failed, __ATOMIC_ACQUIRE);
}

extern "C" void StickIrAcceptPacket(unsigned command, unsigned phase, unsigned bulk_phase) {
  peer_completed_us = candidate_peer_us;
  StickConnectionAcceptPacket(command, phase, bulk_phase);
}

extern "C" void StickIrResynchronized(unsigned bytes) { resync_bytes += bytes; }

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
  while (Serial.available()) {
    const int command = Serial.read();
    if (command == '?') {
      Serial.printf("PW_STICK_DIAGNOSTICS version=1 build=%s\n", PW_STICK_BUILD_ID);
      print_session();
    }
    if (command == 'r') print_capture();
  }
#endif
}

#ifdef PW_STICK_BENCH_CONTROL
extern "C" void StickIrBenchTrace(int enabled) { bench_trace = enabled != 0; }
extern "C" int StickIrBenchTraceEnabled(void) { return bench_trace; }
#endif
