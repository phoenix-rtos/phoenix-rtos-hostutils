/*
 * Phoenix-RTOS
 *
 * HIL sensor scenario sender
 *
 * Copyright 2026 Phoenix Systems
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifdef __linux__
#define _GNU_SOURCE /* CPU_SET, sched_setaffinity */
#endif

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>

#ifdef __linux__
#include <sched.h>
#include <sys/mman.h>
#endif

#include <hostutils-common/serial.h>
#include <libsha256.h>

#include "hilcsv.h"


#define HILS_HEAD_HBYTE 34
#define HILS_HEAD_LBYTE 241
#define HILS_TAIL_HBYTE 35
#define HILS_TAIL_LBYTE 242

/* craft -> sim frames: head(2), type(1), len(1), payload(len), tail(2), see phoenix-pilot-core/craft/hil.h */
#define HILS_RX_HEAD_HBYTE  38
#define HILS_RX_HEAD_LBYTE  245
#define HILS_RX_TAIL_HBYTE  39
#define HILS_RX_TAIL_LBYTE  246
#define HILS_RX_PAYLOAD_MAX 64

#define HILS_RX_TYPE_CMD  1 /* float cmd[n], n <= HILS_CMD_CNT, sent only while motors are armed */
#define HILS_RX_TYPE_MODE 2 /* craft fsm state name, sent on change and once a second */
#define HILS_RX_TYPE_MSG  3 /* craft debug message */
#define HILS_RX_TYPE_ECHO 4 /* uint32_t seq, zero padding, answer to ping sent by hilsense */

/* sim -> hilsense ping: head(2), len(1), echoLen(1), payload(len), tail(2), payload starts with uint32_t seq */
#define HILS_PING_HEAD_HBYTE 36
#define HILS_PING_HEAD_LBYTE 243
#define HILS_PING_TAIL_HBYTE 37
#define HILS_PING_TAIL_LBYTE 244
#define HILS_PING_REQ_MIN    (6 + sizeof(uint32_t))
#define HILS_PING_REQ_MAX    ((size_t)6 + 255)
#define HILS_PING_RESP_MIN   (6 + sizeof(uint32_t))
#define HILS_PING_RESP_MAX   (6 + (size_t)HILS_RX_PAYLOAD_MAX)
#define HILS_PING_SEQ_CNT    256 /* send times kept to measure echoes arriving after their period */

#define HILS_CMD_CNT       8
#define HILS_CMD_PRINT_CNT 4                   /* quad: FR, RR, RL, FL */
#define HILS_RX_TIMEOUT_NS INT64_C(2000000000) /* mode is sent at least once a second, longer silence means no link */

#define HILS_NS_PER_S  INT64_C(1000000000)
#define HILS_NS_PER_US 1000

/* prints time [ns] as seconds with ms resolution, use with "%" PRId64 ".%03" PRId64 */
#define HILS_SEC_MS(ns) ((ns) / HILS_NS_PER_S), (((ns) % HILS_NS_PER_S) / 1000000)

#define HILS_DEFAULT_BAUD 460800

/* by default ping and echo have the size of the sensor frame and the quad motor command frame */
#define HILS_DEFAULT_PING_REQ  sizeof(hils_frame_t)
#define HILS_DEFAULT_PING_RESP (6 + HILS_CMD_PRINT_CNT * sizeof(float))

/* SCHED_FIFO priority, below threaded irq handlers (50 on PREEMPT_RT) so that serial irqs aren't delayed */
#define HILS_RT_PRIO 90
#define HILS_RT_CPU  3


/* sensor readings consumed by craft, see hils_statePld_t in phoenix-pilot-devices/sensors/hil/hilsense.c */
typedef struct __attribute__((packed)) {
	float p;
	float q;
	float r;

	float axm;
	float aym;
	float azm;

	int32_t lon;
	int32_t lat;
	float alt;

	float mx;
	float my;
	float mz;

	float pStat;
	float pTot;

	double time;
} hils_statePld_t;


typedef struct __attribute__((packed)) {
	uint8_t headHigh;
	uint8_t headLow;
	hils_statePld_t state;
	uint8_t tailHigh;
	uint8_t tailLow;
} hils_frame_t;


