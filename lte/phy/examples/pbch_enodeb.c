/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2014 The libLTE Developers. See the
 * COPYRIGHT file at the top-level directory of this distribution.
 *
 * \section LICENSE
 *
 * This file is part of the libLTE library.
 *
 * libLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * A copy of the GNU Lesser General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "liblte/phy/phy.h"

#ifndef DISABLE_UHD
  #include "liblte/cuhd/cuhd.h"
  void *uhd;
#endif

  
lte_cell_t cell = {
  6,            // nof_prb
  1,            // nof_ports
  1,            // cell_id
  CPNORM        // cyclic prefix
};
  
char *output_file_name = NULL;
int nof_frames=-1;
char *uhd_args = "";

float uhd_amp=0.25, uhd_gain=10.0, uhd_freq=2400000000;

filesink_t fsink;
lte_fft_t ifft;
pbch_t pbch;

cf_t *sf_buffer = NULL, *output_buffer = NULL;
int sf_n_re, sf_n_samples;

#define UHD_SAMP_FREQ  1920000

void usage(char *prog) {
  printf("Usage: %s [agmfoncvp]\n", prog);
#ifndef DISABLE_UHD
  printf("\t-a UHD args [Default %s]\n", uhd_args);
  printf("\t-g UHD TX gain [Default %.2f dB]\n", uhd_gain);
  printf("\t-m UHD signal amplitude [Default %.2f]\n", uhd_amp);
  printf("\t-f UHD TX frequency [Default %.1f MHz]\n", uhd_freq/1000000);
#else
  printf("\t   UHD is disabled. CUHD library not available\n");
#endif
  printf("\t-o output_file [Default USRP]\n");
  printf("\t-n number of frames [Default %d]\n", nof_frames);
  printf("\t-c cell id [Default %d]\n", cell.id);
  printf("\t-p nof_prb [Default %d]\n", cell.nof_prb);
  printf("\t-v [set verbose to debug, default none]\n");
}

void parse_args(int argc, char **argv) {
  int opt;
  while ((opt = getopt(argc, argv, "agfmoncpv")) != -1) {
    switch(opt) {
    case 'a':
      uhd_args = argv[optind];
      break;
    case 'g':
      uhd_gain = atof(argv[optind]);
      break;
    case 'm':
      uhd_amp = atof(argv[optind]);
      break;
    case 'f':
      uhd_freq = atof(argv[optind]);
      break;
    case 'o':
      output_file_name = argv[optind];
      break;
    case 'n':
      nof_frames = atoi(argv[optind]);
      break;
    case 'p':
      cell.nof_prb = atoi(argv[optind]);
      break;
    case 'c':
      cell.id = atoi(argv[optind]);
      break;
    case 'v':
      verbose++;
      break;
    default:
      usage(argv[0]);
      exit(-1);
    }
  }
#ifdef DISABLE_UHD
  if (!output_file_name) {
    usage(argv[0]);
    exit(-1);
  }
#endif
}

void base_init() {
  /* init memory */
  sf_buffer = malloc(sizeof(cf_t) * sf_n_re);
  if (!sf_buffer) {
    perror("malloc");
    exit(-1);
  }
  output_buffer = malloc(sizeof(cf_t) * sf_n_samples);
  if (!output_buffer) {
    perror("malloc");
    exit(-1);
  }
  /* open file or USRP */
  if (output_file_name) {
    if (filesink_init(&fsink, output_file_name, COMPLEX_FLOAT_BIN)) {
      fprintf(stderr, "Error opening file %s\n", output_file_name);
      exit(-1);
    }
  } else {
#ifndef DISABLE_UHD
    printf("Opening UHD device...\n");
    if (cuhd_open(uhd_args,&uhd)) {
      fprintf(stderr, "Error opening uhd\n");
      exit(-1);
    }
#else
    printf("Error UHD not available. Select an output file\n");
    exit(-1);
#endif
  }

  /* create ifft object */
  if (lte_ifft_init(&ifft, CPNORM, cell.nof_prb)) {
    fprintf(stderr, "Error creating iFFT object\n");
    exit(-1);
  }
  if (pbch_init(&pbch, cell)) {
    fprintf(stderr, "Error creating PBCH object\n");
    exit(-1);
  }
}

