/*
 * UART loopback latency / jitter tester.
 *
 * Wire TX -> RX of the UART under test. The program writes a packet,
 * waits until the same bytes come back and measures round-trip latency
 * (write() start -> last byte received).
 * Bytes are timestamped when poll() wakes up, before draining them, as in hilsim.
 * Reports min / max / avg / stddev / percentiles plus jitter.
 *
 * Host tool (Linux): termios + poll + clock_gettime, SCHED_FIFO when permitted.
 *
 * Build:         gcc -O2 -Wall -o uart_latency uart_latency.c -lm
 * Usage:         uart_latency <dev> [baud] [count] [size] [interval_ms]
 *   e.g.         uart_latency /dev/ttyUSB0 115200 1000 16 10
 */

#define _GNU_SOURCE /* CPU_SET, sched_setaffinity */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_MS 1000
#define MAX_SIZE   4096

/* SCHED_FIFO 1..99 */
#define RT_PRIO 90
#define RT_CPU  3

/* progress line refresh period */
#define PROGRESS_NS 200000000ULL


static int progress_tty;


static void set_realtime(void)
{
	struct sched_param sp = { .sched_priority = RT_PRIO };
	cpu_set_t cpus;

	if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
		fprintf(stderr, "warning: mlockall: %s\n", strerror(errno));

	CPU_ZERO(&cpus);
	CPU_SET(RT_CPU, &cpus);
	if (sched_setaffinity(0, sizeof(cpus), &cpus) < 0)
		fprintf(stderr, "warning: pin to CPU %d: %s\n", RT_CPU, strerror(errno));
	else
		printf("pinned to CPU %d\n", RT_CPU);

	if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
		fprintf(stderr, "warning: SCHED_FIFO %d: %s (run as root / CAP_SYS_NICE)\n", RT_PRIO, strerror(errno));
	else
		printf("SCHED_FIFO priority %d\n", RT_PRIO);
}


/* Clear the in-place progress line so regular messages don't get mixed with it */
static void progress_clear(void)
{
	if (progress_tty)
		fputs("\r\033[K", stderr);
}


static void progress(long done, long count, size_t ok, long timeouts, long mismatches, uint64_t last_rtt)
{
	if (progress_tty) {
		fprintf(stderr, "\r\033[K[%5.1f%%] %ld/%ld  ok %zu  timeouts %ld  mismatches %ld  last rtt %.1f us",
				100.0 * done / count, done, count, ok, timeouts, mismatches, last_rtt / 1e3);
	}
	else {
		fprintf(stderr, "progress: %ld/%ld (%.0f%%), ok %zu, timeouts %ld, mismatches %ld\n",
				done, count, 100.0 * done / count, ok, timeouts, mismatches);
	}
	fflush(stderr);
}


static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}


static speed_t baud2speed(long baud)
{
	switch (baud) {
		case 9600: return B9600;
		case 19200: return B19200;
		case 38400: return B38400;
		case 57600: return B57600;
		case 115200: return B115200;
		case 230400: return B230400;
#ifdef B460800
		case 460800: return B460800;
#endif
#ifdef B921600
		case 921600: return B921600;
#endif
#ifdef B1000000
		case 1000000: return B1000000;
#endif
#ifdef B2000000
		case 2000000: return B2000000;
#endif
#ifdef B3000000
		case 3000000: return B3000000;
#endif
		default: return (speed_t)-1;
	}
}


static int uart_setup(int fd, long baud)
{
	struct termios tio;
	speed_t speed = baud2speed(baud);

	if (speed == (speed_t)-1) {
		fprintf(stderr, "unsupported baudrate %ld\n", baud);
		return -1;
	}

	if (tcgetattr(fd, &tio) < 0) {
		perror("tcgetattr");
		return -1;
	}

	/* raw 8N1, no flow control */
	tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
	tio.c_oflag &= ~OPOST;
	tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB);
#ifdef CRTSCTS
	tio.c_cflag &= ~CRTSCTS;
#endif
	tio.c_cflag |= CS8 | CREAD | CLOCAL;
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;

	cfsetispeed(&tio, speed);
	cfsetospeed(&tio, speed);

	if (tcsetattr(fd, TCSANOW, &tio) < 0) {
		perror("tcsetattr");
		return -1;
	}

	tcflush(fd, TCIOFLUSH);
	return 0;
}


/* Drain any pending garbage from RX */
static void rx_drain(int fd)
{
	unsigned char tmp[256];
	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	tcflush(fd, TCIFLUSH);
	while (poll(&pfd, 1, 10) > 0 && read(fd, tmp, sizeof(tmp)) > 0)
		;
}


static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}


static void report(const char *name, const uint64_t *samples, size_t n)
{
	uint64_t *s;
	double sum = 0, sq = 0, avg, sd, ipdv = 0;
	size_t i;

	if (n == 0)
		return;

	for (i = 0; i < n; i++) {
		sum += (double)samples[i];
		if (i > 0)
			ipdv += fabs((double)samples[i] - (double)samples[i - 1]);
	}
	avg = sum / n;
	for (i = 0; i < n; i++)
		sq += ((double)samples[i] - avg) * ((double)samples[i] - avg);
	sd = sqrt(sq / n);

	s = malloc(n * sizeof(*s));
	if (s == NULL)
		return;
	memcpy(s, samples, n * sizeof(*s));
	qsort(s, n, sizeof(*s), cmp_u64);

	printf("%s [us]:\n", name);
	printf("  min %10.1f   max %10.1f   avg %10.1f   stddev %8.1f\n",
			s[0] / 1e3, s[n - 1] / 1e3, avg / 1e3, sd / 1e3);
	printf("  p50 %10.1f   p90 %10.1f   p99 %10.1f   p99.9  %8.1f\n",
			s[n / 2] / 1e3, s[(n * 90) / 100] / 1e3, s[(n * 99) / 100] / 1e3, s[(n * 999) / 1000] / 1e3);
	printf("  jitter: peak-to-peak (max-min) %.1f   mean |delta| between consecutive %.1f\n",
			(s[n - 1] - s[0]) / 1e3, (n > 1) ? ipdv / (n - 1) / 1e3 : 0.0);

	free(s);
}


