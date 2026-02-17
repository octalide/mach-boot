mach-boot
=========

Bootstrap compiler for [mach](https://github.com/octalide/mach), written in C.

This is the first stage of the mach self-hosting pipeline. It compiles the mach source code into an intermediate compiler, which then compiles itself into the final self-hosted compiler.

## Building

Prerequisites: clang (or gcc with C23 support), make.

```bash
make
```

Produces `out/bin/cmach`.

## Installing

```bash
make install                       # installs to /usr/local/bin/cmach
make install PREFIX=/opt/mach      # custom prefix
```

## Usage

cmach is not meant to be used directly. It is acquired automatically by the [mach](https://github.com/octalide/mach) build system. To build mach from source:

```bash
git clone https://github.com/octalide/mach
cd mach
make
```

If you have cmach installed or built locally, you can point the mach build at it:

```bash
CMACH=/path/to/cmach make
```

## License

[MIT](LICENSE)
