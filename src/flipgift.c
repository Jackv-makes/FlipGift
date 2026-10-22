// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nicolas Aubry (original Pokemon Mystery Gift app)
// Copyright (C) 2026 @Jackv_makes (FlippIR board support)

/**
 * flipgift.c - Flipgift: Gen II Pokemon Mystery Gift over IR from a Flipper
 * Zero through the FlippIR board (Vishay TFBS4711 on header pins 13/14).
 * App entry + GUI.
 *
 * Screens:
 *   Send Gift     - Gift type / item or decoration picker / Send; runs the
 *                   full sender state machine against a real cart
 *   Settings      - region, trainer name/ID, random ID, attempts
 *   Diagnostics   - TX Test, RX Monitor, Wiring
 *   About         - version and credits
 *
 * The IR work runs on a worker thread; log lines are marshalled back to the GUI
 * thread via a custom ViewDispatcher event.
 */
#include "gbc_ir_hal.h"
#include "mg_protocol.h"
#include "mg_gift.h"
#include "mg_timing.h"

#include <furi.h>
#include <stdio.h>
#include <string.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/text_input.h>
#include <gui/modules/byte_input.h>
#include <furi_hal_random.h>
#include <storage/storage.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>

#define APP_VERSION "1.0"
#define TRACE_DIR "/ext/apps_data/flipgift"
#define TRACE_PATH TRACE_DIR "/last_trace.txt"
#define TRACE_TAIL_LINES 12 // how many trailing trace lines go to the screen

#define TAG "Flipgift"
#define LOG_CAP 1024
#define NAME_MAX 11 // Gen II name field is 11 bytes (up to 10 chars + terminator)
#define GIFT_RANDOM 2 // Gift row: pick a random item or decoration, trainer and ID per send

// Trainer names drawn for random sends (Gen II player and rival names, plus
// a few in the same spirit). A random two-digit suffix is appended.
static const char* const random_names[] = {
    "GOLD",  "SILVER", "KRIS",  "LYRA",  "ETHAN", "RED",   "BLUE",  "GREEN",
    "LEAF",  "YELLOW", "CHRIS", "ELM",   "OAK",   "KURT",  "BILL",  "EUSINE",
};
#define RANDOM_NAME_COUNT (sizeof(random_names) / sizeof(random_names[0]))

typedef enum {
    ViewMenu = 0,
    ViewLog = 1,
    ViewConfig = 2,
    ViewTextInput = 3,
    ViewByteInput = 4,
    ViewSettings = 5,
    ViewDiagnostics = 6,
    ViewPicker = 7, // full-width item / decoration list
} AppView;

typedef enum {
    EventRefreshLog = 1,
    EventWorkerDone = 2,
    EventRebuildConfig = 3, // gift type changed: rebuild the config rows
} AppEvent;

typedef enum {
    ActionSendGift,
    ActionTxTest,
    ActionRxMonitor,
    ActionSettings, // not a worker action: opens the settings screen
    ActionDiagnostics, // not a worker action: opens the diagnostics submenu
    ActionWiring, // not a worker action: pin map (Diagnostics)
    ActionAbout, // not a worker action: credits
} AppAction;

// Config rows, in display order.
// Send Gift screen. The second row is the item picker or the decoration
// picker, never both.
typedef enum {
    ConfigRowGiftType = 0,
    ConfigRowPick,
    ConfigRowSend,
} ConfigRow;

// Settings screen.
typedef enum {
    SettingsRowRegion = 0,
    SettingsRowTrainer,
    SettingsRowTrainerId,
    SettingsRowRandomize,
    SettingsRowAttempts,
} SettingsRow;
#define ATTEMPTS_MAX 30

// Selectable regions (the target cart's own region code).
static const char* const region_names[] = {"USA", "France", "Germany", "Italy", "Spain"};
static const uint8_t region_codes[] = {
    MG_REGION_CODE_USA,
    MG_REGION_CODE_FRA,
    MG_REGION_CODE_GER,
    MG_REGION_CODE_ITA,
    MG_REGION_CODE_SPA,
};
#define REGION_COUNT (sizeof(region_codes) / sizeof(region_codes[0]))

