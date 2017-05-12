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
!CS: pin 24: SPI0_CE0_N / GPIO 8

The clock board will also supply 5 volts to pins 2 and 4 to power the Pi.

It will additionally break out the UART0 TX and RX pins to a 3 pin header.
For the RX pin, it has a diode + pull-up level shifter (with the pull-up from
3V3 on pin 1).

*/

// There is some latency in the system that must be accounted for.
// This value is a guess based on observations made on a single
// system. YMMV. See the fprintf() in update_display().
#define FUDGE (250L * 1000L)

// Various fractions of a second's worth of nanoseconds
// There are one billion nanoseconds in one second
#define SECOND_IN_NANOS (1000L * 1000L * 1000L)
#define TENTH_IN_NANOS (SECOND_IN_NANOS / 10)
#define HUNDREDTH_IN_NANOS (SECOND_IN_NANOS / 100)

// These two values are the same, one in C time, the other a Julian date
// Both represent 1/1/2000 00:00 UTC.
#define EPOCH_CTIME (946684800L)
#define EPOCH_JDATE (2451544.5)

#define _BSD_SOURCE
#define _POSIX_C_SOURCE 199309L

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

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
// or the mask with the digit number 0-7. On the hardware, 0-6
// are the digits from left to right (D6 is tenths of seconds, D0
// is tens of hours). D7 is AM, PM and the four LEDs for the colons.
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

// Digit mapping
#define DIGIT_10_HR (0)
#define DIGIT_1_HR (1)
#define DIGIT_10_MIN (2)
#define DIGIT_1_MIN (3)
#define DIGIT_10_SEC (4)
#define DIGIT_1_SEC (5)
#define DIGIT_100_MSEC (6)
#define DIGIT_MISC (7)

// These things all get accessed across the thread boundary
volatile int spi_fd;
volatile timer_t timer_id;
volatile unsigned char colon = 1;
volatile unsigned char colon_blink = 0;
volatile unsigned char tenth_enable = 1;
volatile float longitude = 0.0;

static void write_reg(unsigned char reg, unsigned char data) {
	// We write two bytes - the register number and then the data.
	unsigned char msgbuf[2];
	msgbuf[0] = reg;
	msgbuf[1] = data;
	struct spi_ioc_transfer tx_xfr;
	memset(&tx_xfr, 0, sizeof(tx_xfr));
	tx_xfr.tx_buf = (unsigned long)msgbuf; // Stupid Linux, why is it not a pointer?
	// tx_xfr.rx_buf = (unsigned long)NULL; // redundant
	tx_xfr.len = sizeof(msgbuf);
	if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tx_xfr) < 0) {
		perror("ioctl(SPI_IOC_MESSAGE(1))");
		exit(1);
	}
}

static void cleanup(int signo) {
	write_reg(MAX_REG_CONFIG, 0); // sleep now.
	exit(1);
}

static void usage() {
	printf("Usage: side_clock [-b n][-d][-l n][-t]\n");
	printf("   -b : set brightness 0-15\n");
	printf("   -d : Don't daemonize (remain in foreground)\n");
	printf("   -l : Longitude east (negative for west). Default is 0.\n");
	printf("   -t : turn tenth of a second digit off\n");
}

static void schedule_timer() {
	struct timespec now;
	if (clock_gettime(CLOCK_REALTIME, &now)) {
		perror("clock_gettime");
		exit(1);
	}
	// We want to round to the nearest tenth. We can start by tuncating to the nearest
	// hundredth
	unsigned int hundredth_val = (unsigned int)(now.tv_nsec / HUNDREDTH_IN_NANOS);
	// We actually want the *next* tenth of a second.
	unsigned int tenth_val = (hundredth_val + 5) / 10 + 1;
	while (tenth_val >= 10) {
		now.tv_sec++;
		tenth_val -= 10;
	}
	// We want the alarm to go off a little early (FUDGE).
	if (tenth_val != 0) {
		now.tv_nsec = tenth_val * TENTH_IN_NANOS - FUDGE;
	} else {
		// Backing up a bit from zero means crossing the second boundary.
		now.tv_nsec = SECOND_IN_NANOS - FUDGE;
		now.tv_sec--;
	}
	// We want to individually schedule each one rather than use the interval,
	// because it gives us better control in the face of variable response latency.
	// We will simply specify exactly when we desire to be woken up every time.
	struct itimerspec my_itimerspec;
	my_itimerspec.it_interval.tv_sec = 0;
	my_itimerspec.it_interval.tv_nsec = 0;
	my_itimerspec.it_value = now;
	if (timer_settime(timer_id, TIMER_ABSTIME, &my_itimerspec, NULL) < 0) {
		perror("timer_settime");
		exit(1);
	}
}

