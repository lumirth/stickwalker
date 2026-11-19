#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "soc/spi_pins.h"
#include "soc/rtc_cntl_reg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum {
    PM1 = 0x6e, BMI = 0x68, CODEC = 0x18,
    FRONT_PIN = GPIO_NUM_11, SIDE_PIN = GPIO_NUM_12,
    PM_SOURCE = 0x04, PM_RAILS = 0x06, PM_HOLD = 0x07, PM_CUTOFF = 0x08,
    PM_SPEED = 0x09, PM_WATCHDOG = 0x0a, PM_FEED = 0x0b, PM_COMMAND = 0x0c,
    PM_DIRECTION = 0x10, PM_LATCH = 0x11, PM_DRIVE = 0x13,
    PM_MUX = 0x16, PM_WAKE_ENABLE = 0x18, PM_BATTERY = 0x22,
    PM_MASK_GPIO = 0x43, PM_MASK_SYSTEM = 0x44,
    PM_MASK_BUTTON = 0x45, PM_POWER_KEY = 0x48, PM_KEY_ACTION = 0x49,
    PM_DOUBLE_CLICK = 0x4a,
    PM_GPIO_MODES = 0x1f, EXT_5V_INPUT = 1 << 1,
    AMP = 1 << 3, LCD = 1 << 2, BOOST = 1 << 3, LED = 1 << 4,
    WATCHDOG_SECONDS = 90,
    POWER_SAMPLE_US = 55000,
};

static const DRAM_ATTR gpio_num_t pins[] = {FRONT_PIN, SIDE_PIN};
static const uint64_t wake_pins = (1ULL << FRONT_PIN) | (1ULL << SIDE_PIN);
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t pm;
static i2c_master_dev_handle_t imu;
static i2c_master_dev_handle_t codec;
static TaskHandle_t foreground;
static portMUX_TYPE switch_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t edge_pressed;
static volatile uint8_t edge_released;
static uint8_t held;
static uint8_t raw_switches;
static uint64_t switch_changed[2];
static uint64_t last_power_key;
static uint64_t next_sensor;
static struct {
    Battery battery;
    uint64_t next_read;
    uint64_t next_source;
    bool source_known;
    bool sleeping;
    uint64_t low_since;
    uint64_t next_watchdog;
    uint64_t next_retry;
    bool watchdog;
    bool ready;
} power;
static unsigned cutoff = 3300;
static MotionMode motion_mode;
static uint64_t peripherals_ready_at;
static bool imu_ready;
static struct {
    int16_t xyz[342][3];
    unsigned count;
    uint64_t first_us, last_us;
    uint64_t clock_us;
    uint32_t sensor_time;
    uint32_t period_q16;
} motion;

extern const uint8_t bmi_image_start[] asm("_binary_bmi270_config_bin_start");

static uint64_t now_us(void)
{
    return esp_timer_get_time();
}

static void delay_ms(unsigned ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

static i2c_master_dev_handle_t attach(uint8_t address, uint32_t hz)
{
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = hz,
    };
    i2c_master_dev_handle_t device = NULL;
    i2c_master_bus_add_device(bus, &config, &device);
    return device;
}

static bool read_reg(i2c_master_dev_handle_t device, uint8_t reg, void *bytes, size_t length)
{
    return device && i2c_master_transmit_receive(device, &reg, 1, bytes, length, 12) == ESP_OK;
}

static bool write_reg(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value)
{
    uint8_t packet[] = {reg, value};
    return device && i2c_master_transmit(device, packet, sizeof(packet), 12) == ESP_OK;
}

static bool update_pm(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current;
    return read_reg(pm, reg, &current, 1) &&
           ((current & mask) == value || write_reg(pm, reg, (current & ~mask) | value));
}