void base_free() {

  pbch_free(&pbch);

  lte_ifft_free(&ifft);

  if (sf_buffer) {
    free(sf_buffer);
  }
  if (output_buffer) {
    free(output_buffer);
  }
  if (output_file_name) {
    filesink_free(&fsink);
  } else {
#ifndef DISABLE_UHD
    cuhd_close(&uhd);
#endif
  }
}

int main(int argc, char **argv) {
  int nf, sf_idx, N_id_2;
  cf_t pss_signal[PSS_LEN];
  float sss_signal0[SSS_LEN]; // for subframe 0
  float sss_signal5[SSS_LEN]; // for subframe 5
  pbch_mib_t mib;
  refsignal_t refs[NSLOTS_X_FRAME];
  int i, n;
  cf_t *sf_symbols[MAX_PORTS];


#ifdef DISABLE_UHD
  if (argc < 3) {
    usage(argv[0]);
    exit(-1);
  }
#endif

  parse_args(argc,argv);

  N_id_2 = cell.id%3;
  sf_n_re = 2 * CPNORM_NSYMB * cell.nof_prb * RE_X_RB;
  sf_n_samples = 2 * SLOT_LEN_CPNORM(lte_symbol_sz(cell.nof_prb));

  /* this *must* be called after setting slot_len_* */
  base_init();

  /* Generate PSS/SSS signals */
  pss_generate(pss_signal, N_id_2);
  sss_generate(sss_signal0, sss_signal5, cell.id);

  /* Generate CRS signals */
  lte_cell_t cell;
  cell.id = cell.id;
  cell.nof_prb = 6;
  cell.cp = CPNORM;
  cell.nof_ports = 1;
  
  for (i=0;i<NSLOTS_X_FRAME;i++) {
    if (refsignal_init_LTEDL(&refs[i], 0, i, cell)) {
      fprintf(stderr, "Error initiating CRS slot=%d\n", i);
      return -1;
    }
  }

  mib.nof_ports = cell.nof_ports;
  mib.nof_prb = cell.nof_prb;
  mib.phich_length = PHICH_NORM;
  mib.phich_resources = R_1;
  mib.sfn = 0;

  for (i=0;i<MAX_PORTS;i++) { // now there's only 1 port
    sf_symbols[i] = sf_buffer;
  }

#ifndef DISABLE_UHD
  if (!output_file_name) {
    printf("Set TX rate: %.2f MHz\n", cuhd_set_tx_srate(uhd, UHD_SAMP_FREQ)/1000000);
    printf("Set TX gain: %.1f dB\n", cuhd_set_tx_gain(uhd, uhd_gain));
    printf("Set TX freq: %.2f MHz\n", cuhd_set_tx_freq(uhd, uhd_freq)/1000000);
  }
#endif

  nf = 0;

  while(nf<nof_frames || nof_frames == -1) {
    for (sf_idx=0;sf_idx<NSUBFRAMES_X_FRAME;sf_idx++) {
      bzero(sf_buffer, sizeof(cf_t) * sf_n_re);

      if (sf_idx == 0 || sf_idx == 5) {
        pss_put_slot(pss_signal, sf_buffer, cell.nof_prb, CPNORM);
        sss_put_slot(sf_idx?sss_signal5:sss_signal0, sf_buffer, cell.nof_prb, CPNORM);
      }
      
      if (sf_idx == 0) {
        pbch_encode(&pbch, &mib, sf_symbols);
      }
      
      for (n=0;n<2;n++) {
        refsignal_put(&refs[2*sf_idx+n], &sf_buffer[n*sf_n_re/2]);
      }

      /* Transform to OFDM symbols */
      lte_ifft_run_sf(&ifft, sf_buffer, output_buffer);

      /* send to file or usrp */
      if (output_file_name) {
        filesink_write(&fsink, output_buffer, sf_n_samples);
        usleep(5000);
      } else {
#ifndef DISABLE_UHD
        vec_sc_prod_cfc(output_buffer, uhd_amp, output_buffer, sf_n_samples);
        cuhd_send(uhd, output_buffer, sf_n_samples, 1);
#endif
      }
    }
    mib.sfn=(mib.sfn+1)%1024;
    printf("SFN: %4d\r", mib.sfn);fflush(stdout);
    nf++;
  }

  base_free();

  printf("Done\n");
  exit(0);
}
