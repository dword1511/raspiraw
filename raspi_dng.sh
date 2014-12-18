#!/bin/bash
# ------------------------------------------------------------------------------
# Shell-wrapper for the raspi_dng converter.
#
# Copyright: Bernhard Bablok (bablokb at gmx dot de)
# License:   GPL3
#
# ------------------------------------------------------------------------------

# --- usage   ------------------------------------------------------------------

usage() {
  echo -e "\n`basename $0`: convert raw-image from Raspberry Pi camera module\n\
  \nusage: `basename $0` [options] input-file\n\
  possible options:\n\
    -M matrix use given matrix instead of embedded color matrix\n\
    -g        use embedded white-balance gains (experimental, use with -c)\n\
    -c        convert from jpg to dng to tif with dcraw\n\
    -D        delete intermediate dng file (only needed with -c)\n\
    -h        show this help\n\
" >&2
  exit 3
}

# --- set defaults   -----------------------------------------------------------

setDefaults() {
  matrix=""
  useGains=0
  useDcraw=0
  keepDNG=1
  DCRAW_ARGS="-v -f -o 1 -q 3 -T -6"
}

# --- parse arguments   --------------------------------------------------------

parseArguments() {
  while getopts ":M:gcDh" opt; do
    case $opt in
      M) matrix="$OPTARG";;
      g) useGains=1;;
      c) useDcraw=1;;
      D) [ $useDcraw -eq 1 ] && keepDNG=0;;
      h) usage;;
      ?) echo "[error] illegal option: $OPTARG" >&2; usage;;
    esac
  done

  shift $((OPTIND-1))
  infile="$1"
  dngfile="${infile/.jpg/.dng}"
  tiffile="${infile/.jpg/.tif}"
}

# --- check arguments   --------------------------------------------------------

checkArguments() {
  if [ -z "$infile" ]; then
    echo -e "[error] Missing input-file" >&2
    usage
  fi
  if [ ! -f "$infile" ]; then
    echo -e "[error] Input-file does not exist" >&2
    exit 3
  fi
}

# --- extract exifinfo from makernotes   ---------------------------------------

extractExifInfo() {
  local exifinfo=`exiftool -b -makernoteunknowntext "$infile"`

  # whitebalance gains
  [ $useGains -eq 0 ] && return
  gain_r=`sed -e 's/.*gain_r=\([^ ]*\) .*/\1/' <<< "$exifinfo"`
  gain_b=`sed -e 's/.*gain_b=\([^ ]*\) .*/\1/' <<< "$exifinfo"`
  echo -e "[info] white-balance gains:: red: $gain_r, blue: $gain_b" >&2
}

# --- convert image   ----------------------------------------------------------

convertImage() {
  PATH=".:$PATH"
  echo -e "[info] creating file $dngfile" >&2
  raspi_dng "$infile" "$dngfile" ${matrix:+"$matrix"}
  exiftool -q -overwrite_original -tagsFromFile "$infile" "$dngfile"
  dcraw -z "$dngfile"

  if [ $useDcraw -eq 1 ]; then
    echo -e "[info] creating file $tiffile" >&2
    if [ $useGains -eq 1 ]; then
      gains="-r $gain_r 1 $gain_b 1"
    else
      gains="-a"
    fi
    dcraw $DCRAW_ARGS $gains -c "$dngfile" > "$tiffile"
    exiftool -q -overwrite_original -tagsFromFile "$infile" "$tiffile"
    dcraw -z "$tiffile"
    [ $keepDNG -eq 0 ] && rm "$dngfile"
  fi
}

# --- main program   -----------------------------------------------------------

setDefaults
parseArguments "$@"
checkArguments
[ $useGains -eq 1 ] && extractExifInfo
convertImage