/* state built from frames received from craft */
typedef struct {
	float cmd[HILS_CMD_CNT];
	unsigned int cmdCnt;     /* motors in use, sent in the last command */
	unsigned int respFrames; /* commands or echoes, since last status print */
	unsigned int frames;     /* all valid frames, since last status print */
	unsigned int badFrames;  /* since last status print */
	unsigned int respTotal;  /* commands or echoes accepted since start */
	unsigned int respPeriod; /* commands or echoes received in response to the last frame */
	unsigned int respLimit;  /* commands or echoes to accept before exit, 0 - unlimited */
	char mode[HILS_RX_PAYLOAD_MAX + 1];

	FILE *dump;           /* optional CSV of received commands or echoes, NULL if disabled */
	unsigned int dumpCmd; /* motors in dump columns, 0 - header not written yet */
	int64_t tStart;       /* monotonic time before the first frame, dump timestamps are relative to it [ns] */
	int64_t time;         /* sim time of the last sent frame [ns] */
	unsigned long idx;    /* idx column of the scenario row of the last sent frame */

	/* latency from start of sending the last sensor frame to receiving a command */
	int64_t tSent;  /* monotonic time at start of the last sensor frame write [ns] */
	int64_t tRx;    /* monotonic time of the read that delivered the current bytes [ns] */
	int64_t latSum; /* since last status print [ns] */
	int64_t latMin; /* since last status print [ns] */
	int64_t latMax; /* since last status print [ns] */

	/* latency of all measured responses since start, printed at exit */
	unsigned int totLatCnt;
	int64_t totLatSum; /* [ns] */
	int64_t totLatMin; /* [ns] */
	int64_t totLatMax; /* [ns] */

	/* round-trip time measurement, sensor frames are replaced by pings echoed by hilsense */
	int ping;
	uint32_t seq;                        /* seq of the last sent ping */
	int64_t tSentSeq[HILS_PING_SEQ_CNT]; /* monotonic send time of recent pings, indexed by seq [ns] */
} hils_rx_t;


/* scenario pregenerated by hilgen, loaded whole before sending so that file reads don't disturb timing */
typedef struct {
	hils_statePld_t *rows; /* sensor readings of consecutive periods, time field is set when sending */
	unsigned long *idx;    /* idx column of rows, written to the dump to refer to the answered row */
	size_t cnt;
	int64_t period;                          /* [ns] */
	char sha256[2 * SHA256_DIGEST_SIZE + 1]; /* of the whole file, hex */
} hils_input_t;


static volatile sig_atomic_t running = 1;


static void hils_signalHandler(int signal)
{
	(void)signal;
	running = 0;
}


static void hils_usage(const char *program)
{
	fprintf(stderr, "Usage: %s -d <serial-device> -i <csv-file> [-b baud] [-o dump-file] [-n count] [-p [-s req,resp]]\n", program);
	fprintf(stderr, "  -d <serial-device>  serial device to transmit HIL frames\n");
	fprintf(stderr, "  -i <csv-file>       scenario generated by hilgen, one frame per row, rows step is the frame period,\n");
	fprintf(stderr, "                      craft is expected to answer from the first row, exits after the last row\n");
	fprintf(stderr, "  -b <baud>           baud rate (default: %d)\n", HILS_DEFAULT_BAUD);
	fprintf(stderr, "  -o <dump-file>      write accepted motor commands or echoes to CSV file, one row per response:\n");
	fprintf(stderr, "                      idx,sent_ns,recv_ns,cmd0,..,cmdN (only motors sent by craft) | idx,sent_ns,recv_ns,seq (-p)\n");
	fprintf(stderr, "                      idx - idx column of the answered scenario row, missing if no response, sent_ns/recv_ns - monotonic frame send/response receive time since start\n");
	fprintf(stderr, "                      preceded by '#' metadata lines: start wall clock time (UTC) and input file sha256,\n");
	fprintf(stderr, "                      followed by '#' line with the number of sent frames\n");
	fprintf(stderr, "                      other events (mode, messages, bad/missing/late responses) are printed to stdout only\n");
	fprintf(stderr, "  -n <count>          exit after receiving <count> motor commands or echoes (default: run until signal)\n");
	fprintf(stderr, "  -p                  measure round-trip time: send a ping echoed by hilsense instead of each row, from the start\n");
	fprintf(stderr, "  -s <req>,<resp>     ping and echo frame sizes in bytes (default: %zu,%zu - sensor and quad command frames)\n",
			HILS_DEFAULT_PING_REQ, HILS_DEFAULT_PING_RESP);
}


/* [ns] */
static int64_t hils_monoNow(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (int64_t)ts.tv_sec * HILS_NS_PER_S + ts.tv_nsec;
}


