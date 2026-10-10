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
#include <signal.h>

#define CHUNK_SIZE 4096

static volatile int flag = 1;

/* use_errno distinguishes failures that set errno from those that do not. */
static void fail(const char *what, int use_errno) {
    if (use_errno)
        fprintf(stderr, "%s: %s\n", what, strerror(errno));
    else
        fprintf(stderr, "%s\n", what);
}


/* write() and read() on a socket may transfer fewer bytes than requested. */
/* return values: -1 error, 0 success */
static int write_all(int fd, const void *buf, size_t len) {
    const char *pos = buf;
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, pos + written, len - written);
        if (n < 0) {
            fail("write", 1);
            return -1;
        }
        if (n == 0) {
            fail("write: connection closed by peer", 0);
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}
static int read_all(int fd, void *buf, size_t len) {
    char *pos = buf;
    size_t nread = 0;
    while (nread < len) {
        ssize_t n = read(fd, pos + nread, len - nread);
        if (n < 0) {
            fail("read", 1);
            return -1;
        }
        if (n == 0) {
            fail("read: server closed the connection early", 0);
            return -1;
        }
        nread += (size_t)n;
    }
    return 0;
}


int get_pcc(int socket, uint32_t *pcc) {
    uint32_t net_size, file_size, remaining;
    uint32_t count = 0;
    
    // for reading from stream
    char chunk[CHUNK_SIZE];
    
    // the first 4 bytes in the stream specify the file size
    if (read_all(socket, &net_size, sizeof(net_size)) < 0)
        return -1;
    file_size = ntohl(net_size);
    printf("file_size: %u\n", file_size);
    // read the file
    remaining = file_size;
    while (remaining > 0) {
        size_t want = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
        printf("before read: %u\n", socket);
        ssize_t n = read(socket, chunk, want);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            printf("if (n < 0)\n");
            fail("read from file", 1);
            return -1;
        }
        if (n == 0){
            fail("file shrank while being sent", 0);
            return -1;
        }
        for (int i = 0; i < n; i++) {
            unsigned char byte = (unsigned char)chunk[i];
            if (32 <= byte && byte <= 126) {
                // increment count for current byte
                count++;
                pcc[byte]++;
            }
        }       
        remaining -= (uint32_t)n;
    }
    return count;
}


void update_total(uint32_t *pcc_total, uint32_t *pcc) {
    for (int i = 32; i <= 126; i++) {
        pcc_total[i] += pcc[i];
    }
}


void sigintHandler(int sig) {
    flag = 0;
}


int main(int argc, char** argv) {
    
    // activate handler
    signal(SIGINT, sigintHandler); 
    
    // validate input
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server-port>\n", argv[0]);
        exit(1);
    }
    // get user-input port
    uint16_t port = (uint16_t)atoi(argv[1]);

    // create data structures for keeping count
    // zero counters
    uint32_t pcc_total[127];
    memset(pcc_total, 0, 127 * sizeof(uint32_t));
    uint32_t pcc[127];
    memset(pcc, 0, 127 * sizeof(uint32_t));

    // create socket for listening
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    
    if (sockfd < 0) {
        fail("socket", 1);
        exit(1);
    }

    int reuse_addr = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) < 0) {
        fail("setsockopt", 1);
        exit(1);
    }
    
    // define server address
    struct sockaddr_in servAddr;
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(port);
    servAddr.sin_addr.s_addr = INADDR_ANY;

    // bind socket to the specified IP and port
    if (bind(sockfd, (struct sockaddr*)&servAddr, sizeof(servAddr)) < 0) {
        fail("bind", 1);
        exit(1);
    }

    // listen for connections
    listen(sockfd, 10);
    int clientCount = 0;
    while(flag) {
        int clientsoc = accept(sockfd, NULL, NULL);
        if (clientsoc < 0) {
            if (errno == EINTR && !flag)
                break;
            if (errno == EINTR)
                continue;
            fail("accept", 1);
            continue;
        }
        printf("new client: %d\n", clientsoc);
        int result1 = get_pcc(clientsoc, pcc);
        printf("result1: %d\n", result1);
        if (result1 < 0) {
            close(clientsoc);
            continue;
        }
        uint32_t count = (uint32_t)result1;
        uint32_t net_count = htonl(count);
        int result2 = write_all(clientsoc, &net_count, sizeof(net_count));
        if (result2 < 0) {
            close(clientsoc);
            continue;
        }
        close(clientsoc);
        // finished processing socket; now updating totals
        clientCount++;
        update_total(pcc_total, pcc);
        // reset counter for next stream
        memset(pcc, 0, 127 * sizeof(uint32_t));
    }
    close(sockfd);

    for (int i = 32; i <= 126; i++) {
        if (pcc_total[i] > 0) {
            printf("char '%c' : %u times\n", i, pcc_total[i]);
        }
    }
    printf("Served %u client(s) successfully\n", clientCount);

    return 0;
}


