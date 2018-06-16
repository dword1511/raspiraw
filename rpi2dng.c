/*
 * Read in JPEG from Raspberry Pi camera captured using 'raspistill --raw'
 * and extract RAW file with 10-bit values stored left-justified at 16 bpp
 * in Adobe DNG (TIFF-EP) format.
 *
 * Does not do any processing on the image data. That's the job of darktable,
 * etc.
 *
 * Data structure of Raspberry Pi's "RAW" JPEG:
 * https://picamera.readthedocs.io/en/release-1.13/recipes2.html?highlight=raw#
 * raw-bayer-data-captures
 *
 * Last modified by Chi Zhang (@dword1511)
 * With contributions from John Beale, Dave Coffin, Bryan See and others.
 * Thanks John Beale for RAW file samples.
 * Free for all uses.
 *
 * TODO: this code needs some serious clean-up (esp. varible names!).
 * TODO: merge rpitrunc
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


#define RPI_RAW_ID_LEN          4             /* ID length, the an additional "@" not counted */
#define RPI_RAW_MARKER          "@BRCM"       /* Marker + ID */
#define RPI_RAW_HDR_LEN         32768         /* RAW header length */
#define RPI_RAW_BIT_DEPTH       10            /* Always 10-bit packed for now, will need new unpacking procedure when 12-bit present */
#define RPI_RAW_MAX_MODEL_LEN   9

#define TIFF_CFA_R              0
#define TIFF_CFA_G              1
#define TIFF_CFA_B              2
#define TIFF_CFA_C              3
#define TIFF_CFA_M              4
#define TIFF_CFA_Y              5
#define TIFF_CFA_K              6             /* White (clear) pixel */

#define RPI_RAW_CFA_FLIP_NONE   0x00
#define RPI_RAW_CFA_FLIP_HORIZ  0x01
#define RPI_RAW_CFA_FLIP_VERT   0x02
#define RPI_RAW_CFA_FLIP_BOTH   (RPI_RAW_CFA_FLIP_HORIZ | RPI_RAW_CFA_FLIP_VERT)
#define RPI_RAW_CFA_PATT_NEW    {TIFF_CFA_G, TIFF_CFA_B, TIFF_CFA_R, TIFF_CFA_G}
#define RPI_RAW_CFA_PATT_OLD    {TIFF_CFA_B, TIFF_CFA_G, TIFF_CFA_G, TIFF_CFA_R}

#define DNG_SOFTWARE_ID         "rpi2dng @dword1511 fork"
#define DNG_VER                 "\001\001\0\0"
#define DNG_BACKWARD_VER        "\001\0\0\0"

/* NOTE: MIN(a, b) and MAX(a, b) already defined by <libexif/exif-data.h> */


typedef struct {
  uint16_t  width;
  uint16_t  height;
  uint16_t  row_len;
  size_t    raw_len;

  char      cfa_pattern[4];
  float     black_lvl[4];
  char      model[RPI_RAW_MAX_MODEL_LEN + 1];
} raw_fmt_t;


const raw_fmt_t fmt_ov5647_old = {
  .width        = 2592,
  .height       = 1944,
  .row_len      = 3264,     /* 8-pixel padding + other stuff, 24 bytes total */
  .raw_len      = 6404096,

  .cfa_pattern  = RPI_RAW_CFA_PATT_OLD,
  .black_lvl    = {12.0f, 12.0f, 12.0f, 12.0f},
  .model        = "ov5647",
};

const raw_fmt_t fmt_ov5647_new = {
  .width        = 2592,
  .height       = 1944,
  .row_len      = 3264,     /* 8-pixel padding + other stuff, 24 bytes total */
  .raw_len      = 6404096,

  .cfa_pattern  = RPI_RAW_CFA_PATT_NEW,
  .black_lvl    = {12.0f, 12.0f, 12.0f, 12.0f},
  .model        = "RP_ov5647",
};

const raw_fmt_t fmt_ov5647_new2 = {
  .width        = 2592,
  .height       = 1944,
  .row_len      = 3264,     /* 8-pixel padding + other stuff, 24 bytes total */
  .raw_len      = 6404096,

  .cfa_pattern  = RPI_RAW_CFA_PATT_NEW,
  .black_lvl    = {12.0f, 12.0f, 12.0f, 12.0f},
  .model        = "RP_OV5647",
};

