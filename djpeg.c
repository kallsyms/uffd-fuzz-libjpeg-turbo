#define _GNU_SOURCE    // for sched_setaffinity and CPU_*
/*
 * djpeg.c
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1991-1997, Thomas G. Lane.
 * Modified 2013-2019 by Guido Vollbeding.
 * libjpeg-turbo Modifications:
 * Copyright (C) 2010-2011, 2013-2017, 2019-2020, 2022, D. R. Commander.
 * Copyright (C) 2015, Google, Inc.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 *
 * This file contains a command-line user interface for the JPEG decompressor.
 * It should work on any system with Unix- or MS-DOS-style command lines.
 *
 * Two different command line styles are permitted, depending on the
 * compile-time switch TWO_FILE_COMMANDLINE:
 *      djpeg [options]  inputfile outputfile
 *      djpeg [options]  [inputfile]
 * In the second style, output is always to standard output, which you'd
 * normally redirect to a file or pipe to some other program.  Input is
 * either from a named file or from standard input (typically redirected).
 * The second style is convenient on Unix but is unhelpful on systems that
 * don't support pipes.  Also, you MUST use the first style if your system
 * doesn't do binary I/O to stdin/stdout.
 * To simplify script writing, the "-outfile" switch is provided.  The syntax
 *      djpeg [options]  -outfile outputfile  inputfile
 * works regardless of which command line style is used.
 */

#ifdef _MSC_VER
#define _CRT_SECURE_NO_DEPRECATE
#endif

#include "cdjpeg.h"             /* Common decls for cjpeg/djpeg applications */
#include "jversion.h"           /* for version message */
#include "jconfigint.h"

#include <ctype.h>              /* to declare isprint() */


/* Create the add-on message string table. */

#define JMESSAGE(code, string)  string,

static const char * const cdjpeg_message_table[] = {
#include "cderror.h"
  NULL
};


/*
 * This list defines the known output image formats
 * (not all of which need be supported by a given version).
 * You can change the default output format by defining DEFAULT_FMT;
 * indeed, you had better do so if you undefine PPM_SUPPORTED.
 */

typedef enum {
  FMT_BMP,                      /* BMP format (Windows flavor) */
  FMT_GIF,                      /* GIF format (LZW-compressed) */
  FMT_GIF0,                     /* GIF format (uncompressed) */
  FMT_OS2,                      /* BMP format (OS/2 flavor) */
  FMT_PPM,                      /* PPM/PGM (PBMPLUS formats) */
  FMT_TARGA,                    /* Targa format */
  FMT_TIFF                      /* TIFF format */
} IMAGE_FORMATS;

#ifndef DEFAULT_FMT             /* so can override from CFLAGS in Makefile */
#define DEFAULT_FMT     FMT_PPM
#endif

static IMAGE_FORMATS requested_fmt;


/*
 * Argument-parsing code.
 * The switch parser is designed to be useful with DOS-style command line
 * syntax, ie, intermixed switches and file names, where only the switches
 * to the left of a given file name affect processing of that file.
 * The main program in this file doesn't actually use this capability...
 */


static const char *progname;    /* program name for error messages */
static char *icc_filename;      /* for -icc switch */
JDIMENSION max_scans;           /* for -maxscans switch */
static char *outfilename;       /* for -outfile switch */
boolean memsrc;                 /* for -memsrc switch */
boolean report;                 /* for -report switch */
boolean skip, crop;
JDIMENSION skip_start, skip_end;
JDIMENSION crop_x, crop_y, crop_width, crop_height;
boolean strict;                 /* for -strict switch */
#define INPUT_BUF_SIZE  4096


LOCAL(void)
usage(void)
/* complain about bad command line */
{
  fprintf(stderr, "usage: %s [switches] ", progname);
#ifdef TWO_FILE_COMMANDLINE
  fprintf(stderr, "inputfile outputfile\n");
#else
  fprintf(stderr, "[inputfile]\n");
#endif

  fprintf(stderr, "Switches (names may be abbreviated):\n");
  fprintf(stderr, "  -colors N      Reduce image to no more than N colors\n");
  fprintf(stderr, "  -fast          Fast, low-quality processing\n");
  fprintf(stderr, "  -grayscale     Force grayscale output\n");
  fprintf(stderr, "  -rgb           Force RGB output\n");
  fprintf(stderr, "  -rgb565        Force RGB565 output\n");
#ifdef IDCT_SCALING_SUPPORTED
  fprintf(stderr, "  -scale M/N     Scale output image by fraction M/N, eg, 1/8\n");
#endif
#ifdef BMP_SUPPORTED
  fprintf(stderr, "  -bmp           Select BMP output format (Windows style)%s\n",
          (DEFAULT_FMT == FMT_BMP ? " (default)" : ""));
#endif
#ifdef GIF_SUPPORTED
  fprintf(stderr, "  -gif           Select GIF output format (LZW-compressed)%s\n",
          (DEFAULT_FMT == FMT_GIF ? " (default)" : ""));
  fprintf(stderr, "  -gif0          Select GIF output format (uncompressed)%s\n",
          (DEFAULT_FMT == FMT_GIF0 ? " (default)" : ""));
#endif
#ifdef BMP_SUPPORTED
  fprintf(stderr, "  -os2           Select BMP output format (OS/2 style)%s\n",
          (DEFAULT_FMT == FMT_OS2 ? " (default)" : ""));
#endif
#ifdef PPM_SUPPORTED
  fprintf(stderr, "  -pnm           Select PBMPLUS (PPM/PGM) output format%s\n",
          (DEFAULT_FMT == FMT_PPM ? " (default)" : ""));
#endif
#ifdef TARGA_SUPPORTED
  fprintf(stderr, "  -targa         Select Targa output format%s\n",
          (DEFAULT_FMT == FMT_TARGA ? " (default)" : ""));
#endif
  fprintf(stderr, "Switches for advanced users:\n");
#ifdef DCT_ISLOW_SUPPORTED
  fprintf(stderr, "  -dct int       Use accurate integer DCT method%s\n",
          (JDCT_DEFAULT == JDCT_ISLOW ? " (default)" : ""));
#endif
#ifdef DCT_IFAST_SUPPORTED
  fprintf(stderr, "  -dct fast      Use less accurate integer DCT method [legacy feature]%s\n",
          (JDCT_DEFAULT == JDCT_IFAST ? " (default)" : ""));
#endif
#ifdef DCT_FLOAT_SUPPORTED
  fprintf(stderr, "  -dct float     Use floating-point DCT method [legacy feature]%s\n",
          (JDCT_DEFAULT == JDCT_FLOAT ? " (default)" : ""));
#endif
  fprintf(stderr, "  -dither fs     Use F-S dithering (default)\n");
  fprintf(stderr, "  -dither none   Don't use dithering in quantization\n");
  fprintf(stderr, "  -dither ordered  Use ordered dither (medium speed, quality)\n");
  fprintf(stderr, "  -icc FILE      Extract ICC profile to FILE\n");
#ifdef QUANT_2PASS_SUPPORTED
  fprintf(stderr, "  -map FILE      Map to colors used in named image file\n");
#endif
  fprintf(stderr, "  -nosmooth      Don't use high-quality upsampling\n");
#ifdef QUANT_1PASS_SUPPORTED
  fprintf(stderr, "  -onepass       Use 1-pass quantization (fast, low quality)\n");
#endif
  fprintf(stderr, "  -maxmemory N   Maximum memory to use (in kbytes)\n");
  fprintf(stderr, "  -maxscans N    Maximum number of scans to allow in input file\n");
  fprintf(stderr, "  -outfile name  Specify name for output file\n");
#if JPEG_LIB_VERSION >= 80 || defined(MEM_SRCDST_SUPPORTED)
  fprintf(stderr, "  -memsrc        Load input file into memory before decompressing\n");
#endif
  fprintf(stderr, "  -report        Report decompression progress\n");
  fprintf(stderr, "  -skip Y0,Y1    Decompress all rows except those between Y0 and Y1 (inclusive)\n");
  fprintf(stderr, "  -crop WxH+X+Y  Decompress only a rectangular subregion of the image\n");
  fprintf(stderr, "                 [requires PBMPLUS (PPM/PGM), GIF, or Targa output format]\n");
  fprintf(stderr, "  -strict        Treat all warnings as fatal\n");
  fprintf(stderr, "  -verbose  or  -debug   Emit debug output\n");
  fprintf(stderr, "  -version       Print version information and exit\n");
  exit(EXIT_FAILURE);
}


