#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "audio.h"
#include "management.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_rom_crc.h"
#include "board.h"
#include "display.h"
#include "esp_mac.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "infrared/infrared.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pokewalker.h"
#include "pokewalker_host.h"
#include "save.h"

enum {
    SETTINGS_HOLD_US = 1500000, SETTINGS_IDLE_US = 90000000,
    SETTINGS_ROWS = 5,
};

typedef struct {
    uint8_t palette;
    uint8_t reverse;
    uint8_t layout;
    uint8_t mappings[4];
} Preferences;

typedef enum { VIEW_PLAY, VIEW_SETTINGS, VIEW_CONTROLS, VIEW_RECOVERY } View;
typedef enum { INPUT_IDLE, INPUT_PRESSING, INPUT_WAIT_RELEASE } InputPhase;

typedef struct {
    InputPhase phase;
    uint8_t switches;
    uint64_t power_since;
} Input;

typedef struct {
    uint8_t used;
    uint8_t gestures[3];
    const char *name;
} Layout;

static const Layout layouts[] = {
    {POWER | FRONT, {POWER, FRONT, POWER | FRONT}, "Power + Front"},
    {ALL_BUTTONS, {POWER, FRONT, SIDE}, "All three"},
    {FRONT | SIDE, {FRONT, SIDE, FRONT | SIDE}, "Front + Side"},
    {POWER | SIDE, {POWER, SIDE, POWER | SIDE}, "Power + Side"},
};
// Each index lists the gestures for Left, Center and Right respectively.
static const uint8_t permutations[][3] = {
    {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0},
};
static Preferences settings = {.layout = 1, .mappings = {1, 0, 4, 1}};
static uint8_t sound_mode = SOUND_PIEZO;
static const char *const sound_names[SOUND_MODES] = {"Piezo", "Piezo+", "Speaker"};
static struct {
    View view;
    Preferences draft;
    unsigned menu_row;
    unsigned control_row;
    bool redraw;
    uint64_t last_activity;
    uint64_t next_page;
} ui = {.redraw = true};
static Input input;
typedef enum { OWNER_GAME, OWNER_IR, OWNER_USB_READ, OWNER_USB_WRITE } EepromOwner;
static EepromOwner eeprom_owner;

static bool usb_owns_eeprom(void)
{
    return eeprom_owner == OWNER_USB_READ || eeprom_owner == OWNER_USB_WRITE;
}
static struct {
    size_t position;
    uint64_t deadline;
} transfer;
static void transfer_clear(void);
static uint64_t preferences_due;
static bool screen_asleep;
static bool audio_wake_pending = true;
static bool game_ready;
static char hardware_id[6];
static uint8_t button_levels;
static unsigned brightness = 100;
static const esp_pm_config_t normal_clock = {.max_freq_mhz = 80, .min_freq_mhz = 10};
static const esp_pm_config_t link_clock = {.max_freq_mhz = 240, .min_freq_mhz = 240};
static uint8_t game_frame[PW_SCREEN_PIXELS];
static const unsigned brightness_steps[] = {2, 5, 10, 18, 28, 42, 64, 96, 155, 255};

// esp_timer runs from IRAM, and CPU0 instruction fetches from IRAM delay the
// receiver's pin writes on CPU1. The CPU clock is fixed at 240 MHz while the
// receiver is open, so the game's clock counts cycles then.
static bool cycle_clock;
static uint64_t clock_us, last_us;
static uint32_t clock_cycle, clock_remainder;

void pw_host_cycle_clock(bool enabled)
{
    if (enabled && !cycle_clock) {
        clock_us = pw_host_now_us();
        clock_cycle = esp_cpu_get_cycle_count();
        clock_remainder = 0;
    }
    cycle_clock = enabled;
}

uint64_t pw_host_now_us(void)
{
    uint64_t now = esp_timer_get_time();
    if (cycle_clock) {
        uint32_t cycle = esp_cpu_get_cycle_count();
        uint64_t elapsed = (uint64_t)(uint32_t)(cycle - clock_cycle) + clock_remainder;
        clock_cycle = cycle;
        clock_us += elapsed / 240u;
        clock_remainder = elapsed % 240u;
        now = clock_us;
    }
    if (now < last_us) now = last_us;
    last_us = now;
    return now;
}

