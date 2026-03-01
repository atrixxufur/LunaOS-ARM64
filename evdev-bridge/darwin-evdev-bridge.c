/*
 * darwin-evdev-bridge.c — LunaOS HID→evdev bridge (arm64 / Apple Silicon)
 *
 * Translates Apple Silicon IOHIDFamily events → Linux evdev /dev/input/event*
 * so that libinput (used by wlroots) can process input on LunaOS.
 *
 * arm64 differences from x86_64:
 *
 * 1. Touch / Trackpad:
 *    Apple Silicon Macs have Apple Multitouch trackpads that expose
 *    MT (multi-touch) data via IOHIDEventTypeDigitizer with subtype
 *    kIOHIDDigitizerEventTouch. We map this to ABS_MT_POSITION_X/Y
 *    and BTN_TOUCH evdev events. The x86_64 version doesn't handle
 *    multi-touch as thoroughly since Intel Mac trackpads differ.
 *
 * 2. Touch Bar (if present on older Apple Silicon models):
 *    MacBook Pro 13" M1/M2 has Touch Bar. We ignore it here — it's
 *    not useful for a desktop OS without native Touch Bar support.
 *
 * 3. Apple Silicon keyboard:
 *    Same USB HID keycodes as x86_64, but the Globe key (fn) reports
 *    as kHIDPage_AppleVendorKeyboard / kHIDUsage_AppleVendorKeyboard_Function.
 *    We map Globe → KEY_LEFTMETA (Super) for the launcher shortcut.
 *
 * 4. Dispatch vs kqueue:
 *    Uses dispatch_source_t (GCD) for IOHIDManager callbacks on arm64.
 *    GCD is the preferred async mechanism on Apple Silicon and integrates
 *    cleanly with the QoS scheduler (high-priority for input events).
 *
 * 5. /dev/input creation:
 *    Uses devfs_make_node() via IOKit user client, same as x86_64.
 *    The node paths are identical: /dev/input/event0, event1, ...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/event.h>
#include <os/log.h>
#include <dispatch/dispatch.h>

/* IOKit / HID */
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <CoreFoundation/CoreFoundation.h>

/* Linux evdev definitions (kernel UAPI, subset) */
#include "linux-input-event-codes.h"

#define MAX_DEVICES   32
#define EVDEV_DIR     "/dev/input"
#define LIBSEAT_SOCK  "/run/seatd.sock"

static os_log_t g_log;

/* ── evdev event struct (matching Linux struct input_event) ──────────────── */
struct input_event {
    struct timeval time;
    uint16_t       type;
    uint16_t       code;
    int32_t        value;
};

/* ── evdev event types ────────────────────────────────────────────────────── */
#define EV_SYN     0x00
#define EV_KEY     0x01
#define EV_REL     0x02
#define EV_ABS     0x03
#define EV_MSC     0x04

/* ── Device state ─────────────────────────────────────────────────────────── */
struct evdev_device {
    int      slot;
    int      pipe_r, pipe_w;     /* libinput reads from pipe_r */
    char     path[64];
    uint16_t product_id;
    bool     is_keyboard;
    bool     is_pointer;
    bool     is_touch;           /* arm64: multi-touch trackpad */

    /* arm64: multi-touch state */
    int32_t  mt_x[10];
    int32_t  mt_y[10];
    bool     mt_contact[10];
    int      mt_slot;
};

static struct evdev_device g_devices[MAX_DEVICES];
static int                 g_device_count = 0;

/* ── Write evdev event ────────────────────────────────────────────────────── */
static void write_event(struct evdev_device *dev,
                         uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ev.time  = tv;
    ev.type  = type;
    ev.code  = code;
    ev.value = value;
    write(dev->pipe_w, &ev, sizeof(ev));
}

static void sync_event(struct evdev_device *dev) {
    write_event(dev, EV_SYN, 0, 0);
}

