#!/bin/bash
set -euo pipefail
test_home=$(mktemp -d /tmp/stitch9-permissions.XXXXXX)
trap 'rm -rf "$test_home"' EXIT
chmod 0777 "$test_home"
chroot --userspec=65534:65534 / /usr/bin/env HOME="$test_home" XDG_CONFIG_HOME="$test_home/config" XDG_DATA_HOME="$test_home/data" XDG_CACHE_HOME="$test_home/cache" /opt/build-lichii/bin/tests 'NUP metadata preserves the archive when transaction creation is denied'
