#include "infrared.h"
#include "../board.h"
#include "../display.h"
#include "diode.h"
#include "infrared_decoder.h"
#include "infrared_capture.h"
#include "infrared_calibration.h"

#include <string.h>
#include "driver/dedic_gpio.h"
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_attr.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/dedic_gpio_cpu_ll.h"
#include "sdkconfig.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/soc.h"

#if CONFIG_FREERTOS_UNICORE
#error "The infrared sampler needs the second ESP32-S3 core"
#endif
#if CONFIG_ESP_INT_WDT_CHECK_CPU1 || CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
#error "CPU1 watchdog checks must be disabled while the infrared sampler owns it"
#endif

#define RECEIVE_PIN GPIO_NUM_5
#define TRANSMIT_PIN GPIO_NUM_46
#define RECEIVE_PIN_MASK (1u << 5)
#define OBSERVATION_CAPACITY INFRARED_CAPTURE_RING_CAPACITY
#define PUBLISH_INTERVAL INFRARED_CAPTURE_BATCH
#define DMA_SYMBOL_CAPACITY 1536u
#define TRANSMIT_RECOVERY_CYCLES 480u
#define MODE_CHANGE_TIMEOUT_US 25000u
// Cover quiet ripple over several milliseconds, not just one locally quiet
// portion. Short windows admitted false bytes on the physical Stick.
#define NOISE_CALIBRATION_SAMPLES 4096u
#define NOISE_CALIBRATION_TIMEOUT_US 12000u

typedef enum {
    RECEIVE_OFF,
    RECEIVE_LISTEN,
    RECEIVE_TRANSMIT
} ReceiveMode;

_Static_assert(RECEIVE_LISTEN == INFRARED_CAPTURE_LISTEN_MODE, "Assembly listen mode");
_Static_assert(INFRARED_ERROR_ACQUISITION_GAP == (1u << 2), "Assembly timeout flag");

typedef struct {
    volatile uint32_t requested_mode;
    volatile uint32_t actual_mode;
    volatile uint32_t initialization_failed;
    volatile uint32_t write_count;
    volatile uint32_t read_count;
    volatile uint32_t last_transmit_pulse;
    volatile uint32_t transmit_started;
    volatile uint32_t receive_turn;  // Published by CPU0 after draining the ring.
    volatile uint32_t overflow_turn; // Published by CPU1 for the batch it dropped.
    uint32_t floating_mux;
    InfraredCalibration calibration;
    uint32_t epoch_cycle;
    uint64_t epoch_us;
    bool epoch_valid;
    _Alignas(8) InfraredObservation observations[OBSERVATION_CAPACITY];
} Receiver;

static DRAM_ATTR Receiver receiver;
static DRAM_ATTR InfraredCaptureStream capture_stream = {
    .write_count = &receiver.write_count,
    .read_count = &receiver.read_count,
    .requested_mode = &receiver.requested_mode,
    .observations = receiver.observations
};
static DRAM_ATTR InfraredObservation overflow_observations[PUBLISH_INTERVAL];
static DRAM_ATTR uint32_t transmit_symbols[INFRARED_MAX_FRAME * 10];
static DRAM_ATTR StaticTask_t worker_control;
static DRAM_ATTR StackType_t worker_stack[4096 / sizeof(StackType_t)];
static TaskHandle_t worker_task;
static rmt_channel_handle_t transmit_channel;
static rmt_encoder_handle_t transmit_encoder;
static bool transmitter_enabled;
static volatile uint64_t transmit_ended_us;
static bool opened;
static InfraredDecoder decoder;
static uint32_t consumed_cycle;
static uint64_t elapsed_cycles;
static uint64_t epoch_us;
static uint64_t last_event_us;

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t cpu_clock_lock;
static esp_pm_lock_handle_t peripheral_clock_lock;
static esp_pm_lock_handle_t awake_lock;
#endif

static inline __attribute__((always_inline)) uint32_t cycle_count(void)
{
    uint32_t cycles;
    __asm__ volatile("rsr.ccount %0" : "=a"(cycles));
    return cycles;
}

static inline __attribute__((always_inline)) uint32_t acquire_word(volatile uint32_t *word)
{
    return __atomic_load_n(word, __ATOMIC_ACQUIRE);
}

static inline __attribute__((always_inline)) void publish_word(volatile uint32_t *word,
                                                              uint32_t value)
{
    __atomic_store_n(word, value, __ATOMIC_RELEASE);
}

