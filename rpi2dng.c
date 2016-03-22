/* Read in JPEG from Raspberry Pi camera captured using 'raspistill --raw'
   and extract RAW file with 10-bit values stored left-justified at 16 bpp
   in Adobe DNG (TIFF-EP) format, convert with 'ufraw out.dng' for example

   John Beale  26 May 2013
   and others

   Contains code written by Dave Coffin for Berkeley Engineering and Research.

   Free for all uses.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <tiffio.h>
#include <errno.h>
#include <libexif/exif-data.h>
#include <unistd.h>
#include <endian.h>

/* Raspberry Pi OV5647 5MP JPEG RAW image format */
#define RPI_OV5647_RAW_HPIXELS 2592    /* number of horizontal pixels on OV5647 sensor */
#define RPI_OV5647_RAW_VPIXELS 1944    /* number of vertical pixels on OV5647 sensor   */
#define RPI_OV5647_RAW_ID_LEN  4       /* number of bytes in raw header ID string      */
#define RPI_OV5647_RAW_HDR_LEN 32768
#define RPI_OV5647_RAW_ROW_LEN 3264    /* number of bytes per row of pixels, including 24 'other' bytes at end */
#define RPI_OV5647_RAW_RAW_LEN 6404096
#define RPI_OV5647_BIT_DEPTH   10
#define RPI_OV5647_TAG_OLD     "ov5647"

/* CFA arrangement for different flip configuration (0 = Red, 1 = Green, 2 = Blue) */
#define CFA_PATTERN_NORMAL     "\001\002\0\001"
#define CFA_PATTERN_FLIP_HORIZ "\002\001\001\0"
#define CFA_PATTERN_FLIP_VERT  "\0\001\001\002"
#define CFA_PATTERN_FLIP_BOTH  "\001\0\002\001"
#define CFA_FLIP_NONE          0x00
#define CFA_FLIP_HORIZ         0x01
#define CFA_FLIP_VERT          0x02
#define CFA_FLIP_BOTH          (CFA_FLIP_HORIZ | CFA_FLIP_VERT)

static void usage(const char* self) {
  fprintf (stderr, "Usage: %s [options] infile1.jpg [infile2.jpg ...]\n\n"
    "Options:\n"
      "\t-H          Assume horizontal flip (option -HF of raspistill)\n"
      "\t-V          Assume vertical flip (option -VF of raspistill)\n"
      "\t-o outfile  Create `outfile' instead of infile with dng-extension (unless multiple file supplied)\n"
      "\t-M matrix   Use given color matrix instead of embedded one for conversion\n",
    self);
  exit(EXIT_FAILURE);
}

static void read_matrix(float* matrix, const char* arg) {
  float mmax = 0;
  int   i;

  sscanf(arg, "%f, %f, %f, "
              "%f, %f, %f, "
              "%f, %f, %f, ",
        &matrix[0], &matrix[1], &matrix[2],
        &matrix[3], &matrix[4], &matrix[5],
        &matrix[6], &matrix[7], &matrix[8]);

  /* scale result if input is not normalized */
  for (i = 0; i < 9; i ++) {
    mmax = matrix[i] > mmax ? matrix[i] : mmax;
  }
  if (mmax > 1.0f) {
    for (i = 0; i < 9; i ++) {
      matrix[i] /= mmax;
    }
  }
}

static float rational_to_float(void *p) {
  uint32_t* r;
  r = (uint32_t*)p;
  return be32toh(r[0]) * 1.0f / be32toh(r[1]);
}

static float srational_to_float(void *p) {
  int32_t* r;
  r = (int32_t*)p;
  return be32toh(r[0]) * 1.0f / be32toh(r[1]);
}

static void print_matrix(float* matrix) {
  fprintf(stderr, "Using color matrix:\n"
                    "\t%.4f\t%.4f\t%.4f\n"
                    "\t%.4f\t%.4f\t%.4f\n"
                    "\t%.4f\t%.4f\t%.4f\n",
        matrix[0], matrix[1], matrix[2],
        matrix[3], matrix[4], matrix[5],
        matrix[6], matrix[7], matrix[8]);
}

