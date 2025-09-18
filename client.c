/* client.c
 *
 * Client CLI simple pour communiquer avec server.c
 *
 * Usage:
 *   ./client
 * Then IDENT, sendTicket, etc.
 *
 * or: ./client <host> <port>
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define BUFSIZE 2048

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = 12345;
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { perror("inet_pton"); return 1; }

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); return 1; }

    char recvbuf[BUFSIZE];
    ssize_t n = recv(s, recvbuf, sizeof(recvbuf)-1, 0);
    if (n > 0) {
        recvbuf[n] = '\0';
        printf("%s", recvbuf);
    }

    char line[BUFSIZE];
    while (1) {
        printf("> "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        // send line to server
        if (send(s, line, strlen(line), 0) < 0) { perror("send"); break; }
        // recv reply (simple, may be fragmented)
        n = recv(s, recvbuf, sizeof(recvbuf)-1, 0);
        if (n <= 0) { printf("Serveur déconnecté\n"); break; }
        recvbuf[n] = '\0';
        printf("%s", recvbuf);
    }

    close(s);
    return 0;
}