raspiraw
========

Raspberry Pi CSI camera board JPEG+RAW photo to Adobe DNG converter (``raspi_dng``)

Changes
=======

Version 2.1
-----------

The default matrix is now a sensible but not optimal default.

With this version, you can pass an additional color-matrix to the the
converter. The new script raspi_dng.sh will extract the color-matrix
from the image and pass it to raspi_dng. This is a quick-and-dirty
workaround. When I figured out how to read the relevant EXIF-data from
within the C-code the wrapper should no longer be necessary.

Version 2
---------

Due to firmware changes (somewhere around end of May 2013), the readout of the
sensor has changed and the Bayer-pattern is no longer correct. This version
of raspi_dng works with raw-images taken with current firmware. If you still
need to process old images, use the original version of raspi_dng.

N.B.: old images are easily identified: the EXIF-tag 'model' is 'ov5647' for
old images and 'RP_OV5647' for new images.

The makefile now supports compilation on 64bit Linux systems. This might
break compilation on a Raspberry Pi, I will check (and correct if necessary)
as soon as possible.

Prerequisites
=============

C compiler, Internet connection to download the required version of libtiff and a patch.

Build instructions
==================

Run ``make``, you will need a working Internet connection on the first run.


Usage
=====

* Take a picture on the RPi embedding RAW data:
	
        raspistill --raw -o out.jpg

* Transfer the output file to where you have ``raspi_dng``
* Convert to Adobe DNG (no EXIF yet):

        ./raspi_dng out.jpg out.dng

* Copy EXIF metadata from JPEG (date, exposure, lens, metering mode, etc.):

        exiftool -tagsFromFile out.jpg out.dng -o out.exif.dng
  Note that this step is not necessary if you use the wrapper-script raspi_dng.sh.

See also
========

* Raspberry Pi forum topic: [http://www.raspberrypi.org/phpBB3/viewtopic.php?t=44918&p=356676](http://www.raspberrypi.org/phpBB3/viewtopic.php?t=44918&p=356676)
* Sample Raspberry Pi JPEG+RAW images: [http://bealecorner.org/best/RPi/](http://bealecorner.org/best/RPi/)
* dcraw homepage: [http://www.cybercom.net/~dcoffin/dcraw/](http://www.cybercom.net/~dcoffin/dcraw/)
