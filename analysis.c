/*
 * (C) Copyright 2006 Jean-Baptiste Note <jean-baptiste.note@m4x.org>
 * All rights reserved.
 */

#include <glib.h>
#include <glib/gprintf.h>

#include "debitlog.h"

#include "wiring.h"
#include "localpips.h"
#include "bitstream.h"
#include "connexity.h"
#include "analysis.h"
#include "design.h"

#include "xdlout.h"

/*
 * This file centralizes the work on bitstream analysis. It gets the
 * sites descriptions for the chip, then the pips for all chip sites,
 * then does the connexity analysis required to get all nets in the
 * FPGA. From here specialized modules can do more in-depth work, such
 * as VHDL/Verilog dumps.
 */

/*
 * Very simple analysis function which only dumps the pips to stdout
 */

static inline void
print_pip(const csite_descr_t *site, const gchar *start, const gchar *end) {
  gchar site_buf[32];
  /* XXX */
  sprint_csite(site_buf, site, 0, 0);
  g_printf("pip %s %s -> %s\n", site_buf, start, end);
}

static void
print_bram_data(const csite_descr_t *site, const guint16 *data) {
  guint i,j;
  g_printf("BRAM_%02x_%02x\n",
	   site->type_coord.x,
	   site->type_coord.y >> 2);
  for (i = 0; i < 64; i++) {
    g_printf("INIT_%02x:",i);
    for (j = 0; j < 16; j++)
      g_printf("%04x", data[16*i + 15 - j]);
    g_printf("\n");
  }
}

/*
 * Get whether or not the input wire is significant
 */
typedef uint16_t lut_t;

static const lut_t array[4] = {
  0xAAAA, 0xCCCC, 0xf0f0, 0xff00,
};

static inline int
get_input_wire(const lut_t lut,
	       const unsigned wire) {
  const lut_t mask = array[wire];
  return ((lut & mask) >> (1<<wire)) ^ (lut & ~mask);
}

static void
print_lut_inputs(const lut_t lut) {
  int i;

  for(i = 0; i < 4; i++) {
    if (get_input_wire(lut,i))
      g_printf("A%i ",i+1);
  }
  g_printf("\n");
}

static void
print_lut_data(const csite_descr_t *site,
	       const chip_descr_t *chip,
	       const unsigned x, const unsigned y,
	       const guint16 data[]) {
  gchar sname[MAX_SITE_NLEN];
  guint i,j;

  sprint_csite(sname, site, x, y);
  g_printf("%s\n", sname);
  for (j = 0; j < 4; j++) {
    gchar slicen[MAX_SITE_NLEN];
    snprint_slice(slicen, MAX_SITE_NLEN, chip, site, j);
    g_printf("%s\n", slicen);
    for (i = 0; i < 2; i++) {
      lut_t lut = data[i|j<<1];
      g_printf("%s::#ROM:D=0x%04x\n", i ? "G" : "F", lut);
      print_lut_inputs(lut);
    }
  }
}

static void
print_switchpip_iter(gpointer data, const pip_t pip,
		     const site_ref_t site_ref) {
  bitstream_analyzed_t *bitstream = data;
  const wire_db_t *wiredb = bitstream->pipdb->wiredb;
  const chip_descr_t *chip = bitstream->chip;
  gchar site_buf[32];

  sprint_switch(site_buf, chip, site_ref);
  g_printf("pip %s %s -> %s\n", site_buf,
	   wire_name(wiredb,pip.source),
	   wire_name(wiredb,pip.target));
}

static void
print_all_pips(const bitstream_analyzed_t *bitstream,
	       const pip_parsed_dense_t *pipdat) {
  iterate_over_bitpips(pipdat, bitstream->chip, print_switchpip_iter,
		       (gpointer)bitstream);
}

static void
print_lut_iter(unsigned site_x, unsigned site_y,
	       csite_descr_t *site, gpointer dat) {
  bitstream_analyzed_t *analysis = dat;
  guint16 luts[8];
  query_bitstream_luts(analysis->bitstream, site, luts);
  print_lut_data(site,analysis->chip,site_x,site_y,luts);
}

static void
print_all_luts(const bitstream_analyzed_t *bitstream) {
  iterate_over_typed_sites(bitstream->chip, CLB, print_lut_iter, (gpointer)bitstream);
}

static void
print_bram_iter(unsigned site_x, unsigned site_y,
		csite_descr_t *site, gpointer dat) {
  bitstream_parsed_t *bitstream = dat;
  guint16 *bram;
  if ((site->type_coord.y & 0x3) != 0)
    return;
  bram = query_bitstream_bram_data(bitstream, site);
  print_bram_data(site,bram);
  debit_log(L_SITES, "Did BRAM %i x %i", site_x, site_y);
  g_free(bram);
}

static void
print_all_bram(const chip_descr_t *chip,
	       const bitstream_parsed_t *bitstream) {
  iterate_over_typed_sites(chip, BRAM, print_bram_iter, (gpointer)bitstream);
}

/** \brief Test function which dumps the pips of a bitstream on stdout.
 *
 * @param bitstream the bitstream data
 *
 */

void dump_pips(bitstream_analyzed_t *bitstream) {
  print_all_pips(bitstream, bitstream->pipdat);
}

