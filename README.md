# Blaustahl Storage Device

The Blaustahl USB dongle provides long-term storage for about 8000 characters of text. Simply plug the device into your computer and open any serial communications program that supports VT100 emulation (PuTTY, Tera Term, Minicom, screen, etc.) in order to access the built-in text editor.

![Blaustahl](https://github.com/machdyne/blaustahl/blob/251dba51e97cc46e1b7918198aa66e48cc741dd9/blaustahl.png)

This repo contains schematics, firmware and a 3D-printable case.

Find usage instructions and more information on the [Blaustahl product page](https://machdyne.com/product/blaustahl-storage-device/).

## Firmware

The firmware can be updated over USB by pressing CTRL-Y in the text editor to enter the UF2 bootloader mode. You can also enter the bootloader by holding the button while plugging in the device.

Once in bootloader mode, you can update the firmware by dragging and dropping a new UF2 file to the device.

```
You can build the firmware from source, if you have the RP2040 toolchain installed:

```
$ cd firmware/blaustahl
$ mkdir build
$ cd build
$ cmake ..
$ make
```

## License

The contents of this repo are released under the [Lone Dynamics Open License](LICENSE.md).