uint64_t pw_host_calendar_us(void)
{
    struct timeval clock;
    gettimeofday(&clock, NULL);
    return (uint64_t)clock.tv_sec * 1000000 + clock.tv_usec;
}

static void style(void)
{
    display_style(settings.palette, settings.reverse, brightness);
}

static bool palette_name_valid(const char name[16])
{
    if (!name[0] || name[0] == ' ') {
        return false;
    }
    for (unsigned i = 0; i < 16; ++i) {
        if (!name[i]) {
            return name[i - 1] != ' ';
        }
        if (name[i] < 32 || name[i] > 126 || name[i] == '|') {
            return false;
        }
    }
    return false;
}

static bool preferences_valid(const Preferences *saved)
{
    return saved->palette < PALETTES && saved->reverse <= 1 &&
           saved->layout < sizeof(layouts) / sizeof(*layouts) &&
           saved->mappings[0] < 6 && saved->mappings[1] < 6 && saved->mappings[2] < 6 && saved->mappings[3] < 6;
}

static void prefs_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open("stick", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    uint16_t table[PALETTES][4];
    size_t table_size = sizeof(table);
    if (nvs_get_blob(nvs, "palettes", table, &table_size) == ESP_OK && table_size == sizeof(table))
        display_palette_table(table, true);
    char names[PALETTES][16];
    size_t names_size = sizeof(names);
    if (nvs_get_blob(nvs, "names", names, &names_size) == ESP_OK && names_size == sizeof(names)) {
        for (unsigned i = 0; i < PALETTES; ++i) {
            if (palette_name_valid(names[i])) {
                memcpy(palette_names[i], names[i], sizeof(names[i]));
            }
        }
    }
    Preferences saved = settings;
    size_t mappings_size = sizeof(saved.mappings);
    bool keyed = nvs_get_u8(nvs, "palette", &saved.palette) == ESP_OK &&
                 nvs_get_u8(nvs, "orientation", &saved.reverse) == ESP_OK &&
                 nvs_get_u8(nvs, "layout", &saved.layout) == ESP_OK &&
                 nvs_get_blob(nvs, "mappings", saved.mappings, &mappings_size) == ESP_OK &&
                 mappings_size == sizeof(saved.mappings);
    uint8_t mode;
    if (nvs_get_u8(nvs, "sound", &mode) == ESP_OK && mode < SOUND_MODES) sound_mode = mode;
    nvs_close(nvs);
    if (keyed && preferences_valid(&saved)) settings = saved;
    audio_mode(sound_mode);
}

static bool prefs_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open("stick", NVS_READWRITE, &nvs) != ESP_OK) return false;
    uint16_t table[PALETTES][4];
    display_palette_table(table, false);
    bool ok = nvs_set_u8(nvs, "palette", settings.palette) == ESP_OK &&
              nvs_set_u8(nvs, "orientation", settings.reverse) == ESP_OK &&
              nvs_set_u8(nvs, "layout", settings.layout) == ESP_OK &&
              nvs_set_blob(nvs, "mappings", settings.mappings, sizeof(settings.mappings)) == ESP_OK &&
              nvs_set_u8(nvs, "sound", sound_mode) == ESP_OK &&
              nvs_set_blob(nvs, "palettes", table, sizeof(table)) == ESP_OK &&
              nvs_set_blob(nvs, "names", palette_names, sizeof(palette_names)) == ESP_OK &&
              nvs_commit(nvs) == ESP_OK;
    nvs_close(nvs);
    return ok;
}

static void enter_view(View view)
{
    button_levels = 0;
    input = (Input){.phase = INPUT_WAIT_RELEASE};
    ui.view = view;
    ui.last_activity = pw_host_now_us();
    ui.redraw = true;
    display_fill(0);
    if (view == VIEW_PLAY) audio_wake_pending = true;
}

static const char *physical(uint8_t mask)
{
    switch (mask) {
    case POWER: return "Power";
    case FRONT: return "Front";
    case SIDE: return "Side";
    case POWER | FRONT: return "Power + Front";
    case FRONT | SIDE: return "Front + Side";
    default: return "Power + Side";
    }
}

