# FlipGift
FlipGift sends a Gen II Pokemon Mystery Gift(Gold, Silver or Crystal) from a Flipper Zero to a real Game Boy Color cartridge over IR, the same way two Game Boys would do it, and shows you the trainer and gift the cart sends back. It runs on the [FlippIR board](https://github.com/Jackv-makes/FlippIR-hardware), the little IR add-on I made for the Flipper. My Pokewalker port, Flipwalker, uses the same board, so if you already have one plugged in you have everything you need. The reason I made this is that I had a drawer of Gen II carts, the board was already on my Flipper, and I wanted to see if it could do Game Boy IR too.

It can. I've tested it against a retail US Pokemon Gold cartridge, the game accepts the gift and the app shows what the cart offered back. It builds for the official firmware 1.5.1 and runs on my Flipper.

The Flipper's own IR port can't talk to a Game Boy, it only understands TV remotes, which is why the board is needed.

# Features
- Sends items or decorations, or picks a random gift with a random trainer name and ID each time
- Tells you when you're lined up before it starts
- Retries on its own if the link drops
- Shows what the cart sent back, trainer name, ID, Pokedex count and the gift it offered
- Open source, so do whatever you want with it

# Disclaimer
I am not a professional embedded developer, this is a hobby project and there are probably rough edges. IR is fussy about alignment, so expect to fiddle with it, and I can't promise to debug your setup. Keep in mind the game only hands out five gifts a day and one per trainer, so if the Pokemon Center refuses you it's probably that and not the IR. If you see something you'd do better, feel free to fork the repo.

# Hardware
You need the [FlippIR board](https://github.com/Jackv-makes/FlippIR-hardware) on the GPIO header, nothing else. No hardware files are provided here.

Getting an IrDA transceiver to talk to a Game Boy took some tricks. If you're curious, [DESIGN.md](DESIGN.md) explains them.

# Building
You'll need [ufbt](https://github.com/flipperdevices/flipperzero-ufbt):

```bash
pip install --upgrade ufbt
ufbt update --channel=release
ufbt
```

That gives you `dist/FlipGift.fap`. Copy it to `apps/GPIO` on the SD card, or plug the Flipper in and run `ufbt launch` to install and start it(close qFlipper first, it holds the serial port). It shows up under Apps, GPIO, as FlipGift.

# Menus
- **Send Gift** is where you pick Item, Deco or Random, choose the item or decoration, and send. Random mode picks a new item, trainer name and ID every time, which gets around the one gift per trainer per day rule, and hides what the gift is until you pick it up at the Pokemon Center.
- **Settings** has Region, Trainer name, Trainer ID, Random ID and how many times to retry.
- **Diagnostics** has a few test screens. You shouldn't need them unless something isn't working.
- **About** shows the version and credits.

There's a recording of the menus in [docs/app-walkthrough.gif](docs/app-walkthrough.gif).

# First Use
1. Plug the FlippIR board in, open FlipGift, pick a gift.
2. On the Game Boy open Mystery Gift from the title screen so it says "Press A to link".
3. Hold the board a few centimetres from the Game Boy's IR window, square on, press Send gift on the Flipper and then A on the game, once.
4. Watch the screen. It tells you when it hears the game, when to hold still, and when the gift is in.

If it fails it says so and tries again on its own. The game goes back to its Press A screen after an error and keeps listening, so you don't need to press A again.

# Notes
- Five gifts a day and one per trainer are game limits. Random mode gets around the per trainer one but not the daily cap.
- Range is a few centimetres, about what two Game Boys manage. A dim room helps, bright or fluorescent light makes it worse.
- The original app was tested against French Crystal and Gold carts and this one against a US Gold cart. Silver and other regions should work but I haven't tried them.
- If something isn't working, every attempt saves a log to `apps_data/FlipGift/last_trace.txt` on the SD card.

# Credit and Thanks
This started as a fork of [TheRealNox's flipper-pokemon-mystery-gift](https://github.com/TheRealNox/flipper-pokemon-mystery-gift). The actual Mystery Gift protocol code is theirs. None of this would exist without that work. What I added is decorations, showing what the cart sends back, the aim and retry flow, and everything that makes it work through the FlippIR board.

TheRealNox in turn built on other people's work, so the same thanks go out from here:
- The folks at [projectpokemon.org](https://projectpokemon.org/home/forums/topic/43930-mystery-gift-reverse-engineering-of-ir-protocol/) for reverse engineering the Gen II Mystery Gift IR protocol in the first place.
- [pret](https://github.com/pret) for [pokecrystal](https://github.com/pret/pokecrystal) and [pokegold](https://github.com/pret/pokegold), which is where the item lists and timing details come from.

# License
GPL-3.0-or-later, see [LICENSE](LICENSE).
