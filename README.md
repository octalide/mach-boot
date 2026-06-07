mach-boot
=========

`cmach` is a minimal bootstrap compiler for [mach](https://github.com/octalide/mach), written in C.

It exists for one purpose: producing a working mach compiler from absolute scratch, on a machine where no mach release binary is available. This is the cold-start seed of the toolchain — **not** part of the normal mach build.

## When you need this

Normally you do not. mach self-seeds from its own releases: you fetch a released `mach` binary and run `mach build .`. `cmach` is no longer a build input for [octalide/mach](https://github.com/octalide/mach), and that repo no longer ships a Makefile.

Reach for `cmach` only when there is no usable mach release to bootstrap from — a brand-new platform, a fresh port, or recovering the toolchain from C source alone. In that situation `cmach` compiles the mach sources into a first working compiler, which then takes over.

## Building cmach

Prerequisites: clang (or gcc with C23 support), make.

```bash
make                               # produces out/bin/cmach
make install                       # installs to /usr/local/bin/cmach
make install PREFIX=/opt/mach      # custom prefix
```

## Cold-start seeding

With `cmach` built, point it at a mach project (a directory containing `mach.toml`) to produce a working binary:

```bash
cmach build .
```

Use the resulting binary as your `mach` toolchain; from there the standard `mach build .` flow takes over and `cmach` is no longer needed.

## License

[MIT](LICENSE)