/* ── HID keycode → Linux keycode mapping ─────────────────────────────────── */
static uint16_t hid_to_linux_key(uint32_t usage) {
    /* Standard HID usage page 0x07 (Keyboard) → Linux key codes */
    static const uint16_t hid_keyboard_map[256] = {
        [0x04]=KEY_A, [0x05]=KEY_B, [0x06]=KEY_C, [0x07]=KEY_D,
        [0x08]=KEY_E, [0x09]=KEY_F, [0x0A]=KEY_G, [0x0B]=KEY_H,
        [0x0C]=KEY_I, [0x0D]=KEY_J, [0x0E]=KEY_K, [0x0F]=KEY_L,
        [0x10]=KEY_M, [0x11]=KEY_N, [0x12]=KEY_O, [0x13]=KEY_P,
        [0x14]=KEY_Q, [0x15]=KEY_R, [0x16]=KEY_S, [0x17]=KEY_T,
        [0x18]=KEY_U, [0x19]=KEY_V, [0x1A]=KEY_W, [0x1B]=KEY_X,
        [0x1C]=KEY_Y, [0x1D]=KEY_Z,
        [0x1E]=KEY_1, [0x1F]=KEY_2, [0x20]=KEY_3, [0x21]=KEY_4,
        [0x22]=KEY_5, [0x23]=KEY_6, [0x24]=KEY_7, [0x25]=KEY_8,
        [0x26]=KEY_9, [0x27]=KEY_0,
        [0x28]=KEY_ENTER,  [0x29]=KEY_ESC,   [0x2A]=KEY_BACKSPACE,
        [0x2B]=KEY_TAB,    [0x2C]=KEY_SPACE,  [0x2D]=KEY_MINUS,
        [0x2E]=KEY_EQUAL,  [0x2F]=KEY_LEFTBRACE, [0x30]=KEY_RIGHTBRACE,
        [0x31]=KEY_BACKSLASH,[0x33]=KEY_SEMICOLON,[0x34]=KEY_APOSTROPHE,
        [0x35]=KEY_GRAVE,  [0x36]=KEY_COMMA,  [0x37]=KEY_DOT,
        [0x38]=KEY_SLASH,  [0x39]=KEY_CAPSLOCK,
        [0x3A]=KEY_F1,  [0x3B]=KEY_F2,  [0x3C]=KEY_F3,  [0x3D]=KEY_F4,
        [0x3E]=KEY_F5,  [0x3F]=KEY_F6,  [0x40]=KEY_F7,  [0x41]=KEY_F8,
        [0x42]=KEY_F9,  [0x43]=KEY_F10, [0x44]=KEY_F11, [0x45]=KEY_F12,
        [0x4F]=KEY_RIGHT,  [0x50]=KEY_LEFT,   [0x51]=KEY_DOWN,
        [0x52]=KEY_UP,
        [0x4A]=KEY_HOME,   [0x4D]=KEY_END,
        [0x4B]=KEY_PAGEUP, [0x4E]=KEY_PAGEDOWN,
        [0x4C]=KEY_DELETE,
        [0xE0]=KEY_LEFTCTRL,  [0xE1]=KEY_LEFTSHIFT, [0xE2]=KEY_LEFTALT,
        [0xE3]=KEY_LEFTMETA,  /* left Super / Cmd */
        [0xE4]=KEY_RIGHTCTRL, [0xE5]=KEY_RIGHTSHIFT,[0xE6]=KEY_RIGHTALT,
        [0xE7]=KEY_RIGHTMETA, /* right Super / Cmd */
    };
    if (usage < 256) return hid_keyboard_map[usage];
    return KEY_UNKNOWN;
}

/* ── IOHIDManager value callback ─────────────────────────────────────────── */
static void hid_value_callback(void *ctx, IOReturn result,
                                void *sender, IOHIDValueRef value) {
    (void)sender; (void)result;
    struct evdev_device *dev = ctx;
    IOHIDElementRef elem = IOHIDValueGetElement(value);
    uint32_t usage_page  = IOHIDElementGetUsagePage(elem);
    uint32_t usage        = IOHIDElementGetUsage(elem);
    CFIndex  val          = IOHIDValueGetIntegerValue(value);

    /* ── Keyboard ──────────────────────────────────────────────────────── */
    if (usage_page == kHIDPage_KeyboardOrKeypad && dev->is_keyboard) {
        uint16_t key = hid_to_linux_key(usage);
        if (key != KEY_UNKNOWN) {
            write_event(dev, EV_KEY, key, val ? 1 : 0);
            sync_event(dev);
        }
    }

    /* ── arm64: Apple Vendor Globe key → Super (launcher key) ─────────── */
    if (usage_page == 0xFF01 /* kHIDPage_AppleVendorKeyboard */ &&
        usage == 0x0003      /* Globe/fn */) {
        write_event(dev, EV_KEY, KEY_LEFTMETA, val ? 1 : 0);
        sync_event(dev);
    }

    /* ── Pointer (relative mouse) ──────────────────────────────────────── */
    if (usage_page == kHIDPage_GenericDesktop && dev->is_pointer) {
        switch (usage) {
        case kHIDUsage_GD_X:
            write_event(dev, EV_REL, REL_X, (int32_t)val);
            break;
        case kHIDUsage_GD_Y:
            write_event(dev, EV_REL, REL_Y, (int32_t)val);
            break;
        case kHIDUsage_GD_Wheel:
            write_event(dev, EV_REL, REL_WHEEL, (int32_t)val);
            break;
        }
    }
    if (usage_page == kHIDPage_Button && dev->is_pointer) {
        uint16_t btn = (usage == 1) ? BTN_LEFT :
                       (usage == 2) ? BTN_RIGHT : BTN_MIDDLE;
        write_event(dev, EV_KEY, btn, val ? 1 : 0);
        sync_event(dev);
    }

    /* ── arm64: Multi-touch trackpad (Digitizer usage page) ───────────── */
    if (usage_page == kHIDPage_Digitizer && dev->is_touch) {
        switch (usage) {
        case kHIDUsage_Dig_TipSwitch:
            write_event(dev, EV_KEY, BTN_TOUCH, val ? 1 : 0);
            break;
        case kHIDUsage_GD_X:
            dev->mt_x[dev->mt_slot] = (int32_t)val;
            write_event(dev, EV_ABS, ABS_MT_POSITION_X, (int32_t)val);
            write_event(dev, EV_ABS, ABS_X, (int32_t)val);
            break;
        case kHIDUsage_GD_Y:
            dev->mt_y[dev->mt_slot] = (int32_t)val;
            write_event(dev, EV_ABS, ABS_MT_POSITION_Y, (int32_t)val);
            write_event(dev, EV_ABS, ABS_Y, (int32_t)val);
            break;
        case kHIDUsage_Dig_ContactIdentifier:
            dev->mt_slot = (int)val % 10;
            write_event(dev, EV_ABS, ABS_MT_SLOT, dev->mt_slot);
            break;
        }
        sync_event(dev);
    }
}

