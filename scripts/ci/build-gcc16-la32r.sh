#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

readonly GCC_VERSION="${GCC_VERSION:-16.1.0}"
readonly BINUTILS_VERSION="${BINUTILS_VERSION:-2.46.1}"
readonly TARGET="${TARGET:-loongarch32-linux-gnusf}"
readonly GCC_SHA256="${GCC_SHA256:-50efb4d94c3397aff3b0d61a5abd748b4dd31d9d3f2ab7be05b171d36a510f79}"
readonly BINUTILS_SHA256="${BINUTILS_SHA256:-e127a709cba24c76de8936cb7083dd768f28cd37eb010492e2f19b71eb1294e4}"
readonly JOBS="${JOBS:-$(nproc)}"

if (($# != 1)); then
	echo "usage: $0 <absolute-install-prefix>" >&2
	exit 2
fi

prefix="$1"
if [[ "${prefix}" != /* ]]; then
	echo "toolchain install prefix must be absolute: ${prefix}" >&2
	exit 2
fi

gcc_bin="${prefix}/bin/${TARGET}-gcc"
ld_bin="${prefix}/bin/${TARGET}-ld"

validate_toolchain()
{
	local actual_target actual_gcc actual_binutils

	[[ -x "${gcc_bin}" && -x "${ld_bin}" ]] || return 1
	actual_target="$("${gcc_bin}" -dumpmachine)"
	actual_gcc="$("${gcc_bin}" -dumpfullversion)"
	actual_binutils="$("${ld_bin}" --version | sed -n '1p')"

	[[ "${actual_target}" == "${TARGET}" ]]
	[[ "${actual_gcc}" == "${GCC_VERSION}" ]]
	[[ "${actual_binutils}" == *"${BINUTILS_VERSION}"* ]]
}

if validate_toolchain; then
	echo "Using existing GCC ${GCC_VERSION} toolchain at ${prefix}"
	exit 0
fi

if [[ -e "${prefix}" ]]; then
	echo "refusing to replace incomplete toolchain directory: ${prefix}" >&2
	exit 1
fi

workdir="$(mktemp -d "${TMPDIR:-/tmp}/la32r-gcc.XXXXXXXX")"
cleanup()
{
	find "${workdir}" -depth -mindepth 1 -delete
	rmdir "${workdir}"
}
trap cleanup EXIT

mkdir -p "${workdir}/src" "${workdir}/build/binutils" "${workdir}/build/gcc"

gcc_archive="${workdir}/src/gcc-${GCC_VERSION}.tar.xz"
binutils_archive="${workdir}/src/binutils-${BINUTILS_VERSION}.tar.xz"

curl --fail --location --retry 5 --show-error \
	"https://ftp.gnu.org/gnu/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz" \
	--output "${gcc_archive}"
curl --fail --location --retry 5 --show-error \
	"https://ftp.gnu.org/gnu/binutils/binutils-${BINUTILS_VERSION}.tar.xz" \
	--output "${binutils_archive}"

printf '%s  %s\n' "${GCC_SHA256}" "${gcc_archive}" | sha256sum --check --strict
printf '%s  %s\n' "${BINUTILS_SHA256}" "${binutils_archive}" | sha256sum --check --strict

tar -xf "${gcc_archive}" -C "${workdir}/src"
tar -xf "${binutils_archive}" -C "${workdir}/src"

(
	cd "${workdir}/build/binutils"
	"${workdir}/src/binutils-${BINUTILS_VERSION}/configure" \
		--target="${TARGET}" \
		--prefix="${prefix}" \
		--disable-nls \
		--disable-werror \
		--disable-gdb \
		--disable-sim \
		--enable-plugins
	make -j"${JOBS}"
	make install
)

export PATH="${prefix}/bin:${PATH}"

(
	cd "${workdir}/build/gcc"
	"${workdir}/src/gcc-${GCC_VERSION}/configure" \
		--target="${TARGET}" \
		--prefix="${prefix}" \
		--with-arch=la32rv1.0 \
		--with-tune=loongarch32 \
		--with-newlib \
		--without-headers \
		--enable-languages=c,lto \
		--disable-bootstrap \
		--disable-multilib \
		--disable-nls \
		--disable-shared \
		--disable-threads \
		--disable-libatomic \
		--disable-libgomp \
		--disable-libquadmath \
		--disable-libsanitizer \
		--disable-libssp \
		--disable-libvtv
	make -j"${JOBS}" all-gcc all-target-libgcc
	make install-gcc install-target-libgcc
)

if ! validate_toolchain; then
	echo "newly built toolchain failed validation" >&2
	exit 1
fi

"${gcc_bin}" -v
"${ld_bin}" --version | sed -n '1p'