static uint8_t logical(uint8_t mask)
{
    static const uint8_t inputs[] = {LEFT, CENTER, RIGHT};
    const Layout *layout = &layouts[settings.layout];
    const uint8_t *mapping = permutations[settings.mappings[settings.layout]];
    for (unsigned i = 0; i < 3; ++i) {
        if (mask == layout->gestures[mapping[i]]) {
            return inputs[i];
        }
    }
    return 0;
}

static unsigned selected_input(const Preferences *prefs, unsigned index)
{
    const uint8_t *mapping = permutations[prefs->mappings[prefs->layout]];
    for (unsigned input = 0; input < 3; ++input) {
        if (mapping[input] == index) {
            return input;
        }
    }
    return 0;
}

static void cycle_input(unsigned index)
{
    uint8_t mapping[3];
    memcpy(mapping, permutations[ui.draft.mappings[ui.draft.layout]], sizeof(mapping));
    unsigned previous = selected_input(&ui.draft, index), next = (previous + 1) % 3;
    mapping[previous] = mapping[next];
    mapping[next] = index;
    for (unsigned i = 0; i < 6; ++i) {
        if (!memcmp(mapping, permutations[i], sizeof(mapping))) {
            ui.draft.mappings[ui.draft.layout] = i;
            return;
        }
    }
}

static void battery_text(char text[24])
{
    Battery battery = board_battery();
    if (!battery.known) {
        snprintf(text, 24, "Unavailable");
        return;
    }
    static const struct { unsigned mv, percent; } scale[] = {
        {3300, 0}, {3600, 10}, {3720, 30}, {3820, 55}, {3990, 80}, {4200, 100},
    };
    unsigned percent = 0;
    if (battery.millivolts >= 4200) {
        percent = 100;
    } else {
        for (unsigned i = 1; i < sizeof(scale) / sizeof(*scale); ++i) {
            if (battery.millivolts >= scale[i - 1].mv && battery.millivolts < scale[i].mv) {
                unsigned voltage = battery.millivolts - scale[i - 1].mv;
                unsigned span = scale[i].mv - scale[i - 1].mv;
                unsigned gain = scale[i].percent - scale[i - 1].percent;
                percent = scale[i - 1].percent + voltage * gain / span;
                break;
            }
        }
    }
    snprintf(text, 24, "%u%% %u.%02u V", percent,
             battery.millivolts / 1000, (battery.millivolts % 1000) / 10);
}

static void row(unsigned index, unsigned selection, const char *label, const char *value, bool opens_page)
{
    unsigned y = 19 + index * 12;
    bool selected = index == selection;
    display_rect(4, y, 232, 11, selected ? 3 : 0);
    display_text(9, y + 2, label, selected ? 0 : 3);
    if (opens_page) display_text(226, y + 2, ">", selected ? 0 : 3);
    if (value) {
        display_text(118, y + 2, value, selected ? 0 : 3);
    }
}

static void draw_page(void)
{
    display_fill(0);
    if (ui.view == VIEW_RECOVERY) {
        display_text(9, 10, "Stickwalker", 3);
        display_text(203, 10, hardware_id, 3);
        display_text(9, 34, "Couldn't load your save.", 3);
        display_text(9, 49, "Connect to Stickwalker Desktop.", 3);
        return;
    }
    display_rect(0, 0, SCREEN_W, 16, 3);
    display_text(9, 4, ui.view == VIEW_CONTROLS ? "Controls" : "Stick Settings", 0);
    display_text(203, 4, hardware_id, 0);
    if (ui.view == VIEW_CONTROLS) {
        const Layout *layout = &layouts[ui.draft.layout];
        static const char *const labels[] = {"Left", "Center", "Right"};
        row(0, ui.control_row, "Buttons", layout->name, false);
        for (unsigned i = 0; i < 3; ++i) {
            row(i + 1, ui.control_row, physical(layout->gestures[i]), labels[selected_input(&ui.draft, i)], false);
        }
        row(4, ui.control_row, "Apply and close", NULL, false);
        row(5, ui.control_row, "Back", NULL, false);
    } else {
        row(0, ui.menu_row, "Controls", layouts[settings.layout].name, true);
        row(1, ui.menu_row, "Orientation", settings.reverse ? "Right side down" : "Left side down", false);
        row(2, ui.menu_row, "Palette", palette_names[settings.palette], false);
        row(3, ui.menu_row, "Sound", sound_names[sound_mode], false);
        row(4, ui.menu_row, "Back to game", NULL, false);
    }
    char battery[24];
    battery_text(battery);
    display_rect(0, 119, SCREEN_W, 16, 3);
    display_text(9, 123, battery, 0);
    uint8_t hour, minute;
    if (pw_clock_time(&hour, &minute)) {
        char clock[8];
        snprintf(clock, sizeof(clock), "%02u:%02u", hour, minute);
        display_text(198, 123, clock, 0);
    }
}

