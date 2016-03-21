CCFLAGS=-Wall

ARCH=$(shell uname -m)
ifeq ($(ARCH),x86_64)
  LIBSUFFIX=64
else
  LIBSUFFIX=
endif

all: rpi2dng

tiff-3.8.2.tar.gz:
	wget -nc 'http://dl.maptools.org/dl/libtiff/tiff-3.8.2.tar.gz'

tiff-3.8.2: tiff-3.8.2.tar.gz libtiff.patch
	tar -zxvf "$<"
	cat "libtiff.patch" | patch -p0 -f

libtiff.patch:
	wget -nc 'http://www.cybercom.net/~dcoffin/dcraw/libtiff.patch'

local/lib$(LIBSUFFIX)/libtiff.a: tiff-3.8.2
	cd $< ; ./configure --prefix=$(PWD)/local
	cd $< ; make -j4
	cd $< ; make install

clean:
	rm -rf local tiff-3.8.2 *.o

raspi_dng.o: local/lib$(LIBSUFFIX)/libtiff.a raspi_dng.c
	$(CC) $(CCFLAGS) -c raspi_dng.c -I./local/include -o $@

raspi_dng: raspi_dng.o local/lib$(LIBSUFFIX)/libtiff.a
	$(CC) raspi_dng.o local/lib$(LIBSUFFIX)/libtiff.a \
			-ljpeg -lm -lz -lexif -o $@

rpi2dng.o: local/lib$(LIBSUFFIX)/libtiff.a rpi2dng.c
	$(CC) $(CCFLAGS) -c rpi2dng.c -I./local/include -o $@

rpi2dng: rpi2dng.o local/lib$(LIBSUFFIX)/libtiff.a
	$(CC) rpi2dng.o local/lib$(LIBSUFFIX)/libtiff.a \
			-ljpeg -lm -lz -lexif -o $@
