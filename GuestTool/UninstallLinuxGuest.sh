#!/bin/sh
set -eu
if [ "$(id -u)" -ne 0 ]; then echo 'Run as root.' >&2; exit 1; fi
if systemctl cat nanabox-gns.service >/dev/null 2>&1; then
    systemctl disable --now nanabox-gns.service
fi
if systemctl cat nanabox-guest-tool.service >/dev/null 2>&1; then
    systemctl disable --now nanabox-guest-tool.service
fi
rm -f /etc/systemd/system/nanabox-guest-tool.service /etc/systemd/system/nanabox-guest-tool.service.d/50-options.conf /opt/nanabox-gns/nanabox-guest-tool
rmdir /etc/systemd/system/nanabox-guest-tool.service.d 2>/dev/null || true
if systemctl cat nanabox-plan9.service >/dev/null 2>&1; then
    systemctl disable --now nanabox-plan9.service
fi
if [ -x /opt/nanabox-gns/nanabox-plan9 ]; then
    /opt/nanabox-gns/nanabox-plan9 cleanup
elif [ -f /opt/nanabox-gns/plan9.py ]; then
    python3 -I /opt/nanabox-gns/plan9.py cleanup
fi
case "$(readlink /usr/local/bin/nanabox-plan9 || true)" in
    /opt/nanabox-gns/plan9.py|/opt/nanabox-gns/nanabox-plan9) rm -f /usr/local/bin/nanabox-plan9 ;;
esac
rm -f /etc/systemd/system/nanabox-plan9.service /opt/nanabox-gns/plan9.py /opt/nanabox-gns/nanabox-plan9
rm -f /etc/systemd/system/nanabox-gns.service /etc/systemd/system/nanabox-gns.service.d/50-options.conf
if [ "$(readlink /usr/local/bin/nanabox-bind || true)" = /opt/nanabox-gns/nanabox-bind ]; then rm -f /usr/local/bin/nanabox-bind; fi
rm -f /opt/nanabox-gns/gns.py /opt/nanabox-gns/libnanabox-bind.so /opt/nanabox-gns/nanabox-bind
rmdir /opt/nanabox-gns /etc/systemd/system/nanabox-gns.service.d 2>/dev/null || true
systemctl daemon-reload
resolver_file=/etc/systemd/resolved.conf.d/90-nanabox-dns-tunnel.conf
if [ ! -L "$resolver_file" ] && [ -f "$resolver_file" ] && grep -Fxq '# Managed by NanaBox DNS Tunnel' "$resolver_file"; then
    rm -f "$resolver_file"
    if systemctl is-active --quiet systemd-resolved.service; then systemctl restart systemd-resolved.service; fi
fi
echo 'Guest service removed. Exchanged files remain in /var/lib/nanabox-gns/exchange.'
