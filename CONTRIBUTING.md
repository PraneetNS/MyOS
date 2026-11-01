# Contributing to MyOS

Thank you for your interest in contributing to MyOS!

## Development Philosophy
1. **Simplicity & Understandability**: Small, clear, readable code over hyper-optimized complexity.
2. **Deterministic Automated Testing**: No feature is merged without automated test coverage in `tools/test_boot.sh` or Python test scripts.
3. **No Regressions**: The operating system must always boot and operate reliably in `-m 64` memory environments.
4. **Honest Security**: Never claim security protections that are not enforced by code or verified by tests. Clearly document known architectural limits.

## Setting Up Your Environment
To build MyOS, you will need:
- Linux (Ubuntu 22.04 LTS recommended or WSL2 on Windows)
- `gcc` (with multilib 32-bit support) or `i686-elf-gcc`
- `qemu-system-i386` (QEMU 7.0+)
- `mtools`, `dosfstools`, `xorriso`, `grub-pc-bin`
- `python3` (3.10+)

Quick build & boot:
```bash
make clean
make all
./userland/build.sh
./tools/build_disk.sh
qemu-system-i386 -m 128 -cdrom myos.iso -drive file=disk.img,format=raw
```

## Running the Automated Test Suite
To run all regression tests locally:
```bash
./tools/test_boot.sh
```

## Submitting Pull Requests
1. Fork the repository and create your branch from `main`.
2. Ensure `make clean && make all && ./tools/test_boot.sh` passes 100%.
3. Write clean, descriptive commit messages.
4. Open a pull request detailing the problem solved and test results.