typedef struct {
    Gui* gui;
    NotificationApp* notif; // RGB LED = link grade while a send runs
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Submenu* diag_menu;
    TextBox* text_box;
    VariableItemList* config_list;
    VariableItemList* settings_list;
    VariableItem* vi_gift_type;
    Submenu* pick_menu; // item or decoration picker
    VariableItem* vi_pick; // the row that opens it
    VariableItem* vi_region;
    VariableItem* vi_trainer;
    VariableItem* vi_tid;
    VariableItem* vi_rnd;
    TextInput* text_input;
    ByteInput* byte_input;
    uint8_t tid_bytes[2]; // ByteInput edit buffer: [hi, lo]

    FuriMutex* log_mutex;
    FuriString* log; // shared, guarded by log_mutex
    char log_buf[LOG_CAP]; // GUI-thread-only display copy
    volatile bool refresh_pending; // one refresh event in flight at most

    FuriThread* worker;
    volatile bool abort;
    AppAction action;
    AppView current_view;

    // Gift configuration (set defaults in app_alloc; edited via the config UI).
    uint8_t region_code;
    char trainer_name[NAME_MAX + 1];
    uint16_t trainer_id;
    bool randomize_id; // pick a fresh random ID on each send
    uint8_t attempts; // exchange attempts per send (1..ATTEMPTS_MAX)
    // Link indicator, graded from the receiver while aiming and from the
    // protocol log during the exchange:
    // 0 none, 1 glitchy, 2 light-not-hello, 3 hello heard, 4 linked, 5 done.
    volatile uint8_t sig_level;
    uint8_t item_index; // MgGiftItemIndex (index into the game's item table)
    uint8_t gift_type; // MG_SENT_ITEM / MG_SENT_DECORATION / GIFT_RANDOM
    uint8_t deco_index; // MgGiftDecoIndex

    bool random_gift_was_deco; // what the last random send chose
} App;

static void app_log(void* ctx, const char* msg);
static void config_build(App* app);

static GbcIrConfig app_ir_config(const App* app) {
    UNUSED(app);
    return gbc_ir_config_default();
}

// Decode and print what the cart told us about itself.
static void log_partner(App* app, const uint8_t* p1, uint8_t len1, const uint8_t* p2, uint8_t len2) {
    char line[48];
    MgPartnerInfo info;
    if(!mg_gift_parse_payload1(p1, len1, &info)) {
        app_log(app, "partner data: none");
        return;
    }
    app_log(app, "--- partner ---");
    snprintf(line, sizeof(line), "%s  ID %05u", info.trainer_name, info.trainer_id);
    app_log(app, line);
    snprintf(line, sizeof(line), "ver %u  dex %u  cnt %u", info.version, info.pokedex_count, info.counter);
    app_log(app, line);
    if(info.sent_deco == MG_SENT_DECORATION) {
        snprintf(line, sizeof(line), "gives: %s", mg_deco_name(info.which_deco));
        app_log(app, line);
        snprintf(line, sizeof(line), " (else %s)", mg_item_name(info.which_item));
    } else {
        snprintf(line, sizeof(line), "gives: %s", mg_item_name(info.which_item));
    }
    app_log(app, line);
    // The second payload (the cart's party) is only used by Crystal's Trainer
    // House; nothing here depends on it, so it is kept for replay but not shown.
    UNUSED(p2);
    UNUSED(len2);
}

// -------------------------------------------------------------------------
// Logging (callable from the worker thread)
// -------------------------------------------------------------------------
static void app_log_raw(App* app, const char* msg);

// Signal grade changes are announced as their own log lines.
// LED colours for the link grade. Red: the sensor sees IR but it is broken
// up or the link was just lost. Yellow: steady light that is not a hello.
// Green: clean hellos, linked, or done. Off: nothing heard / idle.
static const NotificationSequence seq_led_red = {
    &message_red_255, &message_green_0, &message_blue_0, &message_do_not_reset, NULL};
static const NotificationMessage msg_green_160 = {
    .type = NotificationMessageTypeLedGreen, .data.led.value = 160};
static const NotificationSequence seq_led_yellow = {
    &message_red_255, &msg_green_160, &message_blue_0, &message_do_not_reset, NULL};
static const NotificationSequence seq_led_green = {
    &message_red_0, &message_green_255, &message_blue_0, &message_do_not_reset, NULL};
static const NotificationSequence seq_led_off = {
    &message_red_0, &message_green_0, &message_blue_0, NULL};

static void led_for_level(App* app, uint8_t level) {
    const NotificationSequence* seq;
    switch(level) {
    case 1: seq = &seq_led_red; break;
    case 2: seq = &seq_led_yellow; break;
    case 3:
    case 4:
    case 5: seq = &seq_led_green; break;
    default: seq = &seq_led_off; break;
    }
    notification_message(app->notif, seq);
}

static void set_signal(App* app, uint8_t level, const char* text) {
    if(app->sig_level == level) return;
    app->sig_level = level;
    led_for_level(app, level);
    char line[40];
    snprintf(line, sizeof(line), "[%u] %s", level, text);
    app_log_raw(app, line);
}

