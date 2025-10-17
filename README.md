# Projet Ticketing — Serveur & Client C

## Aperçu

Ce projet implémente un **système client-serveur en C** basé sur **TCP/IP** permettant la gestion de "tickets" (demandes ou incidents).
Le serveur gère une **mémoire partagée POSIX** pour stocker les tickets et un **mutex inter-processus** (`PTHREAD_PROCESS_SHARED`) pour synchroniser l'accès concurrent.
Le client, en ligne de commande, permet de se connecter au serveur, s’identifier et envoyer des tickets.

## Fonctionnalités principales

### Côté serveur (`serveur.c`)

* Écoute sur **127.0.0.1:12345** (TCP).
* Gestion d'une **mémoire partagée POSIX** `/ticket_shm`.
* Stockage des tickets avec titre, description, auteur, horodatage.
* Synchronisation par **mutex partagé**.
* Affichage des interactions et états du serveur.
* Gestion concurrente possible des clients.

### Côté client (`client.c`)

* Connexion TCP au serveur (`127.0.0.1:12345` par défaut).
* Commandes disponibles :

  * `IDENT <nom>` → identification de l’utilisateur.
  * `SENDTICKET` → envoi d’un ticket (titre + description).
  * `LIST` → affichage des tickets présents.
  * `QUIT` → fermeture propre de la connexion.
* Interface simple en ligne de commande.

## Structure du projet

```
.
├── serveur.c
├── client.c
└── README.md
```

## Compilation

Compiler les programmes avec `gcc` (ou `clang`) :

```bash
gcc -Wall -pthread -o serveur serveur.c
gcc -Wall -o client client.c
```

> Le flag `-pthread` est requis côté serveur pour la synchronisation.

## Exécution

### 1. Lancer le serveur

```bash
./serveur
```

* Écoute sur le port `12345`.
* Initialise la mémoire partagée `/ticket_shm`.

### 2. Lancer le client

Dans un autre terminal :

```bash
./client
```

ou avec des paramètres :

```bash
./client <adresse_ip> <port>
```

Exemple :

```bash
./client 127.0.0.1 12345
```

### 3. Exemple de session

```
$ ./serveur
[INFO] Serveur en écoute sur 127.0.0.1:12345...

$ ./client
> IDENT Jules
[OK] Bonjour Jules
> SENDTICKET
Titre : Bug interface
Description : Le bouton "Envoyer" ne répond pas.
[OK] Ticket ajouté
> LIST
#1 [Jules] Bug interface - 17/10/2025
> QUIT
Déconnexion...
```


## Technologies utilisées

* **C POSIX** (sockets, threads, mutex, mémoire partagée)
* **TCP/IP**
* **pthread** pour la synchronisation
* **mmap / shm_open** pour la mémoire partagée
* **CLI** minimaliste pour interaction rapide

---

**Dernière mise à jour :** Octobre 2025