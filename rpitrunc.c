/* Removes padded RAW data from JPEG. Use with caution. Currently for OV5647 only. */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RPI_OV5647_RAW_RAW_LEN 6404096
#define RPI_OV5647_RAW_MARKER  "@BRCM"

static void process_file(char* file) {
  FILE*   ifp     = NULL;
  size_t  len;  // number of bytes in file
  ssize_t offset;  // offset into file to start reading pixel data
  uint8_t buffer[16];

  /* Check file existence */
  if (NULL == (ifp = fopen(file, "rb"))) {
    perror(file);
    return;
  }

  /* Check file length */
  fseek(ifp, 0, SEEK_END);
  len = ftell(ifp);
  if (len < RPI_OV5647_RAW_RAW_LEN) {
    fprintf(stderr, "File is too short to contain expected 6MB RAW data.\n");
    goto fail;
  }

  /* Location in file where RAW padding starts */
  offset = (len - RPI_OV5647_RAW_RAW_LEN - 1);
  fseek(ifp, offset - 2, SEEK_SET);
  fread(buffer, 16, 1, ifp);
  if ((buffer[0] != 0xff) || (buffer[1] != 0xd9)) {
    fprintf(stderr, "JPEG EOI not found (want 0xffd9, got 0x%02x%02x).\n", buffer[0], buffer[1]);
    goto fail;
  }
  if (0 != strncmp((const char*)(buffer + 2), RPI_OV5647_RAW_MARKER, 5)) {
    fprintf(stderr, "RAW marker not found.\n");
    goto fail;
  }

  fclose(ifp);
  ifp = NULL;
  truncate(file, offset);

fail:
  if (NULL != ifp) {
    fclose(ifp);
  }

  return;
}

int main(int argc, char* argv[]) {
  int i;
  char c;

  if (argc < 2) {
    return EXIT_FAILURE;
  }

  fprintf(stderr, "File involved:\n");
  for (i = 1; i < argc; i ++) {
    fprintf(stderr, "\t%s\n", argv[i]);
  }

  fprintf(stderr, "Continue[y/N]? ");
  c = getchar();
  if (c != 'y' && c != 'Y') {
    fprintf(stderr, "Canceled.\n");
    return EXIT_FAILURE;
  }

  for (i = 1; i < argc; i ++) {
    fprintf(stderr, "`%s':\n", argv[i]);
    process_file(argv[i]);
  }

  return EXIT_SUCCESS;
}