LOCAL(int)
parse_switches(j_decompress_ptr cinfo, int argc, char **argv,
               int last_file_arg_seen, boolean for_real)
/* Parse optional switches.
 * Returns argv[] index of first file-name argument (== argc if none).
 * Any file names with indexes <= last_file_arg_seen are ignored;
 * they have presumably been processed in a previous iteration.
 * (Pass 0 for last_file_arg_seen on the first or only iteration.)
 * for_real is FALSE on the first (dummy) pass; we may skip any expensive
 * processing.
 */
{
  int argn;
  char *arg;

  /* Set up default JPEG parameters. */
  requested_fmt = DEFAULT_FMT;  /* set default output file format */
  icc_filename = NULL;
  max_scans = 0;
  outfilename = NULL;
  memsrc = FALSE;
  report = FALSE;
  skip = FALSE;
  crop = FALSE;
  strict = FALSE;
  cinfo->err->trace_level = 0;

  /* Scan command line options, adjust parameters */

  for (argn = 1; argn < argc; argn++) {
    arg = argv[argn];
    if (*arg != '-') {
      /* Not a switch, must be a file name argument */
      if (argn <= last_file_arg_seen) {
        outfilename = NULL;     /* -outfile applies to just one input file */
        continue;               /* ignore this name if previously processed */
      }
      break;                    /* else done parsing switches */
    }
    arg++;                      /* advance past switch marker character */

    if (keymatch(arg, "bmp", 1)) {
      /* BMP output format (Windows flavor). */
      requested_fmt = FMT_BMP;

    } else if (keymatch(arg, "colors", 1) || keymatch(arg, "colours", 1) ||
               keymatch(arg, "quantize", 1) || keymatch(arg, "quantise", 1)) {
      /* Do color quantization. */
      int val;

      if (++argn >= argc)       /* advance to next argument */
        usage();
      if (sscanf(argv[argn], "%d", &val) != 1)
        usage();
      cinfo->desired_number_of_colors = val;
      cinfo->quantize_colors = TRUE;

    } else if (keymatch(arg, "dct", 2)) {
      /* Select IDCT algorithm. */
      if (++argn >= argc)       /* advance to next argument */
        usage();
      if (keymatch(argv[argn], "int", 1)) {
        cinfo->dct_method = JDCT_ISLOW;
      } else if (keymatch(argv[argn], "fast", 2)) {
        cinfo->dct_method = JDCT_IFAST;
      } else if (keymatch(argv[argn], "float", 2)) {
        cinfo->dct_method = JDCT_FLOAT;
      } else
        usage();

    } else if (keymatch(arg, "dither", 2)) {
      /* Select dithering algorithm. */
      if (++argn >= argc)       /* advance to next argument */
        usage();
      if (keymatch(argv[argn], "fs", 2)) {
        cinfo->dither_mode = JDITHER_FS;
      } else if (keymatch(argv[argn], "none", 2)) {
        cinfo->dither_mode = JDITHER_NONE;
      } else if (keymatch(argv[argn], "ordered", 2)) {
        cinfo->dither_mode = JDITHER_ORDERED;
      } else
        usage();

    } else if (keymatch(arg, "debug", 1) || keymatch(arg, "verbose", 1)) {
      /* Enable debug printouts. */
      /* On first -d, print version identification */
      static boolean printed_version = FALSE;

      if (!printed_version) {
        fprintf(stderr, "%s version %s (build %s)\n",
                PACKAGE_NAME, VERSION, BUILD);
        fprintf(stderr, "%s\n\n", JCOPYRIGHT);
        fprintf(stderr, "Emulating The Independent JPEG Group's software, version %s\n\n",
                JVERSION);
        printed_version = TRUE;
      }
      cinfo->err->trace_level++;

    } else if (keymatch(arg, "version", 4)) {
      fprintf(stderr, "%s version %s (build %s)\n",
              PACKAGE_NAME, VERSION, BUILD);
      exit(EXIT_SUCCESS);

    } else if (keymatch(arg, "fast", 1)) {
      /* Select recommended processing options for quick-and-dirty output. */
      cinfo->two_pass_quantize = FALSE;
      cinfo->dither_mode = JDITHER_ORDERED;
      if (!cinfo->quantize_colors) /* don't override an earlier -colors */
        cinfo->desired_number_of_colors = 216;
      cinfo->dct_method = JDCT_FASTEST;
      cinfo->do_fancy_upsampling = FALSE;

    } else if (keymatch(arg, "gif", 1)) {
      /* GIF output format (LZW-compressed). */
      requested_fmt = FMT_GIF;

    } else if (keymatch(arg, "gif0", 4)) {
      /* GIF output format (uncompressed). */
      requested_fmt = FMT_GIF0;

    } else if (keymatch(arg, "grayscale", 2) ||
               keymatch(arg, "greyscale", 2)) {
      /* Force monochrome output. */
      cinfo->out_color_space = JCS_GRAYSCALE;

    } else if (keymatch(arg, "rgb", 2)) {
      /* Force RGB output. */
      cinfo->out_color_space = JCS_RGB;

    } else if (keymatch(arg, "rgb565", 2)) {
      /* Force RGB565 output. */
      cinfo->out_color_space = JCS_RGB565;

    } else if (keymatch(arg, "icc", 1)) {
      /* Set ICC filename. */
      if (++argn >= argc)       /* advance to next argument */
        usage();
      icc_filename = argv[argn];
      jpeg_save_markers(cinfo, JPEG_APP0 + 2, 0xFFFF);

    } else if (keymatch(arg, "map", 3)) {
      /* Quantize to a color map taken from an input file. */
      if (++argn >= argc)       /* advance to next argument */
        usage();
      if (for_real) {           /* too expensive to do twice! */
#ifdef QUANT_2PASS_SUPPORTED    /* otherwise can't quantize to supplied map */
        FILE *mapfile;

        if ((mapfile = fopen(argv[argn], READ_BINARY)) == NULL) {
          fprintf(stderr, "%s: can't open %s\n", progname, argv[argn]);
          exit(EXIT_FAILURE);
        }
        read_color_map(cinfo, mapfile);
        fclose(mapfile);
        cinfo->quantize_colors = TRUE;
#else
        ERREXIT(cinfo, JERR_NOT_COMPILED);
#endif
      }

    } else if (keymatch(arg, "maxmemory", 3)) {
      /* Maximum memory in Kb (or Mb with 'm'). */
      long lval;
      char ch = 'x';

      if (++argn >= argc)       /* advance to next argument */
        usage();
      if (sscanf(argv[argn], "%ld%c", &lval, &ch) < 1)
        usage();
      if (ch == 'm' || ch == 'M')
        lval *= 1000L;
      cinfo->mem->max_memory_to_use = lval * 1000L;

    } else if (keymatch(arg, "maxscans", 4)) {
      if (++argn >= argc)       /* advance to next argument */
        usage();
      if (sscanf(argv[argn], "%u", &max_scans) != 1)
        usage();

    } else if (keymatch(arg, "nosmooth", 3)) {
      /* Suppress fancy upsampling */
      cinfo->do_fancy_upsampling = FALSE;

    } else if (keymatch(arg, "onepass", 3)) {
      /* Use fast one-pass quantization. */
      cinfo->two_pass_quantize = FALSE;

    } else if (keymatch(arg, "os2", 3)) {
      /* BMP output format (OS/2 flavor). */
      requested_fmt = FMT_OS2;

    } else if (keymatch(arg, "outfile", 4)) {
      /* Set output file name. */
      if (++argn >= argc)       /* advance to next argument */
        usage();
      outfilename = argv[argn]; /* save it away for later use */

    } else if (keymatch(arg, "memsrc", 2)) {
      /* Use in-memory source manager */
#if JPEG_LIB_VERSION >= 80 || defined(MEM_SRCDST_SUPPORTED)
      memsrc = TRUE;
#else
      fprintf(stderr, "%s: sorry, in-memory source manager was not compiled in\n",
              progname);
      exit(EXIT_FAILURE);
#endif

    } else if (keymatch(arg, "pnm", 1) || keymatch(arg, "ppm", 1)) {
      /* PPM/PGM output format. */
      requested_fmt = FMT_PPM;

    } else if (keymatch(arg, "report", 2)) {
      report = TRUE;

    } else if (keymatch(arg, "scale", 2)) {
      /* Scale the output image by a fraction M/N. */
      if (++argn >= argc)       /* advance to next argument */
        usage();
      if (sscanf(argv[argn], "%u/%u",
                 &cinfo->scale_num, &cinfo->scale_denom) != 2)
        usage();

    } else if (keymatch(arg, "skip", 2)) {
      if (++argn >= argc)
        usage();
      if (sscanf(argv[argn], "%u,%u", &skip_start, &skip_end) != 2 ||
          skip_start > skip_end)
        usage();
      skip = TRUE;

    } else if (keymatch(arg, "crop", 2)) {
      char c;
      if (++argn >= argc)
        usage();
      if (sscanf(argv[argn], "%u%c%u+%u+%u", &crop_width, &c, &crop_height,
                 &crop_x, &crop_y) != 5 ||
          (c != 'X' && c != 'x') || crop_width < 1 || crop_height < 1)
        usage();
      crop = TRUE;

    } else if (keymatch(arg, "strict", 2)) {
      strict = TRUE;

    } else if (keymatch(arg, "targa", 1)) {
      /* Targa output format. */
      requested_fmt = FMT_TARGA;

    } else {
      usage();                  /* bogus switch */
    }
  }

  return argn;                  /* return index of next arg (file name) */
}


