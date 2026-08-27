# Phoenix-RTOS
#
# Coredump parser - gdb script
#
# Copyright 2025 Phoenix Systems
# Authors: Jakub Klimek
#
# This file is part of Phoenix-RTOS.
#
# %LICENSE%
#

from elftools.elf.elffile import ELFFile
from elftools.elf.constants import SH_FLAGS
from itertools import pairwise
from pathlib import Path
import argparse
import struct
import sys
import os
import subprocess
import tempfile

NT_LMA = 0x00414D4C

COREDUMP_PARSER_PATH = (Path(__file__).parent.parent / "prog" / "coredump_parser").absolute()
COREDUMP_PARSER_OUTPUT = Path(tempfile.gettempdir()) / "phoenix-coredumps"

os.makedirs(COREDUMP_PARSER_OUTPUT, exist_ok=True)


def has_lma_note(elffile):
    for segment in elffile.iter_segments():
        if segment['p_type'] != 'PT_NOTE':
            continue
        for note in segment.iter_notes():
            if note['n_type'] == NT_LMA:
                return True
    return False

def parse_core_file(elffile):
    for segment in elffile.iter_segments():
        if segment['p_type'] != 'PT_NOTE':
            continue
        for note in segment.iter_notes():
            if note['n_type'] != NT_LMA:
                continue
            values = struct.unpack(f'<{len(note["n_desc"]) // 4}I', note['n_desc'])
            res = {}
            for lma, vma in pairwise(values):
                res[vma] = lma
            return res
    return {}

def parse_symbol_file(elffile):
    section_to_segment_mapping = {}

    for section in elffile.iter_sections():
        if not (section['sh_flags'] & SH_FLAGS.SHF_ALLOC) or section['sh_size'] == 0:
            continue

        sec_start_addr = section['sh_addr']
        sec_end_addr = sec_start_addr + section['sh_size']

        for segment in elffile.iter_segments():
            if segment['p_type'] != 'PT_LOAD':
                continue
            seg_start_addr = segment['p_vaddr']
            seg_end_addr = seg_start_addr + segment['p_memsz']

            if sec_start_addr >= seg_start_addr and sec_end_addr <= seg_end_addr:
                section_to_segment_mapping[section.name] = {
                    'seg_addr': seg_start_addr,
                    'offset': section['sh_addr'] - seg_start_addr,
                }
                break

    return section_to_segment_mapping

def create_mapping_args(core_file, symbol_file):
    core_segment_mapping = parse_core_file(core_file)
    elf_section_mapping = parse_symbol_file(symbol_file)
    args = []
    for section_name, section_info in elf_section_mapping.items():
        if section_info['seg_addr'] not in core_segment_mapping:
            print(f"Warning: Segment containing '{section_name}' not found in core NT_FILE mapping.", file=sys.stderr)
            if section_name == '.text':
                print("Warning: No valid segment mapping found for '.text' section!", file=sys.stderr)
                return []
            continue
        new_addr = core_segment_mapping[section_info['seg_addr']] + section_info['offset']
        args.append(f"-s {section_name} {new_addr:#x}")
    if len(args) == 0:
        print("Error: No valid segment mappings found.", file=sys.stderr)
    return args

def run_parser(coredump=None, symbolfile=None):
    arglist = [COREDUMP_PARSER_OUTPUT]
    if symbolfile: arglist.append(Path(symbolfile).name)
    try:
        if coredump:
            with open(coredump, "rb") as f:
                result = subprocess.run([COREDUMP_PARSER_PATH, *arglist], check=True, stdin=f, stdout=subprocess.PIPE)
                return result.stdout.decode('utf-8').strip()
        else:
            result = subprocess.run([COREDUMP_PARSER_PATH, *arglist], check=True, stdout=subprocess.PIPE)
            return result.stdout.decode('utf-8').strip()
    except subprocess.CalledProcessError as e:
        print(f"Running coredump_parser failed with exit status: {e.returncode}", file=sys.stderr)
    return []

