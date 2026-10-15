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

#include <errno.h>
#include <inttypes.h>
#include <math.h>
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
#define HILS_CMD_PRINT_CNT 4       /* quad: FR, RR, RL, FL */
#define HILS_RX_TIMEOUT_US 2000000 /* mode is sent at least once a second, longer silence means no link */

#define HILS_US_PER_S      1000000
#define HILS_PERIOD_OFFSET 41 /* [us] added to frame period to hit different partition cycle moments */

/* prints time [us] as seconds with ms resolution, use with "%" PRId64 ".%03" PRId64 */
#define HILS_SEC_MS(us) ((us) / HILS_US_PER_S), (((us) % HILS_US_PER_S) / 1000)

#define HILS_DEFAULT_BAUD 2000000
#define HILS_DEFAULT_RATE 100

/* by default ping and echo have the size of the sensor frame and the quad motor command frame */
#define HILS_DEFAULT_PING_REQ  sizeof(hils_frame_t)
#define HILS_DEFAULT_PING_RESP (6 + HILS_CMD_PRINT_CNT * sizeof(float))

/* scenario: each attitude angle swings as a sinusoid of given amplitude and frequency, only phase is seeded */
#define HILS_DEFAULT_YAW   30.0 /* yaw swing amplitude [deg] */
#define HILS_DEFAULT_PITCH 10.0 /* pitch swing amplitude [deg] */
#define HILS_DEFAULT_ROLL  0.0  /* roll swing amplitude [deg] */
#define HILS_DEFAULT_FREQ  0.5  /* swing frequency [Hz] */
#define HILS_DEFAULT_NOISE 1.0  /* noise scale, multiplies HILS_NOISE_* sigmas */

/* noise standard deviations at scale 1.0 */
#define HILS_NOISE_GYRO  0.005f /* [rad/s] */
#define HILS_NOISE_ACCEL 0.05f  /* [m/s^2] */
#define HILS_NOISE_MAG   0.005f /* [] */
#define HILS_NOISE_GPS   20.0f  /* [deg * 1e7], ~2 m */
#define HILS_NOISE_ALT   0.5f   /* [m] */
#define HILS_NOISE_PRESS 5.0f   /* [Pa] */

#define HILS_DEG2RAD (M_PI / 180.0)

/* SCHED_FIFO priority, below threaded irq handlers (50 on PREEMPT_RT) so that serial irqs aren't delayed */
#define HILS_RT_PRIO 49


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

	FILE *dump;   /* optional log of every received frame, NULL if disabled */
	int64_t time; /* sim time of the last sent frame, used as dump timestamp [us] */

	/* latency from start of sending the last sensor frame to receiving a command */
	int64_t tSent;  /* monotonic time at start of the last sensor frame write [us] */
	int64_t tRx;    /* monotonic time of the read that delivered the current bytes [us] */
	int64_t latSum; /* since last status print [us] */
	int64_t latMin; /* since last status print [us] */
	int64_t latMax; /* since last status print [us] */

	/* round-trip time measurement, sensor frames are replaced by pings echoed by hilsense */
	int ping;
	uint32_t seq;                        /* seq of the last sent ping */
	int64_t tSentSeq[HILS_PING_SEQ_CNT]; /* monotonic send time of recent pings, indexed by seq [us] */
} hils_rx_t;


typedef struct {
	double amp;   /* [rad] */
	double omega; /* [rad/s] */
	double phase; /* [rad] */
} hils_sine_t;


/* pseudorandom hover scenario, same seed and parameters give the same frames */
typedef struct {
	int enabled;
	uint64_t rng;  /* splitmix64 state, own generator to be reproducible across platforms */
	double noise;  /* noise scale, 0 - no noise */
	double fadeIn; /* swing amplitude fade-in time [s], so that scenario starts from hover */

	/* swing of yaw, pitch, roll */
	hils_sine_t swing[3];
} hils_scen_t;


enum { hils_yaw = 0,
	hils_pitch,
	hils_roll };


static volatile sig_atomic_t running = 1;


static void hils_signalHandler(int signal)
{
	(void)signal;
	running = 0;
}


