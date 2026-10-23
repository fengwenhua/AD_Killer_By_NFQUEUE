#!/bin/sh
# Only the Android boot-completed property is mocked; CLI and kernel are real.
[ "$1" = sys.boot_completed ] || exit 1
printf '1\n'
