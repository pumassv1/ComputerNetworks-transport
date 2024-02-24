CC = gcc -g

CFLAGS = -O2 -std=gnu17 -Wall -Wextra -Werror

transport: transport.o
	$(CC) $(CFLAGS) -o transport transport.o

clean:
	rm -f *.o

distclean:
	rm -f *.o transport