static void app_log(void* ctx, const char* msg) {
    App* app = ctx;
    // Grade the link from the protocol's own progress lines.
    if(strncmp(msg, "game hello heard", 16) == 0) set_signal(app, 3, "hello heard");
    else if(strncmp(msg, "region", 6) == 0) set_signal(app, 4, "LINKED both ways");
    else if(strncmp(msg, "pass 2", 6) == 0) set_signal(app, 4, "LINKED: pass 2");
    else if(strncmp(msg, "done: OK", 8) == 0) set_signal(app, 5, "SUCCESS");
    else if(strncmp(msg, "TIMEOUT", 7) == 0 || strncmp(msg, "BAD", 3) == 0)
        set_signal(app, 1, "lost: re-aim");
    app_log_raw(app, msg);
}

static void app_log_raw(App* app, const char* msg) {
    furi_mutex_acquire(app->log_mutex, FuriWaitForever);
    furi_string_cat_printf(app->log, "%s\n", msg);
    // Trim from the front if we exceed the cap.
    size_t len = furi_string_size(app->log);
    if(len > LOG_CAP - 128) {
        furi_string_right(app->log, len - (LOG_CAP - 256));
    }
    // Coalesce refresh events: at most ONE outstanding. A fast logger (RX
    // monitor near a TV remote) would otherwise fill the ViewDispatcher queue
    // and block this thread inside the send -- which deadlocks against
    // worker_stop() joining us from the GUI thread on Back.
    bool need_event = !app->refresh_pending;
    app->refresh_pending = true;
    furi_mutex_release(app->log_mutex);
    if(need_event) {
        view_dispatcher_send_custom_event(app->view_dispatcher, EventRefreshLog);
    }
}

static void app_log_clear(App* app) {
    furi_mutex_acquire(app->log_mutex, FuriWaitForever);
    furi_string_reset(app->log);
    furi_mutex_release(app->log_mutex);
}

// -------------------------------------------------------------------------
// Worker
// -------------------------------------------------------------------------
// Dump the exchange flight recorder: full trace APPENDED to the SD file (one
// file per Send Gift session, attempts separated by headers) + FURI_LOG, last
// lines to the screen. Appending -- not overwriting -- matters: with retries,
// overwriting would destroy the informative attempt with a later stub.
static void worker_dump_trace(App* app, uint32_t attempt, const char* result) {
    uint16_t n = mg_trace_count();

    char line[64];
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, TRACE_DIR);
    File* f = storage_file_alloc(storage);
    bool file_ok = storage_file_open(
        f, TRACE_PATH, FSAM_WRITE, (attempt <= 1) ? FSOM_CREATE_ALWAYS : FSOM_OPEN_APPEND);

    int hl = snprintf(
        line, sizeof(line), "=== attempt %lu: %s (%u ev) ===", (unsigned long)attempt, result, n);
    FURI_LOG_I(TAG, "%s", line);
    if(file_ok && hl > 0) {
        storage_file_write(f, line, (size_t)hl);
        storage_file_write(f, "\n", 1);
    }

    for(uint16_t i = 0; i < n; i++) {
        if(!mg_trace_format(i, line, sizeof(line))) break;
        FURI_LOG_I(TAG, "%s", line);
        if(file_ok) {
            storage_file_write(f, line, strlen(line));
            storage_file_write(f, "\n", 1);
        }
    }
    if(mg_trace_overflowed() && file_ok) {
        storage_file_write(f, "(trace buffer overflowed)\n", 26);
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);

    // Tail on screen: the last events are where a failure shows itself.
    app_log(app, "--- trace tail ---");
    uint16_t start = (n > TRACE_TAIL_LINES) ? (uint16_t)(n - TRACE_TAIL_LINES) : 0;
    for(uint16_t i = start; i < n; i++) {
        if(mg_trace_format(i, line, sizeof(line))) app_log(app, line);
    }
    char note[48];
    snprintf(note, sizeof(note), "%u ev -> last_trace.txt", n);
    app_log(app, note);
}

// Listen for the game's hello and grade what the sensor sees; returns once
// two clean hellos have been heard, or after max_ms. Waiting for a solid aim
// before claiming master is what makes the exchange reliable. The TFBS4711
// turns each mark into a ~2 us pulse, so the hello is
// judged on its gap (two pulses ~731 us apart).
static void aim_phase(App* app, uint32_t max_ms) {
    set_signal(app, 0, "no light");
    uint32_t t0 = furi_get_tick(), t_last_light = 0;
    int good = 0;
    while(furi_get_tick() - t0 < max_ms && !app->abort) {
        uint32_t m1 = gbc_ir_rx_measure(true); // waits up to 30 ms for light
        if(m1 == 0) {
            if(t_last_light && furi_get_tick() - t_last_light > 700) {
                set_signal(app, 0, "no light");
                good = 0;
            }
            continue;
        }
        t_last_light = furi_get_tick();
        uint32_t gap = gbc_ir_rx_measure(false);
        uint32_t m2 = gbc_ir_rx_measure(true);
        bool marks_ok = (m1 <= 12 && m2 <= 12);
        bool gap_ok = (gap >= 480 && gap <= 760);
        if(marks_ok && gap_ok) {
            good++;
            set_signal(app, 3, good >= 2 ? "GOOD: hold still" : "hello seen");
            if(good >= 2) return;
        } else if(gap < 40) {
            set_signal(app, 1, "glitchy: closer");
        } else {
            set_signal(app, 2, "light, not hello");
        }
    }
}

