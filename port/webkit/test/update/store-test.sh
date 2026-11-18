#!/usr/bin/env bash
# The update code the title, the update job and the restart helper share
# (apps/common/ps5update.c) on the Mac: JSON reading and the ProsperoStore
# check, which must fail closed.
#   port/webkit/test/update/store-test.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
work="$(mktemp -d)"
trap 'chmod -R u+rw "$work" 2>/dev/null; rm -rf "$work"' EXIT
cc -O2 -Wall -Wextra -Werror -o "$work/store-test" "$here/store-test.c" "$root/port/webkit/apps/common/ps5update.c"
mkdir "$work/t"
"$work/store-test" "$work/t"
