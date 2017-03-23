# SPI_Clock
Source code for the Raspberry Pi Zero W clock project: https://hackaday.io/project/20156-raspberry-pi-zero-w-desk-clock

Compile with:

cc -O -std=c99 -o spiclock SPI_Clock.c -lrt

You can use SPI_Clock_recipe.xml as a PiBakery recipe for a custom Raspbian
SD card. You can use it to customize the configuration without having
to log into your Pi.

