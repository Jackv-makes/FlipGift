// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nicolas Aubry (original timings)
// Copyright (C) 2026 @Jackv_makes (TFBS4711 receiver thresholds)

/**
 * mg_timing.h - Timing and protocol constants for the Gen II Mystery Gift IR protocol.
 *
 * Send timings are the forum-measured targets that the game's receiver decodes
 * (projectpokemon RE thread), cross-checked against pokecrystal
 * engine/link/mystery_gift.asm. They are the *starting* values -- expect to
 * fine-tune on a scope during bring-up. The game's receiver thresholds bits by
 * SPACE length, so there is slack.
 */
#pragma once

// ---- Line-level timings (microseconds) ----------------------------------
// Hello message: mark, space, mark.
#define MG_HELLO_MARK_US 126
#define MG_HELLO_SPACE_US 537

// Data message framing.
#define MG_PREAMBLE_MARK_US 126
#define MG_PREAMBLE_SPACE_US 725
#define MG_POSTAMBLE_MARK_US 109
#define MG_POSTAMBLE_SPACE_US 620

// Per-bit encoding (MSB first): a fixed mark, then a space whose length is the bit.
#define MG_BIT_MARK_US 52
#define MG_BIT0_SPACE_US 120
#define MG_BIT1_SPACE_US 299

// ---- RX classifier: periods between TFBS4711 leading-edge pulses ---------
// The transceiver emits one ~2 us pulse per rising light edge, so a period
// is the cart's mark plus its following space. Measured against a US Gold
// cart: bit 0 ~175, bit 1 ~355, hello 731, postamble->next 731-771,
// preamble 931. Thresholds sit between those.
#define MG_EDGE_BIT0_DARK_MAX_US 265
#define MG_EDGE_BIT1_DARK_MAX_US 520
#define MG_EDGE_HELLO_DARK_MAX_US 830
#define MG_RX_DARK_MAX_US 1400 // longer = junk (idle, our own TX tail)
// A second leading edge sooner than this after the last one is the receiver
// re-triggering inside one cart mark (seen: 97 us), not a new pulse. The
// shortest real period is a bit 0 at ~172 us.
#define MG_EDGE_DEADTIME_US 120
// After a pulse, how long to wait for the next one before calling the
// message finished. A data bit's next edge comes within ~366 us; the next
// message's preamble pulse comes ~731 us after a postamble and MUST be
// caught inside this (IRQ-masked) wait: on return the OS catches up on
// ~0.7 ms of deferred interrupts and a 2 us pulse arriving then is lost
// (seen with 700 us). Waiting the full 5 ms element timeout instead broke
// the pass-2 handshake: the cart listens for our next hello for only
// 7.8 ms after it ACKs the tail block.
#define MG_EDGE_END_WAIT_US 1000
// Reference wait budgets: 50 ms to find a hello/message, 5 ms per element.
#define MG_RX_WINDOW_US 50000
#define MG_RX_ELEMENT_US 5000

// Gap discipline, mirrored from the proven RPi implementation (mysterygift.c):
// - 3 ms of LED-off before EVERY hello (SND_WAIT).
// - 2 ms of LED-off before every data block and before every ACK we transmit.
//   The game holds its LED off for 61 ticks (~1.86 ms) after a hello/block
//   before it starts listening, so anything under ~1.9 ms lands in a deaf
//   window; anything over ~7.8 ms trips the game's per-phase receive timeout.
// - Between the two payload passes: 2 ms + the hello's own 3 ms lead = 5 ms
//   total, comfortably inside the game's 7.8 ms hello wait.
// - The three messages inside a block run back-to-back (their own ~620 us
//   trailing space is the only separation).
#define MG_HELLO_LEAD_US 3000
#define MG_BLOCK_LEAD_US 2000
#define MG_INTER_MSG_GAP_US 1500 // used by the TX test screen only
#define MG_INTER_PASS_GAP_US 2000

// Opening trigger (RPi flow): wait for the GAME's hello (player presses A),
// ignore it, wait 50 ms for the game's sender attempt to time out and fall
// back to listening, then claim master.
#define MG_TRIGGER_WAIT_US 50000
// The game's idle screen polls its IR sensor every ~100 us (joypad reads in the
// loop), so a single 126 us hello mark can be missed -- retry the opening hello.
#define MG_HELLO_ATTEMPTS 10

// ---- RX timeouts (microseconds) -----------------------------------------
// Generous bounds; the game retries hello for ~4 s if it hears nothing.
#define MG_RX_EDGE_TIMEOUT_US 30000 // wait for the next mark/space to begin
#define MG_RX_PULSE_MAX_US 4000 // cap on how long a single mark/space may last

// ---- Protocol bytes ------------------------------------------------------
#define MG_MESSAGE_PREFIX 0x5A // starts message 1 of every data block
#define MG_REGION_PREFIX 0x96 // region-exchange prefix (region-independent)
#define MG_NAME_CARD_PREFIX 0x3C // (Name Card flow, not implemented in v1)

// Region codes (the game's own region; used in the region-code exchange).
#define MG_REGION_CODE_USA 0x90
#define MG_REGION_CODE_ITA 0x99
#define MG_REGION_CODE_FRA 0x9A
#define MG_REGION_CODE_GER 0x9F
#define MG_REGION_CODE_SPA 0x96

// Version byte (first byte of payload 1).
#define MG_VERSION_GOLD_CRYSTAL 0x01
#define MG_VERSION_SILVER 0x02
#define MG_VERSION_PIKACHU2 0x03

// ACK / status byte. MG_OKAY = ~(WRONG_CHECKSUM|TIMED_OUT|CANCELED|WRONG_PREFIX)
// = ~0x93 = 0x6C.
#define MG_ACK_OKAY 0x6C
