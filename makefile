


CFLAGS=-Wall -Iinclude -I../vstserver/include -I/usr/local/include -g -O2
LDFLAGS=-L/usr/local/lib -L../vstserver -lvst -lpthread -lm -lc

all: vst.so

clean:
	rm *.o *.so *~

install: vst.so
	cp vst.so ${LADSPA_PATH}

vst.so: vst.o makefile
	ld -shared -o vst.so vst.o $(LDFLAGS)

vst.o: vst.c
	gcc -c vst.c $(CFLAGS)