/* best effort, reduces scheduling and page fault jitter in timing measurements, needs CAP_SYS_NICE and CAP_IPC_LOCK */
static void hils_rtSetup(void)
{
#ifdef __linux__
	struct sched_param param = { .sched_priority = HILS_RT_PRIO };
	cpu_set_t cpus;

	if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) {
		fprintf(stderr, "hilsim: warning: cannot lock memory: %s\n", strerror(errno));
	}

	CPU_ZERO(&cpus);
	CPU_SET(HILS_RT_CPU, &cpus);
	if (sched_setaffinity(0, sizeof(cpus), &cpus) < 0) {
		fprintf(stderr, "hilsim: warning: cannot pin to CPU %d: %s\n", HILS_RT_CPU, strerror(errno));
	}
	else {
		fprintf(stderr, "hilsim: pinned to CPU %d\n", HILS_RT_CPU);
	}

	if (sched_setscheduler(0, SCHED_FIFO, &param) < 0) {
		fprintf(stderr, "hilsim: warning: cannot set SCHED_FIFO priority %d: %s, timing measurements may be affected by scheduling\n",
				HILS_RT_PRIO, strerror(errno));
	}
	else {
		fprintf(stderr, "hilsim: running with SCHED_FIFO priority %d\n", HILS_RT_PRIO);
	}
#else
	fprintf(stderr, "hilsim: warning: realtime priority not supported on this platform\n");
#endif
}


static int hils_parsePositive(const char *value, int *result)
{
	char *end;
	long parsed;

	errno = 0;
	parsed = strtol(value, &end, 10);
	if (errno != 0 || *value == '\0' || *end != '\0' || parsed <= 0 || parsed > INT32_MAX) {
		return -1;
	}

	*result = (int)parsed;
	return 0;
}


static int hils_parseSizes(const char *value, int *req, int *resp)
{
	int n = 0;

	if (sscanf(value, "%d,%d%n", req, resp, &n) != 2 || value[n] != '\0') {
		return -1;
	}
	if (*req < (int)HILS_PING_REQ_MIN || *req > (int)HILS_PING_REQ_MAX || *resp < (int)HILS_PING_RESP_MIN || *resp > (int)HILS_PING_RESP_MAX) {
		return -1;
	}

	return 0;
}


static void hils_rxLat(hils_rx_t *rx, int64_t lat)
{
	rx->latSum += lat;
	if (rx->respFrames == 1 || lat < rx->latMin) {
		rx->latMin = lat;
	}
	if (lat > rx->latMax) {
		rx->latMax = lat;
	}

	rx->totLatCnt++;
	rx->totLatSum += lat;
	if (rx->totLatCnt == 1 || lat < rx->totLatMin) {
		rx->totLatMin = lat;
	}
	if (lat > rx->totLatMax) {
		rx->totLatMax = lat;
	}
}


static void hils_rxBad(hils_rx_t *rx, uint8_t type, size_t len)
{
	rx->badFrames++;

	printf("hilsim: t=%" PRId64 ".%03" PRId64 " s, bad frame type %u len %zu\n", HILS_SEC_MS(rx->time), (unsigned int)type, len);
}


/* writes '#' metadata lines at the top of the dump, called right after tStart is taken */
static void hils_dumpMeta(hils_rx_t *rx, const hils_input_t *in, const char *inputPath)
{
	struct timespec ts;
	struct tm tm;
	char date[32];

	if (rx->dump == NULL) {
		return;
	}

	/* wall clock time corresponding to the monotonic tStart, dump timestamps are relative to it */
	clock_gettime(CLOCK_REALTIME, &ts);
	gmtime_r(&ts.tv_sec, &tm);
	strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm);

	fprintf(rx->dump, "# start: %s.%09ldZ\n", date, (long)ts.tv_nsec);
	fprintf(rx->dump, "# input: %s\n", inputPath);
	fprintf(rx->dump, "# input_sha256: %s\n", in->sha256);
	fflush(rx->dump);
}


/* writes accepted command (motors from the last command) or echo (seq) as CSV row with send time of the frame it answers
 * and its receive time, header is written with the first row */
