CC = gcc
CFLAGS = -O2 -Wall -Wextra -D_GNU_SOURCE

BUILDDIR = build

MAIN = af_packet.c
MAINOBJ = af_packet

all: $(BUILDDIR) af_packet

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

af_packet: $(BUILDDIR)
	$(CC) $(CFLAGS) $(MAIN) -o $(BUILDDIR)/$(MAINOBJ)

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean af_packet