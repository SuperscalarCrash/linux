#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0

set -euo pipefail

if (($# != 3)); then
	echo "usage: $0 <kernel-build-dir> <new-dist-dir> <expected-kernel-release>" >&2
	exit 2
fi

readonly build_dir="$1"
readonly dist_dir="$2"
readonly expected_release="$3"
readonly arch="${ARCH:-loongarch}"
readonly cross_compile="${CROSS_COMPILE:-loongarch32-linux-gnusf-}"
readonly source_date_epoch="${SOURCE_DATE_EPOCH:-0}"
toolchain_target="${TARGET:-${cross_compile##*/}}"
readonly toolchain_target="${toolchain_target%-}"

if [[ -e "${dist_dir}" ]]; then
	echo "release output directory already exists: ${dist_dir}" >&2
	exit 1
fi

actual_release="$(make -s O="${build_dir}" ARCH="${arch}" \
	CROSS_COMPILE="${cross_compile}" LOCALVERSION= kernelrelease)"
if [[ "${actual_release}" != "${expected_release}" ]]; then
	echo "kernel release mismatch: expected ${expected_release}, got ${actual_release}" >&2
	exit 1
fi

for file in \
	"${build_dir}/vmlinux" \
	"${build_dir}/System.map" \
	"${build_dir}/.config" \
	"${build_dir}/arch/loongarch/boot/dts/loongson-chiplab.dtb"; do
	[[ -s "${file}" ]] || {
		echo "required release input is missing: ${file}" >&2
		exit 1
	}
done

staging="$(mktemp -d "${TMPDIR:-/tmp}/chiplab-release.XXXXXXXX")"
cleanup()
{
	find "${staging}" -depth -mindepth 1 -delete
	rmdir "${staging}"
}
trap cleanup EXIT

modules_root="${staging}/modules"
headers_root="${staging}/headers"
bundle_root="${staging}/linux-${expected_release}-loongarch32"
mkdir -p "${modules_root}" "${headers_root}" \
	"${bundle_root}/boot" "${bundle_root}/config"

make O="${build_dir}" ARCH="${arch}" CROSS_COMPILE="${cross_compile}" \
	LOCALVERSION= INSTALL_MOD_PATH="${modules_root}" INSTALL_MOD_STRIP=1 \
	modules_install
make O="${build_dir}" ARCH="${arch}" CROSS_COMPILE="${cross_compile}" \
	LOCALVERSION= INSTALL_HDR_PATH="${headers_root}/usr" headers_install

module_dir="${modules_root}/lib/modules/${expected_release}"
[[ -d "${module_dir}" ]] || {
	echo "modules_install did not create ${module_dir}" >&2
	exit 1
}

# The build symlink created by modules_install points into the ephemeral CI
# workspace. Remove it rather than publishing a dangling or misleading link;
# the headers archive contains UAPI headers, not a complete external-module
# development tree.
if [[ -L "${module_dir}/build" ]]; then
	unlink "${module_dir}/build"
fi

install -m 0755 "${build_dir}/vmlinux" \
	"${bundle_root}/boot/vmlinux-${expected_release}"
install -m 0644 "${build_dir}/arch/loongarch/boot/dts/loongson-chiplab.dtb" \
	"${bundle_root}/boot/loongson-chiplab-${expected_release}.dtb"
install -m 0644 "${build_dir}/System.map" \
	"${bundle_root}/boot/System.map-${expected_release}"
install -m 0644 "${build_dir}/.config" \
	"${bundle_root}/config/config-${expected_release}"
if [[ -s "${build_dir}/Module.symvers" ]]; then
	install -m 0644 "${build_dir}/Module.symvers" \
		"${bundle_root}/config/Module.symvers-${expected_release}"
fi
cp -a "${modules_root}/lib" "${bundle_root}/"
cp -a "${headers_root}/usr" "${bundle_root}/"

mkdir -p "${dist_dir}"
install -m 0755 "${build_dir}/vmlinux" \
	"${dist_dir}/vmlinux-${expected_release}"
install -m 0644 "${build_dir}/arch/loongarch/boot/dts/loongson-chiplab.dtb" \
	"${dist_dir}/loongson-chiplab-${expected_release}.dtb"
install -m 0644 "${build_dir}/System.map" \
	"${dist_dir}/System.map-${expected_release}"
install -m 0644 "${build_dir}/.config" \
	"${dist_dir}/config-${expected_release}"

tar_args=(
	--sort=name
	"--mtime=@${source_date_epoch}"
	--owner=0
	--group=0
	--numeric-owner
	--zstd
	-cf
)

tar "${tar_args[@]}" "${dist_dir}/modules-${expected_release}.tar.zst" \
	-C "${modules_root}" lib
tar "${tar_args[@]}" "${dist_dir}/headers-${expected_release}.tar.zst" \
	-C "${headers_root}" usr
tar "${tar_args[@]}" \
	"${dist_dir}/linux-${expected_release}-loongarch32.tar.zst" \
	-C "${staging}" "$(basename "${bundle_root}")"

module_count="$(find "${module_dir}" -type f -name '*.ko*' | wc -l)"
cat >"${dist_dir}/BUILD_INFO.txt" <<EOF
kernel_release=${expected_release}
release_tag=${RELEASE_TAG:-unknown}
source_commit=${SOURCE_COMMIT:-unknown}
architecture=loongarch32
isa=la32rv1.0
abi=ilp32s
cpu_hz=${CPU_HZ:-unknown}
toolchain_target=${toolchain_target}
gcc_version=${GCC_VERSION:-unknown}
binutils_version=${BINUTILS_VERSION:-unknown}
module_count=${module_count}
EOF

(
	cd "${dist_dir}"
	find . -maxdepth 1 -type f ! -name SHA256SUMS -printf '%P\n' \
		| LC_ALL=C sort \
		| xargs sha256sum >SHA256SUMS
)

echo "Packaged Linux ${expected_release}:"
find "${dist_dir}" -maxdepth 1 -type f -printf '  %f (%s bytes)\n' | LC_ALL=C sort
