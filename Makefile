CFLAGS = -Wall -O2
#CFLAGS = -Wall -g3 -gdwarf -O0
LDLIBS = -lexif -ltiff

all: rpi2dng rpitrunc

.PHONY: clean

clean:
	rm -rf *.o rpi2dng rpitrunc
