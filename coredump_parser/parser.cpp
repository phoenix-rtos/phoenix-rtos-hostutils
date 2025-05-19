/*
 * Phoenix-RTOS
 *
 * Coredump parser - text dump parser
 *
 * Copyright 2025 Phoenix Systems
 *
 * Author: Jakub Klimek
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <elf.h>
#include <endian.h>
#include <filesystem>
#include <iostream>
#include <cstring>
#include <optional>
#include <sstream>
#include <stdio.h>
#include <string>
#include <vector>
#include <algorithm>

enum class ParseStatus : int {
	Success = 0,
	EndOfFileReached = -1,
	B64Invalid = -2,
	RleBroken = -3,
	CrcInvalid = -4,
	CoredumpCorrupted = -5,
};

const uint32_t CRC32POLY_LE = 0xedb88320;

const size_t MAX_VARINT_COUNT = 0x40000000; /* 1 GB */

typedef struct {
	std::string process_name;
	std::string exception;
	uint32_t crc32;
} additional_info;


int b64_index(char c)
{
	if ('A' <= c && c <= 'Z')
		return c - 'A';
	if ('a' <= c && c <= 'z')
		return c - 'a' + 26;
	if ('0' <= c && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

ParseStatus read_b64(std::vector<uint8_t> &decoded)
{
	std::string line;
	uint32_t buf = 0;
	int bits = 0;

	while (std::getline(std::cin, line)) {
		if (line.find("_COREDUMP_END_") != std::string::npos) {
			break;
		}
		std::istringstream iss(line);

		char c;
		while (iss >> std::ws >> c) {
			if (c == '=')
				break;
			int val = b64_index(c);
			if (val < 0) {
				std::cerr << "Error: Invalid base64 character: " << c << std::endl;
				return ParseStatus::B64Invalid;
			}

			buf = (buf << 6) | val;
			bits += 6;

			if (bits >= 8) {
				bits -= 8;
				decoded.push_back((buf >> bits) & 0xFF);
			}
		}
	}

	return ParseStatus::Success;
}

ParseStatus decode_rle(std::vector<uint8_t> &rle_encoded,
		std::vector<uint8_t> &decoded)
{
	size_t len = rle_encoded.size();

	for (size_t i = 0; i < len;) {
		uint8_t byte = rle_encoded[i++];
		if (byte == 0xFE) {
			/* Decode varint */
			size_t count = 0;
			int shift = 0;
			while (i < len) {
				uint8_t b = rle_encoded[i++];
				count |= (b & 0x7F) << shift;
				if (!(b & 0x80))
					break;
				shift += 7;
			}

			if (count > MAX_VARINT_COUNT) {
				std::cerr << "Error: Varint count exceeds maximum allowed value: " << count << " repeated bytes at position " << i << "/" << len << ". Continue parsing? (y/N): ";
				char response;
				std::cin.get(response);
				if (response != 'y' && response != 'Y') {
					return ParseStatus::RleBroken;
				}
			}
			if (i < len) {
				uint8_t val = rle_encoded[i++];
				for (size_t j = 0; j < count; j++) {
					decoded.push_back(val);
				}
			}
			else {
				std::cerr << "Error: Unexpected end of data during RLE decoding."
						  << std::endl;
				return ParseStatus::RleBroken;
			}
		}
		else {
			decoded.push_back(byte);
		}
	}
	return ParseStatus::Success;
}

ParseStatus check_crc(std::vector<uint8_t> &data, uint32_t expected_crc32)
{
	uint32_t crc32 = -1;
	for (size_t i = 0; i < data.size(); ++i) {
		crc32 = (crc32 ^ (data[i] & 0xFF));
		for (int j = 0; j < 8; j++) {
			crc32 = (crc32 >> 1) ^ ((crc32 & 1) ? CRC32POLY_LE : 0);
		}
	}
	crc32 = ~crc32;
	if (crc32 != expected_crc32) {
		std::cerr << "Error: CRC32 mismatch!" << std::endl;
		std::cerr << "Calculated: " << std::hex << crc32 << std::endl;
		std::cerr << "Found: " << std::hex << expected_crc32 << std::dec << std::endl;
		std::cerr << "Do you want to continue? (y/N): ";
		char response;
		std::cin.get(response);
		if (response != 'y' && response != 'Y') {
			return ParseStatus::CrcInvalid;
		}
	}
	return ParseStatus::Success;
}

ParseStatus read_decode(std::vector<uint8_t> &res, uint32_t &crc32)
{

	std::vector<uint8_t> rle_encoded;
	ParseStatus ret = read_b64(rle_encoded);
	if (ret != ParseStatus::Success) {
		return ret;
	}
	ret = decode_rle(rle_encoded, res);
	if (ret != ParseStatus::Success) {
		return ret;
	}
	if (res.size() < sizeof(crc32)) {
		std::cerr << "Error: Data too short!" << std::endl;
		return ParseStatus::CoredumpCorrupted;
	}

	memcpy(&crc32, &res.data()[res.size() - sizeof(crc32)], sizeof(crc32));
	res.resize(res.size() - sizeof(crc32));
	if (res[EI_DATA] == ELFDATA2MSB) {
		crc32 = be32toh(crc32);
	}
	ret = check_crc(res, crc32);
	return ret;
}

ParseStatus watch_stdin(std::vector<uint8_t> &data, additional_info &info)
{
	std::string line;

	while (std::getline(std::cin, line)) {
		if (line.find("_COREDUMP_START_") != std::string::npos) {
			data.clear();

			if (!std::getline(std::cin, line, ':')) {
				std::cerr << "Error: Missing first line with process and exception!"
						  << std::endl;
				return ParseStatus::CoredumpCorrupted;
			}
			line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());
			info.process_name = line;

			if (!std::getline(std::cin, line, ';')) {
				std::cerr << "Error: Missing first line with process and exception!"
						  << std::endl;
				return ParseStatus::CoredumpCorrupted;
			}
			line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());
			info.exception = line;

			return read_decode(data, info.crc32);
		}
	}
	return ParseStatus::EndOfFileReached;
}

