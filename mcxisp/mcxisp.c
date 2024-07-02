/*
 * Phoenix-RTOS
 *
 * mcxisp - tool for MCX N94x series ISP
 *
 * Copyright 2024 Phoenix Systems
 * Author: Aleksander Kaminski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <termios.h>
#include <sys/stat.h>

/* clang-format off */
#define TTY_DEBUG(fmt, ...) if (0)  { printf(fmt, ##__VA_ARGS__); }
/* clang-format on */

#define FRAME_SIZE  6
#define FRAME_START 0x5a

#define FLASH_PAGE_SIZE 128
#define FLASH_MEM_ID    0

#define TTY_TIMEOUT  10
#define TTY_BAUDRATE B576000

/* Frame types */
#define kFramingPacketType_Ack          0xa1
#define kFramingPacketType_Nak          0xa2
#define kFramingPacketType_AckAbort     0xa3
#define kFramingPacketType_Command      0xa4
#define kFramingPacketType_Data         0xa5
#define kFramingPacketType_Ping         0xa6
#define kFramingPacketType_PingResponse 0xa7

/* Response lengths */
#define RESPONSE_PING_LENGTH    10
#define RESPONSE_ACK_LENGTH     2
#define RESPONSE_GENERIC_LENGTH 18


static struct {
	int tty;
	int file;
	size_t filesz;
	struct termios orig;
} common;


/* LE serialization functions */


static size_t serialize8(uint8_t *buff, uint8_t v)
{
	buff[0] = v;
	return 1;
}


static size_t serialize16(uint8_t *buff, uint16_t v)
{
	buff[0] = v & 0xff;
	buff[1] = v >> 8;
	return 2;
}


static size_t serialize32(uint8_t *buff, uint32_t v)
{
	buff[0] = v & 0xff;
	buff[1] = v >> 8;
	buff[2] = v >> 16;
	buff[3] = v >> 24;
	return 4;
}


/* CRC16 */


static void crc16(uint16_t *crc, const uint8_t *buff, uint16_t len)
{
	for (uint16_t i = 0; i < len; ++i) {
		*crc ^= ((uint16_t)(buff[i])) << 8;
		for (int j = 0; j < 8; ++j) {
			uint16_t t = *crc << 1;
			if (((*crc) & 0x8000) != 0) {
				t ^= 0x1021;
			}
			*crc = t;
		}
	}
}


/* Commands construction and serialization */


static size_t cmd_ping(uint8_t *buff)
{
	size_t pos = 0;

	pos += serialize8(buff + pos, FRAME_START);
	pos += serialize8(buff + pos, kFramingPacketType_Ping);

	return pos;
}


static void cmd_constructFrame(uint8_t *frame, uint8_t type, uint16_t len)
{
	size_t pos = 0;
	uint16_t crc = 0;

	pos += serialize8(frame + pos, FRAME_START);
	pos += serialize8(frame + pos, type);
	pos += serialize16(frame + pos, len);

	crc16(&crc, frame, pos);
	crc16(&crc, frame + FRAME_SIZE, len);

	serialize16(frame + pos, crc);
}


static size_t cmd_flashEraseAll(uint8_t *buff, int memid)
{
	size_t pos = FRAME_SIZE; /* Skip frame for now */

	pos += serialize8(buff + pos, 0x01);   /* Tag */
	pos += serialize8(buff + pos, 0x00);   /* Flags */
	pos += serialize8(buff + pos, 0x00);   /* Reserved */
	pos += serialize8(buff + pos, 0x01);   /* Parameter count */
	pos += serialize32(buff + pos, memid); /* Memory id */

	cmd_constructFrame(buff, kFramingPacketType_Command, pos - FRAME_SIZE);

	return pos;
}


static size_t cmd_flashWriteMemory(uint8_t *buff, uint32_t address, size_t len, int memid)
{
	size_t pos = FRAME_SIZE; /* Skip frame for now */

	pos += serialize8(buff + pos, 0x04);     /* Tag */
	pos += serialize8(buff + pos, 0x01);     /* Flags */
	pos += serialize8(buff + pos, 0x00);     /* Reserved */
	pos += serialize8(buff + pos, 0x03);     /* Parameter count */
	pos += serialize32(buff + pos, address); /* Start address */
	pos += serialize32(buff + pos, len);     /* Data length */
	pos += serialize32(buff + pos, memid);   /* Memory id */

	cmd_constructFrame(buff, kFramingPacketType_Command, pos - FRAME_SIZE);

	return pos;
}


static size_t cmd_reset(uint8_t *buff)
{
	size_t pos = FRAME_SIZE; /* Skip frame for now */

	pos += serialize8(buff + pos, 0x0b); /* Tag */
	pos += serialize8(buff + pos, 0x00); /* Flags */
	pos += serialize8(buff + pos, 0x00); /* Reserved */
	pos += serialize8(buff + pos, 0x00); /* Parameter count */

	cmd_constructFrame(buff, kFramingPacketType_Command, pos - FRAME_SIZE);

	return pos;
}


