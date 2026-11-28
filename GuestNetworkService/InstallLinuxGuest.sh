#!/bin/sh
set -eu
if [ "$(id -u)" -ne 0 ]; then echo 'Run as root.' >&2; exit 1; fi
exchange=0
clock=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --enable-file-exchange) exchange=1 ;;
        --allow-clock-set) clock=1 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
python3 -c 'import socket; assert hasattr(socket, "AF_VSOCK")'
for path in /opt/nanabox-gns /var/lib/nanabox-gns /var/lib/nanabox-gns/exchange /etc/systemd/system/nanabox-gns.service.d; do
    if [ -L "$path" ]; then echo "Unsafe symbolic link: $path" >&2; exit 1; fi
done
if systemctl is-active --quiet nanabox-gns.service; then
    systemctl stop nanabox-gns.service
fi
install -d -o root -g root -m 0755 /opt/nanabox-gns /etc/systemd/system/nanabox-gns.service.d
install -d -o root -g root -m 0700 /var/lib/nanabox-gns
install -o root -g root -m 0644 "$source_dir/gns.py" /opt/nanabox-gns/gns.py
install -o root -g root -m 0644 "$source_dir/nanabox-gns.service" /etc/systemd/system/nanabox-gns.service
options=
caps=CAP_NET_ADMIN
if [ "$exchange" -eq 1 ]; then
    install -d -o root -g root -m 0700 /var/lib/nanabox-gns/exchange
    options=' --exchange-root /var/lib/nanabox-gns/exchange'
fi
if [ "$clock" -eq 1 ]; then
    options="$options --allow-clock-set"
    caps="$caps CAP_SYS_TIME"
fi
temporary=$(mktemp /etc/systemd/system/nanabox-gns.service.d/.options.XXXXXXXX)
trap 'rm -f "$temporary"' EXIT HUP INT TERM
printf '[Service]\nExecStart=\nExecStart=/usr/bin/python3 -I /opt/nanabox-gns/gns.py serve%s\nCapabilityBoundingSet=\nCapabilityBoundingSet=%s\n' "$options" "$caps" > "$temporary"
chmod 0644 "$temporary"
mv -f "$temporary" /etc/systemd/system/nanabox-gns.service.d/50-options.conf
systemctl daemon-reload
systemctl enable --now nanabox-gns.service
systemctl is-active --quiet nanabox-gns.service
