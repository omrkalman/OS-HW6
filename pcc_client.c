/*
 * pcc_client.c -- Printable Characters Counter, client side.
 * Do not modify.
 *
 * Usage: ./pcc_client <server_ip> <server_port> <file_path>
 *
 * Protocol (32-bit unsigned, network byte order):
 *   client -> server : N, the number of bytes to follow
 *   client -> server : N bytes of file content
 *   server -> client : C, the number of printable bytes among them
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define CHUNK_SIZE 4096

/* use_errno distinguishes failures that set errno from those that do not. */
static void fail(const char *what, int use_errno) {
    if (use_errno)
        fprintf(stderr, "%s: %s\n", what, strerror(errno));
    else
        fprintf(stderr, "%s\n", what);
    exit(1);
}

/* write() and read() on a socket may transfer fewer bytes than requested. */

static void write_all(int fd, const void *buf, size_t len) {
    const char *pos = buf;
    size_t written = 0;

    while (written < len) {
        ssize_t n = write(fd, pos + written, len - written);
        if (n < 0)
            fail("write", 1);
        if (n == 0)
            fail("write: connection closed by peer", 0);
        written += (size_t)n;
    }
}

static void read_all(int fd, void *buf, size_t len) {
    char *pos = buf;
    size_t nread = 0;

    while (nread < len) {
        ssize_t n = read(fd, pos + nread, len - nread);
        if (n < 0)
            fail("read", 1);
        if (n == 0)
            fail("read: server closed the connection early", 0);
        nread += (size_t)n;
    }
}

int main(int argc, char *argv[]) {
    const char *server_ip, *file_path;
    uint16_t port;
    int filefd, sockfd;
    struct stat file_info;
    struct sockaddr_in server_addr;
    uint32_t file_size, remaining, net_size, net_count;
    char chunk[CHUNK_SIZE];

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <server_ip> <server_port> <file_path>\n",
                argv[0]);
        exit(1);
    }

    server_ip = argv[1];
    port      = (uint16_t)atoi(argv[2]);
    file_path = argv[3];

    filefd = open(file_path, O_RDONLY);
    if (filefd < 0)
        fail("open", 1);

    if (fstat(filefd, &file_info) < 0)
        fail("fstat", 1);
    file_size = (uint32_t)file_info.st_size;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        fail("socket", 1);

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(port);

    /* inet_pton() returns 0 on a malformed address without setting errno. */
    switch (inet_pton(AF_INET, server_ip, &server_addr.sin_addr)) {
    case 1:
        break;
    case 0:
        fail("inet_pton: malformed IPv4 address", 0);
        break;
    default:
        fail("inet_pton", 1);
        break;
    }

    if (connect(sockfd, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0)
        fail("connect", 1);

    net_size = htonl(file_size);
    write_all(sockfd, &net_size, sizeof(net_size));

    /* Send exactly file_size bytes: the count already announced to the server. */
    remaining = file_size;
    while (remaining > 0) {
        size_t want = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
        ssize_t n = read(filefd, chunk, want);

        if (n < 0)
            fail("read from file", 1);
        if (n == 0)
            fail("file shrank while being sent", 0);

        write_all(sockfd, chunk, (size_t)n);
        remaining -= (uint32_t)n;
    }
    close(filefd);

    read_all(sockfd, &net_count, sizeof(net_count));
    close(sockfd);

    printf("# of printable characters: %u\n", ntohl(net_count));
    return 0;
}