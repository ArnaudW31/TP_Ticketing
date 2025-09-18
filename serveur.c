/* server.c
 *
 * Serveur de ticketing :
 * - écoute TCP 127.0.0.1:12345
 * - mémoire partagée POSIX /ticket_shm
 * - mutex dans la mémoire partagée (PTHREAD_PROCESS_SHARED)
 *
 * Simplifié pour usage pédagogique.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <inttypes.h>

#define SHM_NAME "/ticket_shm"
#define MAX_TICKETS 5
#define MAX_TITLE 128
#define MAX_DESC 512
#define MAX_USER 64
#define SERVER_PORT 12345
#define BACKLOG 10
#define BUFSIZE 1024
#define PRIORITY_SECONDS (24*3600)

typedef enum {OPEN=0, IN_PROGRESS=1, CLOSED=2, PRIORITY=3} ticket_state_t;

typedef struct {
    uint32_t id;
    char title[MAX_TITLE];
    char desc[MAX_DESC];
    char owner[MAX_USER];            // username of who created
    char technician[MAX_USER];       // assigned technician name or empty
    ticket_state_t state;
    time_t created;
} ticket_t;

typedef struct {
    pthread_mutex_t mutex;           // mutex to protect structure
    int initialized;
    ticket_t tickets[MAX_TICKETS];
    int next_index;                  // for circular buffer
    uint32_t next_id;
} shared_data_t;

static shared_data_t *g_shm = NULL;

/* Helpers */
static void perror_exit(const char *msg){
    perror(msg);
    exit(EXIT_FAILURE);
}

static void shm_init_if_needed() {
    if (!g_shm) return;
    pthread_mutex_lock(&g_shm->mutex);
    if (!g_shm->initialized) {
        // init
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
    pthread_mutex_unlock(&g_shm->mutex);
}

static void shm_open_map() {
    int fd = shm_open(SHM_NAME, O_RDWR | O_CREAT, 0600);
    if (fd < 0) perror_exit("shm_open");

    size_t sz = sizeof(shared_data_t);
    if (ftruncate(fd, sz) == -1) perror_exit("ftruncate");

    void *addr = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) perror_exit("mmap");

    g_shm = (shared_data_t*)addr;

    /* Initialize mutex if not already: we need to setup attr with PTHREAD_PROCESS_SHARED, but
       only once. We'll check an 'initialized' flag protected by a temporary process-local lock:
       Because several server instances could create the shm simultaneously, we attempt to init the mutex
       only if its memory looks uninitialized. Use pthread_mutexattr and setpshared. */
    // We attempt to set mutex attributes unconditionally; if the mutex already initialized, pthread_mutex_init will fail.
    // Safer approach: try to setattr and init only if memory is zero-ish.
    static int tried_init = 0;
    if (!tried_init) {
        tried_init = 1;
        // Detect if mutex appears zeroed (naive)
        int zero = 1;
        for (size_t i=0;i<sizeof(pthread_mutex_t);i++){
            char *p = ((char*)&g_shm->mutex) + i;
            if (*p != 0) { zero = 0; break; }
        }
        if (zero) {
            pthread_mutexattr_t mattr;
            if (pthread_mutexattr_init(&mattr) != 0) perror_exit("mutexattr_init");
            if (pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED) != 0) perror_exit("mutexattr_setpshared");
            if (pthread_mutex_init(&g_shm->mutex, &mattr) != 0) perror_exit("pthread_mutex_init");
            pthread_mutexattr_destroy(&mattr);
            g_shm->initialized = 0; // actual app-level init done later
        } else {
            // assume mutex already initialized by previous process
        }
    }
    shm_init_if_needed();
}

/* Ticket operations (caller must hold mutex) */

static int insert_ticket(const char *owner, const char *title, const char *desc, uint32_t *out_id) {
    ticket_t *t = &g_shm->tickets[g_shm->next_index];
    t->id = g_shm->next_id++;
    strncpy(t->owner, owner, MAX_USER-1);
    t->owner[MAX_USER-1] = '\0';
    strncpy(t->title, title, MAX_TITLE-1); t->title[MAX_TITLE-1]='\0';
    strncpy(t->desc, desc, MAX_DESC-1); t->desc[MAX_DESC-1]='\0';
    t->state = OPEN;
    t->technician[0] = '\0';
    t->created = time(NULL);
    *out_id = t->id;
    g_shm->next_index = (g_shm->next_index + 1) % MAX_TICKETS;
    return 0;
}