// Aim, exchange, retry: up to app->attempts tries, re-aiming whenever the
// link grade dropped below "hello heard". Returns the last result.
static MgResult run_exchange(
    App* app,
    const MgProtocol* proto,
    const uint8_t* p1,
    uint8_t len1,
    const uint8_t* p2,
    uint8_t len2) {
    const uint32_t max_attempts = app->attempts ? app->attempts : 1;
    uint32_t attempt = 0;
    MgResult r = MgTimeout;
    char msg[40];
    do {
        attempt++;
        if(attempt > 1) {
            snprintf(msg, sizeof(msg), "retry #%lu of %lu", (unsigned long)attempt, (unsigned long)max_attempts);
            app_log(app, msg);
        }
        if(attempt == 1 || app->sig_level < 3) {
            app_log(app, "Aim: press A on the GBC");
            aim_phase(app, attempt == 1 ? 30000 : 8000);
            if(app->abort) return MgAborted;
        }
        r = mg_sender_send_gift(proto, p1, len1, p2, len2);
        app_log(app, mg_result_str(r));
        worker_dump_trace(app, attempt, mg_result_str(r));
        furi_delay_ms(50);
    } while(r != MgOk && !app->abort && attempt < max_attempts);
    if(r != MgOk && !app->abort) {
        snprintf(msg, sizeof(msg), "stopped after %lu tries", (unsigned long)max_attempts);
        app_log(app, msg);
    }
    return r;
}

static void worker_send_gift(App* app) {
    GbcIrConfig cfg = app_ir_config(app);
    gbc_ir_init(&cfg);
    app->sig_level = 0;

    uint8_t p1[MG_PAYLOAD1_LEN];
    if(app->gift_type == GIFT_RANDOM) {
        // Fresh identity and gift every send: the game's one-gift-per-partner-
        // per-day check is keyed on the trainer ID.
        uint32_t r = furi_hal_random_get();
        snprintf(
            app->trainer_name, sizeof(app->trainer_name), "%s%02lu",
            random_names[r % RANDOM_NAME_COUNT], (unsigned long)((r >> 8) % 100));
        app->trainer_id = (uint16_t)(furi_hal_random_get() & 0xFFFF);
        app->item_index = (uint8_t)(furi_hal_random_get() % MG_ITEM_COUNT);
        app->deco_index = (uint8_t)(furi_hal_random_get() % MG_DECO_COUNT);
        app->random_gift_was_deco = (furi_hal_random_get() & 1) != 0;
    }
    bool send_deco = app->gift_type == MG_SENT_DECORATION ||
                     (app->gift_type == GIFT_RANDOM && app->random_gift_was_deco);
    MgGiftInfo info = mg_gift_default_item(app->trainer_name, app->trainer_id, app->item_index);
    if(send_deco) {
        // The cart gives the decoration, or which_item if it already owns it.
        info.sent_deco = MG_SENT_DECORATION;
        info.which_deco = app->deco_index;
    }
    uint8_t len1 = mg_gift_build_payload1(p1, &info);

    // A minimal party payload (one Pokemon then terminator). Content isn't
    // validated for an item gift, but the 38-byte length is required.
    MgPartyMon party[1] = {{.level = 5, .species = 155, .move1 = 33}}; // Cyndaquil / Tackle
    uint8_t p2[MG_PAYLOAD2_LEN];
    uint8_t len2 = mg_gift_build_payload2(p2, party, 1);

    uint8_t cap1[MG_CAPTURE_CAP], cap2[MG_CAPTURE_CAP];
    uint8_t cap1_len = 0, cap2_len = 0;
    MgProtocol proto = {
        .region_code = app->region_code,
        .log = app_log,
        .log_ctx = app,
        .abort = &app->abort,
        .partner1 = cap1,
        .partner1_len = &cap1_len,
        .partner2 = cap2,
        .partner2_len = &cap2_len,
    };

    char gift_line[48];
    if(app->gift_type == GIFT_RANDOM) {
        // Keep the surprise: the game reveals it at the Pokemon Center.
        snprintf(gift_line, sizeof(gift_line), "Gift: ???");
    } else if(send_deco) {
        snprintf(gift_line, sizeof(gift_line), "Gift: %s", mg_deco_name(app->deco_index));
    } else {
        snprintf(gift_line, sizeof(gift_line), "Gift: %s", mg_item_name(app->item_index));
    }
    app_log(app, gift_line);
    snprintf(gift_line, sizeof(gift_line), "As: %s", app->trainer_name);
    app_log(app, gift_line);
    snprintf(gift_line, sizeof(gift_line), "TID: 0x%04X", app->trainer_id);
    app_log(app, gift_line);
    app_log(app, "GBC: Mystery Gift screen,");
    app_log(app, "then press A ONCE.");
    MgResult r = run_exchange(app, &proto, p1, len1, p2, len2);
    if(r == MgOk) {
        log_partner(app, cap1, cap1_len, cap2, cap2_len);
    }
    gbc_ir_deinit();
}

