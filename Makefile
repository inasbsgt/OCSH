# ===================================================================
#  Makefile du projet ocsh (mini-shell en C)
#    make         -> compile ocsh.c et crée le programme ./ocsh
#    make clean   -> supprime le programme compilé
# ===================================================================

CC     = gcc  # compilateur C

# -Wall -Wextra : affiche tous les avertissements ; -O2 : code optimisé

CFLAGS = -Wall -Wextra -O2

all: ocsh # cible par défaut : "make" construit ocsh

ocsh: ocsh.c 
	$(CC) $(CFLAGS) -o ocsh ocsh.c

# "make clean" : fait le ménage
clean:
	rm -f ocsh

# all et clean sont des actions, pas des fichiers
.PHONY: all clean

# all fait la compilation du programme ocsh.c et crée l'exécutable ./ocsh. 
# clean supprime l'exécutable ./ocsh.
# .phony indique que all et clean sont des cibles qui ne correspondent pas à des fichiers.