/*
 * Marker processor for COM and interesting APPn markers.
 * This replaces the library's built-in processor, which just skips the marker.
 * We want to print out the marker as text, to the extent possible.
 * Note this code relies on a non-suspending data source.
 */

LOCAL(unsigned int)
jpeg_getc(j_decompress_ptr cinfo)
/* Read next byte */
{
  struct jpeg_source_mgr *datasrc = cinfo->src;

  if (datasrc->bytes_in_buffer == 0) {
    if (!(*datasrc->fill_input_buffer) (cinfo))
      ERREXIT(cinfo, JERR_CANT_SUSPEND);
  }
  datasrc->bytes_in_buffer--;
  return *datasrc->next_input_byte++;
}


METHODDEF(boolean)
print_text_marker(j_decompress_ptr cinfo)
{
  boolean traceit = (cinfo->err->trace_level >= 1);
  long length;
  unsigned int ch;
  unsigned int lastch = 0;

  length = jpeg_getc(cinfo) << 8;
  length += jpeg_getc(cinfo);
  length -= 2;                  /* discount the length word itself */

  if (traceit) {
    if (cinfo->unread_marker == JPEG_COM)
      fprintf(stderr, "Comment, length %ld:\n", (long)length);
    else                        /* assume it is an APPn otherwise */
      fprintf(stderr, "APP%d, length %ld:\n",
              cinfo->unread_marker - JPEG_APP0, (long)length);
  }

  while (--length >= 0) {
    ch = jpeg_getc(cinfo);
    if (traceit) {
      /* Emit the character in a readable form.
       * Nonprintables are converted to \nnn form,
       * while \ is converted to \\.
       * Newlines in CR, CR/LF, or LF form will be printed as one newline.
       */
      if (ch == '\r') {
        fprintf(stderr, "\n");
      } else if (ch == '\n') {
        if (lastch != '\r')
          fprintf(stderr, "\n");
      } else if (ch == '\\') {
        fprintf(stderr, "\\\\");
      } else if (isprint(ch)) {
        putc(ch, stderr);
      } else {
        fprintf(stderr, "\\%03o", ch);
      }
      lastch = ch;
    }
  }

  if (traceit)
    fprintf(stderr, "\n");

  return TRUE;
}