static void IRAM_ATTR switch_edge(void *argument)
{
    unsigned index = (uintptr_t)argument;
    if (index < 2) {
        uint8_t mask = 1u << index;
        bool down = !gpio_get_level(pins[index]);
        portENTER_CRITICAL_ISR(&switch_lock);
        if (down) {
            edge_pressed |= mask;
        } else {
            edge_released |= mask;
        }
        portEXIT_CRITICAL_ISR(&switch_lock);
    }
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(foreground, &woken);
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

static bool configure_pm(void)
{
    uint8_t id;
    if (!read_reg(pm, 0, &id, 1)) {
        if (!read_reg(pm, 0, &id, 1)) {
            // A retained PM1 can start at its previous speed.
            i2c_master_dev_handle_t retained = attach(PM1, 400000);
            bool contacted = write_reg(retained, PM_SPEED, 0);
            if (retained) {
                i2c_master_bus_rm_device(retained);
            }
            if (!contacted) {
                return false;
            }
            delay_ms(3);
        }
    }
    if (!write_reg(pm, PM_SPEED, 0)) {
        return false;
    }
    uint8_t limit;
    if (read_reg(pm, PM_CUTOFF, &limit, 1)) {
        unsigned reserve = 2200 + limit * 781 / 100; // 200 mV above the PM1 threshold
        cutoff = reserve > cutoff ? reserve : cutoff;
    }

    // GPIO2 and GPIO3 own L3B and the amp. Leave the IRQ function disabled:
    // PM1 selects its button-feedback LED mode whenever a GPIO uses IRQ.
    // Stage both low latches before taking either output out of its reset mode.
    if (!update_pm(PM_LATCH, AMP | LCD, 0)) {
        return false;
    }
    if (!update_pm(PM_DRIVE, AMP | LCD, 0)) {
        return false;
    }
    if (!update_pm(PM_DIRECTION, PM_GPIO_MODES, AMP | LCD) ||
        !write_reg(pm, PM_MUX, 0) || !write_reg(pm, PM_MUX + 1, 0) ||
        !write_reg(pm, PM_HOLD, 0) || !write_reg(pm, PM_WAKE_ENABLE, 0)) {
        return false;
    }
    if (!update_pm(PM_RAILS, LED | BOOST | 4, 4)) {
        return false;
    }
    // One-second clicks, four-second download hold, ordinary power actions off.
    // A previously latched DL_LOCK cannot be cleared by this register write.
    if (!write_reg(pm, PM_KEY_ACTION, 0x1f) ||
        !update_pm(PM_DOUBLE_CLICK, 1, 1)) {
        return false;
    }
    if (!write_reg(pm, PM_MASK_GPIO, 0x1f)) {
        return false;
    }
    if (!write_reg(pm, PM_MASK_SYSTEM, 0x3f)) {
        return false;
    }
    return write_reg(pm, PM_MASK_BUTTON, 0x07);
}

void board_init(void)
{
    foreground = xTaskGetCurrentTaskHandle();
    // A GPIO wake must not reverse the flash supply during power-down.
    // 255 RC_FAST/256 cycles give about 3.7 ms minimum off time; IDF then
    // waits its normal 2.7 ms for flash power-up before resuming cached code.
    REG_SET_FIELD(RTC_CNTL_TIMER5_REG, RTC_CNTL_MIN_SLP_VAL, 255);
    // PSRAM is unused. Keep its CS inactive whenever VDD_SPI is supplied,
    // including short sleeps for which IDF declines to power down flash.
    gpio_hold_dis(MSPI_IOMUX_PIN_NUM_CS1);
    gpio_reset_pin(MSPI_IOMUX_PIN_NUM_CS1);
    gpio_set_level(MSPI_IOMUX_PIN_NUM_CS1, 1);
    gpio_set_direction(MSPI_IOMUX_PIN_NUM_CS1, GPIO_MODE_OUTPUT);
    gpio_hold_en(MSPI_IOMUX_PIN_NUM_CS1);
    for (unsigned i = 0; i < sizeof(pins) / sizeof(*pins); ++i) {
        rtc_gpio_hold_dis(pins[i]);
        rtc_gpio_deinit(pins[i]);
    }
    gpio_config_t input = {.pin_bit_mask = wake_pins, .mode = GPIO_MODE_INPUT,
                           .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_ANYEDGE};
    gpio_config(&input);
    if (gpio_install_isr_service(ESP_INTR_FLAG_IRAM) == ESP_OK) {
        for (unsigned i = 0; i < sizeof(pins) / sizeof(*pins); ++i) {
            gpio_isr_handler_add(pins[i], switch_edge, (void *)(uintptr_t)i);
        }
    }
    i2c_master_bus_config_t config = {
        .i2c_port = I2C_NUM_1, .sda_io_num = GPIO_NUM_47, .scl_io_num = GPIO_NUM_48,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
    };
    if (i2c_new_master_bus(&config, &bus) != ESP_OK) {
        return;
    }
    pm = attach(PM1, 100000);
    // PM1 shares these wires and supports at most Fast-mode (400 kHz).
    imu = attach(BMI, 400000);
    codec = attach(CODEC, 400000);
    power.ready = configure_pm();
    power.next_retry = now_us() + 1000000;
    board_power_tick(false, false);
}

Switches board_switches(bool screen_asleep)
{
    uint64_t now = now_us();
    uint8_t previous = held;
    uint8_t brief = 0;
    static bool discard_sleep_click;
    static uint8_t pending_press;
    if (screen_asleep) {
        held &= ~POWER;
        previous &= ~POWER;
        discard_sleep_click = true;
    }
    if (!screen_asleep && (discard_sleep_click || now >= last_power_key + POWER_SAMPLE_US)) {
        uint8_t key;
        if (read_reg(pm, PM_POWER_KEY, &key, 1)) {
            if (key & 1) {
                held |= POWER;
            } else {
                held &= ~POWER;
                if ((key & 0x80) && !(previous & POWER) && !discard_sleep_click) {
                    brief |= POWER;
                }
            }
        }
        discard_sleep_click = false;
        last_power_key = now;
    }
    uint8_t captured_down;
    uint8_t captured_up;
    portENTER_CRITICAL(&switch_lock);
    captured_down = edge_pressed;
    captured_up = edge_released;
    edge_pressed = edge_released = 0;
    portEXIT_CRITICAL(&switch_lock);
    pending_press |= captured_down;
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t mask = 1u << i;
        bool down = !gpio_get_level(pins[i]);
        bool was_down = raw_switches & mask;
        if (down != was_down) {
            raw_switches ^= mask;
            switch_changed[i] = now;
            if (down) pending_press |= mask;
        }
        if (now - switch_changed[i] >= 5000) {
            held = (held & ~mask) | (down ? mask : 0);
        }
        // A FIFO read can outlast a short tap between debounce scans.
        if (!down && !(previous & mask) && (pending_press & mask) &&
            (was_down || (captured_up & mask))) {
            brief |= mask;
        }
        if ((held | previous | brief) & mask) pending_press &= ~mask;
    }
    return (Switches){
        .held = held,
        .pressed = (held & ~previous) | brief,
        .released = (previous & ~held) | brief,
    };
}