const raw_fmt_t fmt_imx219 = {
  .width        = 3280,
  .height       = 2464,
  .row_len      = 4128,     /* 16-pixel padding + other stuff, 28 bytes total */
  .raw_len      = 10270208,

  .cfa_pattern  = RPI_RAW_CFA_PATT_NEW,
  .black_lvl    = {60.0f, 60.0f, 60.0f, 60.0f},
  .model        = "RP_imx219",
};

const raw_fmt_t *const supported_formats[] = {
  &fmt_ov5647_old,
  &fmt_ov5647_new,
  &fmt_ov5647_new2,
  &fmt_imx219,
  NULL
};


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

static float rational_to_float(void* p) {
  uint32_t* r;
  r = (uint32_t*)p;
  return be32toh(r[0]) * 1.0f / be32toh(r[1]);
}

static float srational_to_float(void* p) {
  int32_t* r;
  r = (int32_t*)p;
  return be32toh(r[0]) * 1.0f / be32toh(r[1]);
}

static void print_matrix(float matrix[9]) {
  fprintf(stderr, "Using color matrix:\n"
                    "\t%.4f\t%.4f\t%.4f\n"
                    "\t%.4f\t%.4f\t%.4f\n"
                    "\t%.4f\t%.4f\t%.4f\n",
        matrix[0], matrix[1], matrix[2],
        matrix[3], matrix[4], matrix[5],
        matrix[6], matrix[7], matrix[8]);
}

static int copy_tags(const ExifData* edata, TIFF* tif, const char* matrix, const char* filename, const raw_fmt_t* fmt, int pattern) {
  const long  white     = (1 << RPI_RAW_BIT_DEPTH) - 1;
  const short cfadim[]  = {2, 2}; /* libtiff5 only supports 2x2 CFA */
  ExifEntry*  eentry    = NULL;
  char        cfapatt[] = {TIFF_CFA_K, TIFF_CFA_K, TIFF_CFA_K, TIFF_CFA_K};
  struct tm*  tm        = NULL;
  time_t      rawtime;
  char        datetime[64];
  uint16_t    iso;
  float       gain[]    = {1.0, 1.0, 1.0}; /* Default */
  float       neutral[3];
  size_t      exif_dir_offset = 0;
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

  switch (pattern) {
    case RPI_RAW_CFA_FLIP_NONE: {
      cfapatt[0] = fmt->cfa_pattern[0];
      cfapatt[1] = fmt->cfa_pattern[1];
      cfapatt[2] = fmt->cfa_pattern[2];
      cfapatt[3] = fmt->cfa_pattern[3];
      break;
    }
    case RPI_RAW_CFA_FLIP_HORIZ: {
      cfapatt[0] = fmt->cfa_pattern[1];
      cfapatt[1] = fmt->cfa_pattern[0];
      cfapatt[2] = fmt->cfa_pattern[3];
      cfapatt[3] = fmt->cfa_pattern[2];
      break;
    }
    case RPI_RAW_CFA_FLIP_VERT: {
      cfapatt[0] = fmt->cfa_pattern[2];
      cfapatt[1] = fmt->cfa_pattern[3];
      cfapatt[2] = fmt->cfa_pattern[0];
      cfapatt[3] = fmt->cfa_pattern[1];
      break;
    }
    case RPI_RAW_CFA_FLIP_BOTH: {
      cfapatt[0] = fmt->cfa_pattern[3];
      cfapatt[1] = fmt->cfa_pattern[2];
      cfapatt[2] = fmt->cfa_pattern[1];
      cfapatt[3] = fmt->cfa_pattern[0];
      break;
    }
    default: {
      fprintf(stderr, "Internal error!\n");
      abort();
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

      /* This white balance stuff seems to work for IMX219, need more tests... */
      /* May need to multiply w/ color matrix */
      /*
      sscanf(strstr((const char *)eentry->data, "greenness=") + 10, "%f", &gain[1]);
      if (gain[1] < -80.0f) {
        fprintf(stderr, "WARN: greenness in MakerNotes extremely small (%.0f), truncated.\n", gain[1]);
        gain[1] = 0.2f;
      }
      gain[1] = 100.0f / (100.0f + gain[1]);

      //gain[1] = 0.5f;

      neutral[0] = gain[0];
      neutral[1] = gain[1];
      neutral[2] = gain[2];
      */

      neutral[0] = (1 / gain[0]) / ((1 / gain[0]) + (1 / gain[1]) + (1 / gain[2]));
      neutral[1] = (1 / gain[1]) / ((1 / gain[0]) + (1 / gain[1]) + (1 / gain[2]));
      neutral[2] = (1 / gain[2]) / ((1 / gain[0]) + (1 / gain[1]) + (1 / gain[2]));
    } else {
      fprintf(stderr, "JPEG does not contain MakerNotes! Will use default color matrix.\n");
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
  TIFFSetField(tif, TIFFTAG_ORIENTATION           , ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_SOFTWARE              , DNG_SOFTWARE_ID);
  TIFFSetField(tif, TIFFTAG_DNGVERSION            , DNG_VER);
  TIFFSetField(tif, TIFFTAG_DNGBACKWARDVERSION    , DNG_BACKWARD_VER);
  TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL     , fmt->model);
  TIFFSetField(tif, TIFFTAG_COLORMATRIX1          , 9, cam_xyz);
  TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL         , 3, neutral);
  TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, 21); /* D65 light source */
  TIFFSetField(tif, TIFFTAG_MAKERNOTESAFETY       , 1); /* Safe to copy MakerNote, see DNG standard */
  TIFFSetField(tif, TIFFTAG_SUBFILETYPE           , 0); /* Not reduced, not multi-page and not a mask */
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH            , fmt->width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH           , fmt->height);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE         , 16); /* uint16_t */
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC           , PHOTOMETRIC_CFA);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL       , 1);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG          , PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM   , cfadim);
  TIFFSetField(tif, TIFFTAG_CFAPATTERN            , cfapatt);
  TIFFSetField(tif, TIFFTAG_BLACKLEVEL            , 4, fmt->black_lvl);
  /* TIFFTAG_BLACKLEVELDELTAH and TIFFTAG_BLACKLEVELDELTAV should depend on ISO and exposure time... not calibrating them yet */
  //TIFFSetField(tif, TIFFTAG_LINEARIZATIONTABLE  , 256, curve);
  TIFFSetField(tif, TIFFTAG_WHITELEVEL            , 1, &white);
  TIFFSetField(tif, TIFFTAG_COMPRESSION           , COMPRESSION_NONE);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP          , 1); /* One row per strip */

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

