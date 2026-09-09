# GTK3 vs GTK4 baseline memory probes for ViewMD's widget mix.
# Build both:   make
# Build one:    make gtk3_probe   (needs libgtk-3-dev)
#               make gtk4_probe   (needs libgtk-4-dev)
# Run both:     make run     (or ./run.sh)   - prints each probe's VmRSS/PSS
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall
.PHONY: all run clean

all: gtk3_probe gtk4_probe

gtk3_probe: gtk3_probe.c
	$(CC) $(CFLAGS) gtk3_probe.c -o $@ $(shell pkg-config --cflags --libs gtk+-3.0)

gtk4_probe: gtk4_probe.c
	$(CC) $(CFLAGS) gtk4_probe.c -o $@ $(shell pkg-config --cflags --libs gtk4)

run: all
	bash run.sh

clean:
	rm -f gtk3_probe gtk4_probe
