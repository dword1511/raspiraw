/* Read in jpeg from Raspberry Pi camera captured using 'raspistill --raw'
   and extract raw file with 10-bit values stored left-justified at 16 bpp
   in Adobe DNG (TIFF-EP) format, convert with 'ufraw out.dng' for example

   John Beale  26 May 2013
   and others
   
   Contains code written by Dave Coffin for Berkeley Engineering and Research.

   Free for all uses.

   Requires LibTIFF 3.8.0 plus a patch, see http://www.cybercom.net/~dcoffin/dcraw/
 */



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <tiffio.h>
#include <errno.h>
#include <libexif/exif-data.h>


#define LINELEN 256            // how long a string we need to hold the filename
#define RAWBLOCKSIZE 6404096
#define HEADERSIZE 32768
#define ROWSIZE 3264 // number of bytes per row of pixels, including 24 'other' bytes at end
#define IDSIZE 4    // number of bytes in raw header ID string
#define HPIXELS 2592   // number of horizontal pixels on OV5647 sensor
#define VPIXELS 1944   // number of vertical pixels on OV5647 sensor

void readMatrix(float[9],const char*);
void processFile(char* inFile, char* outFile, char* matrix);

int main (int argc, char **argv) {
  const char* fname = argv[1];

  if (argc < 3 || argc > 4) {
    fprintf (stderr, "Usage: %s infile outfile [color-matrix]\n"
	     "Example: %s rpi.jpg output.dng " 
	     "\"8032,-3478,-274,-1222,5560,-240,100,-2714,6716\"\n",
	     argv[0], argv[0]);
    return 1;
  }

  if (argc == 3) {
    processFile(argv[1],argv[2],NULL);
  } else {
    processFile(argv[1],argv[2],argv[3]);
  }
}

// process single file   -----------------------------------------------------

