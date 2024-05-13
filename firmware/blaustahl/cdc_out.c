/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tusb.h"
#include "pico/stdio_usb.h"

// these may not be set if the user is providing tud support (i.e. LIB_TINYUSB_DEVICE is 1 because
// the user linked in tinyusb_device) but they haven't selected CDC

#include "pico/binary_info.h"
#include "pico/time.h"
#include "pico/stdio/driver.h"
#include "hardware/irq.h"
#include "device/usbd_pvt.h" // for usbd_defer_func

#include "cdc_out.h"

static mutex_t stdio_usb_mutex;

void cdc_usb_out_chars(const char *buf, int length) {
    static uint64_t last_avail_time;
        for (int i = 0; i < length;) {
            int n = length - i;
            int avail = (int) tud_cdc_write_available();
            if (n > avail) n = avail;
            if (n) {
                int n2 = (int) tud_cdc_write(buf + i, (uint32_t)n);
                tud_task();
                tud_cdc_write_flush();
                i += n2;
                last_avail_time = time_us_64();
            } else {
                tud_task();
                tud_cdc_write_flush();
                if ((!tud_cdc_write_available() && time_us_64() > last_avail_time + PICO_STDIO_USB_STDOUT_TIMEOUT_US)) {
                    break;
                }
            }
        }
}

