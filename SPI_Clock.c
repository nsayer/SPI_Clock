/*

SPI Clock for Raspberry Pi
Copyright 2017 Nicholas Sayer

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.


This program will drive a 7 digit clock display driven by a MAX6951 connected
to a Raspberry Pi's SPI port.

Digit 0 through 6 represent the time digits starting from tens of hours (0)
through a tenth of a second (6).

Digit 7 is broken out into individual LEDs. B and C are the colon between minutes and seconds
E and F are the colon between hours and minutes. A is the AM light and D is PM. The G segment
and decimal point are unusued.

The connections on the raspberry pi are

DIN: pin 21: SPI0_MOSI / GPIO 10
CLK: pin 23: SPI0_SCLK / GPIO 11
!CS: pin 24: SPI0_CEN_N / GPIO 8

The clock board will also supply 5 volts to pins 2 and 4 to power the Pi.

It will additionally break out the UART0 TX and RX pins to a 3 pin header.
For the RX pin, it has a diode + pull-up level shifter (with the pull-up from
3V3 on pin 1).

*/

#define _BSD_SOURCE
#define _POSIX_C_SOURCE 199309L

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

// 10 milliseconds
#define SLEEP_NSEC (10L * 1000L * 1000L)

#define _BV(n) (1 << n)

// The MAX6951 registers and their bits
#define MAX_REG_DEC_MODE 0x01
#define MAX_REG_INTENSITY 0x02
#define MAX_REG_SCAN_LIMIT 0x03
#define MAX_REG_CONFIG 0x04
#define MAX_REG_CONFIG_R _BV(5)
#define MAX_REG_CONFIG_T _BV(4)
#define MAX_REG_CONFIG_E _BV(3)
#define MAX_REG_CONFIG_B _BV(2)
#define MAX_REG_CONFIG_S _BV(0)
#define MAX_REG_TEST 0x07
// P0 and P1 are planes - used when blinking is turned on
// or the mask with the digit number 0-7. On the hardware, 0-5
// are the digits from left to right (D5 is single seconds, D0
// is tens of hours). The D7 and D6 decimal points are AM and PM (respectively).
// To blink, you write different stuff to P1 and P0 and turn on
// blinking in the config register (bit E to turn on, bit B for speed).
#define MAX_REG_MASK_P0 0x20
#define MAX_REG_MASK_P1 0x40
#define MAX_REG_MASK_BOTH (MAX_REG_MASK_P0 | MAX_REG_MASK_P1)
// When decoding is turned off, this is the bit mapping.
// Segment A is at the top, the rest proceed clockwise around, and
// G is in the middle. DP is the decimal point.
// When decoding is turned on, bits 0-3 are a hex value, 4-6 are ignored,
// and DP is as before.
#define MASK_DP _BV(7)
#define MASK_A _BV(6)
#define MASK_B _BV(5)
#define MASK_C _BV(4)
#define MASK_D _BV(3)
#define MASK_E _BV(2)
#define MASK_F _BV(1)
#define MASK_G _BV(0)

// Digit 7 has the two colons and the AM & PM lights
#define MASK_COLON_HM (MASK_E | MASK_F)
#define MASK_COLON_MS (MASK_B | MASK_C)
#define MASK_AM (MASK_A)
#define MASK_PM (MASK_D)

int spi_fd;

void write_reg(unsigned char reg, unsigned char data) {
	unsigned char msgbuf[2];
	msgbuf[0] = reg;
	msgbuf[1] = data;
	struct spi_ioc_transfer tx_xfr;
	memset(&tx_xfr, 0, sizeof(tx_xfr));
	tx_xfr.tx_buf = (unsigned long)msgbuf;
	tx_xfr.rx_buf = (unsigned long)NULL;
        tx_xfr.len = sizeof(msgbuf);
	if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tx_xfr) < 0) {
		perror("ioctl(SPI_IOC_MESSAGE(1))");
	}
}

void cleanup(int signo) {
	write_reg(MAX_REG_CONFIG, 0); // sleep now.
	exit(1);
}

void usage() {
	printf("Usage: clock [-a][-b n][-c][-d][-t]\n");
	printf("   -2 : 24 hour display mode (instead of AM/PM)\n");
	printf("   -b : set brightness 0-15\n");
	printf("   -c : turn colons off\n");
	printf("   -d : Don't daemonize (remain in foreground)\n");
	printf("   -t : turn tenth of a second digit off\n");
}

