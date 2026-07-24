.. SPDX-License-Identifier: GPL-2.0

==========================
Gemmont Chiplab LA32R board
==========================

This port runs a 32-bit reduced LoongArch kernel on the Gemmont core in the
Loongson Chiplab NSCSCC FPGA design.  It is intended for the Chiplab
``fpga/loongson`` U-Boot system, not the Baixin SoC.

Hardware description
====================

The built-in ``loongson-chiplab`` device tree describes the hardware needed
for a network-booted initramfs system:

* one strict LA32R CPU, without CPUCFG or implementation-configuration CSRs;
* 128 MiB of DDR starting at physical address 0;
* an NS16550-compatible UART at ``0x1fe001e0``, IRQ 3, with a 33 MHz input
  clock and a 115200 baud console;
* the Chiplab DM9102-compatible Ethernet MAC at ``0x1ff00000``, IRQ 2;
* the direct LoongArch CPU interrupt controller.

The reduced kernel profile describes Gemmont's fixed implementation
resources: a 32-entry fully associative TLB, a 32-bit timer, and separate
8 KiB two-way L1 instruction and data caches with 64-byte lines.  The 10-bit
ASID width is still discovered from the architectural ASID CSR.

The FPGA CPU clock and the 33 MHz UART reference clock are independent.
Strict LA32R provides no architectural clock-discovery instruction.  The CPU
and constant timer use the same input clock, whose rate must therefore be
supplied either by the CPU node's ``clock-frequency``/``clocks`` property or
by the ``cpuclock=<Hz>`` kernel parameter.  The built-in device tree
intentionally does not freeze a particular FPGA bitstream frequency.

The exception path also follows the reduced architecture: ECFG vector spacing
is not used.  A compact common EENTRY dispatcher reads ESTAT and forwards
interrupts and exception codes to the Linux handlers in software.

Source history audit
====================

This port was derived by reviewing all 95 commits in
``la32r-Linux`` after Linux 5.14-rc2, from
``2734d6c1b1a089fb593ef6a23d4b70903526fe0c`` through
``4ed7b98e08e8``.

The following board-specific work is represented here:

* ``f99dddc5``: non-coherent DMA and data-cache
  writeback/invalidation;
* ``3d0b9aa7``, ``f363de7b``, ``fd39596b`` and ``9a47584e``: the Chiplab
  platform DMFE driver, including store-and-forward operation;
* ``65b59ce1``: direct CPU interrupt delivery, implemented here by the
  current generic LoongArch CPU interrupt controller;
* ``37c16aef``: UART IRQ 3 and Ethernet IRQ 2;
* ``0a45e97e`` and ``b3a43e30``: the final Loongson-board device-tree
  separation and cleanup.

The early LoongArch architecture, ACPI, PCI, IRQ-chip and LS7A series, plus
the LA32 ABI, signal and module series, are already superseded by the Linux
7.1 LoongArch implementation and were not replayed as old patches.
Configuration-only filesystem and userspace feature commits are likewise not
board support.

Baixin-only changes (``2a7d4c8e``, ``8d8b7339``, ``072ad81f``,
``30e91481``, ``52423549``, ``bc4f0cd``, ``a70ac108``, ``7435dbc5`` and
their merge commits) are intentionally excluded.  In particular,
``bc4f0cd`` became a ``BX_SOC``-only whole-cache workaround in the final
merged source and is not needed by the Loongson Chiplab design.  The LS1A
NAND driver from ``3d0b9aa7`` is also excluded: it is not required for
TFTP/initramfs boot and the board controller currently stalls while scanning
NAND bad blocks.  The old physical boot-argument workaround in ``7ae1318a``
is unnecessary because this port uses a built-in DT and deliberately ignores
non-EFI U-Boot argument registers.

Building with GCC 16
====================

The kernel has been validated with GCC 16.1.0 and binutils 2.46.1 configured
for ``loongarch32-linux-gnusf`` with ``la32rv1.0`` and the ``ilp32s`` ABI.
For example::

  export PATH=$HOME/.local/toolchains/gcc-16.1.0-loongarch/bin:$PATH
  export ARCH=loongarch
  export CROSS_COMPILE=loongarch32-linux-gnusf-
  export O=$PWD/../linux-build-chiplab

  make chiplab_la32r_defconfig
  scripts/config --file "$O/.config" --set-str CMDLINE \
    "console=ttyS0,115200 earlycon=uart8250,mmio,0x1fe001e0,115200 rdinit=/init cpuclock=${CPU_HZ}"
  make olddefconfig
  make -j"$(nproc)" vmlinux dtbs

Set ``CPU_HZ`` to the clock requested by the FPGA build; it is deliberately a
build input rather than a constant in the board DTS.  ``vmlinux`` is an ELF32
little-endian LoongArch executable with soft-float ABI flags.  The default
configuration leaves ``CONFIG_INITRAMFS_SOURCE`` empty.  To produce a
self-contained image, set it to an LA32R soft-float initramfs directory or
cpio list before the final build, for example::

  scripts/config --file "$O/.config" \
    --set-str INITRAMFS_SOURCE /absolute/path/to/la32r-rootfs
  make olddefconfig
  make -j"$(nproc)" vmlinux

Every executable in that root filesystem, including ``/init``, must use the
LA32R soft-float ABI.  An LA32S or LA64 executable will fault with a reserved
instruction when the kernel starts init.

Tagged GitHub releases
======================

Pushing a semantic version tag such as ``v0.1.0`` starts the
``Chiplab LA32R Linux release`` GitHub Actions workflow.  Before creating the
first tag, define the positive integer Actions repository variable
``CHIPLAB_CPU_HZ``.  This keeps the FPGA-specific clock outside the device
tree while ensuring that every published kernel has a usable timer frequency.

The workflow builds or restores a cached GCC 16.1.0 and Binutils 2.46.1
``loongarch32-linux-gnusf`` toolchain, then builds the kernel, modules and
device trees.  A tag named ``v0.1.0`` produces this exact kernel release
string::

  7.1.4-SuperscalarCrach-la32r-v0.1.0

The GitHub Release contains the loadable ``vmlinux`` ELF, standalone Chiplab
DTB, ``System.map``, kernel configuration, installed modules, UAPI headers, a
complete installation bundle and SHA-256 checksums.  The GCC installation is
kept in the Actions cache and is rebuilt only when its version or build script
changes.  A push changing the toolchain script or release workflow on the
default branch warms that branch's cache, allowing subsequent version tags to
reuse it despite GitHub's per-tag cache isolation.

U-Boot network boot
===================

Load the ELF at a temporary address that does not overlap its linked
``0xa0200000`` load segment.  ``bootelf`` loads the ELF segments and jumps to
the entry recorded in the ELF header::

  setenv ipaddr 172.25.2.2
  setenv serverip 172.25.2.1
  ping ${serverip}
  tftpboot 0xa3000000 vmlinux
  bootelf 0xa3000000

The built-in command line contains the console and init settings shown above,
plus the bitstream-specific ``cpuclock=`` value supplied at build time.
Change the example IP addresses to unused addresses on the local
``172.25.2.0/24`` network.