static void list_tickets_for_owner(const char *owner, char *out, size_t outlen) {
    char buf[1024];
    buf[0] = '\0';
    int found = 0;
    for (int i=0;i<MAX_TICKETS;i++){
        ticket_t *t = &g_shm->tickets[i];
        if (t->id != 0 && strcmp(t->owner, owner) == 0) {
            found = 1;
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
            snprintf(buf+strlen(buf), sizeof(buf)-strlen(buf),
                "ID:%u | %s | %s | tech:%s | created:%s\nTitle: %s\nDesc: %s\n\n",
                t->id, st, t->owner, (t->technician[0] ? t->technician : "-"), timebuf, t->title, t->desc);
        }
    }
    if (!found) snprintf(out, outlen, "Aucun ticket pour %s\n", owner);
    else strncpy(out, buf, outlen-1);
}

static int count_assigned_to_technician(const char *tech) {
    int c=0;
    for (int i=0;i<MAX_TICKETS;i++){
        ticket_t *t = &g_shm->tickets[i];
        if (t->id!=0 && strcmp(t->technician, tech)==0 && t->state==IN_PROGRESS) c++;
    }
    return c;
}

/* When a tech connects, assign any PRIORITY tickets to him if he has capacity */
static int assign_priority_tickets_to(const char *tech) {
    int assigned = 0;
    int capacity = 5 - count_assigned_to_technician(tech);
    if (capacity <= 0) return 0;
    for (int i=0;i<MAX_TICKETS && capacity>0;i++){
        ticket_t *t = &g_shm->tickets[i];
        if (t->id!=0 && t->state==PRIORITY) {
            strncpy(t->technician, tech, MAX_USER-1); t->technician[MAX_USER-1]='\0';
            t->state = IN_PROGRESS;
            assigned++;
            capacity--;
        }
    }
    return assigned;
}

/* Mark tickets older than 24h as PRIORITY (caller holds mutex) */
static void update_priority_flags() {
    time_t now = time(NULL);
    for (int i=0;i<MAX_TICKETS;i++){
        ticket_t *t = &g_shm->tickets[i];
        if (t->id != 0 && t->state == OPEN) {
            if (difftime(now, t->created) >= PRIORITY_SECONDS) {
                t->state = PRIORITY;
            }
        }
    }
}

/* Find ticket by id */
static ticket_t* find_ticket_by_id(uint32_t id) {
    for (int i=0;i<MAX_TICKETS;i++){
        if (g_shm->tickets[i].id == id) return &g_shm->tickets[i];
    }
    return NULL;
}

/* Protocol handling per client */
typedef struct {
    int sock;
} client_thread_arg_t;

static void sendall(int sock, const char *s) {
    size_t len = strlen(s);
    ssize_t r = send(sock, s, len, 0);
    (void)r;
}