static inline __attribute__((always_inline)) void charge_node(void)
{
    REG_WRITE(GPIO_ENABLE_W1TS_REG, RECEIVE_PIN_MASK);
    REG_WRITE(IO_MUX_GPIO5_REG, receiver.floating_mux);
    __asm__ volatile("memw");
}

static bool wait_for_mode(ReceiveMode mode, uint32_t timeout_us)
{
    uint32_t started = cycle_count();
    uint32_t timeout_cycles = timeout_us * (INFRARED_CPU_HZ / 1000000u);
    while (acquire_word(&receiver.actual_mode) != (uint32_t)mode) {
        if (acquire_word(&receiver.initialization_failed) ||
            cycle_count() - started >= timeout_cycles) {
            return false;
        }
        taskYIELD();
    }
    return true;
}

static inline __attribute__((always_inline)) void set_capture_window(InfraredCaptureSettings *capture,
                                                                    uint32_t floating_blocks)
{
    const uint32_t reads_per_block = INFRARED_POLL_BLOCK_CYCLES / INFRARED_POLL_PAIR_CYCLES;
    capture->floating_reads = floating_blocks ? floating_blocks * reads_per_block - 1u : 0;
    capture->pulled_reads = (INFRARED_MEASUREMENT_BLOCKS - floating_blocks) * reads_per_block - 3u;
}

static void IRAM_ATTR __attribute__((noinline)) capture_loop(void)
{
    InfraredCalibration calibration = receiver.calibration;
    InfraredCaptureSettings capture = {
        .floating_mux = receiver.floating_mux,
        .previous_release = 0
    };
    set_capture_window(&capture, calibration.floating_blocks);
    uint32_t write_count = receiver.write_count;
    uint8_t next_flags = INFRARED_OBSERVATION_REARM;

    charge_node();
    uint32_t charged_at = cycle_count();
    while (cycle_count() - charged_at < INFRARED_RECHARGE_CYCLES) {
    }

    for (;;) {
        ReceiveMode requested = (ReceiveMode)acquire_word(&receiver.requested_mode);
        if (requested == RECEIVE_OFF) {
            break;
        }
        if (requested == RECEIVE_TRANSMIT) {
            charge_node();
            publish_word(&receiver.write_count, write_count);
            publish_word(&receiver.actual_mode, RECEIVE_TRANSMIT);

            uint32_t waiting_since = cycle_count();
            while ((dedic_gpio_cpu_ll_read_in() & 2u) == 0) {
                if (acquire_word(&receiver.requested_mode) != RECEIVE_TRANSMIT) {
                    break;
                }
                if (cycle_count() - waiting_since >= 2400000u) {
                    publish_word(&receiver.transmit_started, 2);
                    publish_word(&receiver.requested_mode, RECEIVE_LISTEN);
                    break;
                }
            }

            if (acquire_word(&receiver.requested_mode) == RECEIVE_TRANSMIT) {
                uint32_t first_pulse = cycle_count();
                uint32_t rearm_after = receiver.last_transmit_pulse + TRANSMIT_RECOVERY_CYCLES;
                publish_word(&receiver.transmit_started, 1);
                while (cycle_count() - first_pulse < rearm_after) {
                    if (acquire_word(&receiver.requested_mode) != RECEIVE_TRANSMIT) {
                        break;
                    }
                }
                if (acquire_word(&receiver.requested_mode) == RECEIVE_TRANSMIT) {
                    publish_word(&receiver.requested_mode, RECEIVE_LISTEN);
                }
            }

            capture.previous_release = 0;
            next_flags |= INFRARED_OBSERVATION_REARM;
            publish_word(&receiver.actual_mode, RECEIVE_LISTEN);
            continue;
        }

        uint32_t turn = acquire_word(&receiver.receive_turn);
        bool ring_has_room = write_count - acquire_word(&receiver.read_count) <=
                             OBSERVATION_CAPACITY - PUBLISH_INTERVAL;
        InfraredObservation *records = ring_has_room
            ? &receiver.observations[write_count & (OBSERVATION_CAPACITY - 1u)]
            : overflow_observations;

        if (calibration.ready && !next_flags && ring_has_room) {
            capture.stream = &capture_stream;
            infrared_capture_batch(records, &capture);
            capture.stream = NULL;
            write_count = acquire_word(&receiver.write_count);
            continue;
        }

        infrared_capture_batch(records, &capture);
        records[0].flags |= next_flags;
        next_flags = 0;
        if (ring_has_room) {
            write_count += PUBLISH_INTERVAL;
            publish_word(&receiver.write_count, write_count);
        } else {
            publish_word(&receiver.overflow_turn, turn);
            next_flags = INFRARED_OBSERVATION_REARM;
        }

        if (!calibration.ready) {
            uint32_t longest = 0;
            for (unsigned index = 0; index < PUBLISH_INTERVAL; index++) {
                if (records[index].duration > longest) {
                    longest = records[index].duration;
                }
            }
            uint32_t previous_blocks = calibration.floating_blocks;
            infrared_calibration_observe(&calibration, longest);
            if (calibration.floating_blocks != previous_blocks) {
                set_capture_window(&capture, calibration.floating_blocks);
                next_flags |= INFRARED_OBSERVATION_RECONFIGURE;
            }
        }
    }

    publish_word(&receiver.write_count, write_count);
    receiver.calibration = calibration;
    charge_node();
}