METHODDEF(void)
my_emit_message(j_common_ptr cinfo, int msg_level)
{
  if (msg_level < 0) {
    /* Treat warning as fatal */
    cinfo->err->error_exit(cinfo);
  } else {
    if (cinfo->err->trace_level >= msg_level)
      cinfo->err->output_message(cinfo);
  }
}


/*
 * The main program.
 */

int
target_main(int argc, char **argv)
{
  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;
  struct cdjpeg_progress_mgr progress;
  int file_index;
  djpeg_dest_ptr dest_mgr = NULL;
  FILE *input_file;
  FILE *output_file;
  unsigned char *inbuffer = NULL;
#if JPEG_LIB_VERSION >= 80 || defined(MEM_SRCDST_SUPPORTED)
  unsigned long insize = 0;
#endif
  JDIMENSION num_scanlines;

  progname = argv[0];
  if (progname == NULL || progname[0] == 0)
    progname = "djpeg";         /* in case C library doesn't provide it */

  /* Initialize the JPEG decompression object with default error handling. */
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_decompress(&cinfo);
  /* Add some application-specific error messages (from cderror.h) */
  jerr.addon_message_table = cdjpeg_message_table;
  jerr.first_addon_message = JMSG_FIRSTADDONCODE;
  jerr.last_addon_message = JMSG_LASTADDONCODE;

  /* Insert custom marker processor for COM and APP12.
   * APP12 is used by some digital camera makers for textual info,
   * so we provide the ability to display it as text.
   * If you like, additional APPn marker types can be selected for display,
   * but don't try to override APP0 or APP14 this way (see libjpeg.txt).
   */
  jpeg_set_marker_processor(&cinfo, JPEG_COM, print_text_marker);
  jpeg_set_marker_processor(&cinfo, JPEG_APP0 + 12, print_text_marker);

  /* Scan command line to find file names. */
  /* It is convenient to use just one switch-parsing routine, but the switch
   * values read here are ignored; we will rescan the switches after opening
   * the input file.
   * (Exception: tracing level set here controls verbosity for COM markers
   * found during jpeg_read_header...)
   */

  file_index = parse_switches(&cinfo, argc, argv, 0, FALSE);

  if (strict)
    jerr.emit_message = my_emit_message;

#ifdef TWO_FILE_COMMANDLINE
  /* Must have either -outfile switch or explicit output file name */
  if (outfilename == NULL) {
    if (file_index != argc - 2) {
      fprintf(stderr, "%s: must name one input and one output file\n",
              progname);
      usage();
    }
    outfilename = argv[file_index + 1];
  } else {
    if (file_index != argc - 1) {
      fprintf(stderr, "%s: must name one input and one output file\n",
              progname);
      usage();
    }
  }
#else
  /* Unix style: expect zero or one file name */
  if (file_index < argc - 1) {
    fprintf(stderr, "%s: only one input file\n", progname);
    usage();
  }
#endif /* TWO_FILE_COMMANDLINE */

  /* Open the input file. */
  if (file_index < argc) {
    if ((input_file = fopen(argv[file_index], READ_BINARY)) == NULL) {
      fprintf(stderr, "%s: can't open %s\n", progname, argv[file_index]);
      exit(EXIT_FAILURE);
    }
  } else {
    /* default input file is stdin */
    input_file = read_stdin();
  }

  /* Open the output file. */
  if (outfilename != NULL) {
    if ((output_file = fopen(outfilename, WRITE_BINARY)) == NULL) {
      fprintf(stderr, "%s: can't open %s\n", progname, outfilename);
      exit(EXIT_FAILURE);
    }
  } else {
    /* default output file is stdout */
    output_file = write_stdout();
  }

  if (report || max_scans != 0) {
    start_progress_monitor((j_common_ptr)&cinfo, &progress);
    progress.report = report;
    progress.max_scans = max_scans;
  }

  /* Specify data source for decompression */
#if JPEG_LIB_VERSION >= 80 || defined(MEM_SRCDST_SUPPORTED)
  if (memsrc) {
    size_t nbytes;
    do {
      inbuffer = (unsigned char *)realloc(inbuffer, insize + INPUT_BUF_SIZE);
      if (inbuffer == NULL) {
        fprintf(stderr, "%s: memory allocation failure\n", progname);
        exit(EXIT_FAILURE);
      }
      nbytes = fread(&inbuffer[insize], 1, INPUT_BUF_SIZE, input_file);
      if (nbytes < INPUT_BUF_SIZE && ferror(input_file)) {
        if (file_index < argc)
          fprintf(stderr, "%s: can't read from %s\n", progname,
                  argv[file_index]);
        else
          fprintf(stderr, "%s: can't read from stdin\n", progname);
      }
      insize += (unsigned long)nbytes;
    } while (nbytes == INPUT_BUF_SIZE);
    fprintf(stderr, "Compressed size:  %lu bytes\n", insize);
    jpeg_mem_src(&cinfo, inbuffer, insize);
  } else
#endif
    jpeg_stdio_src(&cinfo, input_file);

  /* Read file header, set default decompression parameters */
  (void)jpeg_read_header(&cinfo, TRUE);

  /* Adjust default decompression parameters by re-parsing the options */
  file_index = parse_switches(&cinfo, argc, argv, 0, TRUE);

  /* Initialize the output module now to let it override any crucial
   * option settings (for instance, GIF wants to force color quantization).
   */
  switch (requested_fmt) {
#ifdef BMP_SUPPORTED
  case FMT_BMP:
    dest_mgr = jinit_write_bmp(&cinfo, FALSE, TRUE);
    break;
  case FMT_OS2:
    dest_mgr = jinit_write_bmp(&cinfo, TRUE, TRUE);
    break;
#endif
#ifdef GIF_SUPPORTED
  case FMT_GIF:
    dest_mgr = jinit_write_gif(&cinfo, TRUE);
    break;
  case FMT_GIF0:
    dest_mgr = jinit_write_gif(&cinfo, FALSE);
    break;
#endif
#ifdef PPM_SUPPORTED
  case FMT_PPM:
    dest_mgr = jinit_write_ppm(&cinfo);
    break;
#endif
#ifdef TARGA_SUPPORTED
  case FMT_TARGA:
    dest_mgr = jinit_write_targa(&cinfo);
    break;
#endif
  default:
    ERREXIT(&cinfo, JERR_UNSUPPORTED_FORMAT);
    break;
  }
  dest_mgr->output_file = output_file;

  /* Start decompressor */
  (void)jpeg_start_decompress(&cinfo);

  /* Skip rows */
  if (skip) {
    JDIMENSION tmp;

    /* Check for valid skip_end.  We cannot check this value until after
     * jpeg_start_decompress() is called.  Note that we have already verified
     * that skip_start <= skip_end.
     */
    if (skip_end > cinfo.output_height - 1) {
      fprintf(stderr, "%s: skip region exceeds image height %u\n", progname,
              cinfo.output_height);
      exit(EXIT_FAILURE);
    }

    /* Write output file header.  This is a hack to ensure that the destination
     * manager creates an output image of the proper size.
     */
    tmp = cinfo.output_height;
    cinfo.output_height -= (skip_end - skip_start + 1);
    (*dest_mgr->start_output) (&cinfo, dest_mgr);
    cinfo.output_height = tmp;

    /* Process data */
    while (cinfo.output_scanline < skip_start) {
      num_scanlines = jpeg_read_scanlines(&cinfo, dest_mgr->buffer,
                                          dest_mgr->buffer_height);
      (*dest_mgr->put_pixel_rows) (&cinfo, dest_mgr, num_scanlines);
    }
    if ((tmp = jpeg_skip_scanlines(&cinfo, skip_end - skip_start + 1)) !=
        skip_end - skip_start + 1) {
      fprintf(stderr, "%s: jpeg_skip_scanlines() returned %u rather than %u\n",
              progname, tmp, skip_end - skip_start + 1);
      exit(EXIT_FAILURE);
    }
    while (cinfo.output_scanline < cinfo.output_height) {
      num_scanlines = jpeg_read_scanlines(&cinfo, dest_mgr->buffer,
                                          dest_mgr->buffer_height);
      (*dest_mgr->put_pixel_rows) (&cinfo, dest_mgr, num_scanlines);
    }

  /* Decompress a subregion */
  } else if (crop) {
    JDIMENSION tmp;

    /* Check for valid crop dimensions.  We cannot check these values until
     * after jpeg_start_decompress() is called.
     */
    if (crop_x + crop_width > cinfo.output_width ||
        crop_y + crop_height > cinfo.output_height) {
      fprintf(stderr, "%s: crop dimensions exceed image dimensions %u x %u\n",
              progname, cinfo.output_width, cinfo.output_height);
      exit(EXIT_FAILURE);
    }

    jpeg_crop_scanline(&cinfo, &crop_x, &crop_width);
    if (dest_mgr->calc_buffer_dimensions)
      (*dest_mgr->calc_buffer_dimensions) (&cinfo, dest_mgr);
    else
      ERREXIT(&cinfo, JERR_UNSUPPORTED_FORMAT);

    /* Write output file header.  This is a hack to ensure that the destination
     * manager creates an output image of the proper size.
     */
    tmp = cinfo.output_height;
    cinfo.output_height = crop_height;
    (*dest_mgr->start_output) (&cinfo, dest_mgr);
    cinfo.output_height = tmp;

    /* Process data */
    if ((tmp = jpeg_skip_scanlines(&cinfo, crop_y)) != crop_y) {
      fprintf(stderr, "%s: jpeg_skip_scanlines() returned %u rather than %u\n",
              progname, tmp, crop_y);
      exit(EXIT_FAILURE);
    }
    while (cinfo.output_scanline < crop_y + crop_height) {
      num_scanlines = jpeg_read_scanlines(&cinfo, dest_mgr->buffer,
                                          dest_mgr->buffer_height);
      (*dest_mgr->put_pixel_rows) (&cinfo, dest_mgr, num_scanlines);
    }
    if ((tmp =
         jpeg_skip_scanlines(&cinfo,
                             cinfo.output_height - crop_y - crop_height)) !=
        cinfo.output_height - crop_y - crop_height) {
      fprintf(stderr, "%s: jpeg_skip_scanlines() returned %u rather than %u\n",
              progname, tmp, cinfo.output_height - crop_y - crop_height);
      exit(EXIT_FAILURE);
    }

  /* Normal full-image decompress */
  } else {
    /* Write output file header */
    (*dest_mgr->start_output) (&cinfo, dest_mgr);

    /* Process data */
    while (cinfo.output_scanline < cinfo.output_height) {
      num_scanlines = jpeg_read_scanlines(&cinfo, dest_mgr->buffer,
                                          dest_mgr->buffer_height);
      (*dest_mgr->put_pixel_rows) (&cinfo, dest_mgr, num_scanlines);
    }
  }

  /* Hack: count final pass as done in case finish_output does an extra pass.
   * The library won't have updated completed_passes.
   */
  if (report || max_scans != 0)
    progress.pub.completed_passes = progress.pub.total_passes;

  if (icc_filename != NULL) {
    FILE *icc_file;
    JOCTET *icc_profile;
    unsigned int icc_len;

    if ((icc_file = fopen(icc_filename, WRITE_BINARY)) == NULL) {
      fprintf(stderr, "%s: can't open %s\n", progname, icc_filename);
      exit(EXIT_FAILURE);
    }
    if (jpeg_read_icc_profile(&cinfo, &icc_profile, &icc_len)) {
      if (fwrite(icc_profile, icc_len, 1, icc_file) < 1) {
        fprintf(stderr, "%s: can't read ICC profile from %s\n", progname,
                icc_filename);
        free(icc_profile);
        fclose(icc_file);
        exit(EXIT_FAILURE);
      }
      free(icc_profile);
      fclose(icc_file);
    } else if (cinfo.err->msg_code != JWRN_BOGUS_ICC)
      fprintf(stderr, "%s: no ICC profile data in JPEG file\n", progname);
  }

  /* Finish decompression and release memory.
   * I must do it in this order because output module has allocated memory
   * of lifespan JPOOL_IMAGE; it needs to finish before releasing memory.
   */
  (*dest_mgr->finish_output) (&cinfo, dest_mgr);
  (void)jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);

  /* Close files, if we opened them */
  if (input_file != stdin)
    fclose(input_file);
  if (output_file != stdout)
    fclose(output_file);

  if (report || max_scans != 0)
    end_progress_monitor((j_common_ptr)&cinfo);

  if (memsrc)
    free(inbuffer);

  /* All done. */
  exit(jerr.num_warnings ? EXIT_WARNING : EXIT_SUCCESS);
  return 0;                     /* suppress no-return-value warnings */
}

