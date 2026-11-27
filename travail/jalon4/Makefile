CC ?= cc
CFLAGS ?= -Wall

all: output/client output/server

output:
	mkdir -p $@

output/client: client.c | output
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

output/server: server.c chained_list.c chained_list.h | output
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ server.c chained_list.c $(LDLIBS)

clean:
	rm -f output/client output/server

.PHONY: all clean

