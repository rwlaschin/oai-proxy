CC ?= cc
CFLAGS ?= -O2 -Wall

oai-proxy: oai-proxy.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f oai-proxy

.PHONY: clean