static int copy_tags(ExifData* edata, TIFF* tif, char* matrix, char* filename, int pattern) {
  const long  white    = (1 << RPI_OV5647_BIT_DEPTH) - 1;
  const short cfadim[] = {2, 2}; /* libtiff5 only supports 2x2 CFA */
  const char* software = "rpi2dng (fork, https://github.com/dword1511/raspiraw)";
  const char* uniquem  = "Raspberry Pi - OV5647";
  const char* dngv     = "\001\001\0\0";
  const char* dngbv    = "\001\0\0\0";
  ExifEntry*  eentry   = NULL;
  char*       cfapatt  = NULL;
  struct tm*  tm       = NULL;
  time_t      rawtime;
  char        datetime[64];
  uint16_t    iso;
  float       gain[]   = {1.0, 1.0, 1.0}; /* Default */
  float       neutral[3];
  uint64_t exif_dir_offset = 0;
  //unsigned short curve[256];
  /* Default color matrix from dcraw */
  float cam_xyz[]  = {
    /*  R        G        B         */
     1.2782, -0.4059, -0.0379, /* R */
    -0.0478,  0.9066,  0.1413, /* G */
     0.1340,  0.1513,  0.5176  /* B */
  };

  /* ExifData, TIFF context, Color matrix buffer and Sub-IFD offset buffer are required. */
  /* Color matrix preset and Original file name are optional. */
  if ((NULL == edata) || (NULL == tif)) {
    fprintf(stderr, "Internal error!\n");
    abort();
  }

  /* New and old formats have different CFA arrangements */
  eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_0], EXIF_TAG_MODEL);
  if (NULL == eentry) {
    fprintf(stderr, "EXIF IFD0 does not contain MODEL tag!");
    return EXIT_FAILURE;
  }
  if (0 == strncmp((const char *)eentry->data, RPI_OV5647_TAG_OLD, strlen(RPI_OV5647_TAG_OLD) + 1)) {
    /* Old version uses current horizontal-flip readout */
    fprintf(stderr, "Image is in old format\n");
    switch (pattern) {
      case CFA_FLIP_NONE: {
        cfapatt = CFA_PATTERN_FLIP_HORIZ;
        break;
      }
      case CFA_FLIP_HORIZ: {
        cfapatt = CFA_PATTERN_NORMAL;
        break;
      }
      case CFA_FLIP_VERT: {
        cfapatt = CFA_PATTERN_FLIP_BOTH;
        break;
      }
      case CFA_FLIP_BOTH: {
        cfapatt = CFA_PATTERN_FLIP_VERT;
        break;
      }
      default: {
        fprintf(stderr, "Internal error!\n");
        abort();
      }
    }
  } else {
    fprintf(stderr, "Image is in new format\n");
    switch (pattern) {
      case CFA_FLIP_NONE: {
        cfapatt = CFA_PATTERN_NORMAL;
        break;
      }
      case CFA_FLIP_HORIZ: {
        cfapatt = CFA_PATTERN_FLIP_HORIZ;
        break;
      }
      case CFA_FLIP_VERT: {
        cfapatt = CFA_PATTERN_FLIP_VERT;
        break;
      }
      case CFA_FLIP_BOTH: {
        cfapatt = CFA_PATTERN_FLIP_BOTH;
        break;
      }
      default: {
        fprintf(stderr, "Internal error!\n");
        abort();
      }
    }
  }

  /* Load color matrix and white balance */
  if (NULL != matrix) {
    read_matrix(cam_xyz, matrix);
  } else {
    if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_MAKER_NOTE))) {
      read_matrix(cam_xyz, strstr((const char *)eentry->data, "ccm=") + 4);
      sscanf(strstr((const char *)eentry->data, "gain_r=") + 7, "%f", &gain[0]);
      sscanf(strstr((const char *)eentry->data, "gain_b=") + 7, "%f", &gain[2]);
      neutral[0] = (1 / gain[0]) / ((1 / gain[0]) + (1 / gain[1]) + (1 / gain[2]));
      neutral[1] = (1 / gain[1]) / ((1 / gain[0]) + (1 / gain[1]) + (1 / gain[2]));
      neutral[2] = (1 / gain[2]) / ((1 / gain[0]) + (1 / gain[1]) + (1 / gain[2]));
    } else {
      fprintf(stderr, "JPEG does not contain MakerNotes! Will use default color matrix.");
    }
  }
  print_matrix(cam_xyz);

  /* Write TIFF tags for DNG */
  /* IFD0 */
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_0], EXIF_TAG_MAKE))) {
    TIFFSetField(tif, TIFFTAG_MAKE, eentry->data);
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_0], EXIF_TAG_MODEL))) {
    TIFFSetField(tif, TIFFTAG_MODEL, eentry->data);
  }
  /* Skipped: XResolution (72 = Unkown) */
  /* Skipped: YResolution (72 = Unkown) */
  /* Skipped: ResolutionUnit */
  /* Skipped: Modify date */
  /* Skipped: YCbCrPositioning (for JPEG only) */
  /* Skipped: ExifOffset */
  /* Addons for DNG */
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_SOFTWARE, software);
  TIFFSetField(tif, TIFFTAG_DNGVERSION, dngv);
  TIFFSetField(tif, TIFFTAG_DNGBACKWARDVERSION, dngbv);
  TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL, uniquem);
  TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, cam_xyz);
  TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, neutral);
  TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, 21); /* D65 light source */
  TIFFSetField(tif, TIFFTAG_MAKERNOTESAFETY, 1); /* Safe to copy MakerNote, see DNG standard */
  TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0); /* Not reduced, not multi-page and not a mask */
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, RPI_OV5647_RAW_HPIXELS);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, RPI_OV5647_RAW_VPIXELS);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16); /* uint16_t */
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, cfadim);
  TIFFSetField(tif, TIFFTAG_CFAPATTERN, cfapatt);
  //TIFFSetField(tif, TIFFTAG_LINEARIZATIONTABLE, 256, curve);
  TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &white);
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 1); /* One row per strip */

  if (NULL != filename) {
    TIFFSetField(tif, TIFFTAG_ORIGINALRAWFILENAME, strlen(filename), filename);
  }
  time(&rawtime);
  tm = localtime(&rawtime);
  snprintf(datetime, 64, "%04d:%02d:%02d %02d:%02d:%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
  TIFFSetField(tif, TIFFTAG_DATETIME, datetime); /* Creation time (for DNG) */

  /* Save IFD0 continue */
  TIFFSetField(tif, TIFFTAG_EXIFIFD, exif_dir_offset);
  TIFFCheckpointDirectory(tif);
  TIFFSetDirectory(tif, 0);

  /* Copy EXIF information */
  /* ExifIFD */
  if (EXIT_SUCCESS != TIFFCreateEXIFDirectory(tif)) {
    fprintf(stderr, "Failed to create EXIF directory!\n");
    return EXIT_FAILURE;
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_EXPOSURE_TIME))) {
    TIFFSetField(tif, EXIFTAG_EXPOSURETIME, rational_to_float(eentry->data));
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_FNUMBER))) {
    TIFFSetField(tif, EXIFTAG_FNUMBER, rational_to_float(eentry->data));
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_EXPOSURE_PROGRAM))) {
    TIFFSetField(tif, EXIFTAG_EXPOSUREPROGRAM, be16toh(*((uint16_t *)eentry->data)));
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_ISO_SPEED_RATINGS))) {
    iso = be16toh(*((uint16_t *)eentry->data));
    TIFFSetField(tif, EXIFTAG_ISOSPEEDRATINGS, 1, &iso);
  }
  /* Skipped: ExifVersion */
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_ORIGINAL))) {
    TIFFSetField(tif, EXIFTAG_DATETIMEORIGINAL, eentry->data);
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_DIGITIZED))) {
    TIFFSetField(tif, EXIFTAG_DATETIMEDIGITIZED, eentry->data);
  }
  /* Skipped: ComponentsConfiguration (for JPEG only) */
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_SHUTTER_SPEED_VALUE))) {
    TIFFSetField(tif, EXIFTAG_SHUTTERSPEEDVALUE, srational_to_float(eentry->data));
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_APERTURE_VALUE))) {
    /* For original lens only */
    TIFFSetField(tif, EXIFTAG_APERTUREVALUE, rational_to_float(eentry->data));
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_BRIGHTNESS_VALUE))) {
    TIFFSetField(tif, EXIFTAG_BRIGHTNESSVALUE, srational_to_float(eentry->data));
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_MAX_APERTURE_VALUE))) {
    /* For original lens only */
    TIFFSetField(tif, EXIFTAG_MAXAPERTUREVALUE, rational_to_float(eentry->data));
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_METERING_MODE))) {
    TIFFSetField(tif, EXIFTAG_METERINGMODE, be16toh(*((uint16_t *)eentry->data)));
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_FLASH))) {
    TIFFSetField(tif, EXIFTAG_FLASH, be16toh(*((uint16_t *)eentry->data)));
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_FOCAL_LENGTH))) {
    /* For original lens only */
    TIFFSetField(tif, EXIFTAG_FOCALLENGTH, rational_to_float(eentry->data));
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_MAKER_NOTE))) {
    TIFFSetField(tif, EXIFTAG_MAKERNOTE, eentry->size, eentry->data);
    /* Various information can be extracted from the maker note. */
    /* Already handled: exp (ExposureTime), ccm (ColorMatrix) */
    /* ag, gain_r, gain_b, greenness, tg, f ar changing. */
    /* Additional: ISP version */
    //sscanf(strstr((const char *)eentry->data, "ev=") + 3, "%f", &ev);
    //TIFFSetField(tif, EXIFTAG_EXPOSUREBIASVALUE, ev);
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_FLASH_PIX_VERSION))) {
    TIFFSetField(tif, EXIFTAG_FLASHPIXVERSION, eentry->data);
  }
  /* Skipped: ColorSpace (for JPEG only) */
  /* Skipped: ExifImageWidth (for JPEG only) */
  /* Skipped: ExifImageHeight (for JPEG only) */
  /* Skipped: InteropOffset */
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_EXPOSURE_MODE))) {
    TIFFSetField(tif, EXIFTAG_EXPOSUREMODE, be16toh(*((uint16_t *)eentry->data)));
  }
  if (NULL != (eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF], EXIF_TAG_WHITE_BALANCE))) {
    TIFFSetField(tif, EXIFTAG_WHITEBALANCE, be16toh(*((uint16_t *)eentry->data)));
  }

  /* Patch EXIF IFD in */
  TIFFWriteCustomDirectory(tif, &exif_dir_offset);
  TIFFSetDirectory(tif, 0);
  TIFFSetField(tif, TIFFTAG_EXIFIFD, exif_dir_offset);
  TIFFCheckpointDirectory(tif);

  /* InteropIFD */
  /* Skipped: InteropIndex */
  /* IFD1 (Thumbnail data) */

  return EXIT_SUCCESS;
}

