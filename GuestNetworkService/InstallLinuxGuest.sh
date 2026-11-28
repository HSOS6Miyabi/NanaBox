#!/bin/sh
set -eu
if [ "$(id -u)" -ne 0 ]; then echo 'Run as root.' >&2; exit 1; fi
exchange=0
clock=0
dns=0
loopback=0
bind=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --enable-file-exchange) exchange=1 ;;
        --allow-clock-set) clock=1 ;;
        --enable-dns-tunnel) dns=1 ;;
        --enable-loopback-routing) loopback=1 ;;
        --enable-bind-coordination) bind=1 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
python3 -c 'import socket; assert hasattr(socket, "AF_VSOCK")'
for path in /opt/nanabox-gns /var/lib/nanabox-gns /var/lib/nanabox-gns/exchange /etc/systemd/system/nanabox-gns.service.d; do
    if [ -L "$path" ]; then echo "Unsafe symbolic link: $path" >&2; exit 1; fi
done
if [ "$bind" -eq 1 ]; then
    command -v cc >/dev/null || { echo 'Install a C compiler for bind coordination.' >&2; exit 1; }
    python3 -I "$source_dir/gns.py" check-bind-support
    if [ -e /usr/local/bin/nanabox-bind ] || [ -L /usr/local/bin/nanabox-bind ]; then
        [ "$(readlink /usr/local/bin/nanabox-bind || true)" = /opt/nanabox-gns/nanabox-bind ] || { echo 'nanabox-bind already exists.' >&2; exit 1; }
    fi
fi
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
resolver_dir=/etc/systemd/resolved.conf.d
resolver_file=$resolver_dir/90-nanabox-dns-tunnel.conf
marker='# Managed by NanaBox DNS Tunnel'
if [ -L "$resolver_dir" ] || [ -L "$resolver_file" ]; then echo 'Unsafe resolver configuration path.' >&2; exit 1; fi
if [ -e "$resolver_file" ] && ! grep -Fxq "$marker" "$resolver_file"; then
    echo "Resolver configuration is owned by another application: $resolver_file" >&2; exit 1
fi
if [ "$dns" -eq 1 ]; then
    if ! systemctl is-active --quiet systemd-resolved.service; then echo 'DNS tunneling requires active systemd-resolved.' >&2; exit 1; fi
    options="$options --dns-tunnel"
    caps="$caps CAP_NET_BIND_SERVICE"
fi
if [ "$loopback" -eq 1 ]; then
    options="$options --loopback-state /run/nanabox-gns/loopback.json"
fi
runtime_mode=0700
extra_groups=
if [ "$bind" -eq 1 ]; then
    getent group nanabox-bind >/dev/null || groupadd --system nanabox-bind
    temporary_library=$(mktemp /opt/nanabox-gns/.bind.XXXXXXXX)
    trap 'rm -f "$temporary_library"' EXIT HUP INT TERM
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -shared -fPIC "$source_dir/bind_coord.c" -o "$temporary_library"
    install -o root -g root -m 0755 "$temporary_library" /opt/nanabox-gns/libnanabox-bind.so
    rm -f "$temporary_library"
    trap - EXIT HUP INT TERM
    install -o root -g root -m 0755 "$source_dir/nanabox-bind.sh" /opt/nanabox-gns/nanabox-bind
    ln -sfn /opt/nanabox-gns/nanabox-bind /usr/local/bin/nanabox-bind
    options="$options --bind-coordination"
    runtime_mode=0755
    extra_groups='SupplementaryGroups=nanabox-bind'
else
    if [ "$(readlink /usr/local/bin/nanabox-bind || true)" = /opt/nanabox-gns/nanabox-bind ]; then rm -f /usr/local/bin/nanabox-bind; fi
    rm -f /opt/nanabox-gns/libnanabox-bind.so /opt/nanabox-gns/nanabox-bind
fi
temporary=$(mktemp /etc/systemd/system/nanabox-gns.service.d/.options.XXXXXXXX)
trap 'rm -f "$temporary"' EXIT HUP INT TERM
printf '[Service]\nExecStart=\nExecStart=/usr/bin/python3 -I /opt/nanabox-gns/gns.py serve%s\nCapabilityBoundingSet=\nCapabilityBoundingSet=%s\nRuntimeDirectoryMode=%s\n%s\n' "$options" "$caps" "$runtime_mode" "$extra_groups" > "$temporary"
chmod 0644 "$temporary"
mv -f "$temporary" /etc/systemd/system/nanabox-gns.service.d/50-options.conf
systemctl daemon-reload
systemctl enable --now nanabox-gns.service
systemctl is-active --quiet nanabox-gns.service
if [ "$dns" -eq 1 ]; then
    install -d -o root -g root -m 0755 "$resolver_dir"
    resolver_tmp=$(mktemp "$resolver_dir/.nanabox-dns.XXXXXXXX")
    printf '%s\n[Resolve]\nDNS=\nDNS=127.0.0.1\nDomains=~.\nFallbackDNS=\n' "$marker" > "$resolver_tmp"
    chmod 0644 "$resolver_tmp"
    mv -f "$resolver_tmp" "$resolver_file"
    if ! systemctl restart systemd-resolved.service; then
        rm -f "$resolver_file"
        systemctl restart systemd-resolved.service || true
        exit 1
    fi
elif [ -f "$resolver_file" ]; then
    rm -f "$resolver_file"
    systemctl restart systemd-resolved.service
fi