def generate_gdb_commands(args):
    """
    If symbols is None, only corefile will be loaded
    If core is None, corefile will be loaded from stdin
    """
    coreelf = None
    symbolelf = None
    corefile = None
    symbolfile = None

    try:
        corefile = open(args.core, 'rb')
        coreelf = ELFFile(corefile)
    except Exception as e:
        args.core = run_parser(args.core, args.symbol)
        if args.core:
            print(f"Using core file '{args.core}' with symbol file '{args.symbol}'.", file=sys.stderr)
            corefile = open(args.core, 'rb')
            coreelf = ELFFile(corefile)

    if args.symbol:
        symbolfile = open(args.symbol, 'rb')
        symbolelf = ELFFile(symbolfile)

    if not coreelf:
        print("No core file found.", file=sys.stderr)
        if corefile: corefile.close()
        if symbolfile: symbolfile.close()
        return []

    commands = []
    if has_lma_note(coreelf):
        if not symbolelf:
            print("Warning: NOMMU coreelf detected, but no symbol file found. Symbols won't be resolved properly when loading symbols separately!", file=sys.stderr)
        else:
            mapping_args = create_mapping_args(coreelf, symbolelf)
            commands.append(f"symbol-file") # clear previous
            commands.append(f"exec-file") # clear previous
            commands.append(f"add-symbol-file {args.symbol} {' '.join(mapping_args)}")
            commands.append(f"exec-file {args.symbol} {' '.join(mapping_args)}")
    elif symbolelf:
        commands.append(f"symbol-file") # clear previous
        commands.append(f"exec-file") # clear previous
        commands.append(f"add-symbol-file {args.symbol}")
        commands.append(f"exec-file {args.symbol}")

    commands.append(f"core-file {args.core}")
    if corefile: corefile.close()
    if symbolfile: symbolfile.close()
    return commands

def parse_args(args):
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("-c", "--core", help="Path to the core file or to text containing dump (by default stdin will be used)")
    parser.add_argument("-s", "--symbol", "--sym", help="Path to the symbol file (by default in gdb currently loaded symbol file will be used)")
    try:
        args = parser.parse_args(args)
    except SystemExit:
        return {"help": True}
    return args

is_in_gdb = True
try:
    import gdb
except ImportError:
    is_in_gdb = False

if is_in_gdb:
    class PhoenixCoreCommand(gdb.Command):
        """
        Auto-load Phoenix core file and offset symbols on NOMMU targets.
        Use `phoenix-load --help` for more information.
        """

        def __init__(self):
            super(PhoenixCoreCommand, self).__init__("phoenix-core", gdb.COMMAND_USER)
            self.target_arch = None

        def invoke(self, arg, from_tty):
            args = parse_args(gdb.string_to_argv(arg))
            if "help" in args:
                return
            if not args.symbol:
                try:
                    if gdb.objfiles():
                        args.symbol = gdb.objfiles()[0].filename
                except gdb.error as e:
                    print(f"Phoenix-core: Note: Could not determine current GDB symbol file:", e, file=sys.stderr)
            commands = generate_gdb_commands(args)
            for command in commands:
                gdb.execute(command)
        def complete(self, text, word):
            options = ["-c", "--core", "-s", "--symbol", "--sym", "-h", "--help"]
            if word and word.startswith("-"):
                return [opt for opt in options if opt.startswith(word)]
            return gdb.COMPLETE_FILENAME


    PhoenixCoreCommand()

elif __name__ == "__main__":
    args = parse_args(sys.argv[1:])
    if "help" in args:
       exit(0)
    commands = generate_gdb_commands(args)
    if commands:
        print("\n\n# Generated GDB commands:\n")
        for cmd in commands:
            print(cmd)
        print()
    else:
        print("No commands generated.", file=sys.stderr)
