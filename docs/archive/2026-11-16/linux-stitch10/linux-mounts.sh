#!/bin/bash
set -euo pipefail
host_work=$WORKSPACE/work
source "$host_work/linux-vm-mounts.sh"
cpu_source="$host_work/cpu-20261111/source"
rootfs=/opt/lichii-amd64
# Keep the old build paths, while all compiled source comes from the isolated CPU checkout.
mkdir -p "$rootfs$host_work/lichii/.git" "$rootfs$cpu_source"
mountpoint -q "$rootfs$host_work/lichii/.git" || mount --bind "$host_work/lichii/.git" "$rootfs$host_work/lichii/.git"
mountpoint -q "$rootfs$cpu_source" || mount --bind "$cpu_source" "$rootfs$cpu_source"
mountpoint -q "$rootfs/work/lichii" || mount --bind "$cpu_source" "$rootfs/work/lichii"
mkdir -p "$rootfs/work/lichii/.cache/cpm"
mountpoint -q "$rootfs/work/lichii/.cache/cpm" || mount --bind "$host_work/lichii/.cache/cpm" "$rootfs/work/lichii/.cache/cpm"
chroot "$rootfs" /usr/bin/git -C /work/lichii rev-parse --show-toplevel HEAD
