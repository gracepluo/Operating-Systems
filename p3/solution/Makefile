# Variables
CC = gcc --sanitize=address
CFLAGS = -Wall -Wextra -Werror -pedantic -std=gnu18
LOGIN = gluo
SUBMITPATH = ~cs537-1/handin/gluo/

# First target: `all`
.PHONY: all
all: wsh wsh-dbg
	


# Target: `wsh` (release no debug)
wsh: wsh.c wsh.h
	$(CC) $(CFLAGS) -O2 -o $@ $^

# Target: `wsh-dbg` (debugging binary)
wsh-dbg: wsh.c wsh.h
	$(CC) $(CFLAGS) -Og -ggdb  -DDEBUG=1 -o $@ $^


valgrind: wsh
 --leak-check=full --show-leak-kinds=all --track-origins=yes --verbose wsh

submit:
	cp wsh.* $(SUBMITPATH)
	cp README.txt $(SUBMITPATH)
	cp Makefile $(SUBMITPATH)
	echo "Submitting as $(LOGIN)"

	cp -r ../ $(SUBMITPATH)


clean:
	rm -f wsh wsh-dbg