#!/bin/sh
set -eu
if [ "$(id -u)" -ne 0 ]; then echo 'Run as root.' >&2; exit 1; fi
if systemctl cat nanabox-gns.service >/dev/null 2>&1; then
    systemctl disable --now nanabox-gns.service
fi
rm -f /etc/systemd/system/nanabox-gns.service /etc/systemd/system/nanabox-gns.service.d/50-options.conf
rm -f /opt/nanabox-gns/gns.py
rmdir /opt/nanabox-gns /etc/systemd/system/nanabox-gns.service.d 2>/dev/null || true
systemctl daemon-reload
echo 'Guest service removed. Exchanged files remain in /var/lib/nanabox-gns/exchange.'