static void capture_worker(void *unused)
{
    (void)unused;
    const int pins[] = { RECEIVE_PIN, TRANSMIT_PIN };
    dedic_gpio_bundle_config_t configuration = {
        .gpio_array = pins,
        .array_size = 2,
        .flags = { .in_en = 1 }
    };
    dedic_gpio_bundle_handle_t inputs;
    if (dedic_gpio_new_bundle(&configuration, &inputs) != ESP_OK) {
        publish_word(&receiver.initialization_failed, 1);
        vTaskDelete(NULL);
        return;
    }

    uint32_t input_offset;
    dedic_gpio_get_in_offset(inputs, &input_offset);
    if (input_offset != 0) {
        dedic_gpio_del_bundle(inputs);
        publish_word(&receiver.initialization_failed, 1);
        vTaskDelete(NULL);
        return;
    }

    portMUX_TYPE interrupt_lock = portMUX_INITIALIZER_UNLOCKED;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (acquire_word(&receiver.requested_mode) != RECEIVE_OFF) {
            if (!receiver.epoch_valid) {
                receiver.epoch_us = esp_timer_get_time();
                receiver.epoch_cycle = cycle_count();
                receiver.epoch_valid = true;
            }
            portENTER_CRITICAL(&interrupt_lock);
            publish_word(&receiver.actual_mode, RECEIVE_LISTEN);
            capture_loop();
            portEXIT_CRITICAL(&interrupt_lock);
        }
        publish_word(&receiver.actual_mode, RECEIVE_OFF);
    }
}

static bool IRAM_ATTR transmit_done(rmt_channel_handle_t channel,
                                    const rmt_tx_done_event_data_t *event, void *context)
{
    (void)channel;
    (void)event;
    (void)context;
    transmit_ended_us = esp_timer_get_time();
    return false;
}

static bool initialize_transmitter(void)
{
    if (transmit_channel) {
        return true;
    }
    rmt_tx_channel_config_t channel_configuration = {
        .gpio_num = TRANSMIT_PIN,
        .clk_src = RMT_CLK_SRC_APB,
        .resolution_hz = 80000000,
        .mem_block_symbols = DMA_SYMBOL_CAPACITY,
        .trans_queue_depth = 1,
        .intr_priority = 1,
        .flags = { .with_dma = 1, .io_loop_back = 1 }
    };
    if (rmt_new_tx_channel(&channel_configuration, &transmit_channel) != ESP_OK) {
        return false;
    }
    rmt_copy_encoder_config_t encoder_configuration = {};
    if (rmt_new_copy_encoder(&encoder_configuration, &transmit_encoder) != ESP_OK) {
        rmt_del_channel(transmit_channel);
        transmit_channel = NULL;
        return false;
    }
    const rmt_tx_event_callbacks_t callbacks = {.on_trans_done = transmit_done};
    if (rmt_tx_register_event_callbacks(transmit_channel, &callbacks, NULL) == ESP_OK) return true;
    rmt_del_channel(transmit_channel);
    rmt_del_encoder(transmit_encoder);
    transmit_channel = NULL;
    transmit_encoder = NULL;
    return false;
}

static void release_receive_pin(void)
{
    gpio_set_direction(RECEIVE_PIN, GPIO_MODE_DISABLE);
    gpio_set_pull_mode(RECEIVE_PIN, GPIO_FLOATING);
}

