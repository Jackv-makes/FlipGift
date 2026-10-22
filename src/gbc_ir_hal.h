// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nicolas Aubry (original bit-bang HAL)
// Copyright (C) 2026 @Jackv_makes (TFBS4711 adaptation)

/**
 * gbc_ir_hal.h - Bit-bang half-duplex RAW infrared HAL for talking to a
 * Game Boy Color's (un-modulated) IR port from a Flipper Zero, through the
 * FlippIR board's Vishay TFBS4711 IrDA transceiver.
 *
 * Why not furi_hal_infrared? That API is carrier-oriented and TX-xor-RX. The
 * GBC IR line is baseband on/off with us-precise, fast half-duplex turnarounds,
 * so we own the two GPIO pins directly.
 *
 * The TFBS4711 was never meant for this protocol, so two things are done here:
 * - TX: it clamps any TXD pulse of 23 us or more to 23..100 us of light and
 *   shuts its driver off past 100 us. Pulses under 23 us are reproduced
 *   exactly, so every mark is sent as a chain of short pulses
 *   (tx_max_pulse_us long, tx_gap_us apart). The GBC's slow receiver smears
 *   the gaps into one mark.
 * - RX: RXD gives one fixed ~2 us low pulse per rising light edge. Mark
 *   lengths carry no information; mg_protocol.c decodes by the period
 *   between leading edges.
 *
 * Timing model: TX and a single RX message run inside a critical section
 * (interrupts masked) so FreeRTOS/USB/audio cannot jitter the waveform. Keep
 * critical sections to one message; re-enable in the >=1 ms inter-message gaps.
 */
#pragma once

#include <furi_hal_gpio.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const GpioPin* tx_pin; // transceiver TXD
    const GpioPin* rx_pin; // transceiver RXD
    bool tx_active_high; // true: GPIO HIGH => LED on
    bool rx_active_low; // true: line LOW => IR edge seen
    uint32_t tx_max_pulse_us; // longest single TXD pulse; longer marks are chained
    uint32_t tx_gap_us; // gap between chained pulses
} GbcIrConfig;

/** FlippIR board: TFBS4711 TXD on pin 13 (PB6), RXD on pin 14 (PB7). */
GbcIrConfig gbc_ir_config_default(void);

/** Claim pins (takes USART1 from the expansion service) and configure GPIO. */
void gbc_ir_init(const GbcIrConfig* cfg);

/** Release pins. Safe to call once after gbc_ir_init. */
void gbc_ir_deinit(void);

// ---- Critical section around a TX message -------------------------------
// Mask/unmask interrupts for the duration of ONE transmitted message. Do NOT
// hold across the inter-message gaps (let the OS breathe there).
void gbc_ir_lock(void);
void gbc_ir_unlock(void);

// ---- Transmit primitives (call between lock/unlock) ---------------------
/** Light for `us` microseconds (a "mark"), chained if longer than tx_max_pulse_us. */
void gbc_ir_tx_mark(uint32_t us);
/** Dark for exactly `us` microseconds (a "space"). */
void gbc_ir_tx_space(uint32_t us);

// ---- Receive primitives -------------------------------------------------
/** True while RXD shows a pulse. */
bool gbc_ir_rx_is_mark(void);

/**
 * Measure one pulse of the requested polarity.
 *
 * Waits (up to MG_RX_EDGE_TIMEOUT_US) for the line to enter the `want_mark`
 * state, then measures how long it stays there (capped at MG_RX_PULSE_MAX_US).
 * With the TFBS4711 a mark is always ~2 us; the spaces carry the timing.
 *
 * @return width in microseconds; 0 if the state never began (edge timeout).
 */
uint32_t gbc_ir_rx_measure(bool want_mark);

/**
 * Wait until the line has shown NO mark for `quiet_us` continuously.
 *
 * @return true when quiet was achieved; false on `timeout_us` elapsed.
 */
bool gbc_ir_rx_wait_quiet(uint32_t quiet_us, uint32_t timeout_us);

/** Blocking OFF hold that still lets the OS run (used for inter-message gaps). */
void gbc_ir_gap(uint32_t us);

/** Free-running microsecond timestamp (wraps ~67 s; fine within one exchange). */
uint32_t gbc_ir_time_us(void);
