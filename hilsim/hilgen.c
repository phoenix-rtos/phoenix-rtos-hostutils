/*
 * Phoenix-RTOS
 *
 * HIL sensor scenario generator
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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libsha256.h>

#include "hilcsv.h"


#define HILG_US_PER_S      1000000
#define HILG_PERIOD_OFFSET 41 /* [us] added to frame period to hit different partition cycle moments */

#define HILG_DEFAULT_RATE  100
#define HILG_DEFAULT_COUNT 1000

/* scenario: each attitude angle swings as a sinusoid of given amplitude and frequency, only phase is seeded */
#define HILG_DEFAULT_YAW   30.0 /* yaw swing amplitude [deg] */
#define HILG_DEFAULT_PITCH 10.0 /* pitch swing amplitude [deg] */
#define HILG_DEFAULT_ROLL  0.0  /* roll swing amplitude [deg] */
#define HILG_DEFAULT_FREQ  0.5  /* swing frequency [Hz] */
#define HILG_DEFAULT_NOISE 1.0  /* noise scale, multiplies HILG_NOISE_* sigmas */

/* noise standard deviations at scale 1.0 */
#define HILG_NOISE_GYRO  0.005f /* [rad/s] */
#define HILG_NOISE_ACCEL 0.05f  /* [m/s^2] */
#define HILG_NOISE_MAG   0.005f /* [] */
#define HILG_NOISE_GPS   20.0f  /* [deg * 1e7], ~2 m */
#define HILG_NOISE_ALT   0.5f   /* [m] */
#define HILG_NOISE_PRESS 5.0f   /* [Pa] */

#define HILG_DEG2RAD (M_PI / 180.0)


/* sensor readings of one frame, columns of HILCSV_HEADER */
typedef struct {
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
} hilg_state_t;


typedef struct {
	double amp;   /* [rad] */
	double omega; /* [rad/s] */
	double phase; /* [rad] */
} hilg_sine_t;


/* pseudorandom hover scenario, same seed and parameters give the same frames */
typedef struct {
	int enabled;
	uint64_t rng;  /* splitmix64 state, own generator to be reproducible across platforms */
	double noise;  /* noise scale, 0 - no noise */
	double fadeIn; /* swing amplitude fade-in time [s], so that scenario starts from hover */

	/* swing of yaw, pitch, roll */
	hilg_sine_t swing[3];
} hilg_scen_t;


enum { hilg_yaw = 0,
	hilg_pitch,
	hilg_roll };


/* output stream, everything written is hashed */
typedef struct {
	FILE *f;
	sha256_ctx_t sha;
} hilg_out_t;


static void hilg_usage(const char *program)
{
	fprintf(stderr, "Usage: %s [-n count] [-o csv-file] [-r rate] [-S seed [-Y deg] [-P deg] [-R deg] [-F hz] [-N scale]]\n", program);
	fprintf(stderr, "  -n <count>          number of rows (frames) (default: %d)\n", HILG_DEFAULT_COUNT);
	fprintf(stderr, "  -o <csv-file>       output file (default: stdout), sha256 of the output is printed to stderr at the end\n");
	fprintf(stderr, "  -r <rate>           frame rate in Hz (default: %d), period is extended by %d us\n", HILG_DEFAULT_RATE, HILG_PERIOD_OFFSET);
	fprintf(stderr, "  -S <seed>           pseudorandom scenario: swing attitude and add sensor noise (default: static hover frames)\n");
	fprintf(stderr, "  -Y <deg>            yaw swing amplitude (default: %.1f)\n", HILG_DEFAULT_YAW);
	fprintf(stderr, "  -P <deg>            pitch swing amplitude (default: %.1f)\n", HILG_DEFAULT_PITCH);
	fprintf(stderr, "  -R <deg>            roll swing amplitude (default: %.1f)\n", HILG_DEFAULT_ROLL);
	fprintf(stderr, "  -F <hz>             swing frequency (default: %.2f)\n", HILG_DEFAULT_FREQ);
	fprintf(stderr, "  -N <scale>          sensor noise scale, 0 disables noise (default: %.1f)\n", HILG_DEFAULT_NOISE);
}