static void update_display(union sigval ignore) {

	struct timespec now_spec;
	if (clock_gettime(CLOCK_REALTIME, &now_spec)) {
		perror("clock_gettime");
		exit(1);
	}

#if 0
	// This can be used to figure out the FUDGE value. You want this line
	// to print small numbers.
	long error = now.tv_nsec % TENTH_IN_NANOS;
	if (error > 5 * HUNDREDTH_IN_NANOS) error = - (TENTH_IN_NANOS - error);
	fprintf(stderr, "%'8ld\n", error);
#endif

	// turn the time into an absolute fraction.
	long double now = now_spec.tv_sec + ((long double)now_spec.tv_nsec) / SECOND_IN_NANOS;

	long double JD = ((now - EPOCH_CTIME) / 86400.0L) + EPOCH_JDATE;

	long double JD0 = (((((long)(now / 86400)) * 86400) - EPOCH_CTIME) / 86400.0) + EPOCH_JDATE;

	long double D0 = JD0 - (EPOCH_JDATE + .5);
	long double H = (JD - JD0) * 24.0;
	long double T = (JD - (EPOCH_JDATE + .5)) / 36525.0;

	long double gmst = (6.697374558L + 0.06570982441908L * D0 + 1.00273790935L * H) + 0.000026 * T * T;
	gmst += (longitude / 360.0L) * 24.0L;
	while (gmst > 24.0) gmst -= 24.0;
	int h = (int)gmst;
	int m = (int)((gmst - h) * 60);
	int s = (int)((((gmst - h) * 60.0) - m) * 60);
	int tenth_val = (int)((((((gmst - h) * 60.0) - m) * 60) - s) * 10);

	unsigned char decode_mask = (unsigned char)(~_BV(DIGIT_MISC)); // All decode except the misc digit.
	if (!tenth_enable) {
		decode_mask &= ~_BV(DIGIT_100_MSEC); // turn off the tenth digit decode. We'll write a 0.
	}
	write_reg(MAX_REG_DEC_MODE, decode_mask);

	write_reg(MAX_REG_MASK_BOTH | DIGIT_10_HR, h / 10);
	write_reg(MAX_REG_MASK_BOTH | DIGIT_1_HR, h % 10);
	write_reg(MAX_REG_MASK_BOTH | DIGIT_10_MIN, m / 10);
	write_reg(MAX_REG_MASK_BOTH | DIGIT_1_MIN, m % 10);
	write_reg(MAX_REG_MASK_BOTH | DIGIT_10_SEC, s / 10);
	write_reg(MAX_REG_MASK_BOTH | DIGIT_1_SEC, (s % 10) | (tenth_enable?MASK_DP:0));
	write_reg(MAX_REG_MASK_BOTH | DIGIT_100_MSEC, tenth_enable?tenth_val:0);

	unsigned char misc_digit = 0;
	if (colon && ((!colon_blink) || (s % 2 == 0))) {
		misc_digit |= MASK_COLON_HM | MASK_COLON_MS;
	}
	write_reg(MAX_REG_MASK_BOTH | DIGIT_MISC, misc_digit);

	// Set us up the bomb.
	schedule_timer();
}

