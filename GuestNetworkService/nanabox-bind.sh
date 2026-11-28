#!/bin/sh
set -eu
if [ "$#" -eq 0 ]; then echo 'Usage: nanabox-bind COMMAND [ARGUMENTS...]' >&2; exit 2; fi
library=/opt/nanabox-gns/libnanabox-bind.so
if [ ! -r "$library" ]; then echo 'NanaBox bind coordination library is not installed.' >&2; exit 1; fi
export LD_PRELOAD="$library${LD_PRELOAD:+:$LD_PRELOAD}"
exec "$@"
