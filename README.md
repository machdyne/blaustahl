# Blaustahl Storage Device

The Blaustahl USB dongle provides long-term storage for about 8000 characters of text. Simply plug the device into your computer and open any serial communications program that supports VT100 emulation (PuTTY, Tera Term, Minicom, screen, etc.) in order to access the built-in text editor.

![Blaustahl](https://github.com/machdyne/blaustahl/blob/251dba51e97cc46e1b7918198aa66e48cc741dd9/blaustahl.png)

This repo contains schematics, firmware and a 3D-printable case.

Find usage instructions and more information on the [Blaustahl product page](https://machdyne.com/product/blaustahl-storage-device/).


## Connecting Blaustahl

The following sections will describe how to connect to Blaustahl on different operating systems. Once you're connected, press CTRL-G to view the on-device help screen.

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

3. Open PuTTY and set connection type to Serial. Enter COMx (e.g. COM4) in the serial line box and press the Open button.

Note: The default firmware on older devices isn't recognized by Windows, if that is the case please [update your firmware](#firmware) to [blaustahl_cdconly.uf2](firmware) in order to access Blaustahl from Windows. If you ordered Blaustahl recently the cdc\_only firmware will be installed by default and should be recognized by Windows.

### Nix Flake Support

For users who prefer Nix, a [Nix Flake](https://nixos.wiki/wiki/Flakes) is now available to simplify building and using Blaustahl's firmware and utility tools. This section offers an alternative method that does not replace the existing instructions for non-Nix environments.

#### Prerequisites:
- Install [Nix](https://nixos.org/download) and enable flake support by adding `experimental-features = nix-command flakes` to your Nix configuration.

#### Building with the Flake:

Instead of cloning the repository manually, you can reference the flake directly using its GitHub address.

1. Use the flake to build the firmware, `bs` utility, or `srwp` tool:

    - To build and obtain the firmware files:
      ```bash
      nix build github:machdyne/blaustahl#firmware
      ```
      The UF2 files will be placed in `result/lib/firmware/`.

    - To build the `bs` utility:
      ```bash
      nix build github:machdyne/blaustahl#bs
      ```
      The binary will be available in the Nix store, with a symlink at `result/bin/bs`.

    - To build the `srwp` tool:
      ```bash
      nix build github:machdyne/blaustahl#srwp
      ```
      Similarly, you'll find the resulting script or binary under `result/bin/srwp`.

**Note:**
- These steps are optional for those using Nix and do not affect the regular installation and usage instructions provided above for other environments.

## Using Blaustahl

Blaustahl includes a built-in text editor with four 80x24 pages of text. The cursor can be moved with the arrow keys. When you'd like to enter or delete text, press CTRL-W to toggle the write mode.

| Keypress | Action |
| -------- | ------ |
| CTRL-G | Help |
| CTRL-L | Refresh screen |
| CTRL-W | Toggle write mode |
| CTRL-S | Toggle status bar |
| CTRL-Y | Enter firmware update mode |
| PGUP/DN | Navigate between pages |
| CTRL-A or ^ | Move to beginning of line |
| CTRL-E or $ | Move to end of line |

## Software

The 'bs' utility program can be used to read and write the FRAM data. Note that it's not possible to use this utility with the cdconly firmware, please install the composite firmware (blaustahl.uf2) in order to use the `bs` utility.

Build and install:

If using OS X:
```
$ brew install libusb
```

If using Linux:
```
$ sudo apt install libusb-1.0-0-dev
```

Then:

```
$ cd sw
$ make
$ sudo make install
$ sudo bs -r my-backup.bin
```
## API

To read or write data to FRAM memory via software, use the built-in API Simple Read Write Protocol (SRWP). An example and description of the protocol can be found in this [repository](https://github.com/binqbit/serialport_srwp).

See `sw/srwp.py` for an SRWP example using Python.

There is also an existing [drivers](./firmware/srwp/) with SRWP support for [Linux](./firmware/srwp/srwp_linux.uf2) and [Windows](./firmware/srwp/srwp_win.uf2).

## Firmware

The [firmware](firmware) can be updated over USB by pressing CTRL-Y in the text editor to enter the UF2 bootloader mode. You can also enter the bootloader by holding the button while plugging in the device.

Once in bootloader mode, you can update the firmware by dragging and dropping a new UF2 file to the device.

There are two firmware images available:

  * blaustahl.uf2: USB composite device with USB-CDC and vendor interface
  * blaustahl\_cdconly.uf2: USB-CDC simple device

Blaustahl ships with blaustahl\_cdconly.uf2 installed.

The vendor interface allows you to access the 'bs' utility and to develop custom
software that interfaces with Blaustahl.

You can build the firmware from source, if you have [pico-sdk](https://github.com/raspberrypi/pico-sdk) installed:

```
$ cd firmware/blaustahl
$ mkdir build
$ cd build
$ cmake ..
$ make
```

## Future data recovery

In the event that the USB interface were to become obsolete or the RP2040 were to become non-functional, it should still be possible to recover the FRAM data using a SOP-8 test clip. The FRAM IC is marked "RS64".

One way to achieve this today is to load the Blaustahl firmware onto a Raspberry Pi Pico using the same SPI GPIOs as shown in the schematics, then hold the Blaustahl boot button while attaching a test clip to the FRAM IC. The text editor or 'bs' utility can then be used to recover the FRAM data.

## License

The contents of this repo are released under the [Lone Dynamics Open License](LICENSE.md).
