# 11. The Raspberry Pi 3 launcher

The second supported launcher, next to the ATtiny85. The IR board's J2 header
plugs onto a Raspberry Pi 3 ([chapter 4](04-attiny-hardware.md), *Raspberry Pi mode*), and `gbcpop`
drives it instead of the ATtiny: the whole launch (bootstrap → loader → menu →
payload, [chapter 5](05-loader-protocol.md)), plus the TCG's Card Pop! commands ([chapter 2](02-tcg-cardpop-protocol.md)): `probe`,
`pop`, `give`, `take`, `dump-save`, … Proven on a Raspberry Pi 3 Model B+.

**Supported: Raspberry Pi 3 (B or B+) with the RT image** — Raspberry Pi OS
Lite (64-bit) plus the official PREEMPT_RT kernel, as set up below. Other
models are not supported: the RT kernel is 64-bit only, `gbcpop`'s direct GPIO
backend sets the input pull-up the Pi 1–3 way (not the Pi 4's), and the Pi 5
has a different GPIO block.

## Image

**Raspberry Pi OS Lite (64-bit)**, from
<https://www.raspberrypi.com/software/operating-systems/> or through Raspberry
Pi Imager. Prepared with the 2026-09-15 release (Debian 13 "trixie"):
[`2026-09-15-raspios-trixie-arm64-lite.img.xz`](https://downloads.raspberrypi.com/raspios_lite_arm64/images/raspios_lite_arm64-2026-09-15/2026-09-15-raspios-trixie-arm64-lite.img.xz),
SHA-256 `cdf4f3bfac35ae947b46e4e767f935453810549779ac3290e05a6754aee627e5`.
Newer 64-bit Lite releases work the same way; the 32-bit images do not.

There is no separate "RT image": the PREEMPT_RT kernel is an official package
(`linux-image-rpi-v8-rt`) that is installed on top in the next step. It
matters because `gbcpop` bit-bangs the IR line from userspace at `SCHED_FIFO`
priority. On a stock kernel an interrupt (network traffic is enough) can still
take the CPU in the middle of a byte, and the byte is lost.

## RT kernel and build

Once the Pi is installed and online:

```sh
sudo apt update && sudo apt full-upgrade -y
sudo apt install -y git
git clone https://github.com/esoteric-programmer/cardpophax.git
cd cardpophax
host/pi-setup.sh
sudo reboot
```

`host/pi-setup.sh` does the following, which can also be done by hand:

1. `sudo apt install -y linux-image-rpi-v8-rt build-essential`. The kernel
   package installs itself as `/boot/firmware/kernel8_rt.img`.
2. It appends `kernel=kernel8_rt.img` (under `[all]`) to
   `/boot/firmware/config.txt`. Without that line the firmware keeps booting
   the stock kernel. `auto_initramfs=1`, set in the stock `config.txt`, makes
   the firmware load the matching `initramfs8_rt`.
3. It runs `make -C host`, which builds `host/gbcpop` with the direct
   `/dev/mem` backend. It needs no libraries. Only this backend has the
   loader's IR link; the `PIGPIO=1` and `WIRINGPI=1` builds speak only the
   Card Pop! stage.

After the reboot, `uname -v` should contain `PREEMPT_RT`. `apt full-upgrade`
keeps the RT kernel updated. To return to the stock kernel, remove the
`kernel=` line.

## Attach the board

Power off, **take the ATtiny85 out of its socket**, leave J1 unpowered and
plug J2 onto the Pi's header ([chapter 4](04-attiny-hardware.md)). The defaults match the board's
wiring: GPIO17 (pin 11) drives the IR LED, GPIO18 (pin 12) reads the
detector, with the Pi's internal pull-up, and GPIO22 (pin 15) drives the blue
status LED.

`gbcpop` needs root, for `/dev/mem` and the real-time priority. Nothing else
may use GPIO17, 18, 22 or 27 while it runs.

## Launch the payloads

The Pi launcher has its own loader, `gb/loader_pi.bin` (`make -C gb` builds it
next to the ATtiny's `loader.bin`), and takes its payloads from a directory. No
payload list is built in; the menu shows whatever the directory holds.

The bootstrap, the loader and the payloads are Game Boy code and not in the
repository; build them with rgbds as in [chapter 10](10-tooling.md) (rgbds is not packaged in
Debian), on the Pi or on a PC, and copy them over. The audio dumper also needs
its upstream checkout ([chapter 7](07-payloads.md)).

**The payload directory.** Each entry is one menu item:

* `<name>.bin`: a bare payload, loaded and entered at `$C000`;
* a directory `<name>/` holding a `manifest.txt` (a multi-segment payload,
  [chapter 7](07-payloads.md)) or a `<name>.bin`.

Anything else is skipped, and so is a payload that cannot be read (missing or
empty files); `-v` names what was skipped. The menu names come from the file or
directory names: upper-cased, `-` and `_` become spaces, at most 17
characters, sorted by name. At least 38 names fit (768 bytes of list), more
if they are short. The repository's own `payloads/` works as it is, once its
payloads are built. A directory set up by hand looks like this:

```
payloads/
  audio-dumper/     manifest.txt  c000.bin  8800.bin  8e00.bin
  card-pop.bin
  save-patcher.bin
  snake.bin
```

Then start the launcher:

```sh
cd cardpophax
sudo host/gbcpop launch gb/bootstrap.bin gb/loader_pi.bin payloads
```

(Payload files can also be listed one by one instead of a directory.) Like the
ATtiny ([chapter 6](06-attiny-firmware.md)), it runs until Ctrl-C and keeps no state about the Game Boy.
Over and over it

1. probes for the TCG's Card Pop! screen (12 Card Pop! syncs, ~33 ms apart):
   when the game answers, it uploads the bootstrap (the screen turns teal),
   the loader and the menu, which appears a few seconds later;
2. otherwise listens ~0.4 s for a choice from the loader's menu and sends that
   payload.

So the Game Boy may be anywhere: on the TCG's Card Pop! screen (**don't press
A** there), in the menu, or in a payload that later returns to the menu; it
may also be switched off and on. The payload directory is read again for every
new menu, so a file dropped in shows up the next time the menu comes from the
TCG. The launcher prints one line with the time for each event (TCG answered,
menu shown, request, sent or failed); unanswered probes are not logged.

The menu shows three payloads at a time, the arrow on the chosen one, its
position (`n/N`) below:

* **UP/DOWN** move the arrow (held down, it repeats); the list scrolls.
* **A** launches the payload.
* **SELECT** in a payload returns to the menu, the arrow where it was.
* **SELECT** in the menu restarts the Game Boy: a jump to the cartridge's entry
  point, set up as after power-on (not a hardware reset), so it boots whatever
  cartridge is inserted.

`gbcpop loader` is the one-shot variant, used on the emulator test bench: it
waits ~20 s for the TCG, sends the menu and exits 60 s after the last request.

**The blue LED** works like the ATtiny's ([chapter 6](06-attiny-firmware.md)), without the red one: it is
lit while the Pi transmits and the Game Boy answers — from the Card Pop! sync
on, and after every acknowledged chunk. It goes dark while the Pi listens for
an acknowledgement, after a copy that got none (where the ATtiny shows red),
while the menu waits for a button, and once the payload is sent. So while it
is lit, a transfer is running and the last exchange succeeded. There is no
blink at startup. `--led <gpio>` moves it, `--led off` disables it.

**The shutdown button.** Without a network or screen, the board's button SW1
(the ATtiny's reset) shuts the Pi down cleanly: a press while `gbcpop launch`
waits for the Game Boy (it looks every ~33 ms; not during a transfer) lights
the blue LED for a second to acknowledge, then the Pi powers off. Unplug it
once its green activity LED has stopped flashing.
`--button <gpio>` moves it, `--button off` disables it.

**Priority.** `gbcpop` runs at real-time priority (`SCHED_FIFO`) only while the
Game Boy talks. Everything that waits — for the TCG, for a menu choice, the
launcher's idle loop — runs at normal priority: a thread spinning at real-time
priority for minutes starves the kernel's own per-CPU threads, and Raspberry Pi
OS's watchdog then resets the Pi after a minute. A byte garbled while waiting is
simply retried. The LED, the button and `launch` need the default (direct)
build.

## Start at boot (service)

For a Pi that runs standalone with the board — no screen, no network — the
launcher can start at boot as a systemd service. With `host/gbcpop` built
(`host/pi-setup.sh`), and `gb/bootstrap.bin`, `gb/loader_pi.bin` and the
payloads in `payloads/` built as above:

```sh
cd cardpophax
host/pi-install-service.sh
```

It installs `gbcpop` to `/usr/local/bin`, the bootstrap and the loader to
`/usr/local/share/cardpop/`, and the payloads it finds built in `payloads/` to
the payload directory `/usr/local/share/cardpop/payloads/`, then enables and
starts `gbcpop-launch.service` (`host/gbcpop-launch.service`). Payloads already
in that directory stay; running the script again updates ours and the program.

* **More payloads:** copy them into the payload directory
  (`sudo cp my-game.bin /usr/local/share/cardpop/payloads/`, then `sync`); the
  next menu from the TCG lists them, no restart needed.
* **Log:** `journalctl -u gbcpop-launch -f`.
* **Shutting down:** the button SW1, as above. The service then ends normally
  and is not restarted; after a crash, systemd restarts it after 2 s.
* **Using `gbcpop` by hand** (`selftest`, `probe`, …): the service holds the
  same pins, so stop it first — `sudo systemctl stop gbcpop-launch`, and
  `sudo systemctl start gbcpop-launch` afterwards.
* **Removing it:** `sudo systemctl disable --now gbcpop-launch`, then delete
  `/etc/systemd/system/gbcpop-launch.service` and `/usr/local/share/cardpop`.

## Card Pop! commands

```sh
cd cardpophax/host
sudo ./gbcpop selftest      # blue LED on for 1 s, IR LED + detector check, listens 3 s
sudo ./gbcpop probe         # handshake: prints the game's mode and player name
sudo ./gbcpop pop           # a Card Pop! (--want <id> forces a card; ids: ./gbcpop cards)
sudo ./gbcpop --help        # everything else (give, take, dump-save, ...)
```

For `probe` and `pop`, the Game Boy waits on its Card Pop! screen as for the
launch. `probe` only reads, so the game then reports the Pop! as unsuccessful.
That is expected.

If nothing gets through, `sudo ./gbcpop trace` records the IR pulses for 3 s
while you press A in Card Pop!. Its report tells a detector problem ("no light seen at
all") from bad timing (check `uname -v` for `PREEMPT_RT`).
