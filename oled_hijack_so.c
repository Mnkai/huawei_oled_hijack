/*
 * Advanced OLED menu for Huawei E5372 portable LTE router.
 * 
 * Compile:
 * arm-linux-androideabi-gcc -shared -ldl -fPIC -O2 -s -o oled_hijack.so oled_hijack_so.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE_INFORMATION 1
#define SUBSYSTEM_GPIO 21002
#define EVT_OLED_WIFI_WAKEUP 14026
#define EVT_DIALUP_REPORT_CONNECT_STATE 4037
#define DIAL_STATE_CONNECTING 900
#define BUTTON_POWER 8
#define BUTTON_MENU 9
#define LED_ON 0

#define OLED_CUSTOM "/online/oled_custom.sh"

/* 
 * Variables from "oled" binary.
 * 
 * g_current_page is a current menu page. -1 for main screen, 0 for main
 * menu screen, 1 for information page.
 * 
 * g_current_Info_page is a pointer to current visible page in the
 * information screen.
 * 
 * Current values are based on E5372 oled binary.
 * MD5: eb4e65509e16c2023f4f9a5e00cd0785
 * 
 */
static uint32_t *g_current_page = (uint32_t*)(0x00029f94);
static uint32_t *g_current_Info_page = (uint32_t*)(0x0002CAB8);
static uint32_t *g_led_status = (uint32_t*)(0x00029FA8);

static uint32_t first_info_screen = 0;

// Do not send BUTTON_PRESSED event notify to "oled" if set.
// something like a mutex, but we don't really care about
// thread-safe as we only care of atomic writes which are
// in the same thread.
static int lock_buttons = 0;

static int current_infopage_item = 0;
static int custom_script_enabled = -1;

/*
 * Real handlers from oled binary and libraries
 */
static int (*register_notify_handler_real)(int subsystemid, void *notify_handler_sync, void *notify_handler_async_orig) = NULL;
static int (*notify_handler_async_real)(int subsystemid, int action, int subaction) = NULL;

/* 
 * Menu-related configuration
 */

static const char *scripts[] = {
    "/app/bin/oled_hijack/radio_mode.sh",
    "/app/bin/oled_hijack/ttlfix.sh",
    "/app/bin/oled_hijack/anticensorship.sh",
    "/app/bin/oled_hijack/imei_change.sh",
    "/app/bin/oled_hijack/remote_access.sh",
    "/app/bin/oled_hijack/no_battery.sh",
    "/app/bin/oled_hijack/usb_mode.sh",
    OLED_CUSTOM,
    NULL
};

static const char *network_mode_mapping[] = {
    // 0
    "Auto",
    // 1
    "GSM Only",
    "UMTS Only",
    "LTE Only",
    "LTE -> UMTS",
    "LTE -> GSM",
    // 6
    "UMTS -> GSM",
    NULL
};

static const char *ttlfix_mapping[] = {
    // 0
    "Disabled",
    // 1
    "TTL=64",
    // 2
    "TTL=128",
    // 3
    "TTL=65 (WiFi Ext.)",
    NULL
};

static const char *imei_change_mapping[] = {
    // 0
    "Stock",
    // 1
    "Random Android",
    // 2
    "Random WinPhone",
    NULL
};

static const char *remote_access_mapping[] = {
    // 0
    "Web & Telnet",
    // 1
    "Web only",
    // 2
    "Web, Telnet, ADB",
    // 3
    "Telnet & ADB only",
    // 4
    "All disabled",
    NULL
};

static const char *usb_mode_mapping[] = {
    // 0
    "Stock",
    // 1
    "AT, Network, SD",
    // 2
    "AT, Network",
    // 3
    "Debug mode",
    NULL
};

static const char *enabled_disabled_mapping[] = {
    // 0
    "Disabled",
    // 1
    "Enabled",
    NULL
};

struct menu_s {
    uint8_t radio_mode;
    uint8_t ttlfix;
    uint8_t anticensorship;
    uint8_t imei_change;
    uint8_t remote_access;
    uint8_t no_battery;
    uint8_t usb_mode;
    uint8_t custom;
} menu_state;

/* *************************************** */

#define LOCKBUTTONS(x) (x ? (lock_buttons = 1) : (lock_buttons = 0))

/* 
 * Execute shell script and return exit code.
 */
static int call_script(char const *script, char const *additional_argument) {
    int ret;
    char arg_buff[1024];

    if (additional_argument) {
        snprintf(arg_buff, sizeof(arg_buff) - 1, "%s %s",
                 script, additional_argument);
    }
    else {
        snprintf(arg_buff, sizeof(arg_buff) - 1, "%s",
                 script);
    }

    fprintf(stderr, "Calling script: %s\n", arg_buff);
    ret = system(arg_buff);
    if (WIFSIGNALED(ret) &&
        (WTERMSIG(ret) == SIGINT || WTERMSIG(ret) == SIGQUIT))
        return -1;

    fprintf(stderr, "GOT RET: %d\n", WEXITSTATUS(ret));
    return WEXITSTATUS(ret);
}

