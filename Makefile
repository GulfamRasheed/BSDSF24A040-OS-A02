# Makefile  --  build script for the `ls` re-engineering project
#
# KEY MAKE CONCEPTS USED HERE
#   TARGET      : the file that gets produced (bin/ls)
#   PREREQUISITE: the files/flags the target depends on
#   RECIPE      : the shell command run when the target is out of date
#   $@          : the target's name        (automatics)
#   $<          : first prerequisite       (automatics)
#   $^          : all prerequisites        (automatics)
#
# make decides what to rebuild by comparing timestamps: a target is rebuilt
# only if a prerequisite is NEWER than it. That is why we compile straight
# into obj/ls.o and link into bin/ls separately - editing one .c file then
# re-compiles just that object and re-links, instead of rebuilding all.
#
# WHICH SOURCE IS BUILT
#   src/ls-v1.0.0.c is the working file: the instructor's starter, extended
#   feature by feature, exactly as the assignment requires ("extend the same
#   base code throughout"). The name is kept from the base version so the
#   repository layout always matches the required structure.
#
#   versions/ holds a snapshot per tagged release, so any tagged version can
#   be rebuilt exactly as it was:
#
#       make                                      # src/ls-v1.0.0.c (current)
#       make SRC=versions/ls-v1.2.0.c             # rebuild v1.2.0
#       make clean SRC=versions/ls-v1.2.0.c
#
# NOTE: the instructor's starter repository contains NO Makefile, so this one
# is written from scratch. SRC is a variable so it can be overridden on the
# make command line. A wildcard ($(wildcard src/*.c)) is deliberately NOT
# used: it would break the moment a second .c file appeared, by trying to
# link two main() functions together.

SRC     ?= src/ls-v1.0.0.c
CC      ?= gcc
CFLAGS  := -std=c11 -D_GNU_SOURCE -Wall -Wextra -O2

OBJ     := obj/ls.o
TARGET  := bin/ls

# `all` is the default goal: bare `make` builds `all`.
all: $(TARGET)

# Order-only prerequisite (the | bar): ensures obj/ exists, but a change in
# the directory's own timestamp does NOT force a rebuild.
$(OBJ): $(SRC) | obj
	$(CC) $(CFLAGS) -c $< -o $@

# Link the object file into the executable.
$(TARGET): $(OBJ) | bin
	$(CC) $(CFLAGS) $^ -o $@

# The directories are targets too, so make can create them on demand.
obj bin:
	mkdir -p $@

# Remove generated artefacts; source is never touched.
clean:
	rm -f obj/*.o bin/ls

# Remove everything from bin/ as well.
distclean: clean
	rm -f bin/*

.PHONY: all clean distclean