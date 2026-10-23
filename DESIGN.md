# How Flipgift talks to a Game Boy
This is the short version of how the app gets a Mystery Gift through the FlippIR board's transceiver, and the handful of things I had to learn the hard way to make it work. If you want the protocol itself, the message layout, checksums, the two pass exchange, all of that is documented in [TheRealNox's repo](https://github.com/TheRealNox/flipper-pokemon-mystery-gift), which this app is built on. I didn't change any of that and I'm not going to repeat it here.

# The problem
The Game Boy's IR port is just a light turning on and off. A message is a long flash, a long pause, then one short flash per bit where the pause after it says whether the bit is a 0 or a 1, and a long flash at the end. The FlippIR board carries a TFBS4711, which is an IrDA transceiver made for laptops and printers in the late 90s. IrDA uses very short flashes, so the chip has a few habits that get in the way:

- If you ask it to flash for 23us or longer, it decides for itself how long the flash is, somewhere between 23 and 100us, and if you hold it past 100us it switches the LED off completely. The Game Boy's flashes are 52 to 126us.
- On receive it doesn't tell you how long a flash was. It just gives a 2us blip every time a flash starts.

So neither side can be done the obvious way.

# Sending
Every flash goes out as a run of 20us pulses with 2us gaps between them. The chip reproduces anything under 23us exactly, and the Game Boy's receiver is slow enough that it never sees the gaps, it just sees one long flash. I checked in the pokegold source that the game measures bits by the time between the ends of flashes, and a run of pulses ends exactly where a solid flash would, so the game can't tell the difference. The cart ACKs every block, so this part is proven.

# Receiving
Since the chip only gives blips, the app works from the time between blips. The time from one flash start to the next is the flash plus the pause after it, and that's enough: a 0 bit comes out around 175us, a 1 around 355us, the hello gap at 731us and the start of a message at 931us, measured off my US Gold cart. The exact numbers are in `mg_timing.h`.

Three things I had to add to make that reliable:

- **Ignore a blip that comes within 120us of the last one.** The chip sometimes fires twice inside one of the Game Boy's long flashes. The shortest real spacing is about 172us, so anything closer is a repeat.
- **Treat the blip as happening the instant it starts.** It's only 2us long. Early on, if the code got to the receiver a moment late it had already missed the blip and the whole exchange fell over at the point where the cart takes its turn. Now the blip is noted the moment its edge arrives and nothing waits on it ending.
- **A message is over 1ms after its last blip.** This number is pinned in from both sides. The next message starts 731us after the last one ends, and if the app stops listening before that(700us was too short, by 31us) the next start blip arrives while the Flipper is busy catching up on the interrupts it held off during the message, and gets lost. Wait too long(the original code waited 5ms) and you miss the cart's turn: after the first pass the cart only listens for your next hello for 7.8ms, and 5ms of waiting plus the hello's own lead time overshoots it.

One more: the chip occasionally drops the end flash of an empty message. An empty message carries nothing and the block checksum still covers it, so the app accepts that and moves on.

# If it goes wrong
Every attempt writes a log of every blip and byte to `apps_data/flipgift/last_trace.txt` on the SD card. The lines you care about are `rx-hello`(the cart's hello was heard), `sync`(a message start), `byte[n]`(decoded bytes), `ack-rx`/`ack-tx`(the ACKs each way), `TMO`(where a receive gave up) and `done r=0`(success). A good exchange is about 500 lines, and whatever line comes right before the first `TMO` is where it broke.
