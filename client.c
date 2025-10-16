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

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define BUFSIZE 2048

int main(int argc, char **argv) {
    struct sockaddr_in addr;
    const char *host = "127.0.0.1";
    int port = 12345;
    //Si pas donné en paramètre, valeur de base
    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    //Création du socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { 
        perror("Erreur lors de la crétion du socket"); 
        return 1; 
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { 
        perror("Erreur lors de la conversion de l'adresse de l'hôte"); 
        return 1; 
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { 
        perror("Erreur lors de la connexion au serveur"); 
        return 1; 
    }

    char recvbuf[BUFSIZE];

    // En attente d'un message du serveur
    ssize_t mess = recv(sock, recvbuf, sizeof(recvbuf)-1, 0);
    if (mess > 0) {
        recvbuf[mess] = '\0';
        printf("%s", recvbuf);
    }

    char line[BUFSIZE];
    while (1) {
        printf("> "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) 
            break;

        // Envoi de la commande au serveur
        if (send(sock, line, strlen(line), 0) < 0) { 
            perror("Erreur lors de l'envoi du message"); 
            break; 
        }
        // En attente de la réponse du serveur
        mess = recv(sock, recvbuf, sizeof(recvbuf)-1, 0);

        // Si on ne reçoit rien, le serveur est déco
        if (mess <= 0) { 
            printf("Serveur déconnecté\n"); 
            break; 
        }
        
        recvbuf[mess] = '\0';
        printf("%s", recvbuf);
    }

    close(sock);
    return 0;
}