Battery board_battery(void)
{
    return power.battery;
}

void board_power_tick(bool linking, bool screen_asleep)
{
    uint64_t now = now_us();
    if (!linking && !power.ready && pm && now >= power.next_retry) {
        power.ready = configure_pm();
        power.next_retry = now + 1000000;
    }
    if (power.sleeping && !screen_asleep) power.next_source = power.next_read = 0;
    // Align the battery sample and watchdog when entering pocket sleep.
    if (!power.sleeping && screen_asleep) power.next_watchdog = power.next_read = now;
    power.sleeping = screen_asleep;
    Battery *battery = &power.battery;
    bool usb_attached = usb_serial_jtag_is_connected();
    if (!linking && (now >= power.next_source || (usb_attached && !battery->external))) {
        uint8_t sources;
        power.source_known = read_reg(pm, PM_SOURCE, &sources, 1);
        if (power.source_known) {
            bool external = (sources & 3) != 0;
            if (external != battery->external) power.next_read = 0;
            battery->external = external;
        } else {
            battery->known = false;
        }
        // In the pocket, sample VBUS on a game wake: every other stationary
        // tick, or at the next FIFO drain while walking.
        power.next_source = now + (screen_asleep ? 1500000 : 1000000);
    }
    if (!linking && now >= power.next_read) {
        uint8_t voltage[2];
        battery->known = power.source_known && read_reg(pm, PM_BATTERY, voltage, sizeof(voltage));
        if (battery->known) {
            battery->millivolts = voltage[0] | (voltage[1] << 8);
            if (battery->external || battery->millivolts > cutoff) {
                power.low_since = 0;
            } else if (!power.low_since) {
                power.low_since = now;
            }
            battery->low = battery->millivolts <= cutoff + 150;
            battery->depleted = power.low_since &&
                (battery->millivolts + 100 <= cutoff || now - power.low_since >= 2000000);
        }
        power.next_read = now + (screen_asleep && !battery->low ? 30000000 : 1000000);
    }
    if (now >= power.next_watchdog && pm) {
        if (!power.watchdog) {
            power.watchdog = write_reg(pm, PM_WATCHDOG, WATCHDOG_SECONDS);
        }
        if (power.watchdog && write_reg(pm, PM_FEED, 0xa5)) {
            power.next_watchdog = now + (screen_asleep ? 30000000 : 15000000);
        }
    }
}