static size_t cmd_data(uint8_t *buff, const uint8_t *data, size_t len)
{
	memcpy(buff + FRAME_SIZE, data, len);
	cmd_constructFrame(buff, kFramingPacketType_Data, len);
	return FRAME_SIZE + len;
}


static int cmd_expectAck(const uint8_t *buff)
{
	if ((buff[0] != 0x5a) || (buff[1] != 0xa1)) {
		fprintf(stderr, "target invalid response\n");
		return -1;
	}

	return 0;
}


static int cmd_expectGenericResponse(const uint8_t *buff)
{
	if ((buff[0] != 0x5a) || (buff[1] != 0xa4)) {
		fprintf(stderr, "target invalid response\n");
		return -1;
	}

	/* Ignore the rest of the packet, not interesting really */

	return 0;
}


static int cmd_expectPingResponse(const uint8_t *buff)
{
	if ((buff[0] != 0x5a) || (buff[1] != 0xa7)) {
		fprintf(stderr, "target invalid response\n");
		return -1;
	}

	/* Ignore the rest of the packet, not interesting really */

	return 0;
}

static void tty_dump(const uint8_t *buff, size_t len)
{
	for (size_t i = 0; i < len; ++i) {
		TTY_DEBUG("%02x", buff[i]);
	}
}


static int tty_write(const uint8_t *buff, size_t len)
{
	int count = 0;

	TTY_DEBUG("Sending: ");
	tty_dump(buff, len);
	TTY_DEBUG("\n");

	while (count < (int)len) {
		int ret = write(common.tty, buff + count, len - count);
		if (ret < 0) {
			fprintf(stderr, "tty write error: %s\n", strerror(errno));
			return -1;
		}

		if (ret == 0) {
			break;
		}

		count += ret;
	}

	return count;
}


static int tty_read(uint8_t *buff, size_t bufflen)
{
	int count = 0;
	int started = 0;

	TTY_DEBUG("Received: ");

	while (count < (int)bufflen) {
		uint8_t byte;
		int ret = read(common.tty, &byte, 1);

		if (ret < 0) {
			return -1;
		}

		if (ret == 0) {
			/* timeout */
			break;
		}

		tty_dump(&byte, 1);

		if (started == 0) {
			/* We're waiting for start marker */
			if (byte != 0x5a) {
				continue;
			}
			started = 1;
		}

		buff[count++] = byte;
	}

	TTY_DEBUG("\n");

	return count;
}


static int tty_ack(void)
{
	const uint8_t cmd[] = { 0x5a, 0xa1 };
	return tty_write(cmd, sizeof(cmd));
}


static int file_read(uint8_t *buff, size_t bufflen)
{
	size_t count = 0;

	while (count < bufflen) {
		int ret = read(common.file, buff + count, bufflen - count);
		if (ret < 0) {
			fprintf(stderr, "read error: %s\n", strerror(errno));
			return -1;
		}

		if (ret == 0) {
			break;
		}

		count += ret;
	}

	return count;
}


static int target_connect(void)
{
	for (int retry = 10; retry > 0; --retry) {
		if (retry != 10) {
			fprintf(stderr, "Retry #%d\n", retry);
		}

		uint8_t buff[32];
		size_t len = cmd_ping(buff);
		if (tty_write(buff, len) < 0) {
			fprintf(stderr, "tty write failed: %s\n", strerror(errno));
			continue;
		}

		int ret = tty_read(buff, RESPONSE_PING_LENGTH);
		if (ret < 0) {
			fprintf(stderr, "tty read failed: %s\n", strerror(errno));
			continue;
		}

		if (cmd_expectPingResponse(buff) != 0) {
			continue;
		}

		return 0;
	}

	return -1;
}


static int target_ackLadder(void)
{
	uint8_t buff[32 + FRAME_SIZE] = { 0 };

	if (tty_read(buff, RESPONSE_ACK_LENGTH) < 2) {
		return -1;
	}

	if (cmd_expectAck(buff) < 0) {
		return -1;
	}

	if (tty_read(buff, RESPONSE_GENERIC_LENGTH) < 2) {
		return -1;
	}

	if (cmd_expectGenericResponse(buff) < 0) {
		return -1;
	}

	if (tty_ack() < 0) {
		return -1;
	}

	return 0;
}