static void close_settings(bool timed_out)
{
    // A power-on reset discards the retained checkpoint, so keep recent steps.
    pw_save_changes();
    enter_view(VIEW_PLAY);
    style();
    display_game(game_frame);
    if (timed_out) {
        pw_sleep_screen();
    } else {
        pw_keep_awake();
        if (pw_sound_enabled()) audio_prepare();
    }
}

static void depleted_shutdown(void)
{
    audio_off();
    display_power(false);
    if (game_ready) {
        if (pw_communicating()) {
            pw_cancel_connection();
        }
        pw_save();
    }
    board_shutdown();
    // External power arrived before shutdown took effect.
    audio_wake_pending = true;
    screen_asleep = false;
    if (game_ready) pw_wake_screen();
}

static void menu_action(uint8_t action)
{
    if (ui.view == VIEW_CONTROLS) {
        if (action == LEFT) {
            ui.control_row = (ui.control_row + 5) % 6;
        } else if (action == RIGHT) {
            ui.control_row = (ui.control_row + 1) % 6;
        } else if (action == CENTER) {
            if (ui.control_row == 0) {
                ui.draft.layout = (ui.draft.layout + 1) % (sizeof(layouts) / sizeof(*layouts));
            } else if (ui.control_row < 4) {
                cycle_input(ui.control_row - 1);
            } else if (ui.control_row == 4) {
                settings = ui.draft;
                prefs_save();
                close_settings(false);
            } else {
                enter_view(VIEW_SETTINGS);
            }
        }
    } else if (action == LEFT) {
        ui.menu_row = (ui.menu_row + SETTINGS_ROWS - 1) % SETTINGS_ROWS;
    } else if (action == RIGHT) {
        ui.menu_row = (ui.menu_row + 1) % SETTINGS_ROWS;
    } else if (action == CENTER) {
        switch (ui.menu_row) {
        case 0:
            enter_view(VIEW_CONTROLS);
            ui.draft = settings;
            ui.control_row = 0;
            break;
        case 1:
            settings.reverse = !settings.reverse;
            prefs_save();
            break;
        case 2:
            settings.palette = (settings.palette + 1) % PALETTES;
            prefs_save();
            break;
        case 3:
            sound_mode = (sound_mode + 1) % SOUND_MODES;
            audio_mode(sound_mode);
            prefs_save();
            if (pw_sound_enabled()) audio_preview();
            break;
        case 4:
            close_settings(false);
            break;
        }
    }
    style();
    ui.redraw = true;
}

static uint8_t three_button_input(Switches switches)
{
    uint8_t direct = switches.pressed & (FRONT | SIDE);
    if (ui.view == VIEW_PLAY) {
        direct |= switches.held & (FRONT | SIDE);
    }
    // Power is dispatched on release, so a hold can open Settings.
    direct |= switches.released & POWER;
    uint8_t levels = 0;
    for (unsigned bit = FRONT; bit <= POWER; bit <<= 1) {
        if (direct & bit) {
            levels |= logical(bit);
            if (ui.view != VIEW_PLAY) {
                break;
            }
        }
    }
    return levels;
}