bool board_link_power_ready(void)
{
    power.next_read = 0;
    power.next_source = 0;
    power.next_watchdog = 0;
    board_power_tick(false, false);
    Battery battery = power.battery;
    return battery.known && (battery.external || !battery.low);
}

bool board_lcd_power(bool on)
{
    if (!update_pm(PM_LATCH, LCD, on ? LCD : 0)) return false;
    peripherals_ready_at = on ? now_us() + 10000 : 0;
    return true;
}

bool board_audio_ready(void)
{
    return peripherals_ready_at && now_us() >= peripherals_ready_at;
}

bool board_amp_power(bool on)
{
    return update_pm(PM_LATCH, AMP, on ? AMP : 0);
}

bool board_ir_power(bool on)
{
    if (!on) {
        power.next_read = 0;
        return update_pm(PM_RAILS, BOOST, 0);
    }
    // The IR emitter shares the Grove/Hat 5 V input and boost output.
    uint8_t source;
    if (!read_reg(pm, PM_SOURCE, &source, 1)) return false;
    bool boost = !(source & EXT_5V_INPUT);
    if (!update_pm(PM_RAILS, BOOST, boost ? BOOST : 0)) return false;
    uint64_t deadline = now_us() + 1200000;
    do {
        uint8_t voltage[2];
        if (read_reg(pm, 0x26, voltage, sizeof(voltage)) &&
            ((unsigned)voltage[0] | ((unsigned)voltage[1] << 8)) >= 4500) {
            return true;
        }
        delay_ms(20);
    } while (now_us() < deadline);
    update_pm(PM_RAILS, BOOST, 0);
    return false;
}

bool board_codec_write(uint8_t reg, uint8_t value)
{
    return write_reg(codec, reg, value);
}

bool board_codec_read(uint8_t reg, uint8_t *value)
{
    return read_reg(codec, reg, value, 1);
}

static bool init_imu(void)
{
    uint8_t id;
    if (!read_reg(imu, 0, &id, 1) || id != 0x24 || !write_reg(imu, 0x7e, 0xb6)) {
        return false;
    }
    delay_ms(3);
    if (!write_reg(imu, 0x7c, 0)) {
        return false;
    }
    esp_rom_delay_us(450);
    if (!write_reg(imu, 0x59, 0)) {
        return false;
    }
    // INIT_ADDR counts 16-bit words. A short buffer keeps upload bounded.
    uint8_t upload[129] = {0x5e};
    for (unsigned offset = 0; offset < 8192; offset += sizeof(upload) - 1) {
        unsigned address = offset / 2;
        uint8_t address_write[] = {0x5b, address & 15, address >> 4};
        __builtin_memcpy(upload + 1, bmi_image_start + offset, sizeof(upload) - 1);
        if (i2c_master_transmit(imu, address_write, sizeof(address_write), 15) != ESP_OK ||
            i2c_master_transmit(imu, upload, sizeof(upload), 15) != ESP_OK) {
            return false;
        }
    }
    if (!write_reg(imu, 0x59, 1)) {
        return false;
    }
    delay_ms(22);
    uint8_t status;
    return read_reg(imu, 0x21, &status, 1) && (status & 15) == 1;
}