/*
 * Call every script in scripts array and update
 * menu_state struct.
 * Called in sprintf.
 */
static void update_menu_state() {
    int i, ret;

    for (i = 0; scripts[i] != NULL; i++) {
        ret = call_script(scripts[i], "get");
        switch (i) {
            case 0:
                menu_state.radio_mode = ret;
                break;
            case 1:
                menu_state.ttlfix = ret;
                break;
            case 2:
                menu_state.anticensorship = ret;
                break;
            case 3:
                menu_state.imei_change = ret;
                break;
            case 4:
                menu_state.remote_access = ret;
                break;
            case 5:
                menu_state.no_battery = ret;
                break;
            case 6:
                menu_state.usb_mode = ret;
                break;
            case 7:
                menu_state.custom = ret;
                break;
        }
    }
}

/*
 * Hijacked page button handler.
 * Executes corresponding script with "set_next" argument.
 * Called when the POWER button is pressed on hijacked page.
 */
static void handle_menu_state_change(int menu_page) {
    call_script(scripts[menu_page], "set_next");
}

/*
 * Create menu of 3 items.
 * Only for E5372 display.
 */
static void create_menu_item(char *buf, const char *mapping[], int current_item) {
    int i, char_list_size = 0;
    char nothing[2] = "";

    for (i = 0; mapping[i] != NULL; i++) {
        char_list_size++;
    }

    fprintf(stderr, "Trying to create menu\n");

    if (current_item == 0) {
        snprintf(buf, 1024 - 1,
             "  > %s\n    %s\n    %s\n",
             (mapping[current_item]),
             ((char_list_size >= 2) ? mapping[current_item + 1] : nothing),
             ((char_list_size >= 3) ? mapping[current_item + 2] : nothing)
        );
    }
    else if (current_item == char_list_size - 1 && char_list_size >= 3) {
        snprintf(buf, 1024 - 1,
             "    %s\n    %s\n  > %s\n",
             ((current_item >= 2 && char_list_size > 2) ? mapping[current_item - 2] : nothing),
             ((current_item >= 1 && char_list_size > 1) ? mapping[current_item - 1] : nothing),
             (mapping[current_item])
        );
    }
    else if (current_item <= char_list_size) {
        snprintf(buf, 1024 - 1,
            "    %s\n  > %s\n    %s\n",
            ((current_item > 0) ? mapping[current_item - 1] : nothing),
            (mapping[current_item]),
            ((current_item < char_list_size && mapping[current_item + 1]) \
                                    ? mapping[current_item + 1] : nothing)
        );
    }
    else {
        snprintf(buf, 1024 - 1,
            "    ERROR\n\n\n");
    }
}

/* 
 * Function which presses buttons to leave information page
 * and enter it again, to force redraw.
 * Very dirty, but works.
 * 
 * Assuming information page is a first menu item.
 * 
 */
static void leave_and_enter_menu(int advance) {
    int i;

    *g_current_Info_page = 0;
    // selecting "back"
    notify_handler_async_real(SUBSYSTEM_GPIO, BUTTON_MENU, 0);
    // pressing "back"
    notify_handler_async_real(SUBSYSTEM_GPIO, BUTTON_POWER, 0);
    // selecting "device information"
    notify_handler_async_real(SUBSYSTEM_GPIO, BUTTON_MENU, 0);
    // pressing "device information"
    notify_handler_async_real(SUBSYSTEM_GPIO, BUTTON_POWER, 0);

    // advancing to the exact page we were on
    for (i = 0; i <= advance; i++) {
        notify_handler_async_real(SUBSYSTEM_GPIO, BUTTON_MENU, 0);
    }
}

static int notify_handler_async(int subsystemid, int action, int subaction) {
    fprintf(stderr, "notify_handler_async: %d, %d, %x\n", subsystemid, action, subaction);

    if (subsystemid == EVT_OLED_WIFI_WAKEUP) {
        // Do NOT notify "oled" of EVT_OLED_WIFI_WAKEUP event.
        // Fixes "exiting sleep mode" on every button
        // if Wi-Fi is completely disabled in web interface.
        return 0;
    }

    else if (*g_current_page == PAGE_INFORMATION &&
        subsystemid == EVT_DIALUP_REPORT_CONNECT_STATE &&
        action == DIAL_STATE_CONNECTING) {
        // Do NOT notify "oled" of EVT_DIALUP_REPORT_CONNECT_STATE
        // with action=DIAL_STATE_CONNECTING while on info page.
        // We do not want to draw animations in the middle of network
        // change from the menu.
        return 0;
    }
    
    if (*g_current_page == PAGE_INFORMATION) {
        if (first_info_screen && first_info_screen != *g_current_Info_page) {
            if (subsystemid == SUBSYSTEM_GPIO && *g_led_status == LED_ON) {
                fprintf(stderr, "We're not on a main info screen!\n");
                if (lock_buttons) {
                    // Do NOT notify "oled" of button events
                    // if buttons are locked by slow script
                    return 0;
                }
                if (action == BUTTON_POWER) {
                    // button pressed
                    fprintf(stderr, "BUTTON PRESSED!\n");
                    // lock buttons to prevent user intervention
                    LOCKBUTTONS(1);
                    handle_menu_state_change(current_infopage_item);
                    leave_and_enter_menu(current_infopage_item);
                    LOCKBUTTONS(0);
                    return notify_handler_async_real(subsystemid, BUTTON_MENU, subaction);
                }
                else if (action == BUTTON_MENU) {
                    current_infopage_item++;
                }
            }
        }
        else {
            current_infopage_item = 0;
        }
    }

    return notify_handler_async_real(subsystemid, action, subaction);
}