static void hils_dumpResp(hils_rx_t *rx, int64_t tSent, uint32_t seq)
{
	unsigned int i, cnt = rx->ping ? 1 : rx->cmdCnt;

	if (rx->dump == NULL) {
		return;
	}

	if (rx->dumpCmd == 0) {
		fprintf(rx->dump, "idx,sent_ns,recv_ns");
		if (rx->ping) {
			fprintf(rx->dump, ",seq");
		}
		else {
			for (i = 0; i < cnt; i++) {
				fprintf(rx->dump, ",cmd%u", i);
			}
		}
		fprintf(rx->dump, "\n");
		rx->dumpCmd = cnt;
	}
	else if (cnt != rx->dumpCmd) {
		/* columns are fixed by the header */
		printf("hilsim: t=%" PRId64 ".%03" PRId64 " s, command with %u motors, %u expected, not written to dump\n",
				HILS_SEC_MS(rx->time), cnt, rx->dumpCmd);
		return;
	}

	fprintf(rx->dump, "%lu,%" PRId64 ",%" PRId64, rx->idx, tSent - rx->tStart, rx->tRx - rx->tStart);
	if (rx->ping) {
		fprintf(rx->dump, ",%u", seq);
	}
	else {
		for (i = 0; i < cnt; i++) {
			/* FLT_DECIMAL_DIG significant digits, value read back is bit-exact with the received float */
			fprintf(rx->dump, ",%.9g", rx->cmd[i]);
		}
	}
	fprintf(rx->dump, "\n");
}


static void hils_rxFrame(hils_rx_t *rx, uint8_t type, const uint8_t *payload, size_t len)
{
	char text[HILS_RX_PAYLOAD_MAX + 1];
	int64_t lat;
	uint32_t seq;

	switch (type) {
		case HILS_RX_TYPE_CMD:
			if (len == 0 || len > sizeof(rx->cmd) || (len % sizeof(rx->cmd[0])) != 0) {
				hils_rxBad(rx, type, len);
				return;
			}
			if (rx->respLimit != 0 && rx->respTotal >= rx->respLimit) {
				/* limit reached, ignore commands that arrived in the same read */
				return;
			}
			/* craft sends only motors in use, the rest stays zero */
			memset(rx->cmd, 0, sizeof(rx->cmd));
			memcpy(rx->cmd, payload, len);
			rx->cmdCnt = len / sizeof(rx->cmd[0]);
			rx->respFrames++;
			rx->respTotal++;
			rx->respPeriod++;

			/* dump has one row per answered frame, extra commands are only reported at the end of the period */
			if (rx->respPeriod == 1) {
				lat = rx->tRx - rx->tSent;
				hils_rxLat(rx, lat);
				hils_dumpResp(rx, rx->tSent, 0);
			}
			break;

		case HILS_RX_TYPE_MODE:
			memcpy(text, payload, len);
			text[len] = '\0';
			if (strcmp(text, rx->mode) != 0) {
				printf("hilsim: t=%" PRId64 ".%03" PRId64 " s, craft mode %s -> %s\n", HILS_SEC_MS(rx->time),
						(rx->mode[0] != '\0') ? rx->mode : "?", text);
				strcpy(rx->mode, text);
			}
			break;

		case HILS_RX_TYPE_MSG:
			memcpy(text, payload, len);
			text[len] = '\0';
			printf("hilsim: t=%" PRId64 ".%03" PRId64 " s, craft: %s\n", HILS_SEC_MS(rx->time), text);
			break;

		case HILS_RX_TYPE_ECHO:
			if (!rx->ping || len < sizeof(seq)) {
				hils_rxBad(rx, type, len);
				return;
			}
			memcpy(&seq, payload, sizeof(seq));
			if ((uint32_t)(rx->seq - seq) >= HILS_PING_SEQ_CNT) {
				/* not sent recently, send time unknown */
				hils_rxBad(rx, type, len);
				return;
			}
			lat = rx->tRx - rx->tSentSeq[seq % HILS_PING_SEQ_CNT];

			if (seq != rx->seq) {
				/* period of this ping already ended and was reported as noresp */
				printf("hilsim: t=%" PRId64 ".%03" PRId64 " s, late response seq %u, rtt %" PRId64 " ns\n", HILS_SEC_MS(rx->time), seq, lat);
				break;
			}
			if (rx->respLimit != 0 && rx->respTotal >= rx->respLimit) {
				return;
			}
			rx->respFrames++;
			rx->respTotal++;
			rx->respPeriod++;
			hils_rxLat(rx, lat);
			hils_dumpResp(rx, rx->tSentSeq[seq % HILS_PING_SEQ_CNT], seq);
			break;

		default:
			hils_rxBad(rx, type, len);
			return;
	}

	rx->frames++;
}


