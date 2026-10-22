# Flipgift design notes

How a Gen II Mystery Gift gets from a Flipper Zero to a Game Boy Color through
a part that was never meant to carry it. The original analog-front-end design
this grew out of is in [docs/upstream-DESIGN.md](docs/upstream-DESIGN.md); the
protocol section there is still the reference for message framing, checksums
and the two-pass exchange, and is not repeated here.

## 1. The link in one paragraph

The Game Boy's IR port is baseband on/off light. A message is a long preamble
mark, a long space, then one mark per bit whose following space is short for a
0 and long for a 1, then a long postamble mark and space. A hello is two long
marks with a gap. Both sides take turns as master, each block of three messages
is answered with a one-byte ACK, and the cart gives up on any wait after a
timer-based window. Everything below is about getting those marks and spaces
through a TFBS4711.

## 2. What the TFBS4711 does to the signal

From the Vishay datasheet (82633, rev 3.9), confirmed on the bench:

| Direction | Behaviour | Consequence |
|---|---|---|
| TX | A TXD pulse under 23 µs is reproduced within ±0.15 µs. | Short pulses are exact. |
| TX | A TXD pulse of 23 µs or more produces 23 to 100 µs of light, typically 50. | A 52 µs bit mark or 126 µs hello mark driven directly comes out at an unknown length. |
| TX | TXD held past 100 µs disables the LED driver until it is released. | A 126 µs mark driven directly is cut off. |
| RX | RXD emits one low pulse of 1.7 to 3 µs on each rising light edge, independent of the light's duration. | Mark lengths are invisible. Only the timing between pulses survives. |
| RX | RXD mirrors TXD while transmitting. | Our own pulses appear on RXD; harmless because the link is half duplex. |
| RX | The receiver occasionally re-triggers inside one of the cart's long marks (seen 97 µs after the leading edge) and occasionally drops the postamble pulse of an empty message. | Needs a dead time and a tolerance, below. |

## 3. Transmit: chained pulses

Every mark, whatever its length, is sent as a chain of 20 µs pulses separated by
2 µs gaps, in `gbc_ir_tx_mark()`. The chain is timed as a whole so the mark
still spans its nominal length including the gaps. The cart's receiver is a
phototransistor into a common-base amplifier with an AC-coupled comparator;
its response is far slower than 2 µs, so the gaps never reach the comparator.
Verified by the cart ACKing every block of a complete exchange.

The cart decodes bits by the interval between consecutive mark *ends*
(`ReceiveIRDataMessage` samples TIMA after each light-to-dark transition, with
$12 timer ticks of 15.26 µs, about 275 µs, as the 0/1 threshold). A chained
mark ends at the end of its last pulse, exactly where a solid mark would, so
the bit timing the cart sees is unchanged. Driving the mark as one long TXD
pulse and letting the transceiver clamp it would have put up to 77 µs of
jitter on that end, against margins of about 100 µs, which is why chaining is
used for bit marks too and not only for the long ones.

## 4. Receive: leading-edge periods

`mg_read_ir()` in `mg_protocol.c` reports two events per pulse. At each
accepted leading edge it returns the *period* since the previous leading edge,
classified as a bit 0, a bit 1, a hello gap or a preamble; on the following
call it returns an instant mark event for the pulse itself. The protocol code
above it, which was written for a mark/dark rhythm, is unchanged.

The period is the cart's mark plus its following space. Measured from a US
Gold cart, which runs its IR timer at 65 536 Hz with a 2-tick reload (30.5 µs
per interrupt) for framing and a 12-tick reload (183 µs) for bits:

| Period | Measured | Threshold used |
|---|---|---|
| Bit 0 (61 µs mark + one 183 µs interrupt) | ~175 µs | < 265 |
| Bit 1 (two interrupts) | ~355 µs | < 520 |
| Hello gap, postamble to next preamble | 731 to 771 µs | < 830 |
| Preamble (150 µs mark + 20 interrupts + one bit interrupt) | 931 µs | ≤ 1400 |

Three rules make this robust:

- **Dead time, 120 µs.** A second leading edge sooner than this is the receiver
  re-triggering inside one of the cart's long marks and is ignored. The
  shortest real period is a bit 0 at about 172 µs.
- **Instant mark events.** The pulse is only 2 µs long. If the code had to
  observe the pulse's end, any step that arrives late, such as the ACK read
  returning just as the cart's next hello begins, would miss it. Queueing the
  mark at the leading edge removes that dependency entirely; this fixed the
  role-swap failure seen during bring-up.
