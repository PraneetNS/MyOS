# MyOS i686 Cross-Toolchain & Newlib Setup

This directory contains the automated build script for building the `i686-elf` cross-compiler and `newlib` C standard library runtime for MyOS.

## Prerequisites (Ubuntu / Debian / WSL)

The following packages are required on the host system:
```bash
sudo apt update
sudo apt install -y build-essential bison flex libgmp3-dev libmpc-dev libmpfr-dev texinfo libisl-dev wget
```

## Toolchain Components & Versions

- **Binutils**: 2.42 (`f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800`)
- **GCC**: 13.2.0 (`e275e76442a6067341a27f04c5c6b83d8613144004c0413528863dc6b5c743da`)
- **Newlib**: 4.4.0.20231231 (`0c166a39e1bf0951dfafcd68949fe0e4b6d3658081d6282f39aeefc6310f2f13`)

## Building

Run the automated build script:
```bash
bash tools/toolchain/build.sh
```

By default, the cross-compiler is installed to `$HOME/opt/myos-cross`. You can customize the installation prefix using the `MYOS_PREFIX` environment variable:
```bash
MYOS_PREFIX=/custom/path bash tools/toolchain/build.sh
```

The script is idempotent: it verifies SHA256 checksums, logs all output to `tools/toolchain/build.log`, and skips components that have already been built.
