CC = gcc -Wall -O2 -msse2
TESTU01_CFLAGS = -I/usr/local/include
TESTU01_LIB = -L/usr/local/lib -ltestu01

# you shouldn't have to edit below here

CFLAGS = `pkg-config --cflags glib-2.0 gretl`
LIBS = `pkg-config --libs glib-2.0 gretl`

gretltest: gretltest.c
	$(CC) $(TESTU01_CFLAGS) $(CFLAGS) -o $@ $< $(TESTU01_LIB) $(LIBS)

clean:
	rm -f gretltest