static void hils_usage(const char *program)
{
	fprintf(stderr, "Usage: %s -d <serial-device> [-b baud] [-r rate] [-o dump-file] [-n count] [-p [-s req,resp]]\n", program);
	fprintf(stderr, "       [-S seed [-Y deg] [-P deg] [-R deg] [-F hz] [-N scale]]\n");
	fprintf(stderr, "  -d <serial-device>  serial device to transmit HIL frames\n");
	fprintf(stderr, "  -b <baud>           baud rate (default: %d)\n", HILS_DEFAULT_BAUD);
	fprintf(stderr, "  -r <rate>           frame rate in Hz (default: %d)\n", HILS_DEFAULT_RATE);
	fprintf(stderr, "  -o <dump-file>      write every frame received from craft to file, one line per frame:\n");
	fprintf(stderr, "                      <time> cmd <c0..cn> <latency> | <time> mode <name> | <time> msg <text> | <time> bad <type> <len> |\n");
	fprintf(stderr, "                      <time> noresp | <time> multiresp <count> | <time> rtt <seq> <latency> | <time> late <seq> <latency>\n");
	fprintf(stderr, "                      time and latency in us, only motors sent by craft are written\n");
	fprintf(stderr, "  -n <count>          exit after receiving <count> motor commands or echoes (default: run until signal)\n");
	fprintf(stderr, "  -p                  measure round-trip time: send pings echoed by hilsense instead of sensor frames\n");
	fprintf(stderr, "  -s <req>,<resp>     ping and echo frame sizes in bytes (default: %zu,%zu - sensor and quad command frames)\n",
			HILS_DEFAULT_PING_REQ, HILS_DEFAULT_PING_RESP);
	fprintf(stderr, "  -S <seed>           pseudorandom scenario: swing attitude and add sensor noise (default: static hover frame)\n");
	fprintf(stderr, "  -Y <deg>            yaw swing amplitude (default: %.1f)\n", HILS_DEFAULT_YAW);
	fprintf(stderr, "  -P <deg>            pitch swing amplitude (default: %.1f)\n", HILS_DEFAULT_PITCH);
	fprintf(stderr, "  -R <deg>            roll swing amplitude (default: %.1f)\n", HILS_DEFAULT_ROLL);
	fprintf(stderr, "  -F <hz>             swing frequency (default: %.2f)\n", HILS_DEFAULT_FREQ);
	fprintf(stderr, "  -N <scale>          sensor noise scale, 0 disables noise (default: %.1f)\n", HILS_DEFAULT_NOISE);
}


/* [us] */
static int64_t hils_monoNow(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * HILS_US_PER_S + ts.tv_nsec / 1000;
}