static int target_flashEraseAll(void)
{
	int retry;
	uint8_t buff[32 + FRAME_SIZE];
	size_t len = cmd_flashEraseAll(buff, FLASH_MEM_ID);
	if (tty_write(buff, len) < 0) {
		return -1;
	}

	/* This can take a while */
	for (retry = 30; retry > 0; --retry) {
		if (target_ackLadder() == 0) {
			break;
		}
	}

	return (retry == 0) ? -1 : 0;
}


static int target_sendFile(void)
{
	uint32_t address = 0;
	uint8_t buff[FLASH_PAGE_SIZE + FRAME_SIZE];
	uint8_t data[FLASH_PAGE_SIZE];
	int total = 0;

	for (;;) {
		memset(data, 0xff, FLASH_PAGE_SIZE);
		int chunk = file_read(data, FLASH_PAGE_SIZE);
		if (chunk < 0) {
			return -1;
		}
		else if (chunk == 0) {
			break;
		}

		size_t len = cmd_flashWriteMemory(buff, address, chunk, FLASH_MEM_ID);
		if (tty_write(buff, len) < 0) {
			return -1;
		}

		if (target_ackLadder() < 0) {
			return -1;
		}

		len = cmd_data(buff, data, FLASH_PAGE_SIZE);
		if (tty_write(buff, len) < 0) {
			return -1;
		}

		if (target_ackLadder() < 0) {
			return -1;
		}

		total += chunk;
		address += chunk;

		if (chunk != FLASH_PAGE_SIZE) {
			break;
		}

		/* print progress */
		printf("Progress: %d/%zu KiB\r", (total + 512) / 1024, (common.filesz + 512) / 1024);
		fflush(stdout);
	}

	printf("\n");

	return total;
}


static int target_reset(void)
{
	uint8_t buff[32 + FRAME_SIZE];
	size_t len = cmd_reset(buff);
	if (tty_write(buff, len) < 0) {
		return -1;
	}

	if (target_ackLadder() < 0) {
		return -1;
	}

	return 0;
}


static int tty_setup(void)
{
	struct termios raw;

	if (tcgetattr(common.tty, &raw) < 0) {
		return -1;
	}
	common.orig = raw;

	cfmakeraw(&raw);
	cfsetspeed(&raw, TTY_BAUDRATE);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = TTY_TIMEOUT;

	if (tcsetattr(common.tty, TCSANOW, &raw) < 0) {
		return -1;
	}

	return 0;
}


static void tty_restore(void)
{
	tcsetattr(common.tty, TCSANOW, &common.orig);
}


static void usage(const char *progname)
{
	printf("MCX N94x series UART ISP util\n");
	printf("Usage: %s -f program file -t ISP tty\n", progname);
}


int main(int argc, char *argv[])
{
	struct stat st;

	common.tty = -1;
	common.file = -1;

	for (;;) {
		int opt = getopt(argc, argv, "hf:t:");
		if (opt == -1) {
			break;
		}

		switch (opt) {
			case 'h':
				usage(argv[0]);
				return EXIT_SUCCESS;

			case 'f':
				common.file = open(optarg, O_RDONLY);
				if (common.file < 0) {
					fprintf(stderr, "%s: Could not open %s (%s)\n",
						argv[0], optarg, strerror(errno));
					return EXIT_FAILURE;
				}
				break;

			case 't':
				common.tty = open(optarg, O_RDWR);
				if (common.tty < 0) {
					fprintf(stderr, "%s: Could not open %s (%s)\n",
						argv[0], optarg, strerror(errno));
					return EXIT_FAILURE;
				}

				if (isatty(common.tty) < 1) {
					fprintf(stderr, "%s: %s: %s\n",
						argv[0], optarg, strerror(errno));
					return EXIT_FAILURE;
				}
		}
	}

	if ((common.tty < 0) || (common.file < 0)) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (fstat(common.file, &st) < 0) {
		fprintf(stderr, "fstat failed\n");
		return EXIT_FAILURE;
	}

	common.filesz = st.st_size;

	printf("Connecting to the target...\n");
	if (tty_setup() < 0) {
		fprintf(stderr, "tty setup failed\n");
		return EXIT_FAILURE;
	}

	if (target_connect() < 0) {
		fprintf(stderr, "failed\n");
		tty_restore();
		return EXIT_FAILURE;
	}

	printf("Connected.\nFlash erase...\n");
	if (target_flashEraseAll() < 0) {
		fprintf(stderr, "failed\n");
		tty_restore();
		return EXIT_FAILURE;
	}

	printf("Erased.\nUploading file...\n");
	if (target_sendFile() < 0) {
		fprintf(stderr, "failed\n");
		tty_restore();
		return EXIT_FAILURE;
	}

	printf("Done.\nReseting target...\n");
	if (target_reset() < 0) {
		fprintf(stderr, "failed\n");
		tty_restore();
		return EXIT_FAILURE;
	}

	printf("Done.\n");

	tty_restore();

	return EXIT_SUCCESS;
}