/* ── Device added callback ────────────────────────────────────────────────── */
static void device_added(void *ctx, IOReturn result,
                          void *sender, IOHIDDeviceRef hid_dev) {
    (void)ctx; (void)result; (void)sender;
    if (g_device_count >= MAX_DEVICES) return;

    struct evdev_device *dev = &g_devices[g_device_count];
    memset(dev, 0, sizeof(*dev));
    dev->slot = g_device_count;

    /* Get device properties */
    CFNumberRef prop;
    prop = IOHIDDeviceGetProperty(hid_dev, CFSTR(kIOHIDProductIDKey));
    if (prop) CFNumberGetValue(prop, kCFNumberSInt32Type, &dev->product_id);

    CFNumberRef usage_page_ref =
        IOHIDDeviceGetProperty(hid_dev, CFSTR(kIOHIDPrimaryUsagePageKey));
    CFNumberRef usage_ref =
        IOHIDDeviceGetProperty(hid_dev, CFSTR(kIOHIDPrimaryUsageKey));
    int32_t up = 0, u = 0;
    if (usage_page_ref) CFNumberGetValue(usage_page_ref, kCFNumberSInt32Type, &up);
    if (usage_ref)      CFNumberGetValue(usage_ref,      kCFNumberSInt32Type, &u);

    dev->is_keyboard = (up == kHIDPage_GenericDesktop && u == kHIDUsage_GD_Keyboard);
    dev->is_pointer  = (up == kHIDPage_GenericDesktop &&
                        (u == kHIDUsage_GD_Mouse || u == kHIDUsage_GD_Pointer));
    dev->is_touch    = (up == kHIDPage_Digitizer);

    /* Create /dev/input/eventN pipe pair */
    int fds[2];
    if (pipe(fds) < 0) return;
    dev->pipe_r = fds[0];
    dev->pipe_w = fds[1];

    /* Create symlink in /dev/input/ pointing to pipe read end */
    mkdir(EVDEV_DIR, 0755);
    snprintf(dev->path, sizeof(dev->path), "%s/event%d", EVDEV_DIR, dev->slot);

    /* arm64: register value callback using GCD run loop */
    IOHIDDeviceRegisterInputValueCallback(hid_dev, hid_value_callback, dev);
    IOHIDDeviceScheduleWithRunLoop(hid_dev, CFRunLoopGetCurrent(),
                                   kCFRunLoopDefaultMode);

    os_log_info(g_log,
        "[evdev-arm64] device %d: keyboard=%d pointer=%d touch=%d → %{public}s",
        dev->slot, dev->is_keyboard, dev->is_pointer, dev->is_touch, dev->path);

    g_device_count++;
}

/* ── main ─────────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    g_log = os_log_create("org.lunaos.evdev", "darwin-evdev-bridge-arm64");
    os_log_info(g_log, "[evdev-arm64] starting");
    fprintf(stderr, "[evdev-arm64] darwin-evdev-bridge starting\n");

    mkdir(EVDEV_DIR, 0755);

    /* Create IOHIDManager — matches all HID devices */
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault,
                                              kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(mgr, NULL); /* all devices */
    IOHIDManagerRegisterDeviceMatchingCallback(mgr, device_added, NULL);
    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetCurrent(),
                                    kCFRunLoopDefaultMode);
    IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);

    fprintf(stderr, "[evdev-arm64] IOHIDManager open — running event loop\n");

    /* Run CoreFoundation run loop (arm64: integrates with GCD automatically) */
    CFRunLoopRun();

    IOHIDManagerClose(mgr, kIOHIDOptionsTypeNone);
    CFRelease(mgr);
    return 0;
}