void board_motion(MotionMode mode)
{
    if (imu_ready && motion_mode == mode) {
        return;
    }
    if (!imu_ready && mode != MOTION_OFF) {
        if (now_us() < next_sensor) {
            return;
        }
        imu_ready = init_imu();
        next_sensor = now_us() + 2000000;
    }
    if (!imu_ready && mode != MOTION_OFF) {
        return;
    }
    if (!write_reg(imu, 0x7c, 0)) {
        imu_ready = false;
        return;
    }
    esp_rom_delay_us(450);
    bool configured;
    if (mode != MOTION_OFF) {
        // 25 Hz FIFO samples are interpolated onto the game's own time grid.
        uint8_t config = mode == MOTION_LIVE ? 0xa7 : 0x16;
        configured = write_reg(imu, 0x40, config);
        if (configured) {
            configured = write_reg(imu, 0x41, 0) &&
                write_reg(imu, 0x45, 0x88) && write_reg(imu, 0x48, 0) &&
                write_reg(imu, 0x49, mode == MOTION_FIFO ? 0x40 : 0) &&
                write_reg(imu, 0x7e, 0xb0) && write_reg(imu, 0x7d, 4);
        }
    } else {
        configured = write_reg(imu, 0x7d, 0);
    }
    imu_ready = configured && write_reg(imu, 0x7c, mode == MOTION_LIVE ? 0 : 1);
    motion_mode = mode;
    motion.count = 0;
    motion.clock_us = 0;
    motion.period_q16 = 40000u << 16;
    esp_rom_delay_us(450);
}

void board_motion_drain(void)
{
    if (!imu_ready || motion_mode != MOTION_FIFO) return;
    // Partial button wakes have no watermark interrupt. Bosch requires APS off
    // before FIFO access; changing APS does not stop acquisition or flush data.
    if (!write_reg(imu, 0x7c, 0)) goto failed;
    esp_rom_delay_us(450);
    uint8_t metadata[14];
    uint64_t captured_at;
    // The clock is latched before FIFO_LENGTH in this burst. Avoid a data
    // update between those reads, which would date the newest frame one ODR late.
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        captured_at = now_us();
        if (!read_reg(imu, 0x18, metadata, sizeof(metadata))) goto failed;
        if (((metadata[0] | (metadata[1] << 8)) & 1023) < 1012) break;
    }
    unsigned bytes = metadata[12] | ((metadata[13] & 0x3f) << 8);
    uint32_t sensor = metadata[0] | (metadata[1] << 8) | (metadata[2] << 16);
    if (bytes >= 2046 || bytes % 6) {
        // Overwrite loses timestamps as well as samples. Discard this window;
        // the game resets its partial FFT instead of inventing missing history.
        motion.count = 0;
        motion.clock_us = 0;
        if (!write_reg(imu, 0x7e, 0xb0)) goto failed;
    } else if (bytes) {
        uint8_t data[2046];
        uint8_t reg = 0x26;
        if (i2c_master_transmit_receive(imu, &reg, 1, data, bytes, 65) != ESP_OK) goto failed;
        uint32_t ticks = (sensor - motion.sensor_time) & 0xffffff;
        if (motion.clock_us && ticks >= 1024) {
            uint64_t period = (captured_at - motion.clock_us) * (1024ULL << 16) / ticks;
            if (period >= (36000u << 16) && period <= (44000u << 16)) {
                motion.period_q16 = period;
            }
        }
        // 25 Hz data updates on bit 10 of SENSORTIME. Preserve that phase across
        // partial drains instead of stretching each buffer to fit its wake time.
        unsigned count = bytes / 6;
        uint64_t last = captured_at - ((uint64_t)(sensor & 1023) * motion.period_q16 >> 26);
        uint64_t first = last - ((uint64_t)(count - 1) * motion.period_q16 >> 16);
        bool retain = motion.count && first > motion.last_us && first - motion.last_us < 80000;
        if (retain) {
            __builtin_memcpy(motion.xyz[0], motion.xyz[motion.count - 1], sizeof(motion.xyz[0]));
            first = motion.last_us;
        }
        for (unsigned i = 0; i < count; i++) {
            for (unsigned axis = 0; axis < 3; axis++) {
                unsigned at = i * 6 + axis * 2;
                motion.xyz[i + retain][axis] = (int16_t)(data[at] | (data[at + 1] << 8));
            }
        }
        motion.count = count + retain;
        motion.first_us = first;
        motion.last_us = last;
        motion.clock_us = captured_at;
        motion.sensor_time = sensor;
    }
    if (!write_reg(imu, 0x7c, 1)) goto failed;
    esp_rom_delay_us(450);
    return;