/* feeds bytes read from the device to the craft frames parser */
static void hils_rxParse(hils_rx_t *rx, const uint8_t *data, size_t len)
{
	static uint8_t buf[2 + 2 + HILS_RX_PAYLOAD_MAX + 2];
	static size_t pos = 0;
	size_t i;

	for (i = 0; i < len; i++) {
		buf[pos++] = data[i];

		/* resync on header mismatch, the mismatched byte may start a new header */
		if ((pos == 1 && buf[0] != HILS_RX_HEAD_HBYTE) || (pos == 2 && buf[1] != HILS_RX_HEAD_LBYTE)) {
			pos = 0;
			if (data[i] == HILS_RX_HEAD_HBYTE) {
				buf[pos++] = data[i];
			}
			continue;
		}

		if (pos == 4 && buf[3] > HILS_RX_PAYLOAD_MAX) {
			hils_rxBad(rx, buf[2], buf[3]);
			pos = 0;
			continue;
		}

		if (pos > 4 && pos == (size_t)buf[3] + 6) {
			if (buf[pos - 2] == HILS_RX_TAIL_HBYTE && buf[pos - 1] == HILS_RX_TAIL_LBYTE) {
				hils_rxFrame(rx, buf[2], &buf[4], buf[3]);
			}
			else {
				hils_rxBad(rx, buf[2], buf[3]);
			}
			pos = 0;
		}
	}
}


static int hils_pingSend(int fd, uint32_t seq, size_t reqLen, size_t respLen)
{
	uint8_t buf[HILS_PING_REQ_MAX] = { 0 };

	buf[0] = HILS_PING_HEAD_HBYTE;
	buf[1] = HILS_PING_HEAD_LBYTE;
	buf[2] = (uint8_t)(reqLen - 6);
	buf[3] = (uint8_t)(respLen - 6);
	memcpy(&buf[4], &seq, sizeof(seq));
	buf[reqLen - 2] = HILS_PING_TAIL_HBYTE;
	buf[reqLen - 1] = HILS_PING_TAIL_LBYTE;

	return serial_write(fd, buf, reqLen);
}


/* parses one HILCSV_HEADER row, returns 0 on success */
static int hils_inputRow(const char *line, unsigned long *idx, int64_t *time, hils_statePld_t *st)
{
	float v[12];
	int32_t lon, lat;
	int n = -1;

	if (sscanf(line, "%lu,%" SCNd64 ",%f,%f,%f,%f,%f,%f,%" SCNd32 ",%" SCNd32 ",%f,%f,%f,%f,%f,%f%n", idx, time,
				&v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &lon, &lat, &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &n) != HILCSV_COLS ||
			n < 0 || line[n] != '\0') {
		return -1;
	}
	/* scenario file keeps time in us */
	*time *= HILS_NS_PER_US;

	st->p = v[0];
	st->q = v[1];
	st->r = v[2];
	st->axm = v[3];
	st->aym = v[4];
	st->azm = v[5];
	st->lon = lon;
	st->lat = lat;
	st->alt = v[6];
	st->mx = v[7];
	st->my = v[8];
	st->mz = v[9];
	st->pStat = v[10];
	st->pTot = v[11];
	st->time = 0.0;

	return 0;
}


