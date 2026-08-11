# Blaustahl Storage Device

The Blaustahl USB dongle provides long-term storage for about 7,680 characters of text, backed by non-volatile FRAM memory — no batteries, no wear-out from repeated writes, and no risk of losing data if the device is unplugged. Simply plug it into your computer and open any serial communications program that supports VT100 emulation (PuTTY, Tera Term, Minicom, screen, etc.) to access the built-in interface.

Beyond the FRAM editor, Blaustahl also includes a small flash filesystem for storing files, a file browser and viewer, a built-in text editor, password-protected encryption, file transfer over XMODEM, and a full Scheme programming environment — all accessible over the same USB serial connection, no drivers or extra software required.

![Blaustahl](https://github.com/machdyne/blaustahl/blob/251dba51e97cc46e1b7918198aa66e48cc741dd9/blaustahl.png)

This repo contains schematics, PCB layouts, firmware, and a 3D-printable case.

Find purchasing information on the [Blaustahl product page](https://machdyne.com/product/blaustahl-storage-device/).

> **This is an early beta of firmware v0.1.0.** Most features described here are stable and well-tested, but as with any beta, please back up anything important and report anything that looks wrong. See firmware-0.0.x for the old firmware.

## Connecting Blaustahl

The following sections describe how to connect to Blaustahl on different operating systems. Once connected, press **CTRL-G** at any time to view the on-device help screen.

### Linux

1. Install screen or minicom:

```
$ sudo apt install screen
$ sudo apt install minicom
```

2. After plugging in Blaustahl, find the device:

```
$ dmesg | grep ACM
```

3. Connect to the device (for example):

```
$ screen /dev/ttyACM0
or
$ minicom -D /dev/ttyACM0
```

If the device is already plugged in and you see nothing, you may need to press CTRL-L to refresh the display.

### OS X

1. Open the Terminal application.

2. After plugging in Blaustahl, find the device:

```
$ ls /dev/tty.usbmodem*
```

3. Connect to the device (for example):

```
$ screen /dev/tty.usbmodem1234561
```

### Windows

1. Download and install [PuTTY](https://www.chiark.greenend.org.uk/~sgtatham/putty/latest.html).

2. After plugging in Blaustahl, find the device by opening Device Manager and looking under "Ports (COM & LPT)" for "USB Serial Device (COMx)".

3. Open PuTTY, set connection type to Serial, enter COMx (e.g. COM4) in the serial line box, and press Open.

## The menu bar

Everything in Blaustahl is reached from one menu bar across the top of the screen:

```
/// | EDITOR: [FRAM] [SRAM] | [VIEWER] [FILES] [CLI] [HELP] |   MODE: [TEXT] [HEX]
```

Press **CTRL-T**, **ESC-ESC**, or a single **ESC** (pause briefly) to open it from anywhere. Use the arrow keys to move between items and Enter to select one, or Esc to close the menu without changing anything.

- **FRAM** / **SRAM** — the built-in grid editor, for the device's permanent (FRAM) or temporary (SRAM) storage.
- **VIEWER** — a read-only viewer for files stored on the flash filesystem.
- **FILES** — browse and manage files on the flash filesystem.
- **CLI** — a command-line interface, including a full Scheme programming environment.
- **HELP** — the same help screen as CTRL-G.
- **MODE: TEXT / HEX** — switches how the grid editor displays data.

Pressing **CTRL-F** from anywhere jumps straight to FILES, skipping the menu.

## The grid editor (FRAM & SRAM)

The grid editor shows 7,680 bytes as a fixed 80x24 grid, split across four pages (PGUP/PGDN to flip between them — there's no scrolling within a page). Move the cursor with the arrow keys, or jump to the start/end of the current row with Home/End.

**FRAM** is the device's permanent, non-volatile storage — this is what survives being unplugged, and what the device is for. **SRAM** is a second, identical-sized scratchpad you can use the same way, but its contents are lost whenever the device is unplugged or power-cycled — a good place for anything temporary you don't want to persist.

| Keypress | Action |
| -------- | ------ |
| CTRL-W | Toggle write mode, or commit staged edits if editing encrypted FRAM |
| CTRL-B | Toggle buffer mode (see "Encryption" below) |
| CTRL-C | Start/finish copying: press once to mark the start, move the cursor to select, press again to copy |
| CTRL-V | Paste at the cursor |
| CTRL-S / CTRL-Q | Toggle the status bar |
| CTRL-L | Refresh the screen |
| CTRL-G | Help |
| PGUP / PGDN | Flip page |

By default the grid is read-only; press CTRL-W to enable editing. Whatever you type overwrites the byte under the cursor and the cursor advances — there's no insert/delete shifting, since every byte has a fixed position in the grid.

The status bar (bottom line) always shows which file you're editing, the current page, your byte offset, and the current mode — useful for keeping your bearings, especially in HEX mode.

## The file viewer

Selecting a file from FILES (or the VIEWER menu item, if you've already opened one) shows it read-only, in the same TEXT/HEX display as the grid editor, but scrolling smoothly instead of paging — useful for files larger than 7,680 bytes.

| Keypress | Action |
| -------- | ------ |
| UP / DOWN | Scroll one line |
| PGUP / PGDN | Scroll one screen |
| HOME | Jump to the start of the file |
| CTRL-C | Copy, same mechanism as the grid editor |

Copying in the viewer and the grid editor share one buffer, so you can copy a range from a file and paste it directly into FRAM or SRAM, or the other way around.

The viewer is read-only. To edit a flash file, use the CLI's `te` command instead (see below).

## Files (the flash filesystem)

Blaustahl has a small flash filesystem, separate from FRAM/SRAM, for storing files — text, Scheme programs, or anything else that fits. Selecting **FILES** from the menu (or pressing CTRL-F) lists what's stored there; select a file to open it in the viewer.

Deleting, renaming, formatting, and saving a FRAM snapshot are all done from the CLI (see below), since those are the kind of actions worth typing out deliberately rather than triggering with a single keystroke.

## The CLI

Selecting **CLI** from the menu drops you into a command line. Type `help` for the full list at any time.

| Command | What it does |
| ------- | ------------ |
| `info` | Firmware version, FRAM/SRAM status, flash usage, and memory diagnostics |
| `ls` | List files on the flash filesystem |
| `rm <filename>` | Delete a file (asks for confirmation) |
| `rename <old> <new>` | Rename a file |
| `format` | Erase the entire flash filesystem (asks for confirmation) |
| `password` | Set, change, or enter a password for FRAM encryption (see below) |
| `disable_encryption` | Turn off FRAM encryption |
| `view <filename>` | Open a flash file in the viewer |
| `te <filename>` | Open a flash file in the text editor |
| `xmodem_up <filename>` | Receive a file from your computer via XMODEM |
| `xmodem_down <filename\|fram\|sram>` | Send a file, or a full copy of FRAM/SRAM, to your computer via XMODEM |
| `load <filename>` | Run a Scheme program stored on the flash filesystem |
| `firmware_update` | Enter USB bootloader mode to install new firmware (asks for confirmation) |
| `snapshot_fram` | Save a full copy of current FRAM contents to a flash file (`fram_snapshot.bin`) |

Anything typed that isn't one of the commands above is evaluated as **Scheme** — the CLI doubles as a full programming environment. See "Writing programs" below.

## Writing and running programs

Blaustahl includes a small text editor (`te`) for writing plain text or code directly on the device, and a built-in Scheme interpreter for running it.

1. From the CLI, run `te myprogram.scm` to create and edit a new file (files can be up to 32KB).
2. Write your program, save, and exit back to the CLI.
3. Run it with `load myprogram.scm`.

The interpreter comes with a standard library (`map`, `reduce`, `filter`, `sort`, and many more list, string, and math procedures) loaded automatically, alongside the usual Scheme built-ins. You can also just type expressions directly at the CLI prompt to try things out interactively — it's a normal REPL. Typing `(dump)` lists every currently available procedure and variable, if you want to see what's on hand.

## Transferring files

Blaustahl supports the standard XMODEM protocol for moving data to and from your computer, using any XMODEM-capable terminal program.

- **`xmodem_up <filename>`** receives a file from your computer and saves it to the flash filesystem. Start the command on the device, then start your terminal's "send file" (XMODEM) action.
- **`xmodem_down <filename>`** sends a flash file to your computer the same way, in reverse.
- **`xmodem_down fram`** or **`xmodem_down sram`** sends a complete copy of FRAM or SRAM (all 8,192 bytes) to your computer — a reliable way to back up or restore your data in one shot.

Uploads are limited to 32KB, the same as the text editor's file size limit.

Both directions can take a little while to start, since the handshake deliberately waits several minutes for you to get your terminal program's transfer dialog open — no need to rush.

## Encryption

FRAM contents can be protected with a password. This is entirely optional and off by default.

- Run **`password`** from the CLI to set one. Once set, FRAM is encrypted and locked whenever the device isn't actively unlocked — the grid editor will show **LOCKED** until you unlock it.
- Run **`password`** again while locked to unlock it for the session by entering your password.
- Run **`password`** again while unlocked to change it to a new one.
- Run **`disable_encryption`** to remove encryption entirely and return FRAM to plain, unprotected storage.

FRAM encryption uses strong, modern encryption (ChaCha20-Poly1305). **There is no way to recover encrypted FRAM data if you forget the password** — treat it the same way you'd treat any other password you can't reset.

Encryption only applies to FRAM. SRAM and the flash filesystem are never encrypted.

While FRAM is unlocked, edits are staged in a buffer rather than written immediately — this is what CTRL-B (toggle buffer mode) and the buffer indicator on the status bar refer to. Press CTRL-W to commit staged changes to FRAM.

## Programmatic access (SRWP)

For scripts and programs that need to read or write FRAM directly — rather than a person using the on-device editor — Blaustahl exposes **SRWP** (Simple Read/Write Protocol), a small binary protocol over the same USB serial connection. It gives raw, byte-level access to the full 8,192 bytes of FRAM.

SRWP operates below the encryption layer described above — it always reads and writes the raw bytes on the chip, whether or not FRAM encryption is enabled. If you're writing your own tooling against Blaustahl, this is the interface to use; see [`docs/srwp.md`](docs/srwp.md) for the full protocol reference, and the [original protocol specification](https://github.com/binqbit/serialport_srwp) it's based on.

## Firmware updates

Run **`firmware_update`** from the CLI to enter USB bootloader mode (this asks for confirmation first, since anything not yet committed will be lost). You can also hold the button on the device while plugging it in. Once in bootloader mode, update the firmware by dragging and dropping a new `.uf2` file onto the device, which will appear as a USB drive.

Blaustahl ships with `ship/blaustahl_cdconly.uf2` installed — a simple USB-CDC (serial) device, which is what makes it work with an ordinary terminal program on any platform with no special drivers.

### Building from source

Most people won't need to do this — pre-built firmware is available in [Releases](releases). If you'd like to build it yourself and have [pico-sdk](https://github.com/raspberrypi/pico-sdk) installed:

```
$ git submodule update --init --recursive
$ cd firmware/blaustahl
$ mkdir build
$ cd build
$ cmake ..
$ make
```

If you'd like to pre-load files onto the flash filesystem as part of a build (rather than uploading them one at a time with `xmodem_up` afterward), `tools/mkimage` builds a littlefs image from a directory of files:

```
$ tools/mkimage dist/ littlefs.bin
```

Write it to the device with [picotool](https://github.com/raspberrypi/picotool), alongside the normal firmware, while the device is in bootloader mode:

```
$ picotool load build/blaustahl_cdconly.uf2
$ picotool load littlefs.bin -t bin -o 0x10200000 -v
```

The `-v` verifies the write by reading it back immediately. Two separate `picotool load` calls, not a combined file — a real RP2040 bootrom erratum (RP2040-E14) can silently drop part of a UF2 that spans multiple, non-adjacent regions, and `picotool` avoids it entirely by writing directly rather than going through that code path.

## Future data recovery

In the event that the USB interface were to become obsolete or the RP2040 were to become non-functional, it should still be possible to recover the FRAM data using a SOP-8 test clip. The FRAM IC is marked "RS64".

One way to achieve this today is to load the Blaustahl firmware onto a Raspberry Pi Pico using the same SPI GPIOs as shown in the schematics, then hold the Blaustahl boot button while attaching a test clip to the FRAM IC. The grid editor or SRWP can then be used to recover the FRAM data.

## License

The contents of this repo are released under the [Lone Dynamics Open License](LICENSE.md).

Note: You can use these designs for commercial purposes, but we ask that instead of producing exact clones, you either replace our trademarks and logos with your own or add your own next to ours.