/* best effort, reduces scheduling and page fault jitter in timing measurements, needs CAP_SYS_NICE and CAP_IPC_LOCK */
static void hils_rtSetup(void)
{
#ifdef __linux__
	struct sched_param param = { .sched_priority = HILS_RT_PRIO };

	if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) {
		fprintf(stderr, "hilsim: warning: cannot lock memory: %s\n", strerror(errno));
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


static int hils_parseDouble(const char *value, double min, double max, double *result)
{
	char *end;
	double parsed;

	errno = 0;
	parsed = strtod(value, &end);
	if (errno != 0 || *value == '\0' || *end != '\0' || !(parsed >= min && parsed <= max)) {
		return -1;
	}

	*result = parsed;
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
}


static void hils_rxBad(hils_rx_t *rx, uint8_t type, size_t len)
{
	rx->badFrames++;

	if (rx->dump != NULL) {
		fprintf(rx->dump, "%" PRId64 " bad %u %zu\n", rx->time, (unsigned int)type, len);
	}
}


static void hils_rxFrame(hils_rx_t *rx, uint8_t type, const uint8_t *payload, size_t len)
{
	char text[HILS_RX_PAYLOAD_MAX + 1];
	int64_t lat;
	uint32_t seq;
	unsigned int i;

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

			lat = rx->tRx - rx->tSent;
			hils_rxLat(rx, lat);

			if (rx->dump != NULL) {
				fprintf(rx->dump, "%" PRId64 " cmd", rx->time);
				for (i = 0; i < rx->cmdCnt; i++) {
					fprintf(rx->dump, " %.4f", rx->cmd[i]);
				}
				fprintf(rx->dump, " %" PRId64 "\n", lat);
			}
			break;

		case HILS_RX_TYPE_MODE:
			memcpy(text, payload, len);
			text[len] = '\0';
			if (strcmp(text, rx->mode) != 0) {
				fprintf(stderr, "hilsim: craft mode %s -> %s\n", (rx->mode[0] != '\0') ? rx->mode : "?", text);
				strcpy(rx->mode, text);
			}

			if (rx->dump != NULL) {
				fprintf(rx->dump, "%" PRId64 " mode %s\n", rx->time, text);
			}
			break;

		case HILS_RX_TYPE_MSG:
			memcpy(text, payload, len);
			text[len] = '\0';
			fprintf(stderr, "hilsim: craft: %s\n", text);

			if (rx->dump != NULL) {
				fprintf(rx->dump, "%" PRId64 " msg %s\n", rx->time, text);
			}
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
				fprintf(stderr, "hilsim: t=%" PRId64 ".%03" PRId64 " s, late response seq %u, rtt %" PRId64 " us\n", HILS_SEC_MS(rx->time), seq, lat);
				if (rx->dump != NULL) {
					fprintf(rx->dump, "%" PRId64 " late %u %" PRId64 "\n", rx->time, seq, lat);
				}
				break;
			}
			if (rx->respLimit != 0 && rx->respTotal >= rx->respLimit) {
				return;
			}
			rx->respFrames++;
			rx->respTotal++;
			rx->respPeriod++;
			hils_rxLat(rx, lat);

			if (rx->dump != NULL) {
				fprintf(rx->dump, "%" PRId64 " rtt %u %" PRId64 "\n", rx->time, seq, lat);
			}
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


/* level hover readings in body frame (FRD, earth frame NED), see hils_toSensor() for what is actually sent
 * time - frame time [us] */
static void hils_hoverFrame(hils_frame_t *frame, int64_t time)
{
	memset(frame, 0, sizeof(*frame));
	frame->headHigh = HILS_HEAD_HBYTE;
	frame->headLow = HILS_HEAD_LBYTE;
	/* angular rates [rad/s] */
	frame->state.p = 0.01f;
	frame->state.q = 0.02f;
	frame->state.r = 0.03f;
	/* measured acceleration [m/s^2], specific force points up */
	frame->state.axm = 0.05f;
	frame->state.aym = 0.04f;
	frame->state.azm = -9.80665f;
	/* gps */
	frame->state.alt = 10.0f;
	frame->state.lat = 515000000;
	frame->state.lon = 190000000;
	/* magnetometer */
	frame->state.mx = 0.2f;
	frame->state.my = 0.05f;
	frame->state.mz = 1.0f;
	frame->state.pStat = 10132500.0f;
	frame->state.pTot = 101330.0f;
	/* frame protocol carries time in seconds as double */
	frame->state.time = (double)time / HILS_US_PER_S;
	frame->tailHigh = HILS_TAIL_HBYTE;
	frame->tailLow = HILS_TAIL_LBYTE;
}


/* splitmix64 */
static uint64_t hils_rngNext(hils_scen_t *scen)
{
	uint64_t z = (scen->rng += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}


/* uniform in [0, 1) */
static double hils_rngUniform(hils_scen_t *scen)
{
	return (double)(hils_rngNext(scen) >> 11) * (1.0 / 9007199254740992.0);
}


/* standard normal, Box-Muller */
static double hils_rngGauss(hils_scen_t *scen)
{
	double u1 = 1.0 - hils_rngUniform(scen); /* (0, 1], log(0) avoided */
	double u2 = hils_rngUniform(scen);

	return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}


/* amp - swing amplitude of yaw, pitch, roll [rad], freq [Hz] */
static void hils_scenInit(hils_scen_t *scen, uint64_t seed, const double amp[3], double freq, double noise)
{
	int axis;

	scen->enabled = 1;
	scen->rng = seed;
	scen->noise = noise;
	scen->fadeIn = 1.0 / freq; /* one swing period */

	for (axis = 0; axis < 3; axis++) {
		scen->swing[axis].amp = amp[axis];
		scen->swing[axis].omega = 2.0 * M_PI * freq;
		scen->swing[axis].phase = 2.0 * M_PI * hils_rngUniform(scen);
	}
}


static void hils_scenAngle(const hils_scen_t *scen, int axis, double time, double *angle, double *rate)
{
	const hils_sine_t *s = &scen->swing[axis];
	double env = 1.0, dEnv = 0.0, x;

	/* smoothstep envelope, angle and rate start from zero (hover) and reach the swing smoothly */
	if (time < scen->fadeIn) {
		x = time / scen->fadeIn;
		env = x * x * (3.0 - 2.0 * x);
		dEnv = 6.0 * x * (1.0 - x) / scen->fadeIn;
	}

	*angle = env * s->amp * sin(s->omega * time + s->phase);
	*rate = dEnv * s->amp * sin(s->omega * time + s->phase) + env * s->amp * s->omega * cos(s->omega * time + s->phase);
}


/*
 * Converts body frame (FRD) readings to the imu sensor frame. Craft applies board mounting correction
 * from calib.conf (@accrot: swap x and y, invert all axes) to imu and mag readings, so the inverse is applied here.
 * The mapping (x, y, z) -> (-y, -x, -z) is its own inverse.
 */
static void hils_toSensor(hils_statePld_t *st)
{
	float tmp;

	tmp = st->p;
	st->p = -st->q;
	st->q = -tmp;
	st->r = -st->r;

	tmp = st->axm;
	st->axm = -st->aym;
	st->aym = -tmp;
	st->azm = -st->azm;

	tmp = st->mx;
	st->mx = -st->my;
	st->my = -tmp;
	st->mz = -st->mz;
}


/* rotates vector from level, north-aligned frame to body frame of yaw-pitch-roll (ZYX) attitude */
static void hils_toBody(double v[3], double yaw, double pitch, double roll)
{
	double x, y, z;

	x = cos(yaw) * v[0] + sin(yaw) * v[1];
	y = -sin(yaw) * v[0] + cos(yaw) * v[1];

	v[0] = cos(pitch) * x - sin(pitch) * v[2];
	z = sin(pitch) * x + cos(pitch) * v[2];

	v[1] = cos(roll) * y + sin(roll) * z;
	v[2] = -sin(roll) * y + cos(roll) * z;
}


/* hover frame with swinging attitude, level hover readings are rotated, static rates are kept as gyro bias
 * time - frame time [us], scenTime - time since scenario start [us] */
static void hils_scenFrame(hils_frame_t *frame, hils_scen_t *scen, int64_t time, int64_t scenTime)
{
	double yaw, pitch, roll, dYaw, dPitch, dRoll;
	double acc[3], mag[3];
	double n = scen->noise;
	double t = (double)scenTime / HILS_US_PER_S; /* swing argument [s] */
	hils_statePld_t *st = &frame->state;

	hils_hoverFrame(frame, time);

	hils_scenAngle(scen, hils_yaw, t, &yaw, &dYaw);
	hils_scenAngle(scen, hils_pitch, t, &pitch, &dPitch);
	hils_scenAngle(scen, hils_roll, t, &roll, &dRoll);

	/* euler angle rates to body angular rates */
	st->p += (float)(dRoll - dYaw * sin(pitch));
	st->q += (float)(dPitch * cos(roll) + dYaw * sin(roll) * cos(pitch));
	st->r += (float)(-dPitch * sin(roll) + dYaw * cos(roll) * cos(pitch));

	acc[0] = st->axm;
	acc[1] = st->aym;
	acc[2] = st->azm;
	hils_toBody(acc, yaw, pitch, roll);
	st->axm = (float)acc[0];
	st->aym = (float)acc[1];
	st->azm = (float)acc[2];

	mag[0] = st->mx;
	mag[1] = st->my;
	mag[2] = st->mz;
	hils_toBody(mag, yaw, pitch, roll);
	st->mx = (float)mag[0];
	st->my = (float)mag[1];
	st->mz = (float)mag[2];

	if (n > 0.0) {
		st->p += (float)(n * HILS_NOISE_GYRO * hils_rngGauss(scen));
		st->q += (float)(n * HILS_NOISE_GYRO * hils_rngGauss(scen));
		st->r += (float)(n * HILS_NOISE_GYRO * hils_rngGauss(scen));
		st->axm += (float)(n * HILS_NOISE_ACCEL * hils_rngGauss(scen));
		st->aym += (float)(n * HILS_NOISE_ACCEL * hils_rngGauss(scen));
		st->azm += (float)(n * HILS_NOISE_ACCEL * hils_rngGauss(scen));
		st->mx += (float)(n * HILS_NOISE_MAG * hils_rngGauss(scen));
		st->my += (float)(n * HILS_NOISE_MAG * hils_rngGauss(scen));
		st->mz += (float)(n * HILS_NOISE_MAG * hils_rngGauss(scen));
		st->lat += (int32_t)lround(n * HILS_NOISE_GPS * hils_rngGauss(scen));
		st->lon += (int32_t)lround(n * HILS_NOISE_GPS * hils_rngGauss(scen));
		st->alt += (float)(n * HILS_NOISE_ALT * hils_rngGauss(scen));
		st->pStat += (float)(n * HILS_NOISE_PRESS * hils_rngGauss(scen));
		st->pTot += (float)(n * HILS_NOISE_PRESS * hils_rngGauss(scen));
	}
}


int main(int argc, char *argv[])
{
	const char *device = NULL;
	speed_t speed;
	hils_frame_t frame;
	int baud = HILS_DEFAULT_BAUD;
	int rate = HILS_DEFAULT_RATE;
	int fd;
	int option;
	int64_t period, tNext, tNow; /* [us] */
	fd_set rfds;
	struct timeval tv;
	int64_t time = 0;          /* sim time [us] */
	int64_t lastPrintTime = 0; /* [us] */
	int64_t lastRxTime = -HILS_RX_TIMEOUT_US;
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
	hils_scen_t scen = { 0 };
	int64_t scenStart = -1; /* sim time of the first scenario frame [us], -1 - not started yet */
	const char *seedStr = NULL;
	unsigned long long seed = 0;
	double swingDeg[3] = { HILS_DEFAULT_YAW, HILS_DEFAULT_PITCH, HILS_DEFAULT_ROLL };
	double swingRad[3];
	double freq = HILS_DEFAULT_FREQ;
	double noise = HILS_DEFAULT_NOISE;
	char *end;

	while ((option = getopt(argc, argv, "d:b:r:o:n:ps:S:Y:P:R:F:N:h")) != -1) {
		switch (option) {
			case 'd':
				device = optarg;
				break;

			case 'b':
				if (hils_parsePositive(optarg, &baud) < 0) {
					fprintf(stderr, "hilsim: invalid baud rate: %s\n", optarg);
					return EXIT_FAILURE;
				}
				break;

			case 'r':
				if (hils_parsePositive(optarg, &rate) < 0 || rate > 1000) {
					fprintf(stderr, "hilsim: rate must be between 1 and 1000 Hz\n");
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

			case 'S':
				errno = 0;
				seed = strtoull(optarg, &end, 0);
				if (errno != 0 || *optarg == '\0' || *optarg == '-' || *end != '\0') {
					fprintf(stderr, "hilsim: invalid seed: %s\n", optarg);
					return EXIT_FAILURE;
				}
				seedStr = optarg;
				break;

			case 'Y':
			case 'P':
			case 'R':
				if (hils_parseDouble(optarg, 0.0, 180.0, &swingDeg[(option == 'Y') ? hils_yaw : (option == 'P') ? hils_pitch :
																												  hils_roll]) < 0) {
					fprintf(stderr, "hilsim: swing amplitude must be between 0 and 180 deg\n");
					return EXIT_FAILURE;
				}
				break;

			case 'F':
				if (hils_parseDouble(optarg, 1e-3, 100.0, &freq) < 0) {
					fprintf(stderr, "hilsim: swing frequency must be between 0.001 and 100 Hz\n");
					return EXIT_FAILURE;
				}
				break;

			case 'N':
				if (hils_parseDouble(optarg, 0.0, 1000.0, &noise) < 0) {
					fprintf(stderr, "hilsim: noise scale must be between 0 and 1000\n");
					return EXIT_FAILURE;
				}
				break;

			default:
				hils_usage(argv[0]);
				return option == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (device == NULL || optind != argc || serial_int2speed(baud, &speed) < 0) {
		hils_usage(argv[0]);
		return EXIT_FAILURE;
	}

	fd = serial_open((char *)device, speed);
	if (fd < 0) {
		fprintf(stderr, "hilsim: cannot open %s at %d baud\n", device, baud);
		return EXIT_FAILURE;
	}

	if (signal(SIGINT, hils_signalHandler) == SIG_ERR || signal(SIGTERM, hils_signalHandler) == SIG_ERR) {
		fprintf(stderr, "hilsim: cannot install signal handler\n");
		close(fd);
		return EXIT_FAILURE;
	}

	if (dumpPath != NULL) {
		rx.dump = fopen(dumpPath, "w");
		if (rx.dump == NULL) {
			fprintf(stderr, "hilsim: cannot open dump file %s: %s\n", dumpPath, strerror(errno));
			close(fd);
			return EXIT_FAILURE;
		}
	}

	if (seedStr != NULL) {
		for (i = 0; i < 3; i++) {
			swingRad[i] = swingDeg[i] * HILS_DEG2RAD;
		}
		hils_scenInit(&scen, (uint64_t)seed, swingRad, freq, noise);
		fprintf(stderr, "hilsim: scenario seed %s, swing yaw %.1f pitch %.1f roll %.1f deg, %.2f Hz, noise %.2f\n",
				seedStr, swingDeg[hils_yaw], swingDeg[hils_pitch], swingDeg[hils_roll], freq, noise);
	}

	hils_rtSetup();

	rx.respLimit = (unsigned int)count;
	period = HILS_US_PER_S / rate + HILS_PERIOD_OFFSET;
	tNext = hils_monoNow();
	while (running && (count == 0 || rx.respTotal < rx.respLimit)) {
		rx.time = time;
		rx.respPeriod = 0;
		if (rx.ping) {
			rx.seq++;
			rx.tSent = hils_monoNow();
			rx.tSentSeq[rx.seq % HILS_PING_SEQ_CNT] = rx.tSent;
			err = hils_pingSend(fd, rx.seq, (size_t)pingReq, (size_t)pingResp);
		}
		else {
			/* static hover frames until craft answers for the first time, so that calibration doesn't fail,
			 * scenario starts from its beginning then, independent of how long hilsim has been running */
			if (scen.enabled && rx.respTotal > 0) {
				if (scenStart < 0) {
					scenStart = time;
				}
				hils_scenFrame(&frame, &scen, time, time - scenStart);
			}
			else {
				hils_hoverFrame(&frame, time);
			}
			hils_toSensor(&frame.state);
			rx.tSent = hils_monoNow();
			err = serial_write(fd, (uint8_t *)&frame, sizeof(frame));
		}
		if (err != 0) {
			fprintf(stderr, "hilsim: serial write failed\n");
			ret = EXIT_FAILURE;
			break;
		}

		/* receive until the next frame is due, each read is timestamped for latency measurement */
		tNext += period;
		while (running && (count == 0 || rx.respTotal < rx.respLimit) && (tNow = hils_monoNow()) < tNext) {
			FD_ZERO(&rfds);
			FD_SET(fd, &rfds);
			tv.tv_sec = (time_t)((tNext - tNow) / HILS_US_PER_S);
			tv.tv_usec = (suseconds_t)((tNext - tNow) % HILS_US_PER_S);

			if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
				/* timeout, or signal (loop condition handles it) */
				continue;
			}

			/* the fd is non-blocking, drain everything that already arrived */
			rx.tRx = hils_monoNow();
			while ((rxLen = read(fd, rxBuf, sizeof(rxBuf))) > 0) {
				hils_rxParse(&rx, rxBuf, (size_t)rxLen);
			}
		}

		/* craft is expected to answer each sensor frame with exactly one command once motors got armed, hilsense each ping with one echo */
		if (running && rx.respTotal > 0 && (count == 0 || rx.respTotal < rx.respLimit) && rx.respPeriod != 1) {
			if (rx.respPeriod == 0) {
				fprintf(stderr, "hilsim: t=%" PRId64 ".%03" PRId64 " s, no response\n", HILS_SEC_MS(time));
				if (rx.dump != NULL) {
					fprintf(rx.dump, "%" PRId64 " noresp\n", time);
				}
			}
			else {
				fprintf(stderr, "hilsim: t=%" PRId64 ".%03" PRId64 " s, multiple responses (%u)\n", HILS_SEC_MS(time), rx.respPeriod);
				if (rx.dump != NULL) {
					fprintf(rx.dump, "%" PRId64 " multiresp %u\n", time, rx.respPeriod);
				}
			}
		}
		time += period;

		if ((time - lastPrintTime) >= HILS_US_PER_S) {
			if (rx.frames > 0) {
				lastRxTime = time;
			}

			if ((time - lastRxTime) > HILS_RX_TIMEOUT_US) {
				fprintf(stderr, "hilsim: t=%" PRId64 ".%03" PRId64 " s, nothing from craft (no link or craft not running)", HILS_SEC_MS(time));
			}
			else if (rx.ping) {
				if (rx.respFrames > 0) {
					fprintf(stderr, "hilsim: t=%" PRId64 ".%03" PRId64 " s, %u echo/s, rtt min %" PRId64 " avg %" PRId64 " max %" PRId64 " us",
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
				fprintf(stderr, ", latency avg %" PRId64 " max %" PRId64 " us", rx.latSum / rx.respFrames, rx.latMax);
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

	if (rx.dump != NULL) {
		fclose(rx.dump);
	}
	close(fd);
	return ret;
}