static const raw_fmt_t* get_format(ExifData* edata) {
  const ExifEntry*          eentry  = NULL;
  const raw_fmt_t *const *  p_fmt   = supported_formats;

  if (NULL == edata) {
    fprintf(stderr, "Internal error!\n");
    abort();
  }

  eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_0], EXIF_TAG_MODEL);
  if (NULL == eentry) {
    fprintf(stderr, "EXIF IFD0 does not contain MODEL tag!\n");
    return NULL;
  }

  while (*p_fmt != NULL) {
    if (0 == strncmp((const char *)eentry->data, (*p_fmt)->model, MIN(eentry->size, RPI_RAW_MAX_MODEL_LEN))) {
      fprintf(stderr, "Model: %s\n", (*p_fmt)->model);
      break;
    }
    p_fmt ++;
  }

  if (p_fmt == NULL) {
    return NULL;
  }

  return *p_fmt;
}

static size_t get_data_offset(FILE* ifp, const raw_fmt_t* fmt) {
  size_t  offset, len;
  uint8_t buffer[16];

  /* Check file length */
  fseek(ifp, 0, SEEK_END);
  len = ftell(ifp);
  if (len <= (fmt->raw_len + 1 + 2)) {
    fprintf(stderr, "File too short to contain expected %zu-byte RAW data.\n", fmt->raw_len);
    return 0;
  }

  offset = (len - (fmt->raw_len + 1));
  fseek(ifp, offset - 2, SEEK_SET);
  fread(buffer, 16, 1, ifp);
  if ((buffer[0] != 0xff) || (buffer[1] != 0xd9)) {
    fprintf(stderr, "JPEG EOI not found (want 0xffd9, got 0x%02x%02x, offset %zu).\n", buffer[0], buffer[1], offset);
    return 0;
  }
  if (0 != strncmp((const char*)(buffer + 2), RPI_RAW_MARKER, strlen(RPI_RAW_MARKER))) {
    fprintf(stderr, "RAW marker not found.\n");
    return 0;
  }

  return (len - fmt->raw_len) + RPI_RAW_HDR_LEN;
}