#ifndef H_PMPARSER
#define H_PMPARSER
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/limits.h>

//maximum line length in a procmaps file
#define PROCMAPS_LINE_MAX_LENGTH  (PATH_MAX + 100) 
/**
 * procmaps_struct
 * @desc hold all the information about an area in the process's  VM
 */
typedef struct procmaps_struct{
	void* addr_start; 	//< start address of the area
	void* addr_end; 	//< end address
	unsigned long length; //< size of the range

	char perm[5];		//< permissions rwxp
	short is_r;			//< rewrote of perm with short flags
	short is_w;
	short is_x;
	short is_p;

	long offset;	//< offset
	char dev[12];	//< dev major:minor
	int inode;		//< inode of the file that backs the area

	char pathname[600];		//< the path of the file that backs the area
	//chained list
	struct procmaps_struct* next;		//<handler of the chinaed list
} procmaps_struct;

/**
 * procmaps_iterator
 * @desc holds iterating information
 */
typedef struct procmaps_iterator{
	procmaps_struct* head;
	procmaps_struct* current;
} procmaps_iterator;
/**
 * pmparser_parse
 * @param pid the process id whose memory map to be parser. the current process if pid<0
 * @return an iterator over all the nodes
 */
procmaps_iterator* pmparser_parse(int pid);

