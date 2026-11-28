#!/bin/sh
set -eu
if [ "$(id -u)" -ne 0 ]; then echo 'Run as root.' >&2; exit 1; fi
exchange=0
clock=0
dns=0
loopback=0
bind=0
plan9=1
while [ "$#" -gt 0 ]; do
    case "$1" in
        --enable-file-exchange) exchange=1 ;;
        --allow-clock-set) clock=1 ;;
        --enable-dns-tunnel) dns=1 ;;
        --enable-loopback-routing) loopback=1 ;;
        --enable-bind-coordination) bind=1 ;;
        --disable-plan9) plan9=0 ;;
        --enable-plan9) plan9=1 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
legacy_network=0
if [ "$loopback" -eq 1 ] || [ "$bind" -eq 1 ]; then
    legacy_network=1
    python3 -c 'import socket; assert hasattr(socket, "AF_VSOCK")'
fi
for path in /opt/nanabox-gns /var/lib/nanabox-gns /var/lib/nanabox-gns/exchange /etc/systemd/system/nanabox-gns.service.d /etc/systemd/system/nanabox-guest-tool.service.d; do
    if [ -L "$path" ]; then echo "Unsafe symbolic link: $path" >&2; exit 1; fi
done
if [ "$bind" -eq 1 ]; then
    command -v cc >/dev/null || { echo 'Install a C compiler for bind coordination.' >&2; exit 1; }
    python3 -I "$source_dir/gns.py" check-bind-support
    if [ -e /usr/local/bin/nanabox-bind ] || [ -L /usr/local/bin/nanabox-bind ]; then
        [ "$(readlink /usr/local/bin/nanabox-bind || true)" = /opt/nanabox-gns/nanabox-bind ] || { echo 'nanabox-bind already exists.' >&2; exit 1; }
    fi
fi
if [ -e /usr/local/bin/nanabox-plan9 ] || [ -L /usr/local/bin/nanabox-plan9 ]; then
    case "$(readlink /usr/local/bin/nanabox-plan9 || true)" in
        /opt/nanabox-gns/plan9.py|/opt/nanabox-gns/nanabox-plan9) ;;
        *) echo 'nanabox-plan9 already exists.' >&2; exit 1 ;;
    esac
fi
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT
trap 'exit 1' HUP INT TERM
native_core="$source_dir/nanabox-guest-tool"
if [ ! -x "$native_core" ]; then
    command -v g++ >/dev/null || { echo 'Install a C++17 compiler, nlohmann-json and OpenSSL development headers, or use the prebuilt GuestTool package.' >&2; exit 1; }
    native_core="$build_dir/nanabox-guest-tool"
    g++ -std=c++17 -O2 -pthread "$source_dir/GuestTool.cpp" -lcrypto -o "$native_core"
fi
"$native_core" --self-test
native_plan9="$source_dir/nanabox-plan9"
if [ "$plan9" -eq 1 ] && [ ! -x "$native_plan9" ]; then
    command -v g++ >/dev/null || { echo 'Install a C++17 compiler and Boost.JSON headers, or use the prebuilt GuestTool package.' >&2; exit 1; }
    native_plan9="$build_dir/nanabox-plan9"
    g++ -std=c++17 -O2 -pthread "$source_dir/Plan9.cpp" -o "$native_plan9"
fi
if systemctl is-active --quiet nanabox-guest-tool.service; then
    systemctl stop nanabox-guest-tool.service
fi
if systemctl is-active --quiet nanabox-plan9.service; then
    systemctl stop nanabox-plan9.service
fi
if systemctl is-active --quiet nanabox-gns.service; then
    systemctl stop nanabox-gns.service
fi
install -d -o root -g root -m 0755 /opt/nanabox-gns /etc/systemd/system/nanabox-gns.service.d /etc/systemd/system/nanabox-guest-tool.service.d
install -o root -g root -m 0755 "$native_core" /opt/nanabox-gns/nanabox-guest-tool
install -o root -g root -m 0644 "$source_dir/nanabox-guest-tool.service" /etc/systemd/system/nanabox-guest-tool.service
install -d -o root -g root -m 0700 /var/lib/nanabox-gns
if [ "$legacy_network" -eq 1 ]; then
    install -o root -g root -m 0644 "$source_dir/gns.py" /opt/nanabox-gns/gns.py
