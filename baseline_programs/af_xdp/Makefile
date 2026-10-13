CC = clang
CFLAGS = -O2 -Wall -Wextra -D_GNU_SOURCE
LDFLAGS = -lpthread -lelf -lz -lbpf -lxdp

BINDIR = bin
BUILDDIR = build
SRCDIR = src

LOADERC = main.c
LOADEROBJ = af_xdp

AFXDPC = af_xdp.c
AFXDPOBJ = af_xdp.o

XDPC = redirect_to_af_xdp.c
XDPOBJ = redirect_to_af_xdp.o
XDPBC = redirect_to_af_xdp.bc

INCS = -I /usr/include/bpf/ -I src/ -I /usr/include/xdp/

all: xsk afxdp loader

$(BINDIR):
	mkdir -p $(BINDIR)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

xsk: $(BINDIR)
	$(CC) $(CFLAGS) -target bpf -g -D__BPF__ -emit-llvm $(INCS) -c $(SRCDIR)/$(XDPC) -o $(BINDIR)/$(XDPBC)
	llc -march=bpf -filetype=obj $(BINDIR)/$(XDPBC) -o $(BINDIR)/$(XDPOBJ)

afxdp: $(BINDIR)
	$(CC) $(CFLAGS) -ggdb3 $(INCS) -c $(SRCDIR)/$(AFXDPC) -o $(BINDIR)/$(AFXDPOBJ)

loader: $(BUILDDIR)
	$(CC) $(CFLAGS) -ggdb3 $(INCS) $(LDFLAGS) -o $(BUILDDIR)/$(LOADEROBJ) $(BINDIR)/$(AFXDPOBJ) $(LOADERC) 

clean:
	rm -rf $(BINDIR) $(BUILDDIR)

.PHONY: all clean xsk afxdp loader