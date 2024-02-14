run: run.c process.c
	gcc -O2 -Wall -Wextra -g -o $@ $^ `pkg-config --cflags --libs x11 libpulse`