int main(int argc, char **argv) {

	unsigned char brightness = 15; // 0-15
	unsigned char background = 1;

	int c;
	while((c = getopt(argc, argv, "b:Bcdl:t")) > 0) {
		switch(c) {
			case 'b':
				brightness = atoi(optarg) & 0xf;
				break;
			case 'B':
				colon_blink = 1;
				break;  
			case 'c':
				colon = 0;
				break;  
			case 'd':
				background = 0;
				break;	
			case 'l':
				longitude = atof(optarg);
				break;	
			case 't':
				tenth_enable = 0;
				break;	
			default:
				usage();
				exit(1);
		}
	}

	if (background) {
		if (daemon(0, 0)) {
			perror("daemon");
			exit(1);
		}
	}

	struct sched_param sp;
	sp.sched_priority = (sched_get_priority_max(SCHED_RR) - sched_get_priority_min(SCHED_RR))/2;
	if (sched_setscheduler(0, SCHED_RR, &sp)) {
		perror("sched_setscheduler");
		exit(1);
	}

	pthread_attr_t my_pthread_attr;
	if (pthread_attr_init(&my_pthread_attr) != 0) {
		perror("pthread_attr_init");
		exit(1);
	}
	if (pthread_attr_setdetachstate(&my_pthread_attr, PTHREAD_CREATE_DETACHED) != 0) {
		perror("pthread_attr_setdetachstate");
		exit(1);
	}
	if (pthread_attr_setschedpolicy(&my_pthread_attr, SCHED_RR) != 0) {
		perror("pthread_attr_setschedpolicy");
		exit(1);
	}
	sp.sched_priority = (sched_get_priority_max(SCHED_RR) - sched_get_priority_min(SCHED_RR))/2;
	if (pthread_attr_setschedparam(&my_pthread_attr, &sp) != 0) {
		perror("pthread_attr_setschedparam");
		exit(1);
	}
	struct sigevent my_sigevent;
	my_sigevent.sigev_notify = SIGEV_THREAD;
	my_sigevent.sigev_value.sival_int = 0;
	my_sigevent.sigev_notify_function = update_display;
	my_sigevent.sigev_notify_attributes = &my_pthread_attr;
	if (timer_create(CLOCK_REALTIME, &my_sigevent, (timer_t*)&timer_id) != 0) {
		perror("timer_create");
		exit(1);
	}
	if (pthread_attr_destroy(&my_pthread_attr) != 0) {
		perror("pthread_attr_destroy");
		exit(1);
	}

	if (mlockall(MCL_CURRENT | MCL_FUTURE)) {
		perror("mlockall");
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

	// Clock is active high, latching on leading edge.
	int spi_mode = SPI_MODE_0;
	int spi_bits = 8;
	// Device max is 26 MHz. Let's ask for 20 MHz.
	int spi_speed = 20000000;

	if (ioctl(spi_fd, SPI_IOC_WR_MODE, &spi_mode)) {
		perror("ioctl(SPI_IOC_WR_MODE)");
		exit(1);
	}
	if (ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &spi_bits)) {
		perror("ioctl(SPI_IOC_WR_BITS_PER_WORD)");
		exit(1);
	}
	if (ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed)) {
		perror("ioctl(SPI_IOC_WR_MAX_SPEED_HZ)");
		exit(1);
	}

	signal(SIGINT, cleanup);
	signal(SIGTERM, cleanup);

	// Turn off the shut-down register, clear the digit data
	write_reg(MAX_REG_CONFIG, MAX_REG_CONFIG_R | MAX_REG_CONFIG_S);
	write_reg(MAX_REG_SCAN_LIMIT, 7); // display all 8 digits
	write_reg(MAX_REG_INTENSITY, brightness);

	write_reg(MAX_REG_TEST, 1);
	sleep(1);
	write_reg(MAX_REG_TEST, 0);

	// Force the first update. It will schedule everything after.
	union sigval ignore;
	update_display(ignore);

	while(1) {
		// Dirt nap
		if (select(0, NULL, NULL, NULL, NULL) < 0) {
			perror("select");
			exit(1);
		}
	}
}
