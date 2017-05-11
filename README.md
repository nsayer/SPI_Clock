# SPI_Clock
Source code for the Raspberry Pi Zero W clock project: https://hackaday.io/project/20156-raspberry-pi-zero-w-desk-clock

In essence, the hardware is a MAX6951 connected up to the Pi's SPI bus 0, device 0. The pin connections are:

* DIN: pin 21: SPI0_MOSI / GPIO 10
* CLK: pin 23: SPI0_SCLK / GPIO 11
* !CS: pin 24: SPI0_CE0_N / GPIO 8

Compile with:

cc -O -std=c11 -Wall -o spiclock SPI_Clock.c -lrt

Alternatively, there's an SPI_Sidereal.c that will display the Local Mean Sidereal Time
if you give it your longitude on the command line, or Greenwich Mean Sidereal Time by
default. Note that while the tenth-of-a-second is calculated in sidereal time, the clock
update cycles are still in UTC. The implication is that you probably want to turn the
tenths off as they may not be particularly accurate.

You can use SPI_Clock_recipe.xml as a PiBakery recipe for a custom Raspbian
SD card. You can use it to customize the configuration without having
to log into your Pi.

If you want to do it by hand, add `dtparam=spi=on` to /boot/config.txt. Run this
program with -d to keep it in the foreground and get any error messages out. When you
get the command line arguments the way you like them, remove the -d and run it that way.

You can also use the service file with systemd.
