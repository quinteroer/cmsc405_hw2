# --- Variables ---
SRCS = $(wildcard *.c)
PROGS = $(patsubst %.c, %, $(SRCS))
CC = gcc
CFLAGS = -Wall -lpthread

# --- Phony Targets ---
.PHONY: all clean pull deploy

all: $(PROGS)
# Pull the latest changes from the remote repository
pull:
	@echo "Pulling latest changes from the remote repository..."
	git pull origin main
	git reset --hard origin/main

# pull and compile the latest code
deploy: pull all
	@echo "Deployment complete. All programs are up to date and compiled."

#3 pattern rules for compiling each program
%: %.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(PROGS)