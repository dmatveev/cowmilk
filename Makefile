CC=gcc
CFLAGS=-O2

CM_PKGCONF=gtk+-2.0 gthread-2.0
CM_CFLAGS=$(CFLAGS) `pkg-config --cflags $(CM_PKGCONF)`
CM_LIBS=`pkg-config --libs $(CM_PKGCONF)`
CM_CC=$(CC) $(CM_CFLAGS) -c -Wall
CM_OBJFILES=cowmilk.o

cowmilk: $(CM_OBJFILES)
	$(CC) $(CM_OBJFILES) $(CM_LIBS) -o cowmilk

cowmilk.o:
	$(CM_CC) cowmilk.c

rebuild: clean cowmilk

clean:
	rm -f *~ *.o module.c
