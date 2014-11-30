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

exifinfo=`exiftool -b -makernoteunknowntext "$infile"`

matrix=`sed -e 's/.*ccm=\([^ ]*\) .*/\1/' <<< "$exifinfo" | \
              cut -d',' -f 1-9`
gain_r=`sed -e 's/.*gain_r=\([^ ]*\) .*/\1/' <<< "$exifinfo"`
gain_b=`sed -e 's/.*gain_b=\([^ ]*\) .*/\1/' <<< "$exifinfo"`


if [ -f "raspi_dng" ]; then
  # raspi_dng is in current directory
  ./raspi_dng "$infile" "$outfile" "$matrix" "$gain_r,$gain_b"
else
  # assume it is in the path
  raspi_dng "$infile" "$outfile" "$matrix"
fi

# copy exif-information, reset date/time information
exiftool -overwrite_original -tagsFromFile "$infile" "$outfile"
touch -r "$infile" "$outfile"