static uint8_t inputs(Switches switches, uint64_t now)
{
    if (ui.view == VIEW_RECOVERY) {
        return 0;
    }
    if (input.phase == INPUT_WAIT_RELEASE) {
        if (!switches.held) {
            input = (Input){0};
            ui.redraw = true;
        }
        return 0;
    }
    if (screen_asleep) {
        if (switches.pressed) {
            pw_tick(); // Credit the partial pocket batch before the first awake frame.
            screen_asleep = false;
            audio_wake_pending = true;
            input = (Input){.phase = switches.held ? INPUT_WAIT_RELEASE : INPUT_IDLE};
            pw_wake_screen();
            display_power(true);
            ui.last_activity = now;
        }
        return 0;
    }

    if (switches.pressed || switches.released) {
        ui.last_activity = now;
    }
    if ((switches.pressed & POWER) && switches.held == POWER) {
        input.power_since = now;
    }
    if (switches.held != POWER || (switches.pressed & (FRONT | SIDE))) {
        input.power_since = 0;
    }
    if (ui.view == VIEW_PLAY && input.power_since &&
        now - input.power_since >= SETTINGS_HOLD_US) {
        enter_view(VIEW_SETTINGS);
        ui.menu_row = 0;
        ui.redraw = true;
        ui.last_activity = now;
        input = (Input){.phase = INPUT_WAIT_RELEASE};
        audio_off();
        return 0;
    }

    uint8_t used = layouts[settings.layout].used;
    if (used == ALL_BUTTONS) {
        return three_button_input(switches);
    }
    uint8_t down = switches.held & used;
    uint8_t pressed = switches.pressed & used;
    if (input.phase == INPUT_IDLE && (pressed || down)) {
        input.phase = INPUT_PRESSING;
        input.switches = down ? down : pressed;
    }
    if ((down | pressed) == used) {
        input.switches = used;
    }
    if (down || input.phase == INPUT_IDLE) {
        return 0;
    }
    uint8_t result = logical(input.switches);
    input = (Input){0};
    return result;
}

uint64_t pw_host_acceleration_until(void)
{
    return board_motion_until();
}

bool pw_host_acceleration(uint64_t sample_us, int8_t acceleration[3])
{
    return board_acceleration(sample_us, acceleration);
}

uint8_t pw_host_buttons(void)
{
    return ui.view == VIEW_PLAY ? button_levels : 0;
}

void pw_host_present(const uint8_t pixels[PW_SCREEN_PIXELS])
{
    memcpy(game_frame, pixels, sizeof(game_frame));
    if (ui.view == VIEW_PLAY) {
        display_game(game_frame);
    }
}

void pw_host_display_sleep(bool asleep)
{
    if (ui.view == VIEW_PLAY && eeprom_owner != OWNER_IR) {
        screen_asleep = asleep;
        if (asleep) {
            audio_off();
            board_motion(MOTION_FIFO);
        }
        audio_wake_pending = !asleep;
        display_power(!asleep);
    }
}

void pw_host_contrast(uint8_t setting)
{
    unsigned next = setting < 10 ? setting : 5;
    brightness = brightness_steps[next];
    style();
}

uint64_t pw_host_tone(uint32_t frequency, uint8_t volume)
{
    if (screen_asleep || eeprom_owner == OWNER_IR || ui.view != VIEW_PLAY) {
        return pw_host_now_us();
    }
    return audio_tone(frequency, volume);
}

bool pw_host_battery_low(void)
{
    Battery battery = board_battery();
    return battery.known && battery.low && !battery.external;
}

bool pw_host_commit(void)
{
    return save_commit();
}

bool pw_host_communication(bool active)
{
    if (!active) {
        esp_pm_configure(&normal_clock);
        save_end_transfer();
        eeprom_owner = OWNER_GAME;
        return true;
    }
    if (eeprom_owner != OWNER_GAME) return false;
    if (!board_link_power_ready() || !pw_save_changes()) {
        return false;
    }
    if (!save_begin_transfer()) {
        return false;
    }
    audio_off();
    board_motion(MOTION_OFF);
    if (esp_pm_configure(&link_clock) != ESP_OK) {
        save_end_transfer();
        return false;
    }
    input = (Input){.phase = INPUT_WAIT_RELEASE};
    eeprom_owner = OWNER_IR;
    return true;
}

void pw_host_device_id(uint8_t identity[40])
{
    memset(identity, 0, 40);
    memcpy(identity, "StickS3 native Pokewalker port", sizeof("StickS3 native Pokewalker port") - 1);
    esp_efuse_mac_get_default(identity + 34);
}

void pw_host_fault(const char *message)
{
    (void)message;
    game_ready = false;
    enter_view(VIEW_RECOVERY);
    eeprom_owner = OWNER_GAME;
    infrared_stop();
    audio_off();
    screen_asleep = false;
    ui.redraw = true;
}