static void rearm_receive_turn(uint32_t consumed)
{
    publish_word(&receiver.read_count, consumed);
    publish_word(&receiver.receive_turn, receiver.receive_turn + 1u);
    infrared_decoder_rearm(&decoder);
}

static void measure_receiver_noise(void)
{
    const int64_t deadline = esp_timer_get_time() + NOISE_CALIBRATION_TIMEOUT_US;
    unsigned clean_start = 0;
    unsigned scanned = 0;
    uint32_t published;
    do {
        published = acquire_word(&receiver.write_count);
        // No records can be overwritten until read_count advances. Measure
        // only after the last capture-window change, not a fixed skip count.
        for (; scanned < published; ++scanned) {
            if (receiver.observations[scanned].flags) clean_start = scanned + 1;
        }
        if (published - clean_start >= NOISE_CALIBRATION_SAMPLES) break;
    } while (esp_timer_get_time() < deadline);
    if (published > clean_start) {
        infrared_decoder_measure_noise(&decoder, receiver.observations + clean_start,
                                       published - clean_start);
    }
    unsigned position = 0;
    while (position < published) {
        unsigned consumed;
        InfraredDecodedByte ignored;
        infrared_decoder_consume(&decoder, receiver.observations + position,
                                 published - position, &consumed, &ignored);
        position += consumed;
    }
    if (published) {
        uint32_t cycle = receiver.observations[published - 1].end_cycle;
        elapsed_cycles += (uint32_t)(cycle - consumed_cycle);
        consumed_cycle = cycle;
    }
    // Keep the measured background, but start the first receive turn cleanly.
    rearm_receive_turn(published);
}

bool infrared_open(void)
{
    if (opened) {
        return true;
    }
    if (acquire_word(&receiver.initialization_failed)) {
        return false;
    }

#if CONFIG_PM_ENABLE
    bool clocks_acquired = false;
    if ((!cpu_clock_lock && esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "infrared_cpu", &cpu_clock_lock) != ESP_OK) ||
        (!peripheral_clock_lock && esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "infrared_bus", &peripheral_clock_lock) != ESP_OK) ||
        (!awake_lock && esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "infrared_awake", &awake_lock) != ESP_OK)) {
        goto failed;
    }
    esp_pm_lock_acquire(cpu_clock_lock);
    esp_pm_lock_acquire(peripheral_clock_lock);
    esp_pm_lock_acquire(awake_lock);
    clocks_acquired = true;
#endif

    if (esp_clk_cpu_freq() != INFRARED_CPU_HZ) {
        goto failed;
    }
    gpio_config_t receive_configuration = {
        .pin_bit_mask = 1ULL << RECEIVE_PIN,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    if (gpio_config(&receive_configuration) != ESP_OK ||
        gpio_set_drive_capability(RECEIVE_PIN, GPIO_DRIVE_CAP_3) != ESP_OK ||
        gpio_set_level(RECEIVE_PIN, 1) != ESP_OK) {
        goto failed;
    }
    if (!initialize_transmitter() || rmt_enable(transmit_channel) != ESP_OK) {
        goto failed;
    }
    transmitter_enabled = true;
    gpio_input_enable(TRANSMIT_PIN);

    receiver.write_count = 0;
    receiver.read_count = 0;
    receiver.receive_turn = 1;
    receiver.overflow_turn = 0;
    receiver.epoch_valid = false;
    receiver.calibration = (InfraredCalibration){
        .floating_blocks = INFRARED_INITIAL_FLOAT_BLOCKS
    };
    receiver.floating_mux = REG_READ(IO_MUX_GPIO5_REG) & ~(FUN_PD | FUN_PU | FILTER_EN);
    infrared_decoder_init(&decoder);

    if (!worker_task) {
        worker_task = xTaskCreateStaticPinnedToCore(capture_worker, "infrared", sizeof(worker_stack),
                                                   NULL, configMAX_PRIORITIES - 2,
                                                   worker_stack, &worker_control, 1);
        if (!worker_task) {
            goto failed;
        }
    }
    publish_word(&receiver.requested_mode, RECEIVE_LISTEN);
    xTaskNotifyGive(worker_task);
    if (!wait_for_mode(RECEIVE_LISTEN, MODE_CHANGE_TIMEOUT_US)) {
        goto failed;
    }

    epoch_us = receiver.epoch_us;
    consumed_cycle = receiver.epoch_cycle;
    elapsed_cycles = 0;
    last_event_us = epoch_us;
    measure_receiver_noise();
    opened = true;
    return true;

failed:
    publish_word(&receiver.requested_mode, RECEIVE_OFF);
    if (worker_task && !acquire_word(&receiver.initialization_failed)) {
        xTaskNotifyGive(worker_task);
        if (!wait_for_mode(RECEIVE_OFF, MODE_CHANGE_TIMEOUT_US)) {
            opened = true;
            return false;
        }
    }
    if (transmitter_enabled) {
        rmt_disable(transmit_channel);
        transmitter_enabled = false;
    }
    release_receive_pin();
#if CONFIG_PM_ENABLE
    if (clocks_acquired) {
        esp_pm_lock_release(cpu_clock_lock);
        esp_pm_lock_release(peripheral_clock_lock);
        esp_pm_lock_release(awake_lock);
    }
#endif
    return false;
}