/*
 * Hijacked functions from various libraries.
 */

int register_notify_handler(int subsystemid, void *notify_handler_sync, void *notify_handler_async_orig) {
    if (!register_notify_handler_real) {
        register_notify_handler_real = dlsym(RTLD_NEXT, "register_notify_handler");
    }
    //fprintf(stderr, "register_notify_handler: %d, %d, %d\n", a1, a2, a3);
    notify_handler_async_real = notify_handler_async_orig;
    return register_notify_handler_real(subsystemid, notify_handler_sync, notify_handler_async);
}

int sprintf(char *str, const char *format, ...) {
    int i = 0, j = 0;
    char network_mode_buf[1024];
    char ttlfix_buf[1024];
    char anticensorship_buf[1024];
    char imei_change_buf[1024];
    char remote_access_buf[1024];
    char no_battery_buf[1024];
    char usb_mode_buf[1024];
    static char custom_buf[1024];
    static char custom_text_buf[] = "# Custom Script:\n";

    va_list args;
    va_start(args, format);
    i = vsprintf(str, format, args);
    va_end(args);
    
    if (format && (strcmp(format, "SSID: %s\n") == 0 ||
        strncmp(str, "SSID0: ", 7) == 0 ||
        strcmp(format, "PWD: %s\n") == 0 ||
        strncmp(str, "PWD0: ", 6) == 0)) {
            va_start(args, format);
            i = vsnprintf(str, 20, format, args);
            str[19] = '\0';
            va_end(args);
    }
    else if (format && (strncmp(str, "SSID1: ", 7) == 0 ||
        strncmp(str, "PWD1: ", 6) == 0)) {
            i = snprintf(str, 2, "");
    }

    // Hijacking "Homepage: %s" string on second information page
    if (format && strcmp(format, "Homepage: %s") == 0) {
        fprintf(stderr, "FOUND STRING!\n");
        update_menu_state();
        create_menu_item(network_mode_buf, network_mode_mapping, menu_state.radio_mode);
        create_menu_item(ttlfix_buf, ttlfix_mapping, menu_state.ttlfix);
        create_menu_item(anticensorship_buf, enabled_disabled_mapping, menu_state.anticensorship);
        create_menu_item(imei_change_buf, imei_change_mapping, menu_state.imei_change);
        create_menu_item(remote_access_buf, remote_access_mapping, menu_state.remote_access);
        create_menu_item(no_battery_buf, enabled_disabled_mapping, menu_state.no_battery);
        create_menu_item(usb_mode_buf, usb_mode_mapping, menu_state.usb_mode);

        if (custom_script_enabled == -1) {
            if (access(OLED_CUSTOM, F_OK) == 0) {
                custom_script_enabled = 1;
            }
            else {
                // Disable custom script support
                custom_script_enabled = 0;
                custom_buf[0] = '\0';
                custom_text_buf[0] = '\0';
                for (j = 0; scripts[j] != NULL; j++) {}
                scripts[j-1] = NULL;
            }
        }

        if (custom_script_enabled) {
            create_menu_item(custom_buf, enabled_disabled_mapping, menu_state.custom);
        }

        snprintf(str, 999,
                 "# Network Mode:\n%s" \
                 "# TTL Mangling:\n%s" \
                 "# Anticensorship:\n%s" \
                 "# Device IMEI:\n%s" \
                 "# Remote Access:\n%s" \
                 "# Work w/o Battery:\n%s" \
                 "# USB Mode:\n%s%s%s",
                 network_mode_buf,
                 ttlfix_buf,
                 anticensorship_buf,
                 imei_change_buf,
                 remote_access_buf,
                 no_battery_buf,
                 usb_mode_buf,
                 custom_text_buf,
                 custom_buf
        );
        //fprintf(stderr, "%s\n",);
    }
    fprintf(stderr, "sprintf %s\n", format);
    return i;
}

int osa_print_log_ex(char *subsystem, char *sourcefile, int line,
                     int offset, const char *message, ...) {
    /*va_list args;
    va_start(args, message);
    fprintf(stderr, "[%s] %s (%d): ", subsystem, sourcefile, line);
    vprintf(message, args);
    va_end(args);*/

    if (*g_current_page == PAGE_INFORMATION) {
        if (!first_info_screen) {
            first_info_screen = *g_current_Info_page;
            fprintf(stderr, "Saved first screen address!\n");
        }
    }
    else {
        first_info_screen = 0;
    }

    return 0;
}