static void read_settings(ManagementSettings *out)
{
    memcpy(out, &settings, sizeof(settings));
    uint16_t colors[PALETTES][4];
    display_palette_table(colors, false);
    memcpy(out->colors, colors, sizeof(colors));
    memcpy(out->names, palette_names, sizeof(palette_names));
}

static bool settings_valid(const ManagementSettings *next)
{
    if (next->palette >= PALETTES || next->reverse > 1 || next->layout >= 4) {
        return false;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (next->mappings[i] >= 6) {
            return false;
        }
    }
    for (unsigned i = 0; i < PALETTES; ++i) {
        if (!palette_name_valid(next->names[i])) {
            return false;
        }
    }
    return true;
}

static void identify_device(void)
{
    if (screen_asleep) {
        board_motion_drain();
        if (game_ready) {
            pw_tick();
            pw_wake_screen();
        }
        screen_asleep = false;
        audio_wake_pending = true;
        display_power(true);
    }
    ui.last_activity = pw_host_now_us();
    if (game_ready) pw_keep_awake();
}

static void apply_settings(const ManagementSettings *next, bool palette_only)
{
    memcpy(&settings, next, sizeof(settings));
    memcpy(palette_names, next->names, sizeof(palette_names));
    if (palette_only) {
        ui.draft.palette = settings.palette;
    } else {
        ui.draft = settings;
    }
    uint16_t colors[PALETTES][4];
    memcpy(colors, next->colors, sizeof(colors));
    display_palette_table(colors, true);
    style();
    ui.redraw = true;
    identify_device();
    preferences_due = pw_host_now_us() + 300000;
}

static bool capture_save(void)
{
    audio_off();
    if (screen_asleep) {
        board_motion_drain();
    }
    pw_tick();
    return eeprom_owner != OWNER_IR && pw_save();
}

static void transfer_clear(void)
{
    if (eeprom_owner == OWNER_USB_WRITE) {
        save_end_transfer();
    }
    eeprom_owner = OWNER_GAME;
    memset(&transfer, 0, sizeof(transfer));
}

static bool begin_import(void)
{
    transfer_clear();
    bool recovery = !game_ready || ui.view == VIEW_RECOVERY;
    if (!recovery && !capture_save()) {
        return false;
    }
    if (!save_begin_import(recovery)) return false;
    eeprom_owner = OWNER_USB_WRITE;
    return true;
}

static unsigned invalid_transfer(void)
{
    transfer_clear();
    return MANAGE_INVALID;
}

static bool finish_import(void)
{
    PwRuntimeState runtime = {0};
    pw_host_runtime_checkpoint(&runtime);
    bool committed = save_commit();
    transfer_clear();
    if (!committed) {
        return false;
    }
    screen_asleep = false;
    board_motion(MOTION_LIVE);
    enter_view(VIEW_PLAY);
    display_power(true);
    pw_init();
    game_ready = ui.view != VIEW_RECOVERY;
    ui.redraw = true;
    return game_ready;
}

