CC = gcc  # compilateur C

# -Wall -Wextra : affiche tous les avertissements ; -O2 : code optimisé

CFLAGS = -Wall -Wextra -O2

all: ocsh # cible par défaut : "make" construit ocsh

ocsh: code/ocsh.c
	$(CC) $(CFLAGS) -o ocsh code/ocsh.c

clean:
	rm -f ocsh

# all et clean sont des actions, pas des fichiers
.PHONY: all clean

# all fait la compilation du programme ocsh.c et crée l'exécutable ./ocsh.
# clean supprime l'exécutable ./ocsh.
# .phony indique que all et clean sont des cibles qui ne correspondent pas à des fichiers.