/**
 * pmparser_next
 * @description move between areas
 * @param p_procmaps_it the iterator to move on step in the chained list
 * @return a procmaps structure filled with information about this VM area
 */
procmaps_struct* pmparser_next(procmaps_iterator* p_procmaps_it);
/**
 * pmparser_free
 * @description should be called at the end to free the resources
 * @param p_procmaps_it the iterator structure returned by pmparser_parse
 */
void pmparser_free(procmaps_iterator* p_procmaps_it);

/**
 * _pmparser_split_line
 * @description internal usage
 */
void _pmparser_split_line(char*buf,char*addr1,char*addr2,char*perm, char* offset, char* device,char*inode,char* pathname);

/**
 * pmparser_print
 * @param map the head of the list
 * @order the order of the area to print, -1 to print everything
 */
void pmparser_print(procmaps_struct* map,int order);
#endif

procmaps_iterator* pmparser_parse(int pid){
	procmaps_iterator* maps_it = malloc(sizeof(procmaps_iterator));
	char maps_path[500];
	if(pid>=0 ){
		sprintf(maps_path,"/proc/%d/maps",pid);
	}else{
		sprintf(maps_path,"/proc/self/maps");
	}
	FILE* file=fopen(maps_path,"r");
	if(!file){
		fprintf(stderr,"pmparser : cannot open the memory maps, %s\n",strerror(errno));
		return NULL;
	}
	int ind=0;char buf[PROCMAPS_LINE_MAX_LENGTH];
	int c;
	procmaps_struct* list_maps=NULL;
	procmaps_struct* tmp;
	procmaps_struct* current_node=list_maps;
	char addr1[20],addr2[20], perm[8], offset[20], dev[10],inode[30],pathname[PATH_MAX];
	while( !feof(file) ){
		fgets(buf,PROCMAPS_LINE_MAX_LENGTH,file);
		//allocate a node
		tmp=(procmaps_struct*)malloc(sizeof(procmaps_struct));
		//fill the node
		_pmparser_split_line(buf,addr1,addr2,perm,offset, dev,inode,pathname);
		//printf("#%s",buf);
		//printf("%s-%s %s %s %s %s\t%s\n",addr1,addr2,perm,offset,dev,inode,pathname);
		//addr_start & addr_end
		unsigned long l_addr_start;
		sscanf(addr1,"%lx",(long unsigned *)&tmp->addr_start );
		sscanf(addr2,"%lx",(long unsigned *)&tmp->addr_end );
		//size
		tmp->length=(unsigned long)(tmp->addr_end-tmp->addr_start);
		//perm
		strcpy(tmp->perm,perm);
		tmp->is_r=(perm[0]=='r');
		tmp->is_w=(perm[1]=='w');
		tmp->is_x=(perm[2]=='x');
		tmp->is_p=(perm[3]=='p');

		//offset
		sscanf(offset,"%lx",&tmp->offset );
		//device
		strcpy(tmp->dev,dev);
		//inode
		tmp->inode=atoi(inode);
		//pathname
		strcpy(tmp->pathname,pathname);
		tmp->next=NULL;
		//attach the node
		if(ind==0){
			list_maps=tmp;
			list_maps->next=NULL;
			current_node=list_maps;
		}
		current_node->next=tmp;
		current_node=tmp;
		ind++;
		//printf("%s",buf);
	}

	//close file
	fclose(file);


	//g_last_head=list_maps;
	maps_it->head = list_maps;
	maps_it->current =  list_maps;
	return maps_it;
}


procmaps_struct* pmparser_next(procmaps_iterator* p_procmaps_it){
	if(p_procmaps_it->current == NULL)
		return NULL;
	procmaps_struct* p_current = p_procmaps_it->current;
	p_procmaps_it->current = p_procmaps_it->current->next;
	return p_current;
	/*
	if(g_current==NULL){
		g_current=g_last_head;
	}else
		g_current=g_current->next;

	return g_current;
	*/
}



void pmparser_free(procmaps_iterator* p_procmaps_it){
	procmaps_struct* maps_list = p_procmaps_it->head;
	if(maps_list==NULL) return ;
	procmaps_struct* act=maps_list;
	procmaps_struct* nxt=act->next;
	while(act!=NULL){
		free(act);
		act=nxt;
		if(nxt!=NULL)
			nxt=nxt->next;
	}

}


void _pmparser_split_line(
		char*buf,char*addr1,char*addr2,
		char*perm,char* offset,char* device,char*inode,
		char* pathname){
	//
	int orig=0;
	int i=0;
	//addr1
	while(buf[i]!='-'){
		addr1[i-orig]=buf[i];
		i++;
	}
	addr1[i]='\0';
	i++;
	//addr2
	orig=i;
	while(buf[i]!='\t' && buf[i]!=' '){
		addr2[i-orig]=buf[i];
		i++;
	}
	addr2[i-orig]='\0';

	//perm
	while(buf[i]=='\t' || buf[i]==' ')
		i++;
	orig=i;
	while(buf[i]!='\t' && buf[i]!=' '){
		perm[i-orig]=buf[i];
		i++;
	}
	perm[i-orig]='\0';
	//offset
	while(buf[i]=='\t' || buf[i]==' ')
		i++;
	orig=i;
	while(buf[i]!='\t' && buf[i]!=' '){
		offset[i-orig]=buf[i];
		i++;
	}
	offset[i-orig]='\0';
	//dev
	while(buf[i]=='\t' || buf[i]==' ')
		i++;
	orig=i;
	while(buf[i]!='\t' && buf[i]!=' '){
		device[i-orig]=buf[i];
		i++;
	}
	device[i-orig]='\0';
	//inode
	while(buf[i]=='\t' || buf[i]==' ')
		i++;
	orig=i;
	while(buf[i]!='\t' && buf[i]!=' '){
		inode[i-orig]=buf[i];
		i++;
	}
	inode[i-orig]='\0';
	//pathname
	pathname[0]='\0';
	while(buf[i]=='\t' || buf[i]==' ')
		i++;
	orig=i;
	while(buf[i]!='\t' && buf[i]!=' ' && buf[i]!='\n'){
		pathname[i-orig]=buf[i];
		i++;
	}
	pathname[i-orig]='\0';

}