/** \brief Test function which dumps the bram data of a bitstream on
 * stdout.
 *
 * @param bitstream the bitstream data
 *
 */

void dump_bram(bitstream_analyzed_t *bitstream) {
  print_all_bram(bitstream->chip, bitstream->bitstream);
}

/** \brief Test function which dumps the lut contents of a bitstream on
 * stdout.
 *
 * @param bitstream the bitstream data
 *
 */

void dump_luts(bitstream_analyzed_t *bitstream) {
  print_all_luts(bitstream);
}

typedef struct _dump_site {
  const gchar *odir;
  const gchar *suffix;
  const bitstream_parsed_t *parsed;
  gsize buffer_len;
  gchar *buffer;
} dump_site_t;

/** \brief Test function which dumps the nets of a bitstream to
 * stdout.
 *
 * @param nlz the parsed bitstream
 */

void dump_nets(const bitstream_analyzed_t *nlz) {
  nets_t * nets;
  /* Then do some work */
  print_design(&nlz->bitstream->header);
  nets = build_nets(nlz->pipdb, nlz->chip, nlz->pipdat);
  print_nets(nets, nlz->pipdb, nlz->chip);
  free_nets(nets);
}

static void
dump_site_iter(unsigned site_x, unsigned site_y,
	       csite_descr_t *site, gpointer dat) {
  dump_site_t *dumpsite = dat;
  gchar *buffer = dumpsite->buffer;
  gsize buffer_len = dumpsite->buffer_len;
  gchar *filename, *fullname, site_buf[32];
  gboolean ok;

  /* Get the bitstream contents */
  query_bitstream_site_data(buffer, buffer_len, dumpsite->parsed, site);

  sprint_csite(site_buf, site, site_x, site_y);
  filename = g_strconcat(site_buf,dumpsite->suffix,NULL);
  fullname = g_build_filename(dumpsite->odir, filename, NULL);
  g_free(filename);

  ok = g_file_set_contents(fullname, buffer, buffer_len, NULL);
  if (!ok)
    g_warning("Failed to dump %s", fullname);

  g_free(fullname);
}

/** \brief Test function which dumps the site configuration data in a
 * specific directory.
 *
 * @param bitstream the bitstream data
 * @param directory the directory where to put the dump files
 *
 */

void dump_sites(const bitstream_analyzed_t *nlz,
		const gchar *odir, const gchar *suffix) {
  dump_site_t dump = { .parsed = nlz->bitstream, .odir = odir, .suffix = suffix };
#if defined(VIRTEX2) || defined(SPARTAN3)
  site_type_t types[] =
	  { CLB,
	    TTERM, LTERM, BTERM, RTERM,
	    TTERMBRAM, BTERMBRAM,
	    TIOI, LIOI, BIOI, RIOI,
	    TIOIBRAM, BIOIBRAM, BRAM };
#elif defined(VIRTEX4) || defined(VIRTEX5)
  site_type_t types[] = { IOB, CLB, DSP48, GCLKC, BRAM };
#endif

  unsigned index;

  for (index = 0; index < G_N_ELEMENTS(types); index++) {
    site_type_t type = types[index];
    dump.buffer_len = query_bitstream_type_size(nlz->bitstream, type);
    dump.buffer = g_new(gchar, dump.buffer_len);
    iterate_over_typed_sites(nlz->chip, type, dump_site_iter, &dump);
    g_free(dump.buffer);
  }
}

/*
 * Allocation / unallocation functions
 * Maybe split this into analysis.c
 */
static void
unfill_analysis(bitstream_analyzed_t *anal) {
  pip_db_t *pipdb = anal->pipdb;
  chip_descr_t *chip = anal->chip;
  pip_parsed_dense_t *pipdat = anal->pipdat;

  if (pipdb)
    free_pipdb(pipdb);
  if (chip)
    release_chip(chip);
  if (pipdat)
    free_pipdat(pipdat);
}

void
free_analysis(bitstream_analyzed_t *anal) {
  unfill_analysis(anal);
  g_free(anal);
}

static int
fill_analysis(bitstream_analyzed_t *anal,
	      bitstream_parsed_t *bitstream,
	      const gchar *datadir) {
  pip_db_t *pipdb;
  chip_descr_t *chip;
  pip_parsed_dense_t *pipdat;
  const chip_struct_t *chip_struct = bitstream->chip_struct;

  anal->bitstream = bitstream;
  /* then fetch the databases */
  pipdb = get_pipdb(datadir);
  if (!pipdb)
    goto err_out;
  anal->pipdb = pipdb;

  chip = get_chip(datadir, chip_struct->chip);
  if (!chip)
    goto err_out;
  anal->chip = chip;

  pipdat = pips_of_bitstream(pipdb, chip, bitstream);
  if (!pipdat)
    goto err_out;
  anal->pipdat = pipdat;

  return 0;

 err_out:
  unfill_analysis(anal);
  return -1;
}

bitstream_analyzed_t *
analyze_bitstream(bitstream_parsed_t *bitstream,
		  const gchar *datadir) {
  bitstream_analyzed_t *anal = g_new0(bitstream_analyzed_t, 1);
  int err;

  err = fill_analysis(anal, bitstream, datadir);
  if (err) {
    g_free(anal);
    return NULL;
  }

  return anal;
}