static void process_file(char* inFile, char* outFile, char* matrix, int pattern) {
  size_t            row, offset;

  char*             dngFile = NULL;
  unsigned char*    buffer  = NULL; /* Row buffer, packed */
  uint16_t*         pixel   = NULL; /* Row buffer, unpacked */
  FILE*             ifp     = NULL;
  TIFF*             tif     = NULL;
  ExifData*         edata   = NULL;
  const raw_fmt_t*  fmt     = NULL;

  /* Check file existence */
  if (NULL == (ifp = fopen(inFile, "rb"))) {
    perror(inFile);
    goto fail;
  }

  /* Load and check EXIF-data */
  if (NULL == (edata = exif_data_new_from_file(inFile))) {
    fprintf(stderr, "No EXIF data found, hence no RAW data.\n");
    goto fail;
  }

  /* Determine format */
  if (NULL == (fmt = get_format(edata))) {
    fprintf(stderr, "File format unsupported.\n");
    goto fail;
  }

  /* Location in file the raw pixel data starts */
  offset = get_data_offset(ifp, fmt);
  if (0 == offset) {
    fprintf(stderr, "Cannot determine RAW data offset.\n");
    goto fail;
  }
  fprintf(stderr, "Found RAW data @ offset %lu.\n", offset);
  fseek(ifp, offset, SEEK_SET);

  /* Allocate memory for one line of pixel data */
  buffer = (unsigned char*) malloc(fmt->row_len + 1);
  pixel  = (uint16_t*) malloc(fmt->width * fmt->height * sizeof(pixel[0]));
  if ((NULL == buffer) || (pixel == NULL)) {
    fprintf(stderr, "Cannot allocate memory for image data!\n");
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
    fprintf(stderr, "Cannot create/open output file `%s'.\n", dngFile);
    goto fail;
  }
  fprintf(stderr, "Creating %s...\n", dngFile);

  /* Copy metadata */
  if (EXIT_SUCCESS != copy_tags(edata, tif, matrix, inFile, fmt, pattern)) {
    goto fail;
  }

  /* Unpack and copy RAW data */
  fprintf(stderr, "Extracting RAW data...\n");
  for (row = 0; row < fmt->height; row ++) {
    int j, col;
    fread(buffer, fmt->row_len, 1, ifp); /* Read next line of pixel data */
    j = 0; /* Offset into buffer */

    /* Iterate over pixel columns (4 pixel per 5 bytes) */
    for (col = 0; col < fmt->width; col += 4) {
      unsigned char     split; /* 5th byte, contains 4 pairs of low-order bits */

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

      /* Right adjust them */
      pixel[col + 0] >>= (16 - RPI_RAW_BIT_DEPTH);
      pixel[col + 1] >>= (16 - RPI_RAW_BIT_DEPTH);
      pixel[col + 2] >>= (16 - RPI_RAW_BIT_DEPTH);
      pixel[col + 3] >>= (16 - RPI_RAW_BIT_DEPTH);
    }

    if (TIFFWriteEncodedStrip(tif, row, pixel, fmt->width * 2) < 0) {
      fprintf(stderr, "Error writing TIFF stripe at row %zu.\n", row);
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

  if (NULL != pixel) {
    free(pixel);
  }

  if (NULL != dngFile) {
    free(dngFile);
  }

  return;
}

int main(int argc, char* argv[]) {
  char* matrix  = NULL;
  char* fout    = NULL;
  char* fname   = NULL;
  int   flip    = 0;
  int   opt;

  /* Scan options */
  while ((opt = getopt(argc, argv, ":HVM:o:")) != -1) {
    switch (opt) {
    case 'H': {
      flip |= RPI_RAW_CFA_FLIP_HORIZ;
      break;
    }
    case 'V': {
      flip |= RPI_RAW_CFA_FLIP_VERT;
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

  if (flip != 0) {
    fprintf(stderr, "NOTE: you have enabled flipping. A better way is to record as is, and then flip in the photo processing software, e.g. darktable.");
  }

  /* Scan file names */
  while (optind < argc) {
    fname = argv[optind ++];
    fprintf(stderr, "\n%s:\n", fname);
    process_file(fname, fout, matrix, flip);
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
