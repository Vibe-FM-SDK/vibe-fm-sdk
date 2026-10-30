/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP on VIBE: no USB device (the VIBE runtime brings MIDI in through sloop_vibe.c, straight into usb.c's
 * MIDI in queue). The serial interface engine is never on, so usb.c's paths stay idle. */
#pragma once
#define FM1_USB_SEEN 1                 /* (hal/fm1_flash.h: usb.c is in) */
#include <stdint.h>
#include "fm1_cc.h"
FM1_INLINE void fm1_usb_reset(void) {}
FM1_INLINE void fm1_usb_attach(void *ep0) { (void)ep0; }
FM1_INLINE void fm1_usb_off(void) {}
FM1_INLINE uint32_t fm1_usb_sie_on(void) { return 0; }
FM1_INLINE void fm1_usb_sie_wr_start(uint32_t r, uint32_t v) { (void)r; (void)v; }
FM1_INLINE void fm1_usb_sie_rd_start(uint32_t r) { (void)r; }
FM1_INLINE uint32_t fm1_usb_sie_done(void) { return 0x8000u; }
FM1_INLINE uint32_t fm1_usb_sie_data(void) { return 0; }
FM1_INLINE void fm1_usb_ep0_buf(void *p) { (void)p; }
FM1_INLINE void fm1_usb_ep_txbuf(uint32_t ep, void *p) { (void)ep; (void)p; }
FM1_INLINE void fm1_usb_ep_rxbuf(uint32_t ep, void *p) { (void)ep; (void)p; }
FM1_INLINE void fm1_usb_ep0_send(void *p, uint32_t n) { (void)p; (void)n; }
FM1_INLINE void fm1_usb_ep_send(uint32_t ep, void *p, uint32_t n) { (void)ep; (void)p; (void)n; }
FM1_INLINE void fm1_usb_ep4_txbuf(void *p) { (void)p; }
FM1_INLINE void fm1_usb_ep4_send(void *p, uint32_t n) { (void)p; (void)n; }
FM1_INLINE void fm1_usb_ep4_rxbuf(void *p) { (void)p; }
FM1_INLINE void fm1_usb_rx_sync(void) {}
FM1_INLINE void fm1_usb_ep_enable(uint32_t eps) { (void)eps; }
FM1_INLINE uint32_t fm1_usb_sof_take(void) { return 0; }
