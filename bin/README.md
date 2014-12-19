Precompiled Binaries of raspi_dng
=================================

This directory contains precompiled binaries of raspi_dng:

  - **raspi_dng.armv6l**: this a version compiled on a RPI with
    Raspbian wheezy running kernel 3.12.26. **Don't use the RPI
    version of raspi_dng, see note below.**
    
  - **raspi_dng.x64**: this is a version compiled on the 64-bit
    version of openSUSE 13.1 running kernel 3.11.10

Both versions are dynamically linked, so you might have to install
additional libraries to make the programs work.

**IMPORTANT NOTE**: raspi_dng does not work correctly on the RPI itself, since
the color-matrix is not stored correctly in the dng-file (negative values
are stored as zeros). This is due to a problem within the tiff-library.
So it is recommended to download the image-files to a PC and to convert the
files there. Help on this issue is highly welcome.