int main(int argc, char **argv) {

	unsigned char ampm = 1; // 0 for a 24 hour display
	unsigned char brightness = 15; // 0-15
	unsigned char colon = 1;
	unsigned char tenth = 1;
	unsigned char background = 1;

	int c;
	while((c = getopt(argc, argv, "2b:cdt")) > 0) {
		switch(c) {
			case '2':
				ampm = 0;
				break;	
			case 'b':
				brightness = atoi(optarg) & 0xf;
				break;
			case 'c':
				colon = 0;
				break;	
			case 'd':
				background = 0;
				break;	
			case 't':
				tenth = 0;
				break;	
			default:
				usage();
				exit(1);
		}
	}

	if (mlockall(MCL_FUTURE)) {
		perror("mlockall");
	}
	struct sched_param sp;
	sp.sched_priority = (sched_get_priority_max(SCHED_RR) - sched_get_priority_min(SCHED_RR))/2;
	if (sched_setscheduler(0, SCHED_RR, &sp)) {
		perror("sched_setscheduler");
	}

	spi_fd = open("/dev/spidev0.0", O_RDWR);
	if (spi_fd < 0) {
		perror("Error opening device");
		exit(1);
	}

	if (flock(spi_fd, LOCK_EX | LOCK_NB) < 0) {
		perror("Error locking device");
		exit(1);
	}

	int spi_mode = SPI_MODE_0;
	int spi_bits = 8;
	int spi_speed = 20000000;

	if (ioctl(spi_fd, SPI_IOC_WR_MODE, &spi_mode)) {
		perror("ioctl(SPI_IOC_WR_MODE)");
	}
	if (ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &spi_bits)) {
		perror("ioctl(SPI_IOC_WR_BITS_PER_WORD)");
	}
	if (ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed)) {
		perror("ioctl(SPI_IOC_WR_MAX_SPEED_HZ)");
	}

	signal(SIGINT, cleanup);
	signal(SIGTERM, cleanup);
	if (background) {
		daemon(0, 0);
	}

        // Turn off the shut-down register, clear the digit data
        write_reg(MAX_REG_CONFIG, MAX_REG_CONFIG_R | MAX_REG_CONFIG_B | MAX_REG_CONFIG_S | MAX_REG_CONFIG_E);
        write_reg(MAX_REG_SCAN_LIMIT, 7); // display all 8 digits
        write_reg(MAX_REG_INTENSITY, brightness);

        write_reg(MAX_REG_TEST, 1);
        sleep(1);
        write_reg(MAX_REG_TEST, 0);


	while(1) {
		static unsigned int last_tenth = 12;
		struct timespec now;
		if (clock_gettime(CLOCK_REALTIME, &now)) {
			perror("clock_gettime");
		}
		struct tm lt;
		localtime_r(&now.tv_sec, &lt);

		unsigned char h = lt.tm_hour;
		unsigned char pm = 0;
		if (ampm) {
			if (h == 0) { h = 12; }
			else if (h == 12) { pm = 1; }
			else {
				if (h > 12) { h -= 12; pm = 1; }
                	}
                }

		unsigned int tenth_val = (unsigned int)(now.tv_nsec / (100L * 1000L * 1000L));
		if (tenth_val != last_tenth) {
			last_tenth = tenth_val;
			unsigned char val = (unsigned char)(~_BV(7)); // All decode except 7.
			if (ampm && h < 10) {
				val &= ~_BV(0); // for the 12 hour display, blank leading 0 for hour
			}
			if (!tenth) {
				val &= ~_BV(6); // turn off the tenth digit decode. We'll write a 0.
			}

			write_reg(MAX_REG_DEC_MODE, val);
			write_reg(MAX_REG_MASK_BOTH + 0, h / 10);
			write_reg(MAX_REG_MASK_BOTH + 1, h % 10);
			write_reg(MAX_REG_MASK_BOTH + 2, lt.tm_min / 10);
			write_reg(MAX_REG_MASK_BOTH + 3, lt.tm_min % 10);
			write_reg(MAX_REG_MASK_BOTH + 4, lt.tm_sec / 10);
			write_reg(MAX_REG_MASK_BOTH + 5, (lt.tm_sec % 10) | (tenth?MASK_DP:0));
			write_reg(MAX_REG_MASK_BOTH + 6, tenth?tenth_val:0);
			val = 0;
			if (colon) {
				val |= MASK_COLON_HM | MASK_COLON_MS;
			}
			if (ampm) {
				val |= (pm?MASK_PM:MASK_AM);
			}
			write_reg(MAX_REG_MASK_BOTH + 7, val);
		}
		struct timespec sleepspec;
		sleepspec.tv_sec = 0;
		sleepspec.tv_nsec = SLEEP_NSEC;
		nanosleep(&sleepspec, NULL);
	}
}