static void worker_tx_test(App* app) {
    GbcIrConfig cfg = app_ir_config(app);
    gbc_ir_init(&cfg);
    app_log(app, "TX test: hello + msg");
    app_log(app, "Scope pin 13 / LED cathode");

    uint8_t sample[3] = {MG_MESSAGE_PREFIX, 0x01, 0xA5};
    uint32_t n = 0;
    while(!app->abort && n < 50) {
        mg_tx_hello();
        gbc_ir_gap(MG_INTER_MSG_GAP_US);
        mg_tx_message(sample, sizeof(sample));
        gbc_ir_gap(20000); // 20 ms between bursts, easy to trigger on
        n++;
    }
    app_log(app, "TX test done");
    gbc_ir_deinit();
}

static void worker_rx_monitor(App* app) {
    GbcIrConfig cfg = app_ir_config(app);
    gbc_ir_init(&cfg);
    app_log(app, "RX monitor: point an");
    app_log(app, "IR source at the sensor");

    while(!app->abort) {
        uint32_t mark = gbc_ir_rx_measure(true);
        if(mark == 0) {
            furi_delay_ms(2); // idle: back off so the GUI thread breathes
            continue;
        }
        uint32_t space = gbc_ir_rx_measure(false);
        char line[48];
        snprintf(line, sizeof(line), "mark=%luus space=%luus", (unsigned long)mark, (unsigned long)space);
        app_log(app, line);
    }
    app_log(app, "RX monitor stopped");
    gbc_ir_deinit();
}

static int32_t worker_thread(void* context) {
    App* app = context;
    switch(app->action) {
    case ActionSendGift: worker_send_gift(app); break;
    case ActionTxTest: worker_tx_test(app); break;
    case ActionRxMonitor: worker_rx_monitor(app); break;
    case ActionSettings:
    case ActionDiagnostics:
    case ActionWiring:
    case ActionAbout: break; // never started as a worker
    }
    view_dispatcher_send_custom_event(app->view_dispatcher, EventWorkerDone);
    return 0;
}

static void worker_start(App* app, AppAction action) {
    if(app->worker) return;
    app->action = action;
    app->abort = false;
    app_log_clear(app);
    app->worker = furi_thread_alloc_ex("MgWorker", 4096, worker_thread, app);
    furi_thread_start(app->worker);
}

static void worker_stop(App* app) {
    if(!app->worker) return;
    app->abort = true;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    app->worker = NULL;
    app->sig_level = 0;
    notification_message(app->notif, &seq_led_off);
}

// -------------------------------------------------------------------------
// Config screen (Send Gift)
// -------------------------------------------------------------------------
// Picker list: one full-width row per name, so long names are not squeezed
// into the VariableItemList value slot (about seven characters wide).
static void picker_callback(void* context, uint32_t index) {
    App* app = context;
    if(app->gift_type == MG_SENT_DECORATION) {
        app->deco_index = (uint8_t)index;
    } else {
        app->item_index = (uint8_t)index;
    }
    // Row labels cannot be changed in place; rebuild the rows with the new
    // name and land on the picker row.
    config_build(app);
    variable_item_list_set_selected_item(app->config_list, ConfigRowPick);
    app->current_view = ViewConfig;
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewConfig);
}

static void picker_open(App* app) {
    bool deco = app->gift_type == MG_SENT_DECORATION;
    submenu_reset(app->pick_menu);
    submenu_set_header(app->pick_menu, deco ? "Decoration" : "Item");
    uint8_t count = deco ? MG_DECO_COUNT : MG_ITEM_COUNT;
    for(uint8_t i = 0; i < count; i++) {
        submenu_add_item(
            app->pick_menu, deco ? mg_deco_name(i) : mg_item_name(i), i, picker_callback, app);
    }
    submenu_set_selected_item(app->pick_menu, deco ? app->deco_index : app->item_index);
    app->current_view = ViewPicker;
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewPicker);
}

static void config_gift_type_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->gift_type = idx; // 0 item, 1 decoration, 2 random
    variable_item_set_current_value_text(item, idx == GIFT_RANDOM ? "Random" : idx ? "Deco" : "Item");
    // Swap the picker row for the other table. The list is rebuilt from the
    // event loop, not here: this item is destroyed by the rebuild.
    view_dispatcher_send_custom_event(app->view_dispatcher, EventRebuildConfig);
}

