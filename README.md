# Projet Ticketing — Serveur & Client C

## Aperçu

Ce projet implémente un **système client-serveur en C** permettant la gestion de tickets.
Le serveur gère une **mémoire partagée POSIX** pour stocker les tickets et un **mutex inter-processus** (`PTHREAD_PROCESS_SHARED`) pour synchroniser l'accès concurrent.
Le client, en ligne de commande, permet de se connecter au serveur, s’identifier et envoyer des tickets.

## Fonctionnalités principales

### Côté serveur (`serveur.c`)

* Écoute sur **127.0.0.1:12345** mais possibilité de changer dans le code.
* Gestion d'une **mémoire partagée POSIX**.
* Stockage des tickets avec titre, description, auteur, date de création.
* Synchronisation par **mutex partagé**.
* Affichage des interactions et états du serveur.
* Gestion concurrente possible des clients.

### Côté client (`client.c`)

* Connexion TCP au serveur (`127.0.0.1:12345` par défaut).
* Commandes disponibles :

  * `IDENT <nom>` → identification de l’utilisateur.
  * `sendTicket` → envoi d’un ticket (titre + description).
  * `list` → affichage des tickets présents.
  * `quit` → fermeture propre de la connexion.
* Interface simple en ligne de commande.

## Structure du projet

```
.
├── serveur.c
├── client.c
└── README.md
```

## Compilation

Compiler les programmes avec `gcc`:

```bash
gcc -o serveur serveur.c
gcc -o client client.c
```

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
Serveur en écoute sur 127.0.0.1:12345...

$ ./client
> IDENT Jules
Identifié en tant que  "Jules" (role = USER)
> sendTicket -new "Bug interface" "Le bouton "Envoyer" ne répond pas."
Ticket créé avec Id 1
> list
ID:1 | IN_PROGRESS | owner:Jules | tech:Jules | created:2025-09-18 16:01:08
Title: Bug interface
Desc: Le bouton "Envoyer" ne répond pas.


## Technologies utilisées

* **C POSIX** (sockets, threads, mutex, mémoire partagée)
* **TCP/IP**
* **pthread**
* **mmap / shm_open** pour la mémoire partagée

---

**Dernière mise à jour :** Octobre 2025