static void process_file(char* inFile, char* outFile, char* matrix, int pattern) {

  int i, j, row, col;
  char*     dngFile = NULL;
  FILE*     ifp     = NULL;
  TIFF*     tif     = NULL;
  ExifData* edata   = NULL;

  unsigned long  fileLen;  // number of bytes in file
  unsigned long  offset;  // offset into file to start reading pixel data
  unsigned char* buffer = NULL;
  uint16_t       pixel[RPI_OV5647_RAW_HPIXELS];  // array holds 16 bits per pixel
  unsigned char  split;        // single byte with 4 pairs of low-order bits

  /* Check file existence */
  if (NULL == (ifp = fopen(inFile, "rb"))) {
    perror(inFile);
    return;
  }

  /* Check file length */
  fseek(ifp, 0, SEEK_END);
  fileLen = ftell(ifp);
  if (fileLen < RPI_OV5647_RAW_RAW_LEN) {
    fprintf(stderr, "File %s too short to contain expected 6MB RAW data.\n", inFile);
    goto fail;
  }

  /* Load and check EXIF-data */
  if (NULL == (edata = exif_data_new_from_file(inFile))) {
    fprintf(stderr, "File %s contains no EXIF-data (and therefore no raw-data)\n", inFile);
    goto fail;
  }

  /* Generate DNG file name */
  if (NULL == outFile) {
    dngFile = strdup(inFile);
    strcpy(dngFile + strlen(dngFile) - 3, "dng"); /* TODO: ad-hoc, fix this */
  } else {
    dngFile = strdup(outFile);
  }

  /* Create output TIFF file */
  if (NULL == (tif = TIFFOpen(dngFile, "w"))) {
    goto fail;
  }
  fprintf(stderr, "Creating %s...\n", dngFile);

  /* Copy metadata */
  if (EXIT_SUCCESS != copy_tags(edata, tif, matrix, inFile, pattern)) {
    goto fail;
  }

  /* Allocate memory for one line of pixel data */
  buffer = (unsigned char *)malloc(RPI_OV5647_RAW_ROW_LEN + 1);
  if (NULL == buffer) {
    fprintf(stderr, "Cannot allocate memory for image data!\n");
    goto fail;
  }

  /* Unpack and copy RAW data */
  /* For one file, (TotalFileLength:11112983 - RawBlockSize:6404096) + Header:32768 = 4741655
   * The pixel data is arranged in the file in rows, with 3264 bytes per row.
   * with 3264 bytes per row x 1944 rows we have 6345216 bytes, that is the full 2592x1944 image area.
   */
  fprintf(stderr, "Extracting RAW data...\n");

  /* Now on to the pixel data */
  /* Location in file the raw pixel data starts */
  offset = (fileLen - RPI_OV5647_RAW_RAW_LEN) + RPI_OV5647_RAW_HDR_LEN;
  fseek(ifp, offset, SEEK_SET);

  /* Iterate over pixel rows */
  for (row = 0; row < RPI_OV5647_RAW_VPIXELS; row ++) {
    fread(buffer, RPI_OV5647_RAW_ROW_LEN, 1, ifp); /* Read next line of pixel data */
    j = 0; /* Offset into buffer */

    /* Iterate over pixel columns (4 pixel per 5 bytes) */
    for (col = 0; col < RPI_OV5647_RAW_HPIXELS; col += 4) {
      pixel[col + 0]  = buffer[j ++] << 8;
      pixel[col + 1]  = buffer[j ++] << 8;
      pixel[col + 2]  = buffer[j ++] << 8;
      pixel[col + 3]  = buffer[j ++] << 8;
      /* Low-order packed bits from previous 4 pixels */
      split           = buffer[j ++];
      /* Unpack the bits, add to 16-bit values, left-justified */
      pixel[col + 0] += (split & 0b11000000);
      pixel[col + 1] += (split & 0b00110000) << 2;
      pixel[col + 2] += (split & 0b00001100) << 4;
      pixel[col + 3] += (split & 0b00000011) << 6;

      /* Right adjust them... TODO: merge with previous steps */
      pixel[col + 0] >>= (16 - RPI_OV5647_BIT_DEPTH);
      pixel[col + 1] >>= (16 - RPI_OV5647_BIT_DEPTH);
      pixel[col + 2] >>= (16 - RPI_OV5647_BIT_DEPTH);
      pixel[col + 3] >>= (16 - RPI_OV5647_BIT_DEPTH);
    }

    if (TIFFWriteEncodedStrip(tif, row, pixel, RPI_OV5647_RAW_HPIXELS * 2) < 0) {
      fprintf(stderr, "Error writing TIFF stripe at row %d.\n", row);
      goto fail;
    }
  }

  TIFFWriteDirectory(tif);

fail:
  if (NULL != tif) {
    TIFFClose(tif);
  }

  if (NULL != edata) {
    exif_data_unref(edata);
  }

  if (NULL != ifp) {
    fclose(ifp);
  }

  if (NULL != buffer) {
    free(buffer);
  }

  if (NULL != dngFile) {
    free(dngFile);
  }

  return;
}

