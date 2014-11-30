#!/bin/bash
# ------------------------------------------------------------------------------
# Simple shell-wrapper to pass the embedded color-matrix to the
# raspi_dng converter.
#
# Actually raspi_dng should do this and hopefully will do so in a future
# version.
#
# Copyright: Bernhard Bablok (bablokb at gmx dot de)
# License:   GPL3
#
# ------------------------------------------------------------------------------

infile="$1"
outfile="${1%.jpg}.dng"

matrix=`exiftool -b -makernoteunknowntext "$infile" | \
           sed -e 's/.*ccm=\([^ ]*\) .*/\1/' | \
              cut -d',' -f 1-9`

if [ -f "raspi_dng" ]; then
  # raspi_dng is in current directory
  ./raspi_dng "$infile" "$outfile" "$matrix"
else
  # assume it is in the path
  raspi_dng "$infile" "$outfile" "$matrix"
fi
