CC     = gcc
CFLAGS = -Wall -Wextra -g -O0

medsh: medsh.c
	$(CC) $(CFLAGS) -o medsh medsh.c

test: medsh
	bash tests/run_tests.sh

clean:
	rm -f medsh
	rm -rf run

.PHONY: test clean