- **End of message after 1 ms of silence.** A message ends when a pulse is
  followed by a non-bit period, or by nothing for `MG_EDGE_END_WAIT_US`
  (1000 µs). The value is boxed in from both sides. Below about 750 µs the next
  message's preamble pulse, 731 µs after the postamble, arrives after the wait
  has returned and interrupts have been unmasked, and the Flipper's OS then
  catches up on roughly 0.7 ms of deferred interrupt work, during which a 2 µs
  pulse is lost (seen with 700 µs). At the reference's 5 ms element timeout the
  wait after the cart's final ACK pushed our pass-2 hello to about 10 ms after
  the ACK, outside the 7.8 ms the cart listens for it (section 5).

One tolerance: the receiver sometimes drops the postamble pulse of a
zero-length message, so the next pulse seen after its preamble is the following
message's preamble about 1.5 ms later. An empty message carries no data and the
block checksum still covers it, so that pattern is accepted as a complete empty
message.

## 5. The cart's timing windows

From `engine/link/mystery_gift.asm` in pokegold and pokecrystal, which are
identical for this purpose:

- The timer runs at 65 536 Hz. Framing uses a 2-tick reload, 30.5 µs per
  interrupt; bits use a 12-tick reload, 183 µs; bit decoding on receive uses a
  256-tick reload for the TIMA measurement.
- A hello is LED off for 60 interrupts (1.83 ms), on for 4 (122 µs plus the
  partial first interval, 148 to 155 µs measured), off for 20 (610 µs), on for
  4, off for 4. The sender then waits for the partner's reply with 256-interrupt
  (7.8 ms) windows on each edge.
- `ReceiveIRHelloMessage` waits up to 7.8 ms for the first mark. This is the
  window our hello has to land in whenever the cart swaps to listening: after
  each of its ACKs mid-exchange, and after it ACKs the tail block between pass
  1 and pass 2. Our hello carries a 3 ms lead of darkness, and the inter-pass
  gap is 2 ms, so the ACK read must return within about 2 ms of the ACK's last
  pulse for the hello to make the window.
- Between passes the cart copies the received trainer record, stages its party
  data, and goes straight to `ReceiveIRHelloMessage`. There is no restart loop
  there: one missed hello is a communication error.
- After a communication error the game prints the message and jumps back to
  `DoMysteryGift`, where `InitializeIRCommunicationRoles` polls both the
  joypad and the IR line. It falls into hello reception as soon as it sees
  light, so a retry from the Flipper works without A being pressed again.

## 6. Interrupt masking on the Flipper

Each transmitted message and each received message runs with interrupts
masked, as in the upstream design, so a context switch can't stretch a 20 µs
pulse or hide a 2 µs one. The cost is that unmasking after a 10 to 40 ms
message releases a burst of deferred interrupt work. Section 4's end-of-message
wait exists so that nothing time-critical happens during that burst. The
multi-second trigger listen at the start stays unmasked; a missed hello there
only costs a retry.

## 7. The flight recorder

`mg_trace()` records up to 1024 events per attempt with a microsecond
timestamp. `worker_dump_trace()` writes them to `apps_data/flipgift/last_trace.txt`
after every attempt, appending within one send and starting a new file for the
next. Lines worth knowing:

| Line | Meaning |
|---|---|
| `edge period N` | A leading edge whose period was longer than a bit; the normal values are 731/771 (postamble to next), 931 (preamble), and large numbers after our own transmissions. |
| `edge dead N` | A re-trigger rejected by the dead time. |
| `rx-hello m1=2 gap=731` | The cart's hello, both pulses seen. |
| `sync s=931` | A message's preamble period. `sync s=1525` is an empty message whose postamble was dropped. |
| `byte[i]=0x..` | A decoded byte. |
| `ack-rx=0x6C` / `ack-tx=0x6C` | The cart's ACK of our block / ours of theirs. |
| `TMO where a b` | Where a receive gave up and the class and width it saw. |
| `done r=0` | Success. |

A complete successful exchange is about 520 events.

## 8. What was tried and dropped

- A level-based receiver (the upstream classifier) with widened thresholds:
  the 2 µs pulses were missed whenever the code arrived late to a pulse's end.
- End-of-message wait of 700 µs: too short by 31 µs, see section 4.
- A level-based classifier path kept behind a Board setting for an analog
  front end: removed when the app was pinned to the FlippIR board.