ParseStatus parse_dump(std::vector<uint8_t> &mem_data, std::string output_file)
{
	FILE *ofs = fopen(output_file.c_str(), "wb");
	if (!ofs) {
		std::cerr << "Error: Unable to open output file." << std::endl;
		return ParseStatus::CoredumpCorrupted;
	}

	fwrite(mem_data.data(), mem_data.size(), 1, ofs);

	std::cerr << "Total bytes written to " << output_file << ": "
			  << mem_data.size() << std::endl;

	fclose(ofs);
	return ParseStatus::Success;
}

std::filesystem::path get_output_path(std::filesystem::path output_dir,
		additional_info info)
{
	int i = 0;
	std::string process = std::filesystem::path(info.process_name).filename().string();
	std::filesystem::path output_file =
			output_dir / (process + "." + std::to_string(info.crc32) + "." + std::to_string(i) + ".core");
	while (std::filesystem::exists(output_file)) {
		++i;
		output_file =
				output_dir / (process + "." + std::to_string(info.crc32) + "." + std::to_string(i) + ".core");
	}
	return output_file;
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		std::cerr << "Error: Invalid number of arguments." << std::endl;
		std::cerr << "Usage: " << argv[0] << " <output dir> [expected process name]" << std::endl;
		return 1;
	}
	std::filesystem::path output_dir = argv[1];
	if (!std::filesystem::exists(output_dir)) {
		std::cerr << "Error: Output directory does not exist: " << output_dir
				  << std::endl;
		return 1;
	}

	std::string expected_process_name;
	if (argc > 2) {
		expected_process_name = argv[2];
	}

	std::optional<std::filesystem::path> first_output_file;

	std::cerr << "Watching stdin for data..." << std::endl;
	std::vector<uint8_t> mem_data;
	additional_info info;
	while (true) {
		ParseStatus ret = watch_stdin(mem_data, info);
		if (ret == ParseStatus::EndOfFileReached) {
			std::cerr << "EOF reached." << std::endl;
			break;
		}

		if (ret != ParseStatus::Success) {
			std::cerr << "Failed to decode coredump" << std::endl;
			continue;
		}

		std::cerr << "\n\nParsing coredump for process: " << info.process_name
				  << " (Exception: " << info.exception << ")" << std::endl;
		auto output_file = get_output_path(output_dir, info);
		parse_dump(mem_data, output_file);
		if (expected_process_name.empty() ||
				std::filesystem::path(info.process_name).filename().string() ==
						expected_process_name) {
			std::cout << output_file.string() << std::endl;
			return 0;
		}
		if (!first_output_file.has_value()) {
			first_output_file = output_file;
		}
	}
	if (first_output_file.has_value()) {
		if (!expected_process_name.empty()) {
			std::cerr << "No process matched the expected name: "
					  << expected_process_name
					  << " using first found coredump file."
					  << std::endl;
		}
		std::cout << first_output_file->string() << std::endl;
		return 0;
	}
	else {
		std::cerr << "No valid coredump found." << std::endl;
		return 1;
	}
}