static void config_region_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->region_code = region_codes[idx];
    variable_item_set_current_value_text(item, region_names[idx]);
}

static void config_update_tid_text(App* app) {
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%04X", app->trainer_id);
    variable_item_set_current_value_text(app->vi_tid, buf);
}

static void config_name_input_done(void* context) {
    App* app = context;
    if(app->trainer_name[0] == '\0') {
        strncpy(app->trainer_name, "FLIPPER", NAME_MAX);
    }
    variable_item_set_current_value_text(app->vi_trainer, app->trainer_name);
    app->current_view = ViewSettings;
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewSettings);
}

static void config_tid_input_done(void* context) {
    App* app = context;
    app->trainer_id = ((uint16_t)app->tid_bytes[0] << 8) | app->tid_bytes[1];
    config_update_tid_text(app);
    app->current_view = ViewSettings;
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewSettings);
}

static void config_attempts_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->attempts = variable_item_get_current_value_index(item) + 1;
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", app->attempts);
    variable_item_set_current_value_text(item, buf);
}

static void config_randomize_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->randomize_id = variable_item_get_current_value_index(item) != 0;
    variable_item_set_current_value_text(item, app->randomize_id ? "On" : "Off");
}

static void settings_enter_callback(void* context, uint32_t index) {
    App* app = context;
    switch(index) {
    case SettingsRowTrainer:
        text_input_set_header_text(app->text_input, "Trainer name");
        text_input_set_result_callback(
            app->text_input, config_name_input_done, app, app->trainer_name, NAME_MAX, false);
        app->current_view = ViewTextInput;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewTextInput);
        break;
    case SettingsRowTrainerId:
        app->tid_bytes[0] = (uint8_t)(app->trainer_id >> 8);
        app->tid_bytes[1] = (uint8_t)(app->trainer_id & 0xFF);
        byte_input_set_header_text(app->byte_input, "Trainer ID (2 bytes)");
        byte_input_set_result_callback(
            app->byte_input, config_tid_input_done, NULL, app, app->tid_bytes, 2);
        app->current_view = ViewByteInput;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewByteInput);
        break;
    default:
        break;
    }
}

static void config_enter_callback(void* context, uint32_t index) {
    App* app = context;
    switch(index) {
    case ConfigRowPick:
        if(app->gift_type != GIFT_RANDOM) picker_open(app);
        break;
    case ConfigRowSend:
        if(app->randomize_id) {
            app->trainer_id = (uint16_t)(furi_hal_random_get() & 0xFFFF);
            config_update_tid_text(app);
        }
        app->current_view = ViewLog;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewLog);
        worker_start(app, ActionSendGift);
        break;
    default:
        break;
    }
}

// Build (or rebuild) the Send Gift rows. Only the picker for the selected
// gift type is shown: Item for an item gift, Decoration for a decoration.
static void config_build(App* app) {
    variable_item_list_reset(app->config_list);

    bool deco = app->gift_type == MG_SENT_DECORATION;
    bool rnd = app->gift_type == GIFT_RANDOM;
    app->vi_gift_type =
        variable_item_list_add(app->config_list, "Gift", 3, config_gift_type_changed, app);
    variable_item_set_current_value_index(app->vi_gift_type, app->gift_type);
    variable_item_set_current_value_text(
        app->vi_gift_type, rnd ? "Random" : deco ? "Deco" : "Item");

    if(rnd) {
        // Nothing to pick: item or decoration, trainer name and ID are all
        // drawn fresh on every send.
        app->vi_pick = variable_item_list_add(app->config_list, "Picks", 1, NULL, app);
        variable_item_set_current_value_text(app->vi_pick, "random");
    } else {
        // The chosen name is the row label (full width); OK opens the picker.
        app->vi_pick = variable_item_list_add(
            app->config_list,
            deco ? mg_deco_name(app->deco_index) : mg_item_name(app->item_index),
            1,
            NULL,
            app);
        variable_item_set_current_value_text(app->vi_pick, ">>");
    }

    VariableItem* vi_send = variable_item_list_add(app->config_list, "Send gift", 1, NULL, app);
    variable_item_set_current_value_text(vi_send, ">>");

    variable_item_list_set_selected_item(app->config_list, ConfigRowGiftType);
}