bool infrared_close(void)
{
    if (!opened) {
        return true;
    }
    publish_word(&receiver.requested_mode, RECEIVE_OFF);
    xTaskNotifyGive(worker_task);
    if (!wait_for_mode(RECEIVE_OFF, MODE_CHANGE_TIMEOUT_US)) {
        return false;
    }
    release_receive_pin();
    if (transmitter_enabled) {
        rmt_disable(transmit_channel);
        transmitter_enabled = false;
    }
    opened = false;
#if CONFIG_PM_ENABLE
    esp_pm_lock_release(cpu_clock_lock);
    esp_pm_lock_release(peripheral_clock_lock);
    esp_pm_lock_release(awake_lock);
#endif
    return true;
}

static void account_for_cycles(uint32_t cycle)
{
    elapsed_cycles += (uint32_t)(cycle - consumed_cycle);
    consumed_cycle = cycle;
}

static void discard_observations(void)
{
    uint32_t available = acquire_word(&receiver.write_count);
    if (available != receiver.read_count) {
        unsigned last = (available - 1u) & (OBSERVATION_CAPACITY - 1u);
        account_for_cycles(receiver.observations[last].end_cycle);
    }
    rearm_receive_turn(available);
}

bool infrared_transmit(const uint8_t *bytes, size_t length, uint64_t *ended_us)
{
    if (!opened || !bytes || !length || length > INFRARED_MAX_FRAME) {
        return false;
    }
    uint32_t last_pulse;
    uint32_t total_ticks;
    size_t symbol_count = infrared_encode_frame(bytes, length, transmit_symbols,
                                                INFRARED_MAX_FRAME * 10u,
                                                &last_pulse, &total_ticks);
    if (!symbol_count || symbol_count + 1u > DMA_SYMBOL_CAPACITY) {
        return false;
    }

    receiver.last_transmit_pulse = last_pulse * 3u;
    receiver.transmit_started = 0;
    publish_word(&receiver.requested_mode, RECEIVE_TRANSMIT);
    xTaskNotifyGive(worker_task);
    if (!wait_for_mode(RECEIVE_TRANSMIT, MODE_CHANGE_TIMEOUT_US)) {
        return false;
    }
    /* Transmitting takes ownership of the half-duplex port. Samples from
     * the previous receive turn cannot belong to the next reply. */
    discard_observations();
    rmt_transmit_config_t configuration = {
        .loop_count = 0,
        .flags = { .eot_level = 0 }
    };
    transmit_ended_us = 0;
    esp_err_t result = rmt_transmit(transmit_channel, transmit_encoder, transmit_symbols,
                                     symbol_count * sizeof(uint32_t), &configuration);
    if (result != ESP_OK || rmt_tx_wait_all_done(transmit_channel, 30) != ESP_OK) {
        rmt_disable(transmit_channel);
        transmitter_enabled = false;
        publish_word(&receiver.requested_mode, RECEIVE_LISTEN);
        return false;
    }
    if (acquire_word(&receiver.transmit_started) != 1) {
        return false;
    }
    *ended_us = transmit_ended_us;
    return *ended_us && wait_for_mode(RECEIVE_LISTEN, MODE_CHANGE_TIMEOUT_US);
}

