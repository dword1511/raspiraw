CFLAGS = -Wall -g -O2
LDLIBS = -ljpeg -lm -lz -lexif -ltiff

all: rpi2dng

rpi2dng: rpi2dng.o
	$(CC) $(LDFLAGS) rpi2dng.o $(LDLIBS) -o $@

.PHONY: clean

clean:
	rm -rf *.o rpi2dng