// Settings screen: who the Flipper claims to be, and the fallback region.
static void settings_build(App* app) {
    variable_item_list_reset(app->settings_list);

    app->vi_region = variable_item_list_add(
        app->settings_list, "Region", REGION_COUNT, config_region_changed, app);
    uint8_t rsel = 0;
    for(uint8_t i = 0; i < REGION_COUNT; i++) {
        if(region_codes[i] == app->region_code) {
            rsel = i;
            break;
        }
    }
    variable_item_set_current_value_index(app->vi_region, rsel);
    variable_item_set_current_value_text(app->vi_region, region_names[rsel]);

    app->vi_trainer = variable_item_list_add(app->settings_list, "Trainer", 1, NULL, app);
    variable_item_set_current_value_text(app->vi_trainer, app->trainer_name);

    app->vi_tid = variable_item_list_add(app->settings_list, "Trainer ID", 1, NULL, app);
    config_update_tid_text(app);

    app->vi_rnd = variable_item_list_add(
        app->settings_list, "Random ID/send", 2, config_randomize_changed, app);
    variable_item_set_current_value_index(app->vi_rnd, app->randomize_id ? 1 : 0);
    variable_item_set_current_value_text(app->vi_rnd, app->randomize_id ? "On" : "Off");

    VariableItem* vi_att = variable_item_list_add(
        app->settings_list, "Attempts", ATTEMPTS_MAX, config_attempts_changed, app);
    variable_item_set_current_value_index(vi_att, app->attempts - 1);
    char att_buf[8];
    snprintf(att_buf, sizeof(att_buf), "%u", app->attempts);
    variable_item_set_current_value_text(vi_att, att_buf);
}

// -------------------------------------------------------------------------
// GUI plumbing
// -------------------------------------------------------------------------
static void submenu_callback(void* context, uint32_t index) {
    App* app = context;
    switch(index) {
    case ActionSendGift:
        app->current_view = ViewConfig;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewConfig);
        break;
    case ActionSettings:
        app->current_view = ViewSettings;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewSettings);
        break;
    case ActionDiagnostics:
        app->current_view = ViewDiagnostics;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewDiagnostics);
        break;
    case ActionTxTest:
    case ActionRxMonitor:
        app->current_view = ViewLog;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewLog);
        worker_start(app, (AppAction)index);
        break;
    case ActionWiring:
        app->action = ActionWiring; // so Back returns to Diagnostics
        app_log_clear(app);
        app_log(app, "FlippIR board (TFBS4711)");
        app_log(app, "9  = 3V3");
        app_log(app, "11 = GND");
        app_log(app, "13 = TXD (PB6)");
        app_log(app, "14 = RXD (PB7)");
        app_log(app, "Marks go out as 20us");
        app_log(app, "pulse chains; RX decodes");
        app_log(app, "by pulse period.");
        app_log(app, "USART1 is borrowed from");
        app_log(app, "the expansion service");
        app_log(app, "while the app runs.");
        app->current_view = ViewLog;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewLog);
        break;
    case ActionAbout:
        app->action = ActionAbout;
        app_log_clear(app);
        app_log(app, "Flipgift  v" APP_VERSION);
        app_log(app, "for the FlippIR board");
        app_log(app, "");
        app_log(app, "Gen II (G/S/C) Mystery");
        app_log(app, "Gift over IR from the");
        app_log(app, "Flipper Zero, through");
        app_log(app, "the FlippIR board.");
        app_log(app, "");
        app_log(app, "FlippIR: @Jackv_makes");
        app_log(app, "Original app:");
        app_log(app, "  TheRealNox");
        app_log(app, "Protocol RE:");
        app_log(app, "  projectpokemon.org");
        app_log(app, "Data tables:");
        app_log(app, "  pret/pokecrystal");
        app_log(app, "");
        app_log(app, "License: GPL-3.0");
        app->current_view = ViewLog;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewLog);
        break;
    default:
        break;
    }
}

static bool app_custom_event_callback(void* context, uint32_t event) {
    App* app = context;
    switch(event) {
    case EventRefreshLog: {
        furi_mutex_acquire(app->log_mutex, FuriWaitForever);
        // Clear the flag BEFORE copying so a line appended after this point
        // triggers a fresh event rather than being silently dropped.
        app->refresh_pending = false;
        strncpy(app->log_buf, furi_string_get_cstr(app->log), LOG_CAP - 1);
        app->log_buf[LOG_CAP - 1] = '\0';
        furi_mutex_release(app->log_mutex);
        text_box_set_text(app->text_box, app->log_buf);
        return true;
    }
    case EventWorkerDone:
        // Worker finished on its own; keep the log on screen. A random send
        // changed the name and ID, so refresh the config rows for next time.
        app->sig_level = 0;
        notification_message(app->notif, &seq_led_off);
        if(app->vi_trainer) variable_item_set_current_value_text(app->vi_trainer, app->trainer_name);
        if(app->vi_tid) config_update_tid_text(app);
        return true;
    case EventRebuildConfig:
        config_build(app);
        return true;
    default:
        return false;
    }
}