unsigned management_request(unsigned op, const uint8_t *in, size_t count,
                            uint8_t *out, size_t *length)
{
    if (op == MANAGE_STATUS && !count) {
        ManagementStatus status = {0};
        const esp_app_desc_t *app = esp_app_get_description();
        memcpy(status.version, app->version, sizeof(status.version));
        memcpy(status.build, app->app_elf_sha256, sizeof(status.build));
        esp_efuse_mac_get_default(status.mac);
        status.state = !game_ready || ui.view == VIEW_RECOVERY ? 2 : eeprom_owner == OWNER_IR ? 1 : 0;
        Battery battery = board_battery();
        status.battery_mv = battery.millivolts;
        status.external_power = battery.external;
        read_settings(&status.settings);
        memcpy(out, &status, sizeof(status));
        *length = sizeof(status);
        return MANAGE_OK;
    }
    if (eeprom_owner == OWNER_IR ||
        (eeprom_owner == OWNER_USB_WRITE && op != MANAGE_WRITE) ||
        (eeprom_owner == OWNER_USB_READ && op != MANAGE_READ)) {
        return MANAGE_BUSY;
    }
    if (op == MANAGE_IDENTIFY && !count) {
        identify_device();
        return MANAGE_OK;
    }
    if (op == MANAGE_SETTINGS && count == sizeof(ManagementSettings)) {
        ManagementSettings next;
        memcpy(&next, in, sizeof(next));
        bool palette_only = next.palette & MANAGE_PALETTE_ONLY;
        if (palette_only) {
            next.palette &= ~MANAGE_PALETTE_ONLY;
            next.reverse = settings.reverse;
            next.layout = settings.layout;
            memcpy(next.mappings, settings.mappings, sizeof(next.mappings));
        }
        if (!settings_valid(&next)) {
            return MANAGE_INVALID;
        }
        apply_settings(&next, palette_only);
        return MANAGE_OK;
    }
    if (op == MANAGE_WRITE) {
        if (!count) {
            if (!begin_import()) {
                return MANAGE_FAILED;
            }
            uint8_t blank[256];
            memset(blank, 0xff, sizeof(blank));
            for (unsigned offset = 0; offset < 65536; offset += sizeof(blank)) {
                pw_host_eeprom_write(offset, blank, sizeof(blank));
            }
            return finish_import() ? MANAGE_OK : MANAGE_FAILED;
        }
        if (count <= sizeof(uint32_t)) {
            return invalid_transfer();
        }
        uint32_t offset;
        memcpy(&offset, in, sizeof(offset));
        size_t bytes = count - sizeof(offset);
        if (offset > 65536 || bytes > 65536 - offset) {
            return invalid_transfer();
        }
        if (!offset) {
            if (bytes < 8 || memcmp(in + sizeof(offset), "nintendo", 8)) {
                return invalid_transfer();
            }
            if (!begin_import()) {
                return MANAGE_FAILED;
            }
        }
        if (eeprom_owner != OWNER_USB_WRITE || offset != transfer.position) {
            return invalid_transfer();
        }
        pw_host_eeprom_write(offset, in + sizeof(offset), bytes);
        transfer.position += bytes;
        transfer.deadline = pw_host_now_us() + 2000000;
        if (transfer.position == 65536) {
            return finish_import() ? MANAGE_OK : MANAGE_FAILED;
        }
        return MANAGE_OK;
    }
    if (!game_ready || ui.view == VIEW_RECOVERY) {
        return MANAGE_BUSY;
    }
    if (op == MANAGE_PREPARE && !count) {
        transfer_clear();
        if (!capture_save() || !prefs_save()) {
            return MANAGE_FAILED;
        }
        preferences_due = 0;
        return MANAGE_OK;
    }
    if (op == MANAGE_READ) {
        ManagementRead page;
        if (count != sizeof(page)) return invalid_transfer();
        memcpy(&page, in, sizeof(page));
        if (!page.length || page.length > MANAGE_PAYLOAD - 1 ||
            page.offset > 65536 - page.length) {
            return invalid_transfer();
        }
        if (!page.offset) {
            transfer_clear();
            if (!capture_save()) {
                return MANAGE_FAILED;
            }
            eeprom_owner = OWNER_USB_READ;
        }
        if (eeprom_owner != OWNER_USB_READ || page.offset != transfer.position) {
            return invalid_transfer();
        }
        pw_host_eeprom_read(page.offset, out, page.length);
        *length = page.length;
        transfer.position += page.length;
        transfer.deadline = pw_host_now_us() + 2000000;
        if (transfer.position == 65536) {
            transfer_clear();
        }
        return MANAGE_OK;
    }
    return MANAGE_INVALID;
}