static void *client_thread(void *arg) {
    client_thread_arg_t *cta = arg;
    int sock = cta->sock;
    free(cta);

    char buf[BUFSIZE];
    char username[MAX_USER] = {0};
    int is_technician = 0;

    sendall(sock, "Bienvenue sur le serveur de ticketing.\nUsage: IDENT <username> <role:user|tech>\n");

    while (1) {
        ssize_t n = recv(sock, buf, sizeof(buf)-1, 0);
        if (n <= 0) break;
        buf[n] = '\0';

        // trim trailing newline
        char *p = buf + strlen(buf)-1;
        while (p >= buf && (*p == '\n' || *p == '\r')) { *p = '\0'; p--; }

        if (strncmp(buf, "IDENT ", 6) == 0) {
            char role[32];
            if (sscanf(buf+6, "%63s %31s", username, role) >= 1) {
                if (strcmp(role, "tech")==0) is_technician = 1;
                else is_technician = 0;
                char tmp[128];
                snprintf(tmp, sizeof(tmp), "Identifié en tant que '%s' (role=%s)\n", username, is_technician?"TECH":"USER");
                sendall(sock, tmp);

                // If technician, try assign priority tickets
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

        if (strncmp(buf, "sendTicket ", 11) == 0) {
            if (username[0]==0) { sendall(sock, "Identifiez-vous d'abord (IDENT ...)\n"); continue; }
            // sendTicket -new "title" "desc" OR sendTicket -l
            if (strncmp(buf+11, "-new", 4) == 0) {
                // naive parse of two quoted args
                char title[MAX_TITLE]="", desc[MAX_DESC]="";
                char *s = strchr(buf+11, '"');
                if (!s) { sendall(sock, "Usage: sendTicket -new \"title\" \"description\"\n"); continue; }
                s++;
                char *e = strchr(s, '"'); if (!e) { sendall(sock, "Missing closing quote for title\n"); continue; }
                size_t l = e - s; if (l >= sizeof(title)) l = sizeof(title)-1;
                strncpy(title, s, l); title[l]=0;
                char *s2 = strchr(e+1, '"');
                if (!s2) { sendall(sock, "Missing opening quote for description\n"); continue; }
                s2++;
                char *e2 = strchr(s2, '"'); if (!e2) { sendall(sock, "Missing closing quote for description\n"); continue; }
                size_t l2 = e2 - s2; if (l2 >= sizeof(desc)) l2 = sizeof(desc)-1;
                strncpy(desc, s2, l2); desc[l2]=0;

                pthread_mutex_lock(&g_shm->mutex);
                uint32_t id;
                insert_ticket(username, title, desc, &id);
                pthread_mutex_unlock(&g_shm->mutex);

                char out[128];
                snprintf(out, sizeof(out), "Ticket créé avec ID %u\n", id);
                sendall(sock, out);
            } else if (strncmp(buf+11, "-l", 2) == 0) {
                if (username[0]==0) { sendall(sock, "Identifiez-vous d'abord (IDENT ...)\n"); continue; }
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

        if (strncmp(buf, "connectTicket", 13) == 0) {
            // client asks to be considered tech — but better to use IDENT role=tech.
            sendall(sock, "Utilisez IDENT <username> tech pour vous connecter comme technicien.\n");
            continue;
        }

        /* Technician commands */
        if (is_technician) {
            if (strncmp(buf, "list", 4) == 0) {
                char out[4096];
                out[0]=0;
                pthread_mutex_lock(&g_shm->mutex);
                for (int i=0;i<MAX_TICKETS;i++){
                    ticket_t *t = &g_shm->tickets[i];
                    if (t->id != 0) {
                        // montrer tous les tickets si non assignés OU assignés à ce tech
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
                            strncpy(t->technician, username, MAX_USER-1); t->technician[MAX_USER-1]=0;
                            t->state = IN_PROGRESS;
                            sendall(sock, "Ticket pris en charge.\n");
                        }
                    }
                }
                pthread_mutex_unlock(&g_shm->mutex);
                continue;
            }
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
                        // Optionally clear fields
                        // memset(t,0,sizeof(*t));
                        sendall(sock, "Ticket clôturé.\n");
                    }
                }
                pthread_mutex_unlock(&g_shm->mutex);
                continue;
            }
        }

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

    close(sock);
    return NULL;
}

/* server main: setup shm, socket, accept loop */
int main(void) {
    shm_open_map();

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) perror_exit("socket");

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) perror_exit("bind");
    if (listen(listenfd, BACKLOG) < 0) perror_exit("listen");

    printf("Serveur de ticketing démarré sur 127.0.0.1:%d\n", SERVER_PORT);

    while (1) {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int c = accept(listenfd, (struct sockaddr*)&cli, &len);
        if (c < 0) {
            perror("accept");
            continue;
        }
        client_thread_arg_t *arg = malloc(sizeof(*arg));
        arg->sock = c;
        pthread_t tid;
        pthread_create(&tid, NULL, client_thread, arg);
        pthread_detach(tid);
    }

    close(listenfd);
    return 0;
}