failed:
    motion.count = 0;
    imu_ready = false;
}

uint64_t board_motion_until(void)
{
    return motion_mode == MOTION_FIFO && motion.count ? motion.last_us : now_us();
}

uint64_t board_motion_deadline(uint64_t sample_us)
{
    if (motion_mode != MOTION_FIFO) return sample_us;
    // The game asks for its last FFT tick. Wait for the sensor frame on the
    // far side of that tick, so interpolation can complete it in this wake.
    if (!motion.count) return sample_us + 44000;
    if (sample_us <= motion.last_us) return sample_us;
    uint64_t elapsed = (sample_us - motion.last_us) << 16;
    uint64_t frames = (elapsed + motion.period_q16 - 1) / motion.period_q16;
    return motion.last_us + (frames * motion.period_q16 >> 16) + 2000;
}

static void orient_acceleration(const int32_t xyz[3], int8_t out[3])
{
    out[0] = xyz[1] / 256;
    out[1] = xyz[0] == INT16_MIN ? INT8_MAX : -xyz[0] / 256;
    out[2] = xyz[2] / 256;
}

bool board_acceleration(uint64_t sample_us, int8_t out[3])
{
    uint8_t data[15];
    if (!imu_ready || motion_mode == MOTION_OFF) {
        return false;
    }
    if (motion_mode == MOTION_FIFO) {
        if (!motion.count || sample_us + 44000 < motion.first_us || sample_us > motion.last_us + 44000) return false;
        uint64_t elapsed = sample_us > motion.first_us ? sample_us - motion.first_us : 0;
        uint64_t position = (elapsed << 16) / motion.period_q16;
        unsigned index = position < motion.count ? position : motion.count - 1;
        unsigned next = index + 1 < motion.count ? index + 1 : index;
        uint64_t begin = (uint64_t)index * motion.period_q16 >> 16;
        unsigned fraction = next == index ? 0 : (elapsed - begin) * 65536 / (motion.period_q16 >> 16);
        if (fraction > 65536) fraction = 65536;
        int32_t xyz[3];
        for (unsigned axis = 0; axis < 3; axis++) {
            xyz[axis] = motion.xyz[index][axis] +
                (int64_t)(motion.xyz[next][axis] - motion.xyz[index][axis]) * fraction / 65536;
        }
        orient_acceleration(xyz, out);
        return true;
    }
    if (!read_reg(imu, 0x03, data, sizeof(data))) {
        imu_ready = false;
        return false;
    }
    if (motion_mode == MOTION_IDLE) esp_rom_delay_us(400);
    if (!(data[0] & 0x80)) {
        return false;
    }
    int32_t x = (int16_t)(data[9] | (data[10] << 8));
    int32_t y = (int16_t)(data[11] | (data[12] << 8));
    int32_t z = (int16_t)(data[13] | (data[14] << 8));
    int32_t xyz[] = {x, y, z};
    orient_acceleration(xyz, out);
    return true;
}

