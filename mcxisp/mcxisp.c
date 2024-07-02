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

#define FRAME_SIZE  6
#define FRAME_START 0x5a

#define FLASH_PAGE_SIZE 128
#define FLASH_MEM_ID    0

/* Frame types */
#define kFramingPacketType_Ack          0xa1
#define kFramingPacketType_Nak          0xa2
#define kFramingPacketType_Ack          0xa3
#define kFramingPacketType_Command      0xa4
#define kFramingPacketType_Data         0xa5
#define kFramingPacketType_Ping         0xa6
#define kFramingPacketType_PingResponse 0xa7


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


void crc16(uint16_t *crc, const uint8_t *buff, uint16_t len)
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


/* Commnads construction and serialization */


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

	pos += serialize8(buff + pos, FRAME_START);
	pos += serialize8(buff + pos, type);
	pos += serialize16(buff + pos, len);

	crc16(&crc, frame, pos);
	crc16(&crc, frame + FRAME_SIZE, len - FRAME_SIZE);

	pos += serialize16(buff + pos, crc);
}


static size_t cmd_flashEraseAll(uint8_t *buff, int memid)
{
	size_t pos = FRAME_SIZE; /* Skip frame for now */

	pos += serialize8(buff + pos, 0x01);   /* Tag */
	pos += serialize8(buff + pos, 0x00);   /* Flags */
	pos += serialize8(buff + pos, 0x00);   /* Reserved */
	pos += serialize8(buff + pos, 0x01);   /* Parameter count */
	pos += serialize32(buff + pos, memid); /* Memory id */

	cmd_constructFrame(buff, kFramingPacketType_Command, pos);

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

	cmd_constructFrame(buff, kFramingPacketType_Command, pos);

	return pos;
}


static size_t cmd_reset(uint8_t *buff)
{
	size_t pos = FRAME_SIZE; /* Skip frame for now */

	pos += serialize8(buff + pos, 0x0b); /* Tag */
	pos += serialize8(buff + pos, 0x00); /* Flags */
	pos += serialize8(buff + pos, 0x00); /* Reserved */
	pos += serialize8(buff + pos, 0x00); /* Parameter count */

	cmd_constructFrame(buff, kFramingPacketType_Command, pos);

	return pos;
}


static size_t cmd_data(uint8_t *buff, const uint8_t *data, size_t len)
{
	memcpy(buff + FRAME_SIZE, data, len);
	cmd_constructFrame(buff, kFramingPacketType_Data, FRAME_SIZE + len);
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
	if ((buff[0] != 0x5a) || (buff[1] != 0xa5)) {
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


static int tty_write(int tty, const uint8_t buff, size_t len)
{
	/* TODO */
}


static int tty_read(int tyy, uint8_t buff, size_t bufflen)
{
	/* TODO */
}


static int tty_ack(int tty)
{
	const uint8_t cmd[] = { 0x5a, 0xa1 };
	return tty_write(tty, cmd, sizeof(cmd));
}


static int file_read(int fd, uint8_t buff, size_t bufflen)
{
	size_t count = 0;

	while (count < bufflen) {
		int ret = read(fd, buff + count, bufflen - count);
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


static int target_connect(int tty)
{
	for (int retry = 10; retry > 0; --retry) {
		if (retry != 10) {
			fprintf(stderr, "Retry #%d\n", retry);
		}

		uint8_t buff[32];
		size_t len = cmd_ping(buff);
		if (tty_write(tty, buff, len) < 0) {
			fprintf(stderr, "tty write failed: %s\n", strerror(errno));
			continue;
		}

		int ret = tty_read(tty, buff, sizeof(buff));
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


static int target_ackLadder(int tty)
{
	uint8_t buff[32 + FRAME_SIZE];

	if (tty_read(tty, buff, sizeof(buff) < 2) {
		return -1;
	}

	if (cmd_expectAck(buff) < 0) {
		return -1;
	}

	if (tty_read(tty, buff, sizeof(buff) < 2) {
		return -1;
	}

	if (cmd_expectGenericResponse(buff) < 0) {
		return -1;
	}

	if (tty_ack(tty) < 0) {
		return -1;
	}

	return 0;
}


static int target_flashEraseAll(int tty)
{
	uint8_t buff[32 + FRAME_SIZE];
	size_t len = cmd_flashEraseAll(buff, FLASH_MEM_ID);
	if (tty_write(tty, buff, len) < 0) {
		return -1;
	}

	if (target_ackLadder(tty) < 0) {
		return -1;
	}

	return 0;
}


static int target_sendFile(int tty, int file)
{
	uint32_t address = 0;
	uint8_t buff[FLASH_PAGE_SIZE + FRAME_SIZE];
	uint8_t data[FLASH_PAGE_SIZE];
	int total = 0;

	for (;;) {
		memset(data, 0xff, FLASH_PAGE_SIZE);
		int chunk = file_read(file, data, FLASH_PAGE_SIZE);
		if (chunk < 0) {
			return -1;
		}
		else if (chunk == 0) {
			break;
		}

		size_t len = cmd_flashWriteMemory(buff, address, chunk, FLASH_MEM_ID);
		if (tty_write(tty, buff, len) < 0) {
			return -1;
		}

		if (target_ackLadder(tty) < 0) {
			return -1;
		}

		len = cmd_data(buff, data, FLASH_PAGE_SIZE);
		if (tty_write(tty, buff, len) < 0) {
			return -1;
		}

		if (target_ackLadder(tty) < 0) {
			return -1;
		}

		total += chunk;
	}

	return total;
}


static int target_reset(int tty)
{
	uint8_t buff[32 + FRAME_SIZE];
	size_t len = cmd_reset(buff, FLASH_MEM_ID);
	if (tty_write(tty, buff, len) < 0) {
		return -1;
	}

	if (target_ackLadder(tty) < 0) {
		return -1;
	}

	return 0;
}


static void usage(const char *progname)
{
	printf("MCX N94x series UART ISP util\n");
	printf("Usage: %s -f program file -t ISP tty\n", progname);
}


int main(int argc, char *argv[])
{
	int tty = -1, file = -1;
	char buff[1024];
	size_t len;

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
				file = open(optarg, O_RDONLY);
				if (file < 0) {
					fprintf(stderr, "%s: Could not open %s (%s)\n",
						argv[0], optarg, strerror(errno));
					return EXIT_FAILURE;
				}
				break;

			case 't':
				tty = open(optarg, O_RDWR);
				if (tty < 0) {
					fprintf(stderr, "%s: Could not open %s (%s)\n",
						argv[0], optarg, strerror(errno));
					return EXIT_FAILURE;
				}

				if (isatty(tty) < 1) {
					fprintf(stderr, "%s: %s: %s\n",
						argv[0], optarg, strerror(errno));
					return EXIT_FAILURE;
				}
		}
	}

	if ((tty < 0) || (file < 0)) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	printf("Connecting to the target...\n");
	if (target_connect(tty) < 0) {
		fprintf(stderr, "failed\n");
		return EXIT_FAILURE;
	}

	printf("Connected.\nFlash erase...\n");
	if (target_flashEraseAll(tty) < 0) {
		fprintf(stderr, "failed\n");
		return EXIT_FAILURE;
	}

	printf("Erased.\nUploading file...\n");
	if (target_sendFile(tty) < 0) {
		fprintf(stderr, "failed\n");
		return EXIT_FAILURE;
	}

	printf("Done.\nReseting target...\n");
	if (target_reset(tty) < 0) {
		fprintf(stderr, "failed\n");
		return EXIT_FAILURE;
	}

	printf("Done.\n");

	return EXIT_SUCCESS;
}