void app_main(void)
{
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(hardware_id, sizeof(hardware_id), "#%02X%02X", mac[4], mac[5]);
    board_init();
    management_init();
    display_palette_table(NULL, true);
    esp_pm_configure(&normal_clock);
    input.phase = INPUT_WAIT_RELEASE;
    // A settings partition failure does not prevent playing or accessing USB.
    esp_err_t nvs_error = nvs_flash_init();
    if (nvs_error == ESP_ERR_NVS_NO_FREE_PAGES || nvs_error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() == ESP_OK) {
            nvs_error = nvs_flash_init();
        }
    }
    if (nvs_error == ESP_OK) {
        prefs_load();
    }
    style();
    board_motion(MOTION_LIVE);
    if (save_open()) {
        pw_init();
        game_ready = ui.view != VIEW_RECOVERY;
    } else {
        pw_host_fault("The save could not be mounted");
    }
    if (game_ready) save_prepare_transfer();
    uint64_t next_prepare = 0;
    ui.last_activity = pw_host_now_us();

    for (;;) {
        uint64_t now = pw_host_now_us();
        management_poll();
        // Fresh requests extend the transfer deadline. The USB SOF monitor can
        // briefly report disconnected during flash writes, even with a live host.
        if (usb_owns_eeprom() && now >= transfer.deadline) {
            transfer_clear();
        }
        if (preferences_due && now >= preferences_due && eeprom_owner == OWNER_GAME) {
            preferences_due = prefs_save() ? 0 : now + 5000000;
        }
        if (usb_owns_eeprom()) {
            // Explicit EEPROM transfers borrow the game buffer briefly. Status
            // polling never enters this path. A lost host releases it on timeout.
            board_power_tick(false, screen_asleep);
            board_wait(now + 1000, false);
            continue;
        }
        if (!screen_asleep) {
            display_power(true);
        }
        uint64_t next_game = now + 1000000;
        Switches switches = {0};
        if (eeprom_owner != OWNER_IR) {
            switches = board_switches(screen_asleep);
        }
        board_power_tick(eeprom_owner == OWNER_IR, screen_asleep);
        Battery battery = board_battery();
        if (battery.depleted) {
            depleted_shutdown();
        }
        if (eeprom_owner != OWNER_IR) {
            if (screen_asleep) board_motion_drain();
            if (switches.pressed && ui.view == VIEW_PLAY && !screen_asleep && pw_sound_enabled()) {
                audio_prepare();
            }
            button_levels = inputs(switches, now);
            if (ui.view == VIEW_SETTINGS || ui.view == VIEW_CONTROLS) {
                if (now - ui.last_activity >= SETTINGS_IDLE_US) {
                    close_settings(true);
                } else if (button_levels) {
                    menu_action(button_levels);
                }
            }
        }
        if (game_ready && ui.view != VIEW_RECOVERY) {
            if (ui.view == VIEW_SETTINGS || ui.view == VIEW_CONTROLS) {
                pw_keep_awake();
            }
            next_game = pw_tick();
            PwMotionMode motion = pw_motion_mode();
            board_motion(motion == PW_MOTION_PAUSED ? MOTION_OFF :
                         motion == PW_MOTION_IDLE ? MOTION_IDLE :
                         screen_asleep ? MOTION_FIFO : MOTION_LIVE);
            next_game = board_motion_deadline(next_game);
        }
        if (eeprom_owner == OWNER_IR) {
            uint64_t service_until = pw_host_now_us() + 20000;
            while (eeprom_owner == OWNER_IR && pw_communicating() && pw_host_now_us() < service_until) {
                pw_tick();
            }
            infrared_yield();
            continue;
        }
        if (ui.view != VIEW_PLAY && (ui.redraw || now >= ui.next_page)) {
            draw_page();
            ui.next_page = now + 1000000;
            ui.redraw = false;
        }
        if (game_ready && (!pw_connection_screen() || ui.view != VIEW_PLAY)) {
            display_diode(DIODE_HIDDEN);
        }
        // Service the score between display strips, including outside Connect.
        uint64_t next_display = UINT64_MAX;
        if (!screen_asleep) {
            next_display = display_tick();
        }
        if (audio_wake_pending && board_audio_ready() && eeprom_owner != OWNER_IR) {
            audio_wake_pending = false;
            if (ui.view == VIEW_PLAY && pw_sound_enabled()) audio_prepare();
        }
        uint64_t next_audio = audio_tick(pw_sound_playing());
        if (game_ready && !audio_active() && now >= next_prepare) {
            next_prepare = save_prepare_transfer() ? 0 : now + 30000000;
        }
        uint64_t wait = next_game;
        if (next_display < wait) {
            wait = next_display;
        }
        if (next_audio < wait) {
            wait = next_audio;
        }
        if (ui.view != VIEW_PLAY && ui.next_page < wait) {
            wait = ui.next_page;
        }
        if (wait > now + 20000 && usb_serial_jtag_is_connected()) wait = now + 20000;
        board_wait(wait, screen_asleep && !audio_active());
    }
}