static int hilg_parsePositive(const char *value, int *result)
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


static int hilg_parseDouble(const char *value, double min, double max, double *result)
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


static int hilg_print(hilg_out_t *out, const char *fmt, ...)
{
	char line[512];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	if (len < 0 || (size_t)len >= sizeof(line)) {
		return -1;
	}
	sha256_process(&out->sha, (const unsigned char *)line, (unsigned long)len);

	return (fwrite(line, 1, (size_t)len, out->f) == (size_t)len) ? 0 : -1;
}


/* level hover readings in body frame (FRD, earth frame NED), see hilg_toSensor() for what is actually sent */
static void hilg_hoverState(hilg_state_t *st)
{
	memset(st, 0, sizeof(*st));
	/* angular rates [rad/s] */
	st->p = 0.01f;
	st->q = 0.02f;
	st->r = 0.03f;
	/* measured acceleration [m/s^2], specific force points up */
	st->axm = 0.05f;
	st->aym = 0.04f;
	st->azm = -9.80665f;
	/* gps */
	st->alt = 10.0f;
	st->lat = 515000000;
	st->lon = 190000000;
	/* magnetometer */
	st->mx = 0.2f;
	st->my = 0.05f;
	st->mz = 1.0f;
	st->pStat = 10132500.0f;
	st->pTot = 101330.0f;
}


