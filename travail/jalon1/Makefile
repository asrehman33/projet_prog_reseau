CC ?= cc
CFLAGS ?= -Wall

all: output/client output/server

output:
	mkdir -p $@

output/client: client.c | output
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

output/server: server.c | output
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f output/client output/server

.PHONY: all clean