/* reads the whole scenario file, prints its sha256 and parses rows */
static int hils_inputLoad(hils_input_t *in, const char *path)
{
	FILE *f;
	char *data = NULL, *line, *next, *tmp;
	size_t size = 0, cap = 0, rowsCap = 0, len;
	hils_statePld_t *rows;
	unsigned long *idx;
	sha256_ctx_t sha;
	unsigned char digest[SHA256_DIGEST_SIZE];
	int64_t time, prevTime = 0;
	unsigned int lineNo;
	int header = 0, err = 0, i;

	f = fopen(path, "rb");
	if (f == NULL) {
		fprintf(stderr, "hilsim: cannot open input %s: %s\n", path, strerror(errno));
		return -1;
	}

	/* size is not known upfront for pipes, buffer grows until EOF */
	do {
		if (size == cap) {
			cap = (cap == 0) ? (1U << 20) : (2 * cap);
			tmp = realloc(data, cap + 1);
			if (tmp == NULL) {
				err = -1;
				break;
			}
			data = tmp;
		}
		len = fread(data + size, 1, cap - size, f);
		size += len;
	} while (len > 0);

	if (err != 0 || ferror(f)) {
		fprintf(stderr, "hilsim: cannot read input %s\n", path);
		fclose(f);
		free(data);
		return -1;
	}
	fclose(f);
	data[size] = '\0';

	sha256_init(&sha);
	sha256_process(&sha, (const unsigned char *)data, (unsigned long)size);
	sha256_done(&sha, digest);
	for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
		sprintf(&in->sha256[2 * i], "%02x", digest[i]);
	}
	fprintf(stderr, "hilsim: input %s, sha256 %s\n", path, in->sha256);

	in->rows = NULL;
	in->idx = NULL;
	in->cnt = 0;
	in->period = 0;
	for (line = data, lineNo = 1; err == 0 && line < data + size; line = next, lineNo++) {
		next = memchr(line, '\n', (size_t)(data + size - line));
		if (next != NULL) {
			*next++ = '\0';
		}
		else {
			next = data + size;
		}

		if (line[0] == '#' || line[0] == '\0') {
			continue;
		}

		if (header == 0) {
			if (strcmp(line, HILCSV_HEADER) != 0) {
				fprintf(stderr, "hilsim: %s:%u: expected header \"%s\"\n", path, lineNo, HILCSV_HEADER);
				err = -1;
			}
			header = 1;
			continue;
		}

		if (in->cnt == rowsCap) {
			rowsCap = (rowsCap == 0) ? 1024 : (2 * rowsCap);
			rows = realloc(in->rows, rowsCap * sizeof(*rows));
			if (rows == NULL) {
				fprintf(stderr, "hilsim: out of memory\n");
				err = -1;
				break;
			}
			in->rows = rows;

			idx = realloc(in->idx, rowsCap * sizeof(*idx));
			if (idx == NULL) {
				fprintf(stderr, "hilsim: out of memory\n");
				err = -1;
				break;
			}
			in->idx = idx;
		}

		if (hils_inputRow(line, &in->idx[in->cnt], &time, &in->rows[in->cnt]) < 0) {
			fprintf(stderr, "hilsim: %s:%u: invalid row\n", path, lineNo);
			err = -1;
			break;
		}

		/* frames are sent periodically, rows have to be equally spaced */
		if (in->cnt == 1) {
			in->period = time - prevTime;
		}
		if (in->cnt > 0 && (in->period <= 0 || time - prevTime != in->period)) {
			fprintf(stderr, "hilsim: %s:%u: rows not equally spaced in time\n", path, lineNo);
			err = -1;
			break;
		}
		prevTime = time;
		in->cnt++;
	}
	free(data);

	if (err == 0 && in->cnt < 2) {
		fprintf(stderr, "hilsim: %s: at least 2 rows needed to determine the frame period\n", path);
		err = -1;
	}
	if (err != 0) {
		free(in->rows);
		free(in->idx);
		in->rows = NULL;
		in->idx = NULL;
		return -1;
	}

	fprintf(stderr, "hilsim: %zu rows, period %" PRId64 " ns, duration %" PRId64 ".%03" PRId64 " s\n",
			in->cnt, in->period, HILS_SEC_MS((int64_t)in->cnt * in->period));

	return 0;
}