int main(int argc, char* argv[]) {
  char* matrix = NULL;
  char* fout   = NULL;
  char* fname  = NULL;
  int pattern=0;
  int opt;

  /* Scan options */
  while ((opt = getopt(argc, argv, ":HVM:o:")) != -1) {
    switch (opt) {
    case 'H': {
      pattern |= CFA_FLIP_HORIZ;
      break;
    }
    case 'V': {
      pattern |= CFA_FLIP_VERT;
      break;
    }
    case 'M': {
      matrix   = strdup(optarg);
      break;
    }
    case 'o': {
      fout     = strdup(optarg);
      break;
    }
    default: /* '?' */
      usage(argv[0]);
    }
  }

  /* Expect at least one input-filename */
  if (optind >= argc) {
    usage(argv[0]);
  }

  /* Prevent user from setting output file name when multiple files are supplied */
  if ((optind < argc - 1) && (fout != NULL)) {
    usage(argv[0]);
  }

  /* Scan file names */
  while (optind < argc) {
    fname = argv[optind ++];
    fprintf(stderr, "\n%s:\n", fname);
    process_file(fname, fout, matrix, pattern);
  }

  /* Clean up */
  if (NULL != matrix) {
    free(matrix);
  }
  if (NULL != fout) {
    free(fout);
  }

  return EXIT_SUCCESS;
}