void pmparser_print(procmaps_struct* map, int order){

	procmaps_struct* tmp=map;
	int id=0;
	if(order<0) order=-1;
	while(tmp!=NULL){
		//(unsigned long) tmp->addr_start;
		if(order==id || order==-1){
			printf("Backed by:\t%s\n",strlen(tmp->pathname)==0?"[anonym*]":tmp->pathname);
			printf("Range:\t\t%p-%p\n",tmp->addr_start,tmp->addr_end);
			printf("Length:\t\t%ld\n",tmp->length);
			printf("Offset:\t\t%ld\n",tmp->offset);
			printf("Permissions:\t%s\n",tmp->perm);
			printf("Inode:\t\t%d\n",tmp->inode);
			printf("Device:\t\t%s\n",tmp->dev);
		}
		if(order!=-1 && id>order)
			tmp=NULL;
		else if(order==-1){
			printf("#################################\n");
			tmp=tmp->next;
		}else tmp=tmp->next;

		id++;
	}
}




#include "../bench.h"

#include <sys/mman.h>   // mmap, PROT_*, MAP_*
#include <asm/unistd.h> // __NR_*
#include <stdint.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>  // ioctl
#include <pthread.h>
#include <poll.h>

#include "../pmparser.h"

#ifndef UFFDIO_WRITEPROTECT_MODE_WP
#include "../uffdio_wp.h"
#endif

#define PAGE_SIZE 4096


// stack for the uffd handler thread.
__attribute__((section(".writeignored"))) uint8_t uffd_handler_stack[0x10000];
// stack used by run() before switching to old_stack to run the target code
__attribute__((section(".writeignored"))) uint8_t main_stack[0x10000];

// old stack to pivot back after everything is remapped
__attribute__((section(".writeignored"))) uintptr_t old_stack;

// These have to be all inline asm because syscall() and anything else will hit libc
#include "../syscalls_x86_64.h"
#define save_old_stack() do { register long sp __asm__ ("rsp"); old_stack = sp; sp = (long)&main_stack[0xf000]; } while (0)
#define restore_old_stack() do { register long sp __asm__("rsp") = old_stack; } while (0)
#define swap_old_stack() do { register long sp __asm__("rsp"); register long tmp = old_stack; old_stack = sp; sp = tmp; } while (0)
#define switch_uffd_handler_stack() do { register long sp __asm__("rsp") = (long)&uffd_handler_stack[0xf000]; } while (0)

__attribute__((section(".remap"))) void *remap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    return (void *)my_syscall6(__NR_mmap, addr, length, prot, flags, fd, offset);
}

__attribute__((section(".remap"))) int remap_mprotect(void *addr, size_t length, int prot)
{
    return (int)my_syscall3(__NR_mprotect, addr, length, prot);
}

__attribute__((section(".remap"))) int remap_munmap(void *addr, size_t length)
{
    return (int)my_syscall2(__NR_mprotect, addr, length);
}


// marked writeignored so that they don't get pulled out from underneath us in remap()
__attribute__((section(".writeignored"))) size_t n_maps = 0;
__attribute__((section(".writeignored"))) procmaps_struct maps[0x100];

typedef struct {
    uintptr_t addr;
    uint8_t data[PAGE_SIZE];
} page_t;

__attribute__((section(".writeignored"))) size_t n_pages = 0;
// TODO: variable length
__attribute__((section(".writeignored"))) page_t pages[0x1000];

// stuff so the main thread can wait until UFFD write protecting is done
__attribute__((section(".writeignored"))) pthread_cond_t uffd_ready = PTHREAD_COND_INITIALIZER;
__attribute__((section(".writeignored"))) pthread_mutex_t uffd_ready_lock = PTHREAD_MUTEX_INITIALIZER;

// remaps everything anon rw (i think because you can't set WP on file mapped stuff?)
__attribute__((noinline, section(".remap"))) void remap()
{
    for (off_t i = 0; i < n_maps; i++) {
        procmaps_struct *cur_map = &maps[i];
        int prot = (cur_map->is_r ? PROT_READ : 0) | (cur_map->is_w ? PROT_WRITE : 0) | (cur_map->is_x ? PROT_EXEC : 0);

        // copy from region to tmp
        uint8_t *tmp = remap_mmap((void *)0xdead0000, cur_map->length, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
        for (off_t j = 0; j < cur_map->length; j++) {
            tmp[j] = *((uint8_t *)(cur_map->addr_start) + j);
        }

        // unmap original
        remap_munmap(cur_map->addr_start, cur_map->length);

        // recreate RW in orignal's place
        uint8_t *new = remap_mmap(cur_map->addr_start, cur_map->addr_end - cur_map->addr_start, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);

        // copy back from tmp
        // not memcpy so libc doesn't get invoked
        for (off_t j = 0; j < cur_map->length; j++) {
            new[j] = tmp[j];
        }

        // clean up tmp
        remap_munmap((void *)0xdead0000, cur_map->length);

        // re-set permissions
        remap_mprotect(cur_map->addr_start, cur_map->addr_end - cur_map->addr_start, prot);
    }

    return;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
#ifdef DEBUG
    puts("Intercepted call to mmap");
#endif
    // TODO: unmap after a run
    return remap_mmap(addr, length, prot, flags, fd, offset);
}

int load_maps()
{
    procmaps_iterator* maps_it = pmparser_parse(-1);

    if (maps_it == NULL) {
        perror("pmparser_parse");
        return -1;
    }

    procmaps_struct* cur_map = NULL;

    while ((cur_map = pmparser_next(maps_it)) != NULL) {
        // ignore .remap and .writeignored
        if (cur_map->addr_start == (void *)REMAP_ADDR || cur_map->addr_start == (void *)WRITE_IGNORED_ADDR) {
            continue;
        }

        // ignore --- regions (libc has a couple?)
        if (!(cur_map->is_r || cur_map->is_w || cur_map->is_x)) {
#ifdef DEBUG
            printf("Skipping --- region at %p-%p\n", cur_map->addr_start, cur_map->addr_end);
#endif
            continue;
        }

        if (!strcmp(cur_map->pathname, "[vsyscall]") || !strcmp(cur_map->pathname, "[vvar]") || !strcmp(cur_map->pathname, "[vdso]")) {
#ifdef DEBUG
            printf("Skipping %s region\n", cur_map->pathname);
#endif
            continue;
        }

        // only monitor the program BSS
        if (cur_map->addr_start == (void *)GOT_PLT_ADDR) {
#ifdef DEBUG
            printf("Skipping .got.plt at %p-%p\n", cur_map->addr_start, cur_map->addr_end);
#endif
            continue;
        }

#ifdef DEBUG
        pmparser_print(cur_map, 0);
#endif

        // TODO: need some way to pre-fill the PLT for anything uffd_monitor_thread uses
        // XXX: right now just building statically
        // this may be the best solution though to make sure that mmap hook works even with other libs calling it

        maps[n_maps] = *cur_map;
        n_maps++;
    }

    pmparser_free(maps_it);

    return 0;
}

__attribute__((noreturn)) void *uffd_monitor_thread(void *data)
{
    switch_uffd_handler_stack();
    int uffd = *(int *)data;

    // TODO: ignore PLT writes (probably just set the section address in the linker args)

    for (off_t i = 0; i < n_maps; i++) {
        procmaps_struct *cur_map = &maps[i];

        struct uffdio_writeprotect wp = {0};
        wp.range.start = (unsigned long)cur_map->addr_start;
        wp.range.len = (unsigned long)cur_map->addr_end - (unsigned long)cur_map->addr_start;
        wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
        if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) == -1) {
            perror("ioctl(UFFDIO_WRITEPROTECT)");
            _exit(1);
        }
    }

    pthread_cond_signal(&uffd_ready);

    // TODO: replace _exits with UFFD unregister, prints, exit

    for (;;) {
        struct uffd_msg msg;

        struct pollfd pollfd[1];
        pollfd[0].fd = uffd;
        pollfd[0].events = POLLIN;
        int pollres;

        pollres = poll(pollfd, 1, -1);
        switch (pollres) {
            case -1:
                // perror("poll");
                continue;
                break;
            case 0: continue; break;
            case 1: break;
        }
        if (pollfd[0].revents & POLLERR) {
            // fprintf(stderr, "POLLERR on userfaultfd\n");
            _exit(2);
        }
        if (!(pollfd[0].revents & POLLIN)) {
            continue;
        }

        int readret;
        readret = read(uffd, &msg, sizeof(msg));
        if (readret == -1) {
            if (errno == EAGAIN)
                continue;
            //perror("read userfaultfd");
            _exit(3);
        }
        if (readret != sizeof(msg)) {
            //fprintf(stderr, "short read, not expected, exiting\n");
            _exit(4);
        }

        if (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP) {
            // record contents
            uintptr_t page_addr = msg.arg.pagefault.address & ~(PAGE_SIZE - 1);
            pages[n_pages].addr = page_addr;
            memcpy(pages[n_pages].data, (void *)page_addr, PAGE_SIZE);
            n_pages++;

            // send write unlock
            struct uffdio_writeprotect wp;
            wp.range.start = page_addr;
            wp.range.len = PAGE_SIZE;
            wp.mode = 0;
            if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) == -1) {
                //perror("ioctl(UFFDIO_WRITEPROTECT)");
                _exit(5);
            }
        }
    }
}

