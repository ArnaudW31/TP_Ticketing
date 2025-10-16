/* server.c
 *
 * Serveur de ticketing :
 * - écoute TCP 127.0.0.1:12345
 * - mémoire partagée POSIX /ticket_shm
 * - mutex dans la mémoire partagée (PTHREAD_PROCESS_SHARED)
 *
 * Simplifié pour usage pédagogique.
 */

#define _POSIX_C_SOURCE 200809L  // Active certaines fonctions POSIX modernes
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>     // Pour mmap, shm_open
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>    // Pour les sockets TCP/IP
#include <pthread.h>      // Pour les threads et mutex partagés
#include <inttypes.h>     // Pour les types entiers fixes

// Constantes générales
#define SHM_NAME "/ticket_shm"      // Nom de la mémoire partagée POSIX
#define MAX_TICKETS 5               // Nombre maximum de tickets en mémoire
#define MAX_TITLE 128
#define MAX_DESC 512
#define MAX_USER 64
#define SERVER_PORT 12345           // Port TCP du serveur
#define BACKLOG 10                  // File d’attente de connexions
#define BUFSIZE 1024
#define PRIORITY_SECONDS (24*3600)  // 24 heures pour devenir prioritaire

// États possibles d’un ticket
typedef enum {OPEN=0, IN_PROGRESS=1, CLOSED=2, PRIORITY=3} ticket_state_t;

// Structure d’arguments pour un thread client
typedef struct {
    int sock;
} client_thread_arg_t;

// Structure d’un ticket
typedef struct {
    uint32_t id;                    // ID unique du ticket
    char title[MAX_TITLE];          // Titre
    char desc[MAX_DESC];            // Description
    char owner[MAX_USER];           // Utilisateur ayant créé le ticket
    char technician[MAX_USER];      // Technicien assigné (ou vide)
    ticket_state_t state;           // État du ticket
    time_t created;                 // Date/heure de création
} ticket_t;

// --- Structure partagée entre processus ---
typedef struct {
    pthread_mutex_t mutex;          // Mutex partagé entre processus
    int initialized;                // Indique si la mémoire est initialisée
    ticket_t tickets[MAX_TICKETS];  // Tableau circulaire de tickets
    int next_index;                 // Position d’insertion suivante
    uint32_t next_id;               // Prochain ID de ticket
} shared_data_t;

static shared_data_t *g_shm = NULL; // Pointeur global vers la mémoire partagée

// --- Fonction utilitaire pour quitter avec message d’erreur ---
static void perror_exit(const char *msg){
    perror(msg);
    exit(EXIT_FAILURE);
}

// --- Initialisation de la mémoire partagée (si pas encore faite) ---
static void shm_init_if_needed() {
    // Si l'espace mémoire du mutex n'est pas dfinie
    if (!g_shm) return;

    // Lock le mutex
    pthread_mutex_lock(&g_shm->mutex);
    if (!g_shm->initialized) {
        // Réinitialise tout le contenu
        g_shm->next_index = 0;
        g_shm->next_id = 1;
        for (int i=0;i<MAX_TICKETS;i++){
            g_shm->tickets[i].id = 0;
            g_shm->tickets[i].state = CLOSED;
            g_shm->tickets[i].created = 0;
            g_shm->tickets[i].owner[0] = '\0';
            g_shm->tickets[i].technician[0] = '\0';
            g_shm->tickets[i].title[0]= '\0';
            g_shm->tickets[i].desc[0]= '\0';
        }
        g_shm->initialized = 1;
    }

    // Unlock le mutex
    pthread_mutex_unlock(&g_shm->mutex);
}

