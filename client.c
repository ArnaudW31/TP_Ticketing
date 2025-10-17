<<<<<<< Updated upstream
=======
/* client.c
 * 
 * Client CLI simple pour communiquer avec server.c
 *
 * Usage:
 *    ./client
 * ou ./client <host> <port>
 * Commandes dispo : IDENT, sendTicket, etc.
 *
 * 
 */

>>>>>>> Stashed changes
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>

#define BUFSIZE 2048
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 12345

static int sock = -1;

// Fermeture propre sur Ctrl+C
void handle_sigint(int sig) {
    (void)sig;
    if (sock >= 0) close(sock);
    printf("\nDéconnexion du client.\n");
    exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
    struct sockaddr_in addr = {0};
    const char *host = DEFAULT_HOST;
    int port = DEFAULT_PORT;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) {
        char *end = NULL;
        long val = strtol(argv[2], &end, 10);
        if (*end != '\0' || val <= 0 || val > 65535) {
            fprintf(stderr, "Port invalide : %s\n", argv[2]);
            return EXIT_FAILURE;
        }
        port = (int)val;
    }

    signal(SIGINT, handle_sigint);

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Erreur socket");
        return EXIT_FAILURE;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "Adresse IP invalide : %s\n", host);
        close(sock);
        return EXIT_FAILURE;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Erreur connexion serveur");
        close(sock);
        return EXIT_FAILURE;
    }

    printf("Connecté à %s:%d\n", host, port);

    char sendbuf[BUFSIZE];
    char recvbuf[BUFSIZE];

    // Lecture du message de bienvenue
    ssize_t n = recv(sock, recvbuf, sizeof(recvbuf) - 1, 0);
    if (n > 0) {
        recvbuf[n] = '\0';
        printf("%s", recvbuf);
    }

    // Boucle principale
    while (1) {
        printf("> ");
        fflush(stdout);

        if (!fgets(sendbuf, sizeof(sendbuf), stdin))
            break;  // EOF

        if (send(sock, sendbuf, strlen(sendbuf), 0) < 0) {
            perror("Erreur envoi");
            break;
        }

        n = recv(sock, recvbuf, sizeof(recvbuf) - 1, 0);
        if (n <= 0) {
            printf("Serveur déconnecté.\n");
            break;
        }

        recvbuf[n] = '\0';
        printf("%s", recvbuf);
    }

    close(sock);
    return EXIT_SUCCESS;
}