fi
install -o root -g root -m 0644 "$source_dir/nanabox-gns.service" /etc/systemd/system/nanabox-gns.service
options=
caps=CAP_NET_ADMIN
tool_options=
tool_caps=
if [ "$exchange" -eq 1 ]; then
    install -d -o root -g root -m 0700 /var/lib/nanabox-gns/exchange
    tool_options=' --exchange-root /var/lib/nanabox-gns/exchange'
fi
if [ "$clock" -eq 1 ]; then
    tool_options="$tool_options --allow-clock-set"
    tool_caps=CAP_SYS_TIME
fi
printf '[Service]\nExecStart=\nExecStart=/opt/nanabox-gns/nanabox-guest-tool --serve%s\nCapabilityBoundingSet=\nCapabilityBoundingSet=%s\n' "$tool_options" "$tool_caps" > "$build_dir/tool-options.conf"
install -o root -g root -m 0644 "$build_dir/tool-options.conf" /etc/systemd/system/nanabox-guest-tool.service.d/50-options.conf
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
    temporary_library="$build_dir/libnanabox-bind.so"
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -shared -fPIC "$source_dir/bind_coord.c" -o "$temporary_library"
    chmod 0755 "$temporary_library"
    chown root:root "$temporary_library"
    mv -f "$temporary_library" /opt/nanabox-gns/libnanabox-bind.so
    rm -f "$temporary_library"
    install -o root -g root -m 0755 "$source_dir/nanabox-bind.sh" /opt/nanabox-gns/nanabox-bind
    ln -sfn /opt/nanabox-gns/nanabox-bind /usr/local/bin/nanabox-bind
    options="$options --bind-coordination"
    runtime_mode=0755
    extra_groups='SupplementaryGroups=nanabox-bind'
else
    if [ "$(readlink /usr/local/bin/nanabox-bind || true)" = /opt/nanabox-gns/nanabox-bind ]; then rm -f /usr/local/bin/nanabox-bind; fi
    rm -f /opt/nanabox-gns/libnanabox-bind.so /opt/nanabox-gns/nanabox-bind
fi
temporary="$build_dir/network-options.conf"
if [ "$legacy_network" -eq 1 ]; then
    network_command="/usr/bin/python3 -I /opt/nanabox-gns/gns.py serve$options"
    cleanup_command="/usr/bin/python3 -I /opt/nanabox-gns/gns.py clear-loopback /run/nanabox-gns/loopback.json"
else
    network_command="/opt/nanabox-gns/nanabox-guest-tool --serve --network$options"
    cleanup_command=
fi
printf '[Service]\nExecStart=\nExecStart=%s\nExecStopPost=\n%s\nCapabilityBoundingSet=\nCapabilityBoundingSet=%s\nRuntimeDirectoryMode=%s\n%s\nEnvironment=LC_ALL=C\nEnvironment=PATH=/usr/sbin:/usr/bin:/sbin:/bin\n' "$network_command" "$(if [ -n "$cleanup_command" ]; then printf 'ExecStopPost=%s' "$cleanup_command"; fi)" "$caps" "$runtime_mode" "$extra_groups" > "$temporary"
chmod 0644 "$temporary"
mv -f "$temporary" /etc/systemd/system/nanabox-gns.service.d/50-options.conf
if [ "$plan9" -eq 1 ]; then
    install -o root -g root -m 0755 "$native_plan9" /opt/nanabox-gns/nanabox-plan9
    rm -f /opt/nanabox-gns/plan9.py
    install -o root -g root -m 0644 "$source_dir/nanabox-plan9.service" /etc/systemd/system/nanabox-plan9.service
    ln -sfn /opt/nanabox-gns/nanabox-plan9 /usr/local/bin/nanabox-plan9
else
    if systemctl cat nanabox-plan9.service >/dev/null 2>&1; then systemctl disable --now nanabox-plan9.service; fi
    case "$(readlink /usr/local/bin/nanabox-plan9 || true)" in
        /opt/nanabox-gns/plan9.py|/opt/nanabox-gns/nanabox-plan9) rm -f /usr/local/bin/nanabox-plan9 ;;
    esac
    rm -f /etc/systemd/system/nanabox-plan9.service /opt/nanabox-gns/plan9.py /opt/nanabox-gns/nanabox-plan9
fi
systemctl daemon-reload
systemctl enable --now nanabox-gns.service
systemctl enable --now nanabox-guest-tool.service
systemctl is-active --quiet nanabox-guest-tool.service
if [ "$plan9" -eq 1 ]; then
    systemctl enable --now nanabox-plan9.service
    systemctl is-active --quiet nanabox-plan9.service
fi
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