int main(int argc, char *argv[])
{
	const char *device = NULL;
	const char *inputPath = NULL;
	speed_t speed;
	hils_frame_t frame;
	hils_input_t in = { 0 };
	size_t row = 0;
	int baud = HILS_DEFAULT_BAUD;
	int fd;
	int option;
	int64_t period, tNext, tNow; /* [ns] */
	fd_set rfds;
	struct timespec ts;
	int64_t time = 0;          /* sim time [ns] */
	int64_t lastPrintTime = 0; /* [ns] */
	int64_t lastRxTime = -HILS_RX_TIMEOUT_NS;
	hils_rx_t rx = { 0 };
	uint8_t rxBuf[256];
	ssize_t rxLen;
	int i;
	const char *dumpPath = NULL;
	int count = 0; /* motor commands to receive before exit, 0 - unlimited */
	int ret = EXIT_SUCCESS;
	int err;
	int pingReq = HILS_DEFAULT_PING_REQ;
	int pingResp = HILS_DEFAULT_PING_RESP;

	while ((option = getopt(argc, argv, "d:i:b:o:n:ps:h")) != -1) {
		switch (option) {
			case 'd':
				device = optarg;
				break;

			case 'i':
				inputPath = optarg;
				break;

			case 'b':
				if (hils_parsePositive(optarg, &baud) < 0) {
					fprintf(stderr, "hilsim: invalid baud rate: %s\n", optarg);
					return EXIT_FAILURE;
				}
				break;

			case 'o':
				dumpPath = optarg;
				break;

			case 'n':
				if (hils_parsePositive(optarg, &count) < 0) {
					fprintf(stderr, "hilsim: invalid command count: %s\n", optarg);
					return EXIT_FAILURE;
				}
				break;

			case 'p':
				rx.ping = 1;
				break;

			case 's':
				if (hils_parseSizes(optarg, &pingReq, &pingResp) < 0) {
					fprintf(stderr, "hilsim: invalid ping sizes: %s (req %zu..%zu, resp %zu..%zu)\n", optarg,
							HILS_PING_REQ_MIN, HILS_PING_REQ_MAX, HILS_PING_RESP_MIN, HILS_PING_RESP_MAX);
					return EXIT_FAILURE;
				}
				break;

			default:
				hils_usage(argv[0]);
				return option == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (device == NULL || inputPath == NULL || optind != argc || serial_int2speed(baud, &speed) < 0) {
		hils_usage(argv[0]);
		return EXIT_FAILURE;
	}

	/* events are printed to stdout, keep them in order with stderr status lines also when redirected */
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* before opening the device, so that the hash can be checked even without the board attached */
	if (hils_inputLoad(&in, inputPath) < 0) {
		return EXIT_FAILURE;
	}

	fd = serial_open((char *)device, speed);
	if (fd < 0) {
		fprintf(stderr, "hilsim: cannot open %s at %d baud\n", device, baud);
		free(in.rows);
		free(in.idx);
		return EXIT_FAILURE;
	}

	if (signal(SIGINT, hils_signalHandler) == SIG_ERR || signal(SIGTERM, hils_signalHandler) == SIG_ERR) {
		fprintf(stderr, "hilsim: cannot install signal handler\n");
		close(fd);
		free(in.rows);
		free(in.idx);
		return EXIT_FAILURE;
	}

	if (dumpPath != NULL) {
		rx.dump = fopen(dumpPath, "w");
		if (rx.dump == NULL) {
			fprintf(stderr, "hilsim: cannot open dump file %s: %s\n", dumpPath, strerror(errno));
			close(fd);
			free(in.rows);
			free(in.idx);
			return EXIT_FAILURE;
		}
	}

	hils_rtSetup();

	frame.headHigh = HILS_HEAD_HBYTE;
	frame.headLow = HILS_HEAD_LBYTE;
	frame.tailHigh = HILS_TAIL_HBYTE;
	frame.tailLow = HILS_TAIL_LBYTE;

	rx.respLimit = (unsigned int)count;
	period = in.period;
	tNext = hils_monoNow();
	rx.tStart = tNext;
	hils_dumpMeta(&rx, &in, inputPath);
	while (running && (count == 0 || rx.respTotal < rx.respLimit) && row < in.cnt) {
		rx.time = time;
		rx.idx = in.idx[row];
		rx.respPeriod = 0;
		if (rx.ping) {
			rx.seq++;
			rx.tSent = hils_monoNow();
			rx.tSentSeq[rx.seq % HILS_PING_SEQ_CNT] = rx.tSent;
			err = hils_pingSend(fd, rx.seq, (size_t)pingReq, (size_t)pingResp);
		}
		else {
			frame.state = in.rows[row];
			/* frame protocol carries time in seconds as double */
			frame.state.time = (double)time / HILS_NS_PER_S;
			rx.tSent = hils_monoNow();
			err = serial_write(fd, (uint8_t *)&frame, sizeof(frame));
		}
		if (err != 0) {
			fprintf(stderr, "hilsim: serial write failed\n");
			ret = EXIT_FAILURE;
			break;
		}
		row++;

		/* receive until the next frame is due, each read is timestamped for latency measurement */
		tNext += period;
		while (running && (count == 0 || rx.respTotal < rx.respLimit) && (tNow = hils_monoNow()) < tNext) {
			FD_ZERO(&rfds);
			FD_SET(fd, &rfds);
			ts.tv_sec = (time_t)((tNext - tNow) / HILS_NS_PER_S);
			ts.tv_nsec = (long)((tNext - tNow) % HILS_NS_PER_S);

			if (pselect(fd + 1, &rfds, NULL, NULL, &ts, NULL) <= 0) {
				/* timeout, or signal (loop condition handles it) */
				continue;
			}

			/* the fd is non-blocking, drain everything that already arrived */
			rx.tRx = hils_monoNow();
			while ((rxLen = read(fd, rxBuf, sizeof(rxBuf))) > 0) {
				hils_rxParse(&rx, rxBuf, (size_t)rxLen);
			}
		}

		/* craft is expected to answer each sensor frame with exactly one command from the first one, hilsense each ping with one echo */
		if (running && (count == 0 || rx.respTotal < rx.respLimit) && rx.respPeriod != 1) {
			if (rx.respPeriod == 0) {
				printf("hilsim: t=%" PRId64 ".%03" PRId64 " s, no response\n", HILS_SEC_MS(time));
			}
			else {
				printf("hilsim: t=%" PRId64 ".%03" PRId64 " s, multiple responses (%u)\n", HILS_SEC_MS(time), rx.respPeriod);
			}
		}
		time += period;

		if ((time - lastPrintTime) >= HILS_NS_PER_S) {
			if (rx.frames > 0) {
				lastRxTime = time;
			}

			if ((time - lastRxTime) > HILS_RX_TIMEOUT_NS) {
				fprintf(stderr, "hilsim: t=%" PRId64 ".%03" PRId64 " s, nothing from craft (no link or craft not running)", HILS_SEC_MS(time));
			}
			else if (rx.ping) {
				if (rx.respFrames > 0) {
					fprintf(stderr, "hilsim: t=%" PRId64 ".%03" PRId64 " s, %u echo/s, rtt min %" PRId64 " ns avg %" PRId64 " ns max %" PRId64 " ns",
							HILS_SEC_MS(time), rx.respFrames, rx.latMin, rx.latSum / rx.respFrames, rx.latMax);
				}
				else {
					fprintf(stderr, "hilsim: t=%" PRId64 ".%03" PRId64 " s, no echoes", HILS_SEC_MS(time));
				}
			}
			else if (rx.respFrames > 0) {
				fprintf(stderr, "hilsim: t=%" PRId64 ".%03" PRId64 " s, mode %s, %u cmd/s, motors:", HILS_SEC_MS(time),
						(rx.mode[0] != '\0') ? rx.mode : "?", rx.respFrames);
				for (i = 0; i < (int)rx.cmdCnt; i++) {
					fprintf(stderr, " %.3f", rx.cmd[i]);
				}
				fprintf(stderr, ", latency avg %" PRId64 " ns max %" PRId64 " ns", rx.latSum / rx.respFrames, rx.latMax);
			}
			else {
				fprintf(stderr, "hilsim: t=%" PRId64 ".%03" PRId64 " s, mode %s, no motor commands", HILS_SEC_MS(time),
						(rx.mode[0] != '\0') ? rx.mode : "?");
			}
			if (rx.badFrames > 0) {
				fprintf(stderr, " (%u bad frames)", rx.badFrames);
			}
			fprintf(stderr, "\n");

			lastPrintTime = time;
			rx.frames = 0;
			rx.respFrames = 0;
			rx.badFrames = 0;
			rx.latSum = 0;
			rx.latMin = 0;
			rx.latMax = 0;
		}
	}

	if (count != 0 && rx.respTotal == rx.respLimit) {
		fprintf(stderr, "hilsim: received %u %s, exiting\n", rx.respTotal, rx.ping ? "echoes" : "motor commands");
	}
	else if (row == in.cnt) {
		fprintf(stderr, "hilsim: all %zu input rows sent, exiting\n", in.cnt);
	}

	fprintf(stderr, "hilsim: sent %zu %s, received %u %s\n", row, rx.ping ? "pings" : "sensor frames",
			rx.respTotal, rx.ping ? "echoes" : "motor commands");

	if (rx.totLatCnt > 0) {
		fprintf(stderr, "hilsim: %s of %u responses: min %" PRId64 " ns avg %" PRId64 " ns max %" PRId64 " ns\n",
				rx.ping ? "rtt" : "latency", rx.totLatCnt, rx.totLatMin, rx.totLatSum / rx.totLatCnt, rx.totLatMax);
	}
	else {
		fprintf(stderr, "hilsim: no responses measured\n");
	}

	if (rx.dump != NULL) {
		fprintf(rx.dump, "# sent: %zu\n", row);
		fclose(rx.dump);
	}
	close(fd);
	free(in.rows);
	free(in.idx);
	return ret;
}