bool infrared_receive(InfraredEvent *event)
{
    if (!opened || !event) {
        return false;
    }
    if (acquire_word(&receiver.overflow_turn) == receiver.receive_turn) {
        discard_observations();
        event->value = 0;
        event->errors = INFRARED_ERROR_OVERRUN;
        event->kind = INFRARED_EVENT_BYTE;
        event->time_us = epoch_us + elapsed_cycles / 240u;
        last_event_us = event->time_us;
        return true;
    }

    uint32_t read_count = receiver.read_count;
    uint32_t available = acquire_word(&receiver.write_count);
    uint32_t last_cycle = consumed_cycle;
    while (read_count != available) {
        unsigned offset = read_count & (OBSERVATION_CAPACITY - 1u);
        unsigned count = available - read_count;
        if (count > PUBLISH_INTERVAL) {
            count = PUBLISH_INTERVAL;
        }
        if (count > OBSERVATION_CAPACITY - offset) {
            count = OBSERVATION_CAPACITY - offset;
        }
        unsigned consumed;
        InfraredDecodedByte decoded;
        const InfraredObservation *observations = &receiver.observations[offset];
        bool received = infrared_decoder_consume(&decoder, observations, count, &consumed, &decoded);
        read_count += consumed;
        last_cycle = observations[consumed - 1u].end_cycle;
        publish_word(&receiver.read_count, read_count);
        if (received) {
            account_for_cycles(last_cycle);
            uint64_t completed = elapsed_cycles - (uint32_t)(last_cycle - decoded.completed_cycle);
            event->time_us = epoch_us + completed / 240u;
            event->value = decoded.value;
            event->errors = decoded.errors;
            event->kind = INFRARED_EVENT_BYTE;
            last_event_us = event->time_us;
            return true;
        }
    }

    account_for_cycles(last_cycle);
    publish_word(&receiver.read_count, read_count);
    if (decoder.processed_cycle && !decoder.receiving_byte && !decoder.pulse.active) {
        uint32_t quiet_cycle = decoder.processed_cycle - INFRARED_MAX_PULSE_CYCLES -
                               INFRARED_RELEASE_LEAD_CYCLES;
        uint32_t pending_cycles = last_cycle - quiet_cycle;
        if (elapsed_cycles >= pending_cycles) {
            uint64_t quiet_us = epoch_us + (elapsed_cycles - pending_cycles) / 240u;
            if (quiet_us > last_event_us) {
                *event = (InfraredEvent){.time_us = quiet_us, .kind = INFRARED_EVENT_QUIET};
                last_event_us = quiet_us;
                return true;
            }
        }
    }
    return false;
}

bool infrared_can_yield(void)
{
    uint32_t pending = acquire_word(&receiver.write_count) - receiver.read_count;
    return !opened || pending <= OBSERVATION_CAPACITY / 4u;
}

bool pw_host_infrared_open(void)
{
    // A previous stop may have timed out. Reclaim CPU1 before touching its pin.
    if (!infrared_close()) return false;
    DiodeProbe probe = diode_probe();
    display_diode(probe.icon);
    // Finish ready display strips before CPU1 starts measuring the optical pin.
    // A panel awaiting power-up or retry keeps its own future deadline.
    while (display_tick() <= (uint64_t)esp_timer_get_time()) {}
    if (!probe.receive_safe || !board_ir_power(true)) return false;
    if (!infrared_open()) {
        infrared_stop();
        return false;
    }
    return true;
}

void infrared_stop(void)
{
    infrared_close();
    board_ir_power(false);
}

void pw_host_infrared_close(void)
{
    infrared_stop();
}

bool pw_host_infrared_receive(PwInfraredEvent *event)
{
    InfraredEvent sample;
    if (!infrared_receive(&sample)) return false;
    *event = (PwInfraredEvent){.time_us = sample.time_us, .value = sample.value,
                              .errors = sample.errors, .byte = sample.kind == INFRARED_EVENT_BYTE};
    return true;
}

uint64_t pw_host_infrared_transmit(const uint8_t *bytes, size_t length)
{
    uint64_t ended_us;
    return infrared_transmit(bytes, length, &ended_us) ? ended_us : UINT64_MAX;
}

void infrared_yield(void)
{
    static uint64_t last_yield;
    uint64_t now = esp_timer_get_time();
    if (now - last_yield >= 20000 && infrared_can_yield()) {
        vTaskDelay(1);
        last_yield = esp_timer_get_time();
    }
}

void pw_host_infrared_detected(void)
{
    display_diode(DIODE_ACTIVE);
}