void processFile(char* inFile, char* outFile, char* matrix) {
  static const short CFARepeatPatternDim[] = { 2,2 };

  // Bayer patterns (0 = Red, 1 = Green, 2 = Blue)
  static char* CFA_PATTERN_N  = "\001\002\0\001";  // GBRG
  static char* CFA_PATTERN_HF = "\002\001\001\0";  // BGGR
  char* cfaPattern;

  // default color matrix from dcraw
  float cam_xyz[] = {
    //  R        G        B
    1.2782,	-0.4059, -0.0379, // R
    -0.0478,	 0.9066,  0.1413, // G
    0.1340,	 0.1513,  0.5176  // B
  };
  float neutral[] = { 1.0, 1.0, 1.0 }; // TODO calibrate
  long sub_offset=0, white=0xffff;

  int i, j, row, col;
  unsigned short curve[256];
  struct stat st;
  struct tm tm;
  char datetime[64];
  FILE *ifp;
  TIFF *tif;

  unsigned long fileLen;  // number of bytes in file
  unsigned long offset;  // offset into file to start reading pixel data
  unsigned char *buffer;
  unsigned short pixel[HPIXELS];  // array holds 16 bits per pixel
  unsigned char split;        // single byte with 4 pairs of low-order bits

  // check if input-file exists
  if (!(ifp = fopen (inFile, "rb"))) {
    perror(inFile);
    return;
  }

  // process EXIF-data
  ExifData* edata = exif_data_new_from_file(inFile);
  ExifEntry* eentry = NULL;
  if (edata) {
    eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_0],EXIF_TAG_MODEL);
    if (!strncmp(eentry->data,"ov5647",6)) {
      // old version uses current horizontal-flip readout
      cfaPattern = CFA_PATTERN_HF;
    } else {
      // assume normal readout
      cfaPattern = CFA_PATTERN_N;
    }
  }

  // read color-matrix if passed as third argument
  if (matrix != NULL) {
    readMatrix(cam_xyz,matrix);
  } else if (edata) {
    eentry = exif_content_get_entry(edata->ifd[EXIF_IFD_EXIF],0x927c);
    readMatrix(cam_xyz,strstr(eentry->data,"ccm=")+4);
  }

  if (edata) {
    exif_data_unref(edata);
  }

  // create dng
  stat (inFile, &st);
  gmtime_r (&st.st_mtime, &tm);
  sprintf (datetime, "%04d:%02d:%02d %02d:%02d:%02d",
	   tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec);

  //Get file length
  fseek(ifp, 0, SEEK_END);
  fileLen=ftell(ifp);
  if (fileLen < RAWBLOCKSIZE) {
    fprintf(stderr, "File %s too short to contain expected 6MB RAW data.\n", inFile);
    exit(1);
  }
  offset = (fileLen - RAWBLOCKSIZE) ;  // location in file the raw header starts
  fseek(ifp, offset, SEEK_SET); 
 
  //printf("File length = %d bytes.\n",fileLen);
  //printf("offset = %d:",offset);

  //Allocate memory for one line of pixel data
  buffer=(unsigned char *)malloc(ROWSIZE+1);
  if (!buffer)
    {
      fprintf(stderr, "Memory error!");
      goto fail;
    }
		
  if (!(tif = TIFFOpen (outFile, "w"))) goto fail;

  //fprintf(stderr, "Writing TIFF header...\n");
	
  TIFFSetField (tif, TIFFTAG_SUBFILETYPE, 1);
  TIFFSetField (tif, TIFFTAG_IMAGEWIDTH, HPIXELS >> 4);
  TIFFSetField (tif, TIFFTAG_IMAGELENGTH, VPIXELS >> 4);
  TIFFSetField (tif, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField (tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  TIFFSetField (tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField (tif, TIFFTAG_MAKE, "Raspberry Pi");
  TIFFSetField (tif, TIFFTAG_MODEL, "Model OV5647");
  TIFFSetField (tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField (tif, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField (tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField (tif, TIFFTAG_SOFTWARE, "raspi_dng");
  TIFFSetField (tif, TIFFTAG_DATETIME, datetime);
  TIFFSetField (tif, TIFFTAG_SUBIFD, 1, &sub_offset);
  TIFFSetField (tif, TIFFTAG_DNGVERSION, "\001\001\0\0");
  TIFFSetField (tif, TIFFTAG_DNGBACKWARDVERSION, "\001\0\0\0");
  TIFFSetField (tif, TIFFTAG_UNIQUECAMERAMODEL, "Raspberry Pi - OV5647");
  TIFFSetField (tif, TIFFTAG_COLORMATRIX1, 9, cam_xyz);
  TIFFSetField (tif, TIFFTAG_ASSHOTNEUTRAL, 3, neutral);
  TIFFSetField (tif, TIFFTAG_CALIBRATIONILLUMINANT1, 21);
  TIFFSetField (tif, TIFFTAG_ORIGINALRAWFILENAME, inFile);

  // fprintf(stderr, "Writing TIFF thumbnail...\n");
  memset (pixel, 0, HPIXELS);	// all-black thumbnail 
  for (row=0; row < VPIXELS >> 4; row++)
    TIFFWriteScanline (tif, pixel, row, 0);
  TIFFWriteDirectory (tif);

  // fprintf(stderr, "Writing TIFF header for main image...\n");

  TIFFSetField (tif, TIFFTAG_SUBFILETYPE, 0);
  TIFFSetField (tif, TIFFTAG_IMAGEWIDTH, HPIXELS);
  TIFFSetField (tif, TIFFTAG_IMAGELENGTH, VPIXELS);
  TIFFSetField (tif, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField (tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
  TIFFSetField (tif, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField (tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField (tif, TIFFTAG_CFAREPEATPATTERNDIM, CFARepeatPatternDim);
  TIFFSetField (tif, TIFFTAG_CFAPATTERN, 4, cfaPattern);
  //TIFFSetField (tif, TIFFTAG_LINEARIZATIONTABLE, 256, curve);
  TIFFSetField (tif, TIFFTAG_WHITELEVEL, 1, &white);

  fprintf(stderr, "Processing RAW data...\n");
  // for one file, (TotalFileLength:11112983 - RawBlockSize:6404096) + Header:32768 = 4741655
  // The pixel data is arranged in the file in rows, with 3264 bytes per row.
  // with 3264 bytes per row x 1944 rows we have 6345216 bytes, that is the full 2592x1944 image area.
 
  //Read one line of pixel data into buffer
  fread(buffer, IDSIZE, 1, ifp);

  // now on to the pixel data
  offset = (fileLen - RAWBLOCKSIZE) + HEADERSIZE;  // location in file the raw pixel data starts
  fseek(ifp, offset, SEEK_SET);

  for (row=0; row < VPIXELS; row++) {  // iterate over pixel rows
    fread(buffer, ROWSIZE, 1, ifp);  // read next line of pixel data
    j = 0;  // offset into buffer
    for (col = 0; col < HPIXELS; col+= 4) {  // iterate over pixel columns
      pixel[col+0] = buffer[j++] << 8;
      pixel[col+1] = buffer[j++] << 8;
      pixel[col+2] = buffer[j++] << 8;
      pixel[col+3] = buffer[j++] << 8;
      split = buffer[j++];    // low-order packed bits from previous 4 pixels
      pixel[col+0] += (split & 0b11000000);  // unpack them bits, add to 16-bit values, left-justified
      pixel[col+1] += (split & 0b00110000)<<2;
      pixel[col+2] += (split & 0b00001100)<<4;
      pixel[col+3] += (split & 0b00000011)<<6;
    }
    if (TIFFWriteScanline (tif, pixel, row, 0) != 1) {
      fprintf(stderr, "Error writing TIFF scanline.");
      exit(1);
    }
  } // end for(k..)

  free(buffer); // free up that memory we allocated

  TIFFClose (tif);
 fail:
  fclose (ifp);
  return;
}

// parse color-matrix from command-line   ------------------------------------

void readMatrix(float* matrix,const char* arg) {
  sscanf(arg,"%f, %f, %f, "
             "%f, %f, %f, "
             "%f, %f, %f, ",
        &matrix[0], &matrix[1], &matrix[2], 
        &matrix[3], &matrix[4], &matrix[5], 
        &matrix[6], &matrix[7], &matrix[8]);

  // scale result if input is not normalized
  if (matrix[0] > 10) {
    int i;
    for (i=0; i<9; ++i) {
      matrix[i] /= 10000;
    }
  }
}