int uffd_setup()
{
    int uffd = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);

    if (uffd < 0) {
        perror("userfaultfd");
        return 0;
    }

    // UFFD "handshake" with the kernel
    struct uffdio_api uffdio_api;
    uffdio_api.api = UFFD_API;
    uffdio_api.features = 0;

    if (ioctl(uffd, UFFDIO_API, &uffdio_api)) {
        perror("ioctl(UFFDIO_API)");
        return 0;
    }

    if (uffdio_api.api != UFFD_API) {
        fprintf(stderr, "UFFDIO_API error %Lu\n", uffdio_api.api);
        return 0;
    }

    if (!(uffdio_api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP)) {
        fprintf(stderr, "UFFD doesn't have WP capability (kernel too old?)\n");
        return 0;
    }

    for (off_t i = 0; i < n_maps; i++) {
        procmaps_struct *cur_map = &maps[i];

        // could check for is_w here, but might as well not in case something mprotects

        struct uffdio_register uffdio_register = {0};
        uffdio_register.range.start = (unsigned long)cur_map->addr_start;
        uffdio_register.range.len = (unsigned long)cur_map->addr_end - (unsigned long)cur_map->addr_start;
        uffdio_register.mode = UFFDIO_REGISTER_MODE_WP;

        if (ioctl(uffd, UFFDIO_REGISTER, &uffdio_register) == -1) {
            perror("ioctl(UFFDIO_REGISTER)");
            return 0;
        }
    }

    return uffd;
}

int uffd_deregister(int uffd)
{
    for (off_t i = 0; i < n_maps; i++) {
        procmaps_struct *cur_map = &maps[i];

        // could check for is_w here, but might as well not in case something mprotects

        struct uffdio_range range = {0};
        range.start = (unsigned long)cur_map->addr_start;
        range.len = (unsigned long)cur_map->addr_end - (unsigned long)cur_map->addr_start;

        if (ioctl(uffd, UFFDIO_UNREGISTER, &range) == -1) {
            perror("ioctl(UFFDIO_UNREGISTER)");
            return 1;
        }
    }

    return 0;
}

void restore_pages()
{
#ifdef DEBUG
    printf("See %lu pages:\n", n_pages);
    for (size_t i = 0; i < n_pages; i++) {
        printf("  %p\n", (void *)pages[i].addr);
    }
#endif

    // TODO: maybe this should be kept as two arrays so the data always lives on a page-aligned boundary?
    for (size_t i = 0; i < n_pages; i++) {
        page_t *cur_page = &pages[i];
        memcpy((void *)cur_page->addr, cur_page->data, PAGE_SIZE);
    }
}

int run(int _argc, char **_argv)
{
    // not sure if necessary, but pin to register so stack swapping doesn't do anything bad if these spill
    register int argc = _argc;
    register char **argv = _argv;

    if (load_maps()) {
        return 1;
    }

    remap();

    // Do basic UFFD setup here mainly just for error handling's sake
    int uffd = uffd_setup();
    if (!uffd) {
        return 1;
    }

    pthread_t uffd_thread;
    pthread_create(&uffd_thread, NULL, uffd_monitor_thread, &uffd);

    // could also like msgsnd/msgrcv?
    pthread_mutex_lock(&uffd_ready_lock);
    pthread_cond_wait(&uffd_ready, &uffd_ready_lock);

    redirect_stdout();
    setaffinity(3);

    for (int i = 0; i < ITERS; i++) {
        struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

        swap_old_stack();
        target_main(argc, argv);
        swap_old_stack();

        restore_pages();

        clock_gettime(CLOCK_MONOTONIC, &end);
        times[i] = timespecDiff(&end, &start);
    }

    dup2(stdout_fd, STDOUT_FILENO);
    report_times();

    return 0;
}

int main(int _argc, char **_argv)
{
    // not sure if necessary, but save_old_stack moves sp around so could break spilled
    register int argc = _argc;
    register char **argv = _argv;

    save_old_stack();
    register int ret = run(argc, argv);
    restore_old_stack();

    return ret;
}