/* splitmix64 */
static uint64_t hilg_rngNext(hilg_scen_t *scen)
{
	uint64_t z = (scen->rng += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}


/* uniform in [0, 1) */
static double hilg_rngUniform(hilg_scen_t *scen)
{
	return (double)(hilg_rngNext(scen) >> 11) * (1.0 / 9007199254740992.0);
}


/* standard normal, Box-Muller */
static double hilg_rngGauss(hilg_scen_t *scen)
{
	double u1 = 1.0 - hilg_rngUniform(scen); /* (0, 1], log(0) avoided */
	double u2 = hilg_rngUniform(scen);

	return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}


/* amp - swing amplitude of yaw, pitch, roll [rad], freq [Hz] */
static void hilg_scenInit(hilg_scen_t *scen, uint64_t seed, const double amp[3], double freq, double noise)
{
	int axis;

	scen->enabled = 1;
	scen->rng = seed;
	scen->noise = noise;
	scen->fadeIn = 1.0 / freq; /* one swing period */

	for (axis = 0; axis < 3; axis++) {
		scen->swing[axis].amp = amp[axis];
		scen->swing[axis].omega = 2.0 * M_PI * freq;
		scen->swing[axis].phase = 2.0 * M_PI * hilg_rngUniform(scen);
	}
}


static void hilg_scenAngle(const hilg_scen_t *scen, int axis, double time, double *angle, double *rate)
{
	const hilg_sine_t *s = &scen->swing[axis];
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
static void hilg_toSensor(hilg_state_t *st)
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
static void hilg_toBody(double v[3], double yaw, double pitch, double roll)
{
	double x, y, z;

	x = cos(yaw) * v[0] + sin(yaw) * v[1];
	y = -sin(yaw) * v[0] + cos(yaw) * v[1];

	v[0] = cos(pitch) * x - sin(pitch) * v[2];
	z = sin(pitch) * x + cos(pitch) * v[2];

	v[1] = cos(roll) * y + sin(roll) * z;
	v[2] = -sin(roll) * y + cos(roll) * z;
}


/* hover readings with swinging attitude, level hover readings are rotated, static rates are kept as gyro bias
 * scenTime - time since scenario start [us] */
static void hilg_scenState(hilg_state_t *st, hilg_scen_t *scen, int64_t scenTime)
{
	double yaw, pitch, roll, dYaw, dPitch, dRoll;
	double acc[3], mag[3];
	double n = scen->noise;
	double t = (double)scenTime / HILG_US_PER_S; /* swing argument [s] */

	hilg_hoverState(st);

	hilg_scenAngle(scen, hilg_yaw, t, &yaw, &dYaw);
	hilg_scenAngle(scen, hilg_pitch, t, &pitch, &dPitch);
	hilg_scenAngle(scen, hilg_roll, t, &roll, &dRoll);

	/* euler angle rates to body angular rates */
	st->p += (float)(dRoll - dYaw * sin(pitch));
	st->q += (float)(dPitch * cos(roll) + dYaw * sin(roll) * cos(pitch));
	st->r += (float)(-dPitch * sin(roll) + dYaw * cos(roll) * cos(pitch));

	acc[0] = st->axm;
	acc[1] = st->aym;
	acc[2] = st->azm;
	hilg_toBody(acc, yaw, pitch, roll);
	st->axm = (float)acc[0];
	st->aym = (float)acc[1];
	st->azm = (float)acc[2];

	mag[0] = st->mx;
	mag[1] = st->my;
	mag[2] = st->mz;
	hilg_toBody(mag, yaw, pitch, roll);
	st->mx = (float)mag[0];
	st->my = (float)mag[1];
	st->mz = (float)mag[2];

	if (n > 0.0) {
		st->p += (float)(n * HILG_NOISE_GYRO * hilg_rngGauss(scen));
		st->q += (float)(n * HILG_NOISE_GYRO * hilg_rngGauss(scen));
		st->r += (float)(n * HILG_NOISE_GYRO * hilg_rngGauss(scen));
		st->axm += (float)(n * HILG_NOISE_ACCEL * hilg_rngGauss(scen));
		st->aym += (float)(n * HILG_NOISE_ACCEL * hilg_rngGauss(scen));
		st->azm += (float)(n * HILG_NOISE_ACCEL * hilg_rngGauss(scen));
		st->mx += (float)(n * HILG_NOISE_MAG * hilg_rngGauss(scen));
		st->my += (float)(n * HILG_NOISE_MAG * hilg_rngGauss(scen));
		st->mz += (float)(n * HILG_NOISE_MAG * hilg_rngGauss(scen));
		st->lat += (int32_t)lround(n * HILG_NOISE_GPS * hilg_rngGauss(scen));
		st->lon += (int32_t)lround(n * HILG_NOISE_GPS * hilg_rngGauss(scen));
		st->alt += (float)(n * HILG_NOISE_ALT * hilg_rngGauss(scen));
		st->pStat += (float)(n * HILG_NOISE_PRESS * hilg_rngGauss(scen));
		st->pTot += (float)(n * HILG_NOISE_PRESS * hilg_rngGauss(scen));
	}
}


static int hilg_printRow(hilg_out_t *out, int idx, int64_t time, const hilg_state_t *st)
{
	return hilg_print(out, "%d,%" PRId64 "," HILCSV_FLT "," HILCSV_FLT "," HILCSV_FLT "," HILCSV_FLT "," HILCSV_FLT "," HILCSV_FLT ",%" PRId32 ",%" PRId32 "," HILCSV_FLT "," HILCSV_FLT "," HILCSV_FLT "," HILCSV_FLT "," HILCSV_FLT "," HILCSV_FLT "\n",
			idx, time, st->p, st->q, st->r, st->axm, st->aym, st->azm, st->lon, st->lat, st->alt, st->mx, st->my, st->mz, st->pStat, st->pTot);
}


int main(int argc, char *argv[])
{
	hilg_out_t out;
	hilg_state_t st;
	hilg_scen_t scen = { 0 };
	const char *outPath = NULL;
	const char *seedStr = NULL;
	unsigned long long seed = 0;
	double swingDeg[3] = { HILG_DEFAULT_YAW, HILG_DEFAULT_PITCH, HILG_DEFAULT_ROLL };
	double swingRad[3];
	double freq = HILG_DEFAULT_FREQ;
	double noise = HILG_DEFAULT_NOISE;
	int rate = HILG_DEFAULT_RATE;
	int rows = HILG_DEFAULT_COUNT;
	int64_t period; /* [us] */
	unsigned char digest[SHA256_DIGEST_SIZE];
	char *end;
	int option, i, idx, err;

	while ((option = getopt(argc, argv, "o:n:r:S:Y:P:R:F:N:h")) != -1) {
		switch (option) {
			case 'o':
				outPath = optarg;
				break;

			case 'n':
				/* hilsim needs at least 2 rows to determine the frame period */
				if (hilg_parsePositive(optarg, &rows) < 0 || rows < 2) {
					fprintf(stderr, "hilgen: row count must be at least 2\n");
					return EXIT_FAILURE;
				}
				break;

			case 'r':
				if (hilg_parsePositive(optarg, &rate) < 0 || rate > 1000) {
					fprintf(stderr, "hilgen: rate must be between 1 and 1000 Hz\n");
					return EXIT_FAILURE;
				}
				break;

			case 'S':
				errno = 0;
				seed = strtoull(optarg, &end, 0);
				if (errno != 0 || *optarg == '\0' || *optarg == '-' || *end != '\0') {
					fprintf(stderr, "hilgen: invalid seed: %s\n", optarg);
					return EXIT_FAILURE;
				}
				seedStr = optarg;
				break;

			case 'Y':
			case 'P':
			case 'R':
				if (hilg_parseDouble(optarg, 0.0, 180.0, &swingDeg[(option == 'Y') ? hilg_yaw : (option == 'P') ? hilg_pitch :
																												  hilg_roll]) < 0) {
					fprintf(stderr, "hilgen: swing amplitude must be between 0 and 180 deg\n");
					return EXIT_FAILURE;
				}
				break;

			case 'F':
				if (hilg_parseDouble(optarg, 1e-3, 100.0, &freq) < 0) {
					fprintf(stderr, "hilgen: swing frequency must be between 0.001 and 100 Hz\n");
					return EXIT_FAILURE;
				}
				break;

			case 'N':
				if (hilg_parseDouble(optarg, 0.0, 1000.0, &noise) < 0) {
					fprintf(stderr, "hilgen: noise scale must be between 0 and 1000\n");
					return EXIT_FAILURE;
				}
				break;

			default:
				hilg_usage(argv[0]);
				return option == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (optind != argc) {
		hilg_usage(argv[0]);
		return EXIT_FAILURE;
	}

	period = HILG_US_PER_S / rate + HILG_PERIOD_OFFSET;

	if (outPath != NULL) {
		out.f = fopen(outPath, "w");
		if (out.f == NULL) {
			fprintf(stderr, "hilgen: cannot open %s: %s\n", outPath, strerror(errno));
			return EXIT_FAILURE;
		}
	}
	else {
		out.f = stdout;
	}
	sha256_init(&out.sha);

	err = hilg_print(&out, "# hilgen: rate %d Hz, period %" PRId64 " us, duration %.3f s, %d rows\n",
			rate, period, (double)(rows * period) / HILG_US_PER_S, rows);
	if (seedStr != NULL) {
		for (i = 0; i < 3; i++) {
			swingRad[i] = swingDeg[i] * HILG_DEG2RAD;
		}
		hilg_scenInit(&scen, (uint64_t)seed, swingRad, freq, noise);
		err |= hilg_print(&out, "# scenario: seed %s, swing yaw %.1f pitch %.1f roll %.1f deg, %.2f Hz, noise %.2f\n",
				seedStr, swingDeg[hilg_yaw], swingDeg[hilg_pitch], swingDeg[hilg_roll], freq, noise);
	}
	else {
		err |= hilg_print(&out, "# scenario: static hover\n");
	}
	err |= hilg_print(&out, "%s\n", HILCSV_HEADER);

	for (idx = 0; err == 0 && idx < rows; idx++) {
		int64_t time = idx * period; /* [us] */

		if (scen.enabled) {
			hilg_scenState(&st, &scen, time);
		}
		else {
			hilg_hoverState(&st);
		}
		hilg_toSensor(&st);
		err = hilg_printRow(&out, idx, time, &st);
	}

	if (fflush(out.f) != 0) {
		err = -1;
	}
	if (out.f != stdout && fclose(out.f) != 0) {
		err = -1;
	}
	if (err != 0) {
		fprintf(stderr, "hilgen: write failed\n");
		return EXIT_FAILURE;
	}

	sha256_done(&out.sha, digest);
	fprintf(stderr, "hilgen: %d rows written, sha256 ", rows);
	for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
		fprintf(stderr, "%02x", digest[i]);
	}
	fprintf(stderr, "\n");

	return EXIT_SUCCESS;
}