int main(int argc, char **argv)
{
	const char *dev;
	long baud = 115200, count = 1000, size = 16, interval_ms = 10;
	unsigned char *tx, *rx;
	uint64_t *rtt;
	size_t ok = 0;
	long i, timeouts = 0, mismatches = 0, next_pct = 10;
	uint64_t t_progress, last_rtt = 0;
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <dev> [baud=115200] [count=1000] [size=16] [interval_ms=10]\n", argv[0]);
		return 1;
	}
	dev = argv[1];
	if (argc > 2)
		baud = strtol(argv[2], NULL, 0);
	if (argc > 3)
		count = strtol(argv[3], NULL, 0);
	if (argc > 4)
		size = strtol(argv[4], NULL, 0);
	if (argc > 5)
		interval_ms = strtol(argv[5], NULL, 0);

	if (count <= 0 || size <= 0 || size > MAX_SIZE || interval_ms < 0) {
		fprintf(stderr, "invalid arguments\n");
		return 1;
	}

	fd = open(dev, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return 1;
	}
	if (uart_setup(fd, baud) < 0) {
		close(fd);
		return 1;
	}

	rtt = malloc(count * sizeof(*rtt));
	tx = malloc(size);
	rx = malloc(size);
	if (rtt == NULL || tx == NULL || rx == NULL) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}

	/* touch buffers so they're faulted in before mlockall / measurement */
	memset(rtt, 0, count * sizeof(*rtt));
	set_realtime();

	printf("dev %s, %ld baud 8N1, %ld packets x %ld B, interval %ld ms\n", dev, baud, count, size, interval_ms);
	printf("theoretical wire time: %.1f us/packet (10 bits/byte)\n\n", size * 10.0 * 1e6 / baud);

	rx_drain(fd);

	progress_tty = isatty(STDERR_FILENO);
	t_progress = now_ns();

	for (i = 0; i < count; i++) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		uint64_t t0, t_last = 0, deadline;
		size_t got = 0, sent = 0;
		long k;
		ssize_t r;

		/* unique pattern per packet: seq number + pseudo-random fill */
		for (k = 0; k < size; k++)
			tx[k] = (unsigned char)((i * 131 + k * 7 + (k >> 8)) ^ (i >> 8));

		t0 = now_ns();
		while (sent < (size_t)size) {
			r = write(fd, tx + sent, size - sent);
			if (r < 0) {
				if (errno == EINTR || errno == EAGAIN)
					continue;
				progress_clear();
				perror("write");
				goto out;
			}
			sent += r;
		}

		deadline = t0 + (uint64_t)TIMEOUT_MS * 1000000ULL;
		while (got < (size_t)size) {
			uint64_t now = now_ns();
			int tmo;

			if (now >= deadline)
				break;
			tmo = (int)((deadline - now) / 1000000ULL) + 1;
			if (poll(&pfd, 1, tmo) <= 0)
				continue;

			/* timestamp at wakeup and drain everything that already arrived, the same as rx.tRx in hilsim */
			now = now_ns();
			while (got < (size_t)size && (r = read(fd, rx + got, (size_t)size - got)) > 0) {
				got += (size_t)r;
				t_last = now;
			}
			if (r < 0 && errno != EINTR && errno != EAGAIN) {
				progress_clear();
				perror("read");
				goto out;
			}
		}

		if (got < (size_t)size) {
			timeouts++;
			progress_clear();
			fprintf(stderr, "pkt %ld: timeout (%zu/%ld B)\n", i, got, size);
			rx_drain(fd);
		}
		else if (memcmp(tx, rx, size) != 0) {
			mismatches++;
			progress_clear();
			fprintf(stderr, "pkt %ld: data mismatch\n", i);
			rx_drain(fd);
		}
		else {
			rtt[ok] = t_last - t0;
			last_rtt = rtt[ok];
			ok++;
		}

		/* tty: refresh in place periodically; pipe/file: one line per 10% */
		if (progress_tty) {
			uint64_t now = now_ns();
			if (now - t_progress >= PROGRESS_NS || i + 1 == count) {
				progress(i + 1, count, ok, timeouts, mismatches, last_rtt);
				t_progress = now;
			}
		}
		else if ((i + 1) * 100 >= next_pct * count) {
			progress(i + 1, count, ok, timeouts, mismatches, last_rtt);
			while (next_pct <= 100 && (i + 1) * 100 >= next_pct * count)
				next_pct += 10;
		}

		if (interval_ms > 0)
			usleep(interval_ms * 1000);
	}

out:
	if (progress_tty)
		fputs("\n", stderr);
	printf("packets: %ld sent, %zu ok, %ld timeouts, %ld mismatches\n\n", i, ok, timeouts, mismatches);
	report("full-packet round trip", rtt, ok);

	free(rtt);
	free(tx);
	free(rx);
	close(fd);
	return (ok == (size_t)count) ? 0 : 2;
}
