/* Optional libc bind interposition for explicitly launched Linux applications. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

static int real_bind(int fd, const struct sockaddr *address, socklen_t length)
{
    return (int)syscall(SYS_bind, fd, address, length);
}

static int transfer(int fd, unsigned char *buffer, size_t length, int writing)
{
    while (length) {
        ssize_t result = writing ? send(fd, buffer, length, MSG_NOSIGNAL) : recv(fd, buffer, length, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return -1;
        buffer += result;
        length -= (size_t)result;
    }
    return 0;
}

static int request_reservation(unsigned char request[20])
{
    const char *path = getenv("NANABOX_BIND_SOCKET");
    if (!path || !*path) path = "/run/nanabox-gns/bind.sock";
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) return 2;
    memcpy(address.sun_path, path, strlen(path) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 2;
    int result = 2;
    struct timeval timeout = { .tv_sec = 4, .tv_usec = 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        connect(fd, (struct sockaddr *)&address, sizeof(address))) goto done;
    struct ucred peer;
    socklen_t size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) ||
        (peer.uid != 0 && peer.uid != geteuid())) goto done;
    unsigned char status = 2;
    if (!transfer(fd, request, 20, 1) && !transfer(fd, &status, 1, 0))
        result = status;
done:
    close(fd);
    return result;
}

int bind(int fd, const struct sockaddr *address, socklen_t length)
{
    /* Copy through the kernel so invalid user addresses still receive the
       ordinary bind() error instead of crashing in the interposer. */
    struct sockaddr_storage storage = {0};
    if (length < sizeof(sa_family_t) || length > sizeof(storage))
        return real_bind(fd, address, length);
    struct iovec local = { &storage, length };
    struct iovec remote = { (void *)address, length };
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != (ssize_t)length)
        return real_bind(fd, address, length);
    unsigned char request[20] = {0};
    int domain = 0, protocol = 0;
    socklen_t size = sizeof(domain);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &size))
        return real_bind(fd, address, length);
    size = sizeof(protocol);
    if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL, &protocol, &size) ||
        (protocol != IPPROTO_TCP && protocol != IPPROTO_UDP))
        return real_bind(fd, address, length);
    request[0] = (unsigned char)protocol;
    uint16_t port;
    if (storage.ss_family == AF_INET && domain == AF_INET && length >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)&storage;
        request[1] = 4;
        port = v4->sin_port;
        memcpy(request + 4, &v4->sin_addr, 4);
    } else if (storage.ss_family == AF_INET6 && domain == AF_INET6 && length >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&storage;
        port = v6->sin6_port;
        if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) {
            request[1] = 4;
            memcpy(request + 4, v6->sin6_addr.s6_addr + 12, 4);
        } else {
            request[1] = 6;
            memcpy(request + 4, &v6->sin6_addr, 16);
        }
    } else {
        return real_bind(fd, address, length);
    }
    if (!port) return real_bind(fd, address, length);
    memcpy(request + 2, &port, 2);
    int result = request_reservation(request);
    if (result != 0) {
        errno = result == 1 ? EADDRINUSE : ENETDOWN;
        return -1;
    }
    return real_bind(fd, (const struct sockaddr *)&storage, length);
}
