# Coredump Parser

## Debugging using coredump

### Preparation

Install a suitable GDB (e.g., `gdb-multiarch` from apt)

Install required python packages: `sudo apt install python3-pyelftools`

Run Phoenix build for host-generic-pc target
```
TARGET=host-generic-pc ./phoenix-rtos-build/build.sh all
```

### GDB usage

Add loading script to `.gdbinit`:

```bash
 source <REPO_ROOT>/_build/host-generic-pc/scripts/gdb-phoenix-core.py
```

Alternatively, use gdb parameter *\--command=\<REPO_ROOT\>/_build/host-generic-pc/scripts/gdb-phoenix-core.py*

Now, inside GDB, there will be `phoenix-core` command available, allowing you to load coredump, core elf and symbols.

```bash
usage: phoenix-core [-h] [-c CORE] [-s SYMBOL]

options:
  -h, --help            show this help message and exit
  -c CORE, --core CORE  Path to the core file or to text containing dump (by default stdin will be used)
  -s SYMBOL, --symbol SYMBOL, --sym SYMBOL
                        Path to the symbol file (by default in gdb currently loaded symbol file will be used)
```

Example usages (with script added to `.gdbinit`):

```bash
# gdb-multiarch _build/ia32-generic-qemu/prog/psh
...
(gdb) phoenix-core -c
<paste text dump here>

(gdb) ph -c dump.txt -s _build/ia32-generic-qemu/prog/test_program
```

Example with elf core file (look at Tracing Guide to obtain file from QEMU image):

```bash
(gdb) phoenix-core -c test_program.153 -s _build/ia32-generic-qemu/prog/test_program
```


### GDB usage without builtin Python interpreter

In case your GDB installation doesn't support python interpreter, you can run the script directly from command line and forward stdout to gdb input.

It will output commands that you can run inside your GDB, eg:
```
{ python3 <REPO_ROOT>/_build/host-generic-pc/scripts/gdb-phoenix-core.py -c <dump file> -s <symbol file>; cat; } | gdb-multiarch
```


## Manual Parser Usage (without Python script)
In case you want to manually parse coredump into elf file, use binary `_build/host-generic-pc/prog/coredump_parser <output directory>` with text dump on standard input. An ELF core file will be created in the specified directory if the dump is valid.

You can load this core file in GDB manually using `--core` argument or `core-file` command.

Please note that `core-file` will not work for NOMMU targets, because load addresses are unmatched with symbol elf virtual addresses.