static bool app_navigation_callback(void* context) {
    App* app = context;
    switch(app->current_view) {
    case ViewLog:
        worker_stop(app);
        if(app->action == ActionTxTest || app->action == ActionRxMonitor ||
           app->action == ActionWiring) {
            app->current_view = ViewDiagnostics;
            view_dispatcher_switch_to_view(app->view_dispatcher, ViewDiagnostics);
        } else {
            app->current_view = ViewMenu;
            view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
        }
        return true;
    case ViewConfig:
    case ViewSettings:
    case ViewDiagnostics:
        app->current_view = ViewMenu;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
        return true;
    case ViewPicker:
        app->current_view = ViewConfig;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewConfig);
        return true;
    case ViewTextInput:
    case ViewByteInput:
        app->current_view = ViewSettings;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewSettings);
        return true;
    default: // ViewMenu
        return false; // let the dispatcher stop -> exit app
    }
}

// -------------------------------------------------------------------------
// Lifecycle
// -------------------------------------------------------------------------
static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));

    // Defaults - all editable from the Send Gift config screen.
    app->region_code = MG_REGION_CODE_USA;
    strncpy(app->trainer_name, "FLIPPER", sizeof(app->trainer_name) - 1);
    app->trainer_id = 0x1234;
    app->randomize_id = true; // fresh ID each send dodges the daily-limit check
    app->attempts = 10;
    app->item_index = MG_ITEM_BERRY; // first entry of the game's table
    app->gift_type = MG_SENT_ITEM;
    app->deco_index = MG_DECO_JIGGLYPUFF_DOLL; // first entry of the game's table

    app->log = furi_string_alloc();
    app->log_mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    app->gui = furi_record_open(RECORD_GUI);
    app->notif = furi_record_open(RECORD_NOTIFICATION);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, app_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, app_navigation_callback);

    app->submenu = submenu_alloc();
    submenu_add_item(app->submenu, "Send Gift", ActionSendGift, submenu_callback, app);
    submenu_add_item(app->submenu, "Settings", ActionSettings, submenu_callback, app);
    submenu_add_item(app->submenu, "Diagnostics", ActionDiagnostics, submenu_callback, app);
    submenu_add_item(app->submenu, "About", ActionAbout, submenu_callback, app);
    view_dispatcher_add_view(app->view_dispatcher, ViewMenu, submenu_get_view(app->submenu));

    app->diag_menu = submenu_alloc();
    submenu_set_header(app->diag_menu, "Diagnostics");
    submenu_add_item(app->diag_menu, "TX Test", ActionTxTest, submenu_callback, app);
    submenu_add_item(app->diag_menu, "RX Monitor", ActionRxMonitor, submenu_callback, app);
    submenu_add_item(app->diag_menu, "Wiring", ActionWiring, submenu_callback, app);
    view_dispatcher_add_view(app->view_dispatcher, ViewDiagnostics, submenu_get_view(app->diag_menu));

    app->pick_menu = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, ViewPicker, submenu_get_view(app->pick_menu));

    app->text_box = text_box_alloc();
    text_box_set_font(app->text_box, TextBoxFontText);
    view_dispatcher_add_view(app->view_dispatcher, ViewLog, text_box_get_view(app->text_box));

    // Send Gift screen: Gift type / picker / Send. Identity and region live in Settings.
    app->config_list = variable_item_list_alloc();
    variable_item_list_set_enter_callback(app->config_list, config_enter_callback, app);

    config_build(app);

    app->settings_list = variable_item_list_alloc();
    variable_item_list_set_enter_callback(app->settings_list, settings_enter_callback, app);
    settings_build(app);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewSettings, variable_item_list_get_view(app->settings_list));

    view_dispatcher_add_view(
        app->view_dispatcher, ViewConfig, variable_item_list_get_view(app->config_list));

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ViewTextInput, text_input_get_view(app->text_input));

    app->byte_input = byte_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, ViewByteInput, byte_input_get_view(app->byte_input));

    app->current_view = ViewMenu;
    return app;
}

static void app_free(App* app) {
    worker_stop(app);
    view_dispatcher_remove_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewLog);
    view_dispatcher_remove_view(app->view_dispatcher, ViewConfig);
    view_dispatcher_remove_view(app->view_dispatcher, ViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, ViewByteInput);
    view_dispatcher_remove_view(app->view_dispatcher, ViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, ViewDiagnostics);
    view_dispatcher_remove_view(app->view_dispatcher, ViewPicker);
    submenu_free(app->submenu);
    submenu_free(app->diag_menu);
    submenu_free(app->pick_menu);
    text_box_free(app->text_box);
    variable_item_list_free(app->config_list);
    variable_item_list_free(app->settings_list);
    text_input_free(app->text_input);
    byte_input_free(app->byte_input);
    view_dispatcher_free(app->view_dispatcher);
    notification_message(app->notif, &seq_led_off);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    furi_string_free(app->log);
    furi_mutex_free(app->log_mutex);
    free(app);
}

int32_t flipgift_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_run(app->view_dispatcher);
    app_free(app);
    return 0;
}