void board_park(uint64_t mask)
{
    // Clear the output latch before routing it to the pad.
    for (unsigned pin = 0; pin < 49; ++pin) {
        if (mask & (1ULL << pin)) {
            gpio_reset_pin(pin);
            gpio_set_level(pin, 0);
            gpio_set_direction(pin, GPIO_MODE_OUTPUT);
            gpio_pullup_dis(pin);
            gpio_pulldown_dis(pin);
        }
    }
}

void board_wait(uint64_t deadline, bool screen_asleep)
{
    uint64_t now = now_us();
    if (!screen_asleep && power.next_source > now && power.next_source < deadline) {
        deadline = power.next_source;
    }
    if (power.next_read > now && power.next_read < deadline) {
        deadline = power.next_read;
    }
    if (power.next_watchdog > now && power.next_watchdog < deadline) {
        deadline = power.next_watchdog;
    }
    if (!power.ready && power.next_retry > now && power.next_retry < deadline) {
        deadline = power.next_retry;
    }
    if ((held & POWER) && last_power_key + POWER_SAMPLE_US < deadline) {
        deadline = last_power_key + POWER_SAMPLE_US;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if ((raw_switches ^ held) & (1u << i)) {
            uint64_t debounce = switch_changed[i] + 5000;
            if (debounce < deadline) {
                deadline = debounce;
            }
        }
    }
    if (!screen_asleep || held || !power.battery.known || power.battery.external ||
        usb_serial_jtag_is_connected()) {
        if (now < deadline) {
            uint64_t ms = (deadline - now + 999) / 1000;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
        }
        return;
    }

    if (deadline <= now + 10000) {
        // Too short to cycle the flash supply. Wait once for the deadline;
        // polling every millisecond would repeatedly drain the same FIFO.
        uint64_t ms = deadline > now ? (deadline - now + 999) / 1000 : 1;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
        return;
    }
    if (!gpio_get_level(FRONT_PIN) || !gpio_get_level(SIDE_PIN) ||
        ulTaskNotifyTake(pdTRUE, 0)) {
        ulTaskNotifyTake(pdTRUE, 1);
        return;
    }
    bool armed = esp_sleep_enable_gpio_wakeup() == ESP_OK;
    for (unsigned i = 0; i < sizeof(pins) / sizeof(*pins); ++i) {
        gpio_intr_disable(pins[i]);
        armed &= gpio_wakeup_enable(pins[i], GPIO_INTR_LOW_LEVEL) == ESP_OK;
    }
    now = now_us();
    if (armed && deadline > now + 2500 &&
        esp_sleep_enable_timer_wakeup(deadline - now) == ESP_OK) {
        esp_light_sleep_start();
    }
    for (unsigned i = 0; i < sizeof(pins) / sizeof(*pins); ++i) {
        gpio_wakeup_disable(pins[i]);
        gpio_set_intr_type(pins[i], GPIO_INTR_ANYEDGE);
        gpio_intr_enable(pins[i]);
    }
}

void board_shutdown(void)
{
    board_motion(MOTION_OFF);
    board_ir_power(false);
    board_amp_power(false);
    board_lcd_power(false);

    // PM1 owns actual power-off and its Power/5 V restart paths. Recheck the
    // source after saving; a cable may have arrived during the flash write.
    for (;;) {
        uint8_t sources;
        if (read_reg(pm, PM_SOURCE, &sources, 1)) {
            if (sources & 3) {
                power.next_read = 0;
                board_power_tick(false, false);
                return;
            }
            write_reg(pm, PM_COMMAND, 0xa1);
        }
        // A failed I2C command must not resume gameplay or repeat the save.
        board_power_tick(false, false);
        delay_ms(1000);
    }
}
