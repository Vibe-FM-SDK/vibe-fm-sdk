/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP on VIBE (from zp12's simulator): the SPI NOR (the app's RAM driver API of firmware/hal/fm1_flash.h) on sim_flash, 1 MiB of RAM:
 * erase sets 0xFF, programming clears bits, the same address windows are refused. The factory kit is in the
 * image, as on the device; the store and the own-sample rooms start erased.
 *
 * This header comes after usb.c in felucca.c's includes: from here on the USB device is cut off (the simulator has
 * no USB host; usb.c's register-level paths are never compiled in). */
#pragma once
#include <stdint.h>
#include "fm1_xip.h"
#define FL_XIP(off)     FM1_XIP(off)
#define FL_DATA_LO      0x00097000u
#define FL_DATA_HI      0x000E0000u
#define FL_GLOB_LO      0x000FC000u
#define FL_GLOB_HI      0x000FF000u
#define FL_OTA_LO       0x000E0000u
#define FL_OTA_HI       0x000E5000u
#define FL_FM6_LO       0x000E5000u
#define FL_FM6_HI       0x000E7000u
#define FL_USR4_LO      0x000E7000u
#define FL_USR4_HI      0x000FB000u
/* felucca-bsp 1.2's names for the same map (fm1-base: the autosave A/B above the OTA staging, the extra data rooms) */
#define FL_AUTO_LO      0x000E5000u
#define FL_AUTO_HI      0x000E7000u
#define FL_XDATA0_LO    0x00093000u
#define FL_XDATA0_HI    0x00097000u
#define FL_XDATA1_LO    0x000E7000u
#define FL_XDATA1_HI    0x000FC000u
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && \
                               (uint32_t)(n) <= (hi) - (uint32_t)(off))
#define FL_STORE_OK(off, n) (FL_IN(off, n, FL_DATA_LO, FL_DATA_HI) || FL_IN(off, n, FL_GLOB_LO, FL_GLOB_HI) || \
                             FL_IN(off, n, FL_FM6_LO, FL_FM6_HI) || FL_IN(off, n, FL_USR4_LO, FL_USR4_HI))
#ifndef FL_RANGE_OK
#define FL_RANGE_OK(off, n) (FL_STORE_OK(off, n) || FL_IN(off, n, FL_OTA_LO, FL_OTA_HI))
#endif
#define FL_XDATA_OK(off, n) (FL_IN(off, n, FL_XDATA0_LO, FL_XDATA0_HI) || FL_IN(off, n, FL_XDATA1_LO, FL_XDATA1_HI))
#define FL_FAR(fn) (fn)
static uint32_t sim_flash_writes;               /* (counts erases and programs: the page keeps the store then) */
/* tools/fm1 (22.10.): every 4 KB sector written since the adapter last saved – the adapter keeps them on the VIBE
 * (vibe_store "nor/<sector>"), so a song saved on the fake FM-1 is still there after power off */
static uint32_t sim_dirty[0x100000u / 0x1000u / 32u];
#define SIM_DIRTY(off) (sim_dirty[((off) >> 12) >> 5] |= 1u << (((off) >> 12) & 31u))
static uint32_t fl_jedec_ram(void) { return 0x856014u; }
static uint32_t fl_status_ram(void) { return 0; }
static int fl_erase4k_ram(uint32_t off, uint32_t *took_us)
{
    uint32_t i;
    if (!FL_RANGE_OK(off, 0x1000u) || (off & 0xFFFu))
        return -1;
    for (i = 0; i < 0x1000u; i++) sim_flash[off + i] = 0xFF;
    sim_flash_writes++; SIM_DIRTY(off);
    *took_us = 45000u;
    return 0;
}
static int fl_prog_ram(uint32_t off, const uint8_t *src, uint32_t n, uint32_t *took_us)
{
    uint32_t i;
    if (!FL_RANGE_OK(off, n) || n == 0 || n > 256u || ((off & 0xFFu) + n) > 256u)
        return -1;
    for (i = 0; i < n; i++) sim_flash[off + i] &= src[i];
    sim_flash_writes++; SIM_DIRTY(off);
    *took_us = 700u;
    return 0;
}
static int fl_read_ram(uint32_t off, uint8_t *dst, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) dst[i] = sim_flash[(off + i) & 0xFFFFFu];
    return 0;
}
static void fl_plain_window_init(void) {}
static inline void fl_inval(uint32_t off, uint32_t len) { (void)off; (void)len; }   /* (no cache over sim_flash) */
static inline uint32_t irq_save(void) { return 0; }
static inline void irq_restore(uint32_t f) { (void)f; }
static int fl_erase4k(uint32_t off, uint32_t *took_us) { return fl_erase4k_ram(off, took_us); }
static int fl_write(uint32_t off, const uint8_t *src_ram, uint32_t n)
{
    uint32_t took;
    while (n) {
        uint32_t chunk = 256u - (off & 0xFFu);
        int rc;
        if (chunk > n) chunk = n;
        if ((rc = fl_prog_ram(off, src_ram, chunk, &took)) != 0) return rc;
        off += chunk; src_ram += chunk; n -= chunk;
    }
    return 0;
}
static int fl_verify_xip(uint32_t off, const uint8_t *ref, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) if (sim_flash[off + i] != ref[i]) return -(int)(i + 1);
    return 0;
}

/* no USB in the simulator (see the top) */
#ifdef FM1_USB_SEEN                    /* (usb.c came first, as in Felucca: its calls cut off from here; a firmware that has
                                          *  usb.c after this header – FiMba, X0X … – keeps them: they run on the idle stubs) */
#define usb_start() ((void)0)
#define usb_retry(t) ((void)(t))
#define usb_poll() ((void)0)
#define usb_detach() ((void)0)
#endif
