# single file low-level meterfs image parser

import os
import sys
import struct
import operator
import functools
from dataclasses import dataclass
from pathlib import Path
import logging
from argparse import ArgumentParser

try:
    from rich.pretty import pprint
except ImportError:
    pprint = print

MAGIC = bytes.fromhex("66414bbb")


def _checksum(data: bytes):
    return functools.reduce(operator.xor, data)


def _order(byteorder):
    assert byteorder == "little" or byteorder == "big"
    return {"little": "<", "big": ">"}[byteorder]


@dataclass
class FsHeader:
    valid: bool
    no: int
    filecnt: int
    checksum: int
    magic: bytes

    @classmethod
    def from_bytes(cls, raw: bytes, byteorder=sys.byteorder):
        header_id, filecnt, checksum, magic = struct.unpack(_order(byteorder) + "III4s", raw)
        return cls(valid=not header_id & 1, no=header_id >> 1, filecnt=filecnt, checksum=checksum, magic=magic)


@dataclass
class FileHeader:
    sector: int
    sectorcnt: int
    filesz: int
    recordsz: int
    name: bytes
    uid: int
    ncrypt: int

    def __post_init__(self):
        self.name = self.name.rstrip(b"\x00")

    @classmethod
    def from_bytes(cls, raw: bytes, byteorder=sys.byteorder):
        values = struct.unpack(_order(byteorder) + "IIII8sIH", raw)
        return cls(*values)


@dataclass
class Record:
    valid: bool
    no: int
    checksum: int

    @classmethod
    def from_bytes(cls, raw: bytes, byteorder=sys.byteorder):
        header_id, checksum = struct.unpack(_order(byteorder) + "II", raw)
        return cls(not header_id & 1, header_id >> 1, checksum)


def iter_files(raw, offs, count, byteorder=sys.byteorder):
    hgrain = 32
    for i in range(count):
        raw.seek(offs + hgrain * (i + 1))
        yield FileHeader.from_bytes(raw.read(30), byteorder=byteorder)


def print_files(raw, offs, count):
    for fh in iter_files(raw, offs, count):
        pprint(fh)


DETECT = object()


def parse_addr(raw: str):
    return int(raw, 0)


def main():
    parser = ArgumentParser(description="low level meterfs image parser")
    parser.add_argument("image", type=Path)
    parser.add_argument("--part-offset", type=parse_addr, default=0)
    parser.add_argument("--part-size", type=parse_addr)
    parser.add_argument("--sector-size", type=parse_addr, default=DETECT)
    parser.add_argument("--print-records", action="store_true")
    parser.add_argument("--data-crop", type=int, help="limit number of content bytes printed for each record")
    parser.add_argument("--byteorder", choices=("big", "little"), default=sys.byteorder)
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO)

    with args.image.open("rb") as raw:
        raw.seek(args.part_offset)
        headers: list[tuple[int, FsHeader]] = []
        headers.append((0, FsHeader.from_bytes(raw.read(16), byteorder=args.byteorder)))

        if args.sector_size is DETECT:
            for i in range(9, 16):
                sector_size = 2**i
                raw.seek(args.part_offset + sector_size)
                header = FsHeader.from_bytes(raw.read(16), byteorder=args.byteorder)
                if header.magic == MAGIC:
                    headers.append((sector_size, header))

            if len(headers) != 2:
                for header in headers:
                    pprint(header)
                logging.error(f"Sector size detection requires 2 valid headers. Found {len(headers)}")
                return

            sector_size = headers[1][0] // 2
            logging.info(f"Detected sector size: {sector_size} bytes")
        else:
            sector_size = args.sector_size
            raw.seek(args.part_offset + sector_size)
            header = FsHeader.from_bytes(raw.read(16), byteorder=args.byteorder)
            if header.magic == MAGIC:
                headers.append((sector_size, header))

        headers = sorted(headers, key=lambda x: x[1].no)

        fshead_offs, fshead = headers[-1]
        pprint(fshead)

        # TODO fshead_end
        last_sector = 0

        for fh in iter_files(raw, args.part_offset + fshead_offs, fshead.filecnt):
            if args.print_records:
                print()
            pprint(fh)

            offs = fh.sector * sector_size
            last_sector = max(last_sector, fh.sector + fh.sectorcnt)
            recsize = fh.recordsz + 8

            if not args.print_records:
                continue

            count = fh.sectorcnt * sector_size // recsize
            valid_records = 0
            for i in range(count):
                recoff = offs + recsize * i
                raw.seek(args.part_offset + recoff)
                raw_rec = raw.read(8)
                rec = Record.from_bytes(raw_rec, byteorder=args.byteorder)
                raw_data = raw.read(fh.recordsz)

                if args.data_crop is None or len(raw_data) < args.data_crop:
                    str_data = raw_data.hex()
                else:
                    str_data = raw_data[0 : args.data_crop].hex() + "…"

                if rec.valid and rec.checksum != 0:
                    valid_records += 1
                    print(f"idx:{i} off=0x{recoff:08x} no={rec.no:2d} {str_data}")
                    assert rec.checksum == _checksum(raw_data)

                # rec_data = raw.read(fh.recordsz)
                # yield FileHeader.from_bytes(raw.read(24), byteorder=args.byteorder)

            noun = "record" if valid_records == 1 else "records"
            print(f"Found {valid_records} valid {noun} for file {fh.name}")

        raw.seek(0, os.SEEK_END)

        part_size = args.part_size
        if part_size is None:
            part_size = raw.tell() - args.part_offset
        sector_count = part_size / sector_size

        print()
        print(f"Sectors used: {last_sector}/{sector_count} {100.0*last_sector/sector_count:.2f}%")


if __name__ == "__main__":
    main()
