run: run.c
	gcc -Wall -Wextra -g -o $@ $< `pkg-config --cflags --libs x11`