// --- Création/attachement de la mémoire partagée ---
static void shm_open_map() {
    int fd;
    size_t sz;

    fd = shm_open(SHM_NAME, O_RDWR | O_CREAT, 0600); // Ouvre ou crée la mémoire partagée
    if (fd < 0) 
        perror_exit("Erreur lors de l'ouverture de la mémoire partagée");

    sz = sizeof(shared_data_t);
    if (ftruncate(fd, sz) == -1) perror_exit("Erreur lors du troncage de la mémoire partagée"); // Définit la taille du segment

    void *addr = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); // Mappe dans l’espace mémoire
    if (addr == MAP_FAILED) 
        perror_exit("Erreur lors du mappage");

    g_shm = (shared_data_t*)addr;

    // --- Initialisation du mutex partagé entre processus ---
    static int tried_init = 0;
    if (!tried_init) {
        tried_init = 1;

        // Vérifie si le mutex semble vierge (non initialisé)
        int is_mutex_empty = 1;
        for (size_t i = 0; i < sizeof(pthread_mutex_t) ;i++){
            char *p = ((char*)&g_shm->mutex) + i;
            if (*p != 0) { 
                is_mutex_empty = 0; 
                break; 
            }
        }
        if (is_mutex_empty == 1) {
            // Création du mutex partagé
            pthread_mutexattr_t mattr;

            // Initialisation des attributs du mutex
            if (pthread_mutexattr_init(&mattr) != 0) 
                perror_exit("Erreur lors de l'initialisation des attributs du mutex");
            
            // Attribut du mutex partagé entre les thread
            if (pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED) != 0) 
                perror_exit("Erreur lors de l'attribution de l'espace partagé");

            // Initialisation du mutex global avec les attributs
            if (pthread_mutex_init(&g_shm->mutex, &mattr) != 0) 
                perror_exit("Erreur lors de l'initialisation du mutex global");
            
            pthread_mutexattr_destroy(&mattr);
            g_shm->initialized = 0; // Marque non initialisé au niveau applicatif
        }
    }
    shm_init_if_needed(); // Termine l’initialisation logique
}

/* -------------------
 * Fonctions de gestion des tickets
 * ------------------- */

// Ajoute un nouveau ticket
// Paramètres : 
// owner = le nom d'utilisateur créant le ticket
// title = le titre du ticket
// desc = la description du ticket
// out_id = pointeur vers l'adresse qui sera l'id du ticket créé
static int insert_ticket(const char *owner, const char *title, const char *desc, uint32_t *out_id) {
    
    // Récupère l'addrese du nouveau ticket
    ticket_t *t = &g_shm->tickets[g_shm->next_index];

    // On remplit les infos du ticket
    t->id = g_shm->next_id++;
    strncpy(t->owner, owner, MAX_USER-1);
    strncpy(t->title, title, MAX_TITLE-1);
    strncpy(t->desc, desc, MAX_DESC-1);
    t->state = OPEN;
    t->technician[0] = '\0';
    t->created = time(NULL);

    // Id du ticket créé
    *out_id = t->id;

    // Index circulaire
    g_shm->next_index = (g_shm->next_index + 1) % MAX_TICKETS;

    return 0;
}

// Liste les tickets appartenant à un utilisateur
static void list_tickets_for_owner(const char *owner, char *out, size_t outlen) {
    char buf[1024];
    buf[0] = '\0';
    int found = 0;

    // On parcourt tout les tickets
    for (int i=0;i<MAX_TICKETS;i++){
        ticket_t *t = &g_shm->tickets[i];

        // Si le propriétaire du ticket est le propriétaire demandé
        if (t->id != 0 && strcmp(t->owner, owner) == 0) {
            // On a trouvé au moins 1 ticket
            found = 1;

            // On remplis le buffer de réponse
            char st[16];
            switch(t->state){
                case OPEN: strcpy(st,"OPEN"); break;
                case IN_PROGRESS: strcpy(st,"IN_PROGRESS"); break;
                case CLOSED: strcpy(st,"CLOSED"); break;
                case PRIORITY: strcpy(st,"PRIORITY"); break;
            }
            // Formatte la date
            char timebuf[64];
            struct tm tm;
            localtime_r(&t->created, &tm);
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
            snprintf(buf+strlen(buf), sizeof(buf)-strlen(buf),
                "ID:%u | %s | %s | tech:%s | created:%s\nTitle: %s\nDesc: %s\n\n",
                t->id, st, t->owner, (t->technician[0] ? t->technician : "-"), timebuf, t->title, t->desc);
        }
    }
    
    // Remplissage de la réponse
    if (!found)
        snprintf(out, outlen, "Aucun ticket pour %s\n", owner);
    else 
        strncpy(out, buf, outlen-1);
}

// Compte les tickets pris par un technicien
static int count_assigned_to_technician(const char *tech) {
    int c=0;
    for (int i=0;i<MAX_TICKETS;i++){

        ticket_t *t = &g_shm->tickets[i];

        // Si le tech du ticket est le tech demandé
        if (t->id!=0 && strcmp(t->technician, tech)==0 && t->state==IN_PROGRESS)
            c++;
    }
    return c;
}

// Assigne les tickets prioritaires à un technicien libre
static int assign_priority_tickets_to(const char *tech) {
    int assigned = 0;
    int capacity = 5 - count_assigned_to_technician(tech);

    // Si le technicien a moins de 5 tickets assignés
    if (capacity <= 0) return 0;
    
    // Il faut que le technicien aie moins de 5 tickets
    for (int i=0;i<MAX_TICKETS && capacity>0;i++){

        ticket_t *t = &g_shm->tickets[i];

        // Si le ticket est prioritaire
        if (t->id!=0 && t->state==PRIORITY) {
            strncpy(t->technician, tech, MAX_USER-1);
            t->state = IN_PROGRESS;
            assigned++;
            capacity--;
        }
    }
    return assigned;
}

// Met à jour les tickets vieux de 24h en PRIORITY
static void update_priority_flags() {
    time_t now = time(NULL);
    for (int i=0;i<MAX_TICKETS;i++){
        ticket_t *t = &g_shm->tickets[i];
        if (t->id != 0 && t->state == OPEN) {
            // Si le ticket a été créé il y a + de PRIORITY_SECNDS secondes (24 * 3600), il est prioritaire
            if (difftime(now, t->created) >= PRIORITY_SECONDS) {
                t->state = PRIORITY;
            }
        }
    }
}

// Recherche d’un ticket par ID
static ticket_t* find_ticket_by_id(uint32_t id) {
    for (int i=0;i<MAX_TICKETS;i++){
        if (g_shm->tickets[i].id == id) return &g_shm->tickets[i];
    }
    return NULL;
}

/* -------------------
 * Gestion des clients (threads)
 * ------------------- */

// Envoi du message s au client sock
static void sendall(int sock, const char *s) {
    size_t len = strlen(s);

    send(sock, s, len, 0);
}

// Fonction principale exécutée par chaque thread client
static void *client_thread(void *arg) {
    client_thread_arg_t *cta = arg;
    int sock = cta->sock;
    free(cta);

    char buf[BUFSIZE];
    char username[MAX_USER] = {0};
    int is_technician = 0;

    // Message d’accueil
    sendall(sock, "Bienvenue sur le serveur de ticketing. \nUsage: IDENT <username> <role:user|tech>\n");

    // Boucle d'écoute du client
    while (1) {

        // En attente d'une commande du client
        ssize_t mess = recv(sock, buf, sizeof(buf)-1, 0);
        if (mess <= 0) break; // Déconnexion
        buf[mess] = '\0';

        // Supprime les \n finaux
        char *p = buf + strlen(buf)-1;
        while (p >= buf && (*p == '\n' || *p == '\r')) { *p = '\0'; p--; }

        // --- Commande IDENT ---
        if (strncmp(buf, "IDENT ", 6) == 0) {
            char role[32];
            if (sscanf(buf+6, "%63s %31s", username, role) >= 1) {
                if (strcmp(role, "tech")==0)
                    is_technician = 1;
                else
                    is_technician = 0;

                char tmp[128];
                snprintf(tmp, sizeof(tmp), "Identifié en tant que '%s' (role=%s)\n", username, is_technician?"TECH":"USER");
                sendall(sock, tmp);

                // Si technicien → assigne tickets prioritaires
                if (is_technician) {
                    pthread_mutex_lock(&g_shm->mutex);
                    update_priority_flags();
                    int assigned = assign_priority_tickets_to(username);
                    pthread_mutex_unlock(&g_shm->mutex);
                    if (assigned > 0) {
                        char tmsg[128];
                        snprintf(tmsg, sizeof(tmsg), "Assigné %d ticket(s) PRIORITY à vous.\n", assigned);
                        sendall(sock, tmsg);
                    } else {
                        sendall(sock, "Aucun ticket prioritaire à vous assigner maintenant.\n");
                    }
                }
            } else {
                sendall(sock, "Usage IDENT <username> <role:user|tech>\n");
            }
            continue;
        }

        // --- Commandes utilisateur ---
        if (strncmp(buf, "sendTicket ", 11) == 0) {
            if (username[0]==0) { sendall(sock, "Identifiez-vous d'abord (IDENT ...)\n"); continue; }

            // Création d’un ticket
            if (strncmp(buf+11, "-new", 4) == 0) {
                char title[MAX_TITLE]="", desc[MAX_DESC]="";

                // Extraction naïve entre guillemets
                char *s = strchr(buf+11, '"');
                if (!s) { sendall(sock, "Usage: sendTicket -new \"title\" \"description\"\n"); continue; }
                s++;

                char *e = strchr(s, '"');
                if (!e) { sendall(sock, "Guillemet de fermeture manquante pour le titre\n"); continue; }
                size_t l = e - s; 

                if (l >= sizeof(title)) 
                    l = sizeof(title)-1;

                // Recup le titre
                strncpy(title, s, l); 
                title[l]=0;

                char *s2 = strchr(e+1, '"');
                if (!s2) { sendall(sock, "Guillemet d'ouverture manquante pour la description\n"); continue; }
                s2++;

                char *e2 = strchr(s2, '"');
                if (!e2) { sendall(sock, "Guillemet de fermeture manquante pour la description\n"); continue; }

                size_t l2 = e2 - s2; 
                if (l2 >= sizeof(desc)) 
                    l2 = sizeof(desc)-1;
                
                // Recup la description
                strncpy(desc, s2, l2); desc[l2]=0;

                pthread_mutex_lock(&g_shm->mutex);
                uint32_t id;
                insert_ticket(username, title, desc, &id);
                pthread_mutex_unlock(&g_shm->mutex);

                char out[128];
                snprintf(out, sizeof(out), "Ticket créé avec ID %u\n", id);
                sendall(sock, out);
            }
            // Liste des tickets
            else if (strncmp(buf+11, "-l", 2) == 0) {
                char out[4096];
                pthread_mutex_lock(&g_shm->mutex);
                list_tickets_for_owner(username, out, sizeof(out));
                pthread_mutex_unlock(&g_shm->mutex);
                sendall(sock, out);
            } else {
                sendall(sock, "Usage: sendTicket -new \"title\" \"description\" OR sendTicket -l\n");
            }
            continue;
        }

        // --- Commandes technicien ---
        if (is_technician) {
            // Liste les tickets visibles
            if (strncmp(buf, "list", 4) == 0) {
                char out[4096];
                out[0]=0;
                pthread_mutex_lock(&g_shm->mutex);
                for (int i=0;i<MAX_TICKETS;i++){
                    ticket_t *t = &g_shm->tickets[i];
                    if (t->id != 0) {
                        // Affiche si non assigné ou assigné à ce tech
                        if (t->technician[0] == '\0' || strcmp(t->technician, username) == 0) {
                            char st[16];
                            switch(t->state){
                                case OPEN: strcpy(st,"OPEN"); break;
                                case IN_PROGRESS: strcpy(st,"IN_PROGRESS"); break;
                                case CLOSED: strcpy(st,"CLOSED"); break;
                                case PRIORITY: strcpy(st,"PRIORITY"); break;
                            }
                            char timebuf[64];
                            struct tm tm;
                            localtime_r(&t->created, &tm);
                            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm);
                            snprintf(out+strlen(out), sizeof(out)-strlen(out),
                                "ID:%u | %s | owner:%s | tech:%s | created:%s\nTitle: %s\nDesc: %s\n\n",
                                t->id, st, t->owner,
                                (t->technician[0]?t->technician:"-"),
                                timebuf, t->title, t->desc);
                        }
                    }
                }
                pthread_mutex_unlock(&g_shm->mutex);
                if (out[0]==0) sendall(sock, "Aucun ticket à afficher.\n");
                else sendall(sock, out);
                continue;
            }

            // Prendre un ticket
            if (strncmp(buf, "take ", 5) == 0) {
                uint32_t id = (uint32_t)strtoul(buf+5, NULL, 10);
                pthread_mutex_lock(&g_shm->mutex);
                ticket_t *t = find_ticket_by_id(id);
                if (!t) {
                    sendall(sock, "Ticket introuvable.\n");
                } else {
                    if (t->state == CLOSED) sendall(sock, "Ticket déjà clos.\n");
                    else {
                        int assigned_count = count_assigned_to_technician(username);
                        if (assigned_count >= 5) {
                            sendall(sock, "Capacité maximale atteinte (5 tickets).\n");
                        } else {
                            strncpy(t->technician, username, MAX_USER-1);
                            t->state = IN_PROGRESS;
                            sendall(sock, "Ticket pris en charge.\n");
                        }
                    }
                }
                pthread_mutex_unlock(&g_shm->mutex);
                continue;
            }

            // Fermer un ticket
            if (strncmp(buf, "close ", 6) == 0) {
                uint32_t id = (uint32_t)strtoul(buf+6, NULL, 10);
                pthread_mutex_lock(&g_shm->mutex);
                ticket_t *t = find_ticket_by_id(id);
                if (!t) {
                    sendall(sock, "Ticket introuvable.\n");
                } else {
                    if (strcmp(t->technician, username)!=0) {
                        sendall(sock, "Vous n'êtes pas assigné à ce ticket.\n");
                    } else {
                        t->state = CLOSED;
                        sendall(sock, "Ticket clôturé.\n");
                    }
                }
                pthread_mutex_unlock(&g_shm->mutex);
                continue;
            }
        }

        // Aide
        if (strcmp(buf, "help") == 0) {
            sendall(sock,
                "Commandes:\n"
                "IDENT <username> <role:user|tech>\n"
                "sendTicket -new \"title\" \"description\"\n"
                "sendTicket -l\n"
                "list (technicien pour voir ses tickets)\n"
                "take <id> (technicien)\n"
                "close <id> (technicien)\n"
            );
            continue;
        }

        sendall(sock, "Commande inconnue. 'help' pour l'aide.\n");
    }

    close(sock); // Ferme la connexion client
    return NULL;
}

/* -------------------
 * Fonction principale du serveur
 * ------------------- */

int main(void) {
    shm_open_map(); // Crée et mappe la mémoire partagée

    int listenfd;
    struct sockaddr_in addr;
    int one = 1;

    // Configuration du socket serveur

    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) 
        perror_exit("Erreur lors de la création du socket");

    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))==-1) // Réutilisation d’adresse
        fatal_error("Echec de setsockopt(SO_REUSEADDR)");

    memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) 
        perror_exit("Echec de bind");
    if (listen(listenfd, BACKLOG) == -1) 
        perror_exit("Echec de listen");

    printf("Serveur de ticketing démarré sur 127.0.0.1:%d\n", SERVER_PORT);

    // --- Boucle principale d’acceptation des clients ---
    while (1) {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);

        //Bloquage en attendant la connexion d'un client
        int client_descriptor = accept(listenfd, (struct sockaddr*)&client, &len);

        if (client_descriptor == -1) {
            perror("Erreur lors de la connexion du client ");
            continue;
        }

        client_thread_arg_t *arg;
        arg = malloc(sizeof(*arg));
        arg->sock = client_descriptor;
        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, arg); // Crée un thread par client
        pthread_detach(tid); // Détache le thread (pas besoin de join)
    }

    close(listenfd);
    return 0;
}