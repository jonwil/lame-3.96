/* -*- mode: C; mode: fold -*- */
/*
 *	LAME MP3 encoding engine
 *
 *	Copyright (c) 1999 Mark Taylor
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/* $Id: lame.c,v 1.261 2004/03/23 01:57:17 olcios Exp $ */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif


#include <assert.h>
#include "lame-analysis.h"
#include "lame.h"
#include "util.h"
#include "bitstream.h"
#include "version.h"
#include "tables.h"
#include "quantize_pvt.h"
#include "psymodel.h"
#include "VbrTag.h"
#include "machine.h"
#include "gain_analysis.h"
#include "set_get.h"

#if defined(__FreeBSD__) && !defined(__alpha__)
#include <floatingpoint.h>
#endif
#ifdef __riscos__
#include "asmstuff.h"
#endif

#ifdef WITH_DMALLOC
#include <dmalloc.h>
#endif

#ifdef __sun__
/* woraround for SunOS 4.x, it has SEEK_* defined here */
#include <unistd.h>
#endif


#define LAME_DEFAULT_QUALITY 3

static FLOAT8
filter_coef(FLOAT8 x)
{
    if (x > 1.0) return 0.0;
    if (x <= 0.0) return 1.0;

    return cos(PI/2 * x);
}

static void
lame_init_params_ppflt(lame_global_flags * gfp)
{
    lame_internal_flags *gfc = gfp->internal_flags;
    /***************************************************************/
    /* compute info needed for polyphase filter (filter type==0, default) */
    /***************************************************************/

    int     band, maxband, minband;
    FLOAT   freq;
    int lowpass_band = 32;
    int highpass_band = -1;

    if (gfc->lowpass1 > 0) {
	minband = 999;
	for (band = 0; band <= 31; band++) {
	    freq = band / 31.0;
	    /* this band and above will be zeroed: */
	    if (freq >= gfc->lowpass2) {
		lowpass_band = Min(lowpass_band, band);
	    }
	    if (gfc->lowpass1 < freq && freq < gfc->lowpass2) {
		minband = Min(minband, band);
	    }
	}

        /* compute the *actual* transition band implemented by
         * the polyphase filter */
        if (minband == 999) {
            gfc->lowpass1 = (lowpass_band - .75) / 31.0;
        }
        else {
            gfc->lowpass1 = (minband - .75) / 31.0;
        }
        gfc->lowpass2 = lowpass_band / 31.0;
    }

    /* make sure highpass filter is within 90% of what the effective
     * highpass frequency will be */
    if (gfc->highpass2 > 0) {
        if (gfc->highpass2 < .9 * (.75 / 31.0)) {
            gfc->highpass1 = 0;
            gfc->highpass2 = 0;
            MSGF(gfc, "Warning: highpass filter disabled.  "
                 "highpass frequency too small\n");
        }
    }

    if (gfc->highpass2 > 0) {
        maxband = -1;
        for (band = 0; band <= 31; band++) {
            freq = band / 31.0;
            /* this band and below will be zereod */
            if (freq <= gfc->highpass1) {
                highpass_band = Max(highpass_band, band);
            }
            if (gfc->highpass1 < freq && freq < gfc->highpass2) {
                maxband = Max(maxband, band);
            }
        }
        /* compute the *actual* transition band implemented by
         * the polyphase filter */
        gfc->highpass1 = highpass_band / 31.0;
        if (maxband == -1) {
            gfc->highpass2 = (highpass_band + .75) / 31.0;
        }
        else {
            gfc->highpass2 = (maxband + .75) / 31.0;
        }
    }

    for (band = 0; band < 32; band++) {
	freq = band / 31.0;
	gfc->amp_filter[band]
	    = filter_coef((gfc->highpass2 - freq)
			  / (gfc->highpass2 - gfc->highpass1 + 1e-37))
	    * filter_coef((freq - gfc->lowpass1)
			  / (gfc->lowpass2 - gfc->lowpass1 - 1e-37));
    }
}


static void
optimum_bandwidth(double *const lowerlimit,
                  double *const upperlimit,
                  const unsigned bitrate)
{
/* 
 *  Input:
 *      bitrate     total bitrate in kbps
 *
 *   Output:
 *      lowerlimit: best lowpass frequency limit for input filter in Hz
 *      upperlimit: best highpass frequency limit for input filter in Hz
 */
    int index;

    typedef struct {
        int bitrate; /* only indicative value */
        int lowpass;
    } band_pass_t;

    const band_pass_t freq_map[] = {
        {  8,  2000},
        { 16,  3700},
        { 24,  3900},
        { 32,  5500},
        { 40,  7000},
        { 48,  7500},
        { 56, 10000},
        { 64, 11000},
        { 80, 13500},
        { 96, 15300},
        {112, 16000},
        {128, 17500},
        {160, 18000},
        {192, 19500},
        {224, 20000},
        {256, 20500},
        {320, 20500}
    };


    index = nearestBitrateFullIndex(bitrate);

    *lowerlimit = freq_map[index].lowpass;


/*
 *  Now we try to choose a good high pass filtering frequency.
 *  This value is currently not used.
 *    For fu < 16 kHz:  sqrt(fu*fl) = 560 Hz
 *    For fu = 18 kHz:  no high pass filtering
 *  This gives:
 *
 *   2 kHz => 160 Hz
 *   3 kHz => 107 Hz
 *   4 kHz =>  80 Hz
 *   8 kHz =>  40 Hz
 *  16 kHz =>  20 Hz
 *  17 kHz =>  10 Hz
 *  18 kHz =>   0 Hz
 *
 *  These are ad hoc values and these can be optimized if a high pass is available.
 */
/*    if (f_low <= 16000)
        f_high = 16000. * 20. / f_low;
    else if (f_low <= 18000)
        f_high = 180. - 0.01 * f_low;
    else
        f_high = 0.;*/

    /*  
     *  When we sometimes have a good highpass filter, we can add the highpass
     *  frequency to the lowpass frequency
     */

    /*if (upperlimit != NULL)
        *upperlimit = f_high;*/
}


static int
optimum_samplefreq(int lowpassfreq, int input_samplefreq)
{
/*
 * Rules:
 *  - if possible, sfb21 should NOT be used
 *
 */
    int suggested_samplefreq = input_samplefreq;

    if (lowpassfreq == -1)
        return suggested_samplefreq;

    if (lowpassfreq <= 15960)
        suggested_samplefreq = 44100;
    if (lowpassfreq <= 15250)
        suggested_samplefreq = 32000;
    if (lowpassfreq <= 11220)
        suggested_samplefreq = 24000;
    if (lowpassfreq <= 9970)
        suggested_samplefreq = 22050;
    if (lowpassfreq <= 7230)
        suggested_samplefreq = 16000;
    if (lowpassfreq <= 5420)
        suggested_samplefreq = 12000;
    if (lowpassfreq <= 4510)
        suggested_samplefreq = 11025;
    if (lowpassfreq <= 3970)
        suggested_samplefreq = 8000;

    if (input_samplefreq < suggested_samplefreq)
        suggested_samplefreq = input_samplefreq;
    return suggested_samplefreq;
}





/* set internal feature flags.  USER should not access these since
 * some combinations will produce strange results */
void
lame_init_qval(lame_global_flags * gfp)
{
    lame_internal_flags *gfc = gfp->internal_flags;

    switch (gfp->quality) {
    case 9:            /* no psymodel, no noise shaping */
        gfc->filter_type = 0;
        gfc->psymodel = 0;
        gfc->quantization = 0;
        gfc->noise_shaping = 0;
        gfc->noise_shaping_amp = 0;
        gfc->noise_shaping_stop = 0;
        gfc->use_best_huffman = 0;
        break;

    case 8:
        gfp->quality = 7;
    case 7:            /* use psymodel (for short block and m/s switching), but no noise shapping */
        gfc->filter_type = 0;
        gfc->psymodel = 1;
        gfc->quantization = 0;
        gfc->noise_shaping = 0;
        gfc->noise_shaping_amp = 0;
        gfc->noise_shaping_stop = 0;
        gfc->use_best_huffman = 0;
        break;

    case 6:
        gfc->filter_type = 0;
        gfc->psymodel = 1;
        gfc->quantization = 0;
        if (gfc->noise_shaping == 0)
            gfc->noise_shaping = 1;
        gfc->noise_shaping_amp = 0;
        gfc->noise_shaping_stop = 0;
        if (gfc->subblock_gain == -1)
            gfc->subblock_gain = 1;
        gfc->use_best_huffman = 0;
        break;

    case 5:
        gfc->filter_type = 0;
        gfc->psymodel = 1;
        gfc->quantization = 1;
        if (gfc->noise_shaping == 0)
            gfc->noise_shaping = 1;
        gfc->noise_shaping_amp = 0;
        gfc->noise_shaping_stop = 0;
        if (gfc->subblock_gain == -1)
            gfc->subblock_gain = 1;
        gfc->use_best_huffman = 0;
        break;

    case 4:
        gfc->filter_type = 0;
        gfc->psymodel = 1;
        gfc->quantization = 1;
        if (gfc->noise_shaping == 0)
            gfc->noise_shaping = 1;
        gfc->noise_shaping_amp = 0;
        gfc->noise_shaping_stop = 0;
        if (gfc->subblock_gain == -1)
            gfc->subblock_gain = 1;
        gfc->use_best_huffman = 1;
        break;

    case 3:
        gfc->filter_type = 0;
        gfc->psymodel = 1;
        gfc->quantization = 1;
        if (gfc->noise_shaping == 0)
            gfc->noise_shaping = 1;
        gfc->noise_shaping_amp = 1;
        gfc->noise_shaping_stop = 1;
        if (gfc->subblock_gain == -1)
            gfc->subblock_gain = 1;
        gfc->use_best_huffman = 1;
        break;

    case 2:
        gfc->filter_type = 0;
        gfc->psymodel = 1;
        gfc->quantization = 1;
        if (gfc->noise_shaping == 0)
            gfc->noise_shaping = 1;
        if (gfc->substep_shaping != 0)
	        gfc->substep_shaping = 2;
        gfc->noise_shaping_amp = 1;
        gfc->noise_shaping_stop = 1;
        if (gfc->subblock_gain == -1)
            gfc->subblock_gain = 1;
        gfc->use_best_huffman = 1; /* inner loop */
        break;

    case 1:
        gfc->filter_type = 0; /* 1 not yet coded */
        gfc->psymodel = 1;
        gfc->quantization = 1;
        if (gfc->noise_shaping == 0)
            gfc->noise_shaping = 1;
        if (gfc->substep_shaping != 0)
	        gfc->substep_shaping = 2;
        gfc->noise_shaping_amp = 2;
        gfc->noise_shaping_stop = 1;
        if (gfc->subblock_gain == -1)
            gfc->subblock_gain = 1;
        gfc->use_best_huffman = 1;
        break;

    case 0:
        gfc->filter_type = 0; /* 1 not yet coded */
        gfc->psymodel = 1;
        gfc->quantization = 1;
        if (gfc->noise_shaping == 0)
            gfc->noise_shaping = 1;
        if (gfc->substep_shaping != 0)
	        gfc->substep_shaping = 2;
        gfc->noise_shaping_amp = 2;
        gfc->noise_shaping_stop = 1;
        if (gfc->subblock_gain == -1)
            gfc->subblock_gain = 1;
        gfc->use_best_huffman = 2;
        break;
    }
    
}







/********************************************************************
 *   initialize internal params based on data in gf
 *   (globalflags struct filled in by calling program)
 *
 *  OUTLINE:
 *
 * We first have some complex code to determine bitrate, 
 * output samplerate and mode.  It is complicated by the fact
 * that we allow the user to set some or all of these parameters,
 * and need to determine best possible values for the rest of them:
 *
 *  1. set some CPU related flags
 *  2. check if we are mono->mono, stereo->mono or stereo->stereo
 *  3.  compute bitrate and output samplerate:
 *          user may have set compression ratio
 *          user may have set a bitrate  
 *          user may have set a output samplerate
 *  4. set some options which depend on output samplerate
 *  5. compute the actual compression ratio
 *  6. set mode based on compression ratio
 *
 *  The remaining code is much simpler - it just sets options
 *  based on the mode & compression ratio: 
 *   
 *   set allow_diff_short based on mode
 *   select lowpass filter based on compression ratio & mode
 *   set the bitrate index, and min/max bitrates for VBR modes
 *   disable VBR tag if it is not appropriate
 *   initialize the bitstream
 *   initialize scalefac_band data
 *   set sideinfo_len (based on channels, CRC, out_samplerate)
 *   write an id3v2 tag into the bitstream
 *   write VBR tag into the bitstream
 *   set mpeg1/2 flag
 *   estimate the number of frames (based on a lot of data)
 *         
 *   now we set more flags:
 *   nspsytune:
 *      see code
 *   VBR modes
 *      see code      
 *   CBR/ABR
 *      see code   
 *
 *  Finally, we set the algorithm flags based on the gfp->quality value
 *  lame_init_qval(gfp);
 *
 ********************************************************************/
int
lame_init_params(lame_global_flags * const gfp)
{

    int     i;
    int     j;
    lame_internal_flags *gfc = gfp->internal_flags;

    gfc->gfp = gfp;

    gfc->Class_ID = 0;

    /* report functions */
    gfc->report.msgf   = gfp->report.msgf;
    gfc->report.debugf = gfp->report.debugf;
    gfc->report.errorf = gfp->report.errorf;


    if (gfp->asm_optimizations.amd3dnow )
        gfc->CPU_features.AMD_3DNow = has_3DNow();
    else
        gfc->CPU_features.AMD_3DNow = 0;

    if (gfp->asm_optimizations.mmx )
        gfc->CPU_features.MMX = has_MMX();
    else 
        gfc->CPU_features.MMX = 0;

    if (gfp->asm_optimizations.sse ) {
        gfc->CPU_features.SSE = has_SSE();
        gfc->CPU_features.SSE2 = has_SSE2();
    } else {
        gfc->CPU_features.SSE = 0;
        gfc->CPU_features.SSE2 = 0;
    }


    if (NULL == gfc->ATH)
        gfc->ATH = calloc(1, sizeof(ATH_t));

    if (NULL == gfc->ATH)
        return -2;  /* maybe error codes should be enumerated in lame.h ?? */

    if (NULL == gfc->VBR)
        gfc->VBR = calloc(1, sizeof(VBR_t));
    if (NULL == gfc->VBR)
        return -2;
        
    if (NULL == gfc->PSY)
        gfc->PSY = calloc(1, sizeof(PSY_t));
    if (NULL == gfc->PSY)
        return -2;
        
    if (NULL == gfc->rgdata)
        gfc->rgdata = calloc(1, sizeof(replaygain_t));
    if (NULL == gfc->rgdata)
        return -2;
        
    gfc->channels_in = gfp->num_channels;
    if (gfc->channels_in == 1)
        gfp->mode = MONO;
    gfc->channels_out = (gfp->mode == MONO) ? 1 : 2;
    gfc->mode_ext = MPG_MD_MS_LR;
    if (gfp->mode == MONO)
        gfp->force_ms = 0; /* don't allow forced mid/side stereo for mono output */


    if (gfp->VBR != vbr_off)
        gfp->free_format = 0; /* VBR can't be mixed with free format */

    if (gfp->VBR == vbr_off && gfp->brate == 0) {
        /* no bitrate or compression ratio specified, use 11.025 */
        if (gfp->compression_ratio == 0)
            gfp->compression_ratio = 11.025; /* rate to compress a CD down to exactly 128000 bps */
    }

    /* find bitrate if user specify a compression ratio */
    if (gfp->VBR == vbr_off && gfp->compression_ratio > 0) {

        if (gfp->out_samplerate == 0)
            gfp->out_samplerate = map2MP3Frequency( (int)( 0.97 * gfp->in_samplerate ) ); /* round up with a margin of 3% */

        /* choose a bitrate for the output samplerate which achieves
         * specified compression ratio 
         */
        gfp->brate =
            gfp->out_samplerate * 16 * gfc->channels_out / (1.e3 *
                                                            gfp->
                                                            compression_ratio);

        /* we need the version for the bitrate table look up */
        gfc->samplerate_index = SmpFrqIndex(gfp->out_samplerate, &gfp->version);

        if (!gfp->free_format) /* for non Free Format find the nearest allowed bitrate */
            gfp->brate =
                FindNearestBitrate(gfp->brate, gfp->version);
    }

    if (gfp->VBR != vbr_off && gfp->brate >= 320)
        gfp->VBR = vbr_off; /* at 160 kbps (MPEG-2/2.5)/ 320 kbps (MPEG-1) only Free format or CBR are possible, no VBR */




  /****************************************************************/
  /* if a filter has not been enabled, see if we should add one: */
  /****************************************************************/
    if (gfp->lowpassfreq == 0) {
        double lowpass = 16000;
        double highpass;

        switch (gfp->VBR) {
        case vbr_off: {
            optimum_bandwidth(&lowpass,
                              &highpass,
                              gfp->brate);
            break;
        }
        case vbr_abr: {
            optimum_bandwidth(&lowpass,
                              &highpass,
                              gfp->VBR_mean_bitrate_kbps);
            break;
        }
        default: {
            switch (gfp->VBR_q) {
            case 9: {
                lowpass = 10000;
               break;
            }
            case 8: {
                lowpass = 12500;
                break;
            }
            case 7: {
                lowpass = 14900;
                break;
            }
            case 6: {
                lowpass = 16000;
                break;
            }
            case 5: {
                lowpass = 17000;
                break;
            }
            case 4: {
                lowpass = 18000;
                break;
            }
            case 3: {
                lowpass = 18000;
                break;
            }
            case 2: {
                lowpass = 19000;
                break;
            }
            case 1: {
                lowpass = 19000;
                break;
            }
            case 0: {
                lowpass = 19500;
                break;
            }
            }
        }
        }

        if (gfp->mode == MONO && (gfp->VBR == vbr_off || gfp->VBR == vbr_abr))
            lowpass *= 1.5;
        
        gfp->lowpassfreq = lowpass;
    }


    if (gfp->out_samplerate == 0)
        gfp->out_samplerate = optimum_samplefreq( (int)gfp->lowpassfreq, gfp->in_samplerate);

    gfp->lowpassfreq = Min(20500, gfp->lowpassfreq);
    gfp->lowpassfreq = Min(gfp->out_samplerate / 2, gfp->lowpassfreq);

    if (gfp->VBR == vbr_off) {
        gfp->compression_ratio =
            gfp->out_samplerate * 16 * gfc->channels_out / (1.e3 *
                                                        gfp->brate);
    }
    if (gfp->VBR == vbr_abr) {
        gfp->compression_ratio =
            gfp->out_samplerate * 16 * gfc->channels_out / (1.e3 *
                                                            gfp->
                                                            VBR_mean_bitrate_kbps);
    }

    /* do not compute ReplayGain values and do not find the peak sample
       if we can't store them */
    if (!gfp->bWriteVbrTag){
        gfp->findReplayGain = 0;
	gfp->decode_on_the_fly = 0;
        gfc->findPeakSample = 0;
    }

    if (gfp->decode_on_the_fly) 
      gfc->findPeakSample = 1;

    if (gfp->findReplayGain) {
      if (InitGainAnalysis(gfc->rgdata, gfp->out_samplerate) == INIT_GAIN_ANALYSIS_ERROR)
        return -6;
    }

#ifdef DECODE_ON_THE_FLY
    if (gfp->decode_on_the_fly && !gfp->decode_only)
      lame_decode_init();  /* initialize the decoder  */
#endif

    gfc->mode_gr = gfp->out_samplerate <= 24000 ? 1 : 2; /* Number of granules per frame */
    gfp->framesize = 576 * gfc->mode_gr;
    gfp->encoder_delay = ENCDELAY;
    gfc->frame_size = gfp->framesize;

    gfc->resample_ratio = (double) gfp->in_samplerate / gfp->out_samplerate;

    /* 
     *  sample freq       bitrate     compression ratio
     *     [kHz]      [kbps/channel]   for 16 bit input
     *     44.1            56               12.6
     *     44.1            64               11.025
     *     44.1            80                8.82
     *     22.05           24               14.7
     *     22.05           32               11.025
     *     22.05           40                8.82
     *     16              16               16.0
     *     16              24               10.667
     *
     */
    /* 
     *  For VBR, take a guess at the compression_ratio. 
     *  For example:
     *
     *    VBR_q    compression     like
     *     -        4.4         320 kbps/44 kHz
     *   0...1      5.5         256 kbps/44 kHz
     *     2        7.3         192 kbps/44 kHz
     *     4        8.8         160 kbps/44 kHz
     *     6       11           128 kbps/44 kHz
     *     9       14.7          96 kbps
     *
     *  for lower bitrates, downsample with --resample
     */

    switch (gfp->VBR) {
    case vbr_mt:
    case vbr_rh:
    case vbr_mtrh:
        {
            /*numbers are a bit strange, but they determine the lowpass value*/
            FLOAT  cmp[] = { 5.7, 6.5, 7.3, 8.2, 10, 11.9, 13, 14, 15, 16.5 };
            gfp->compression_ratio = cmp[gfp->VBR_q];
        }
        break;
    case vbr_abr:
        gfp->compression_ratio =
            gfp->out_samplerate * 16 * gfc->channels_out / (1.e3 *
                                                            gfp->
                                                            VBR_mean_bitrate_kbps);
        break;
    default:
        gfp->compression_ratio =
            gfp->out_samplerate * 16 * gfc->channels_out / (1.e3 * gfp->brate);
        break;
    }


    /* mode = -1 (not set by user) or 
     * mode = MONO (because of only 1 input channel).  
     * If mode has not been set, then select J-STEREO
    */
    if (gfp->mode == NOT_SET) {
        gfp->mode = JOINT_STEREO;
    }


    /* apply user driven high pass filter */
    if (gfp->highpassfreq > 0) {
        gfc->highpass1 = 2. * gfp->highpassfreq;

        if (gfp->highpasswidth >= 0)
            gfc->highpass2 =
                2. * (gfp->highpassfreq + gfp->highpasswidth);
        else            /* 0% above on default */
            gfc->highpass2 =
		(1 + 0.00) * 2. * gfp->highpassfreq;

	gfc->highpass1 /= gfp->out_samplerate;
	gfc->highpass2 /= gfp->out_samplerate;
    }

    /* apply user driven low pass filter */
    if (gfp->lowpassfreq > 0) {
	gfc->lowpass2 = 2. * gfp->lowpassfreq;
        if (gfp->lowpasswidth >= 0) {
            gfc->lowpass1 =
                2. * (gfp->lowpassfreq - gfp->lowpasswidth);
            if (gfc->lowpass1 < 0) /* has to be >= 0 */
                gfc->lowpass1 = 0;
        }
        else {          /* 0% below on default */
            gfc->lowpass1 =
                (1 - 0.00) * 2. * gfp->lowpassfreq;
        }
	gfc->lowpass1 /= gfp->out_samplerate;
	gfc->lowpass2 /= gfp->out_samplerate;
    }




  /**********************************************************************/
  /* compute info needed for polyphase filter (filter type==0, default) */
  /**********************************************************************/
    lame_init_params_ppflt(gfp);


  /*******************************************************/
  /* compute info needed for FIR filter (filter_type==1) */
  /*******************************************************/
   /* not yet coded */



  /*******************************************************
   * samplerate and bitrate index
   *******************************************************/
    gfc->samplerate_index = SmpFrqIndex(gfp->out_samplerate, &gfp->version);
    if (gfc->samplerate_index < 0)
        return -1;

    if (gfp->VBR == vbr_off) {
        if (gfp->free_format) {
            gfc->bitrate_index = 0;
        }
        else {
            gfc->bitrate_index = BitrateIndex(gfp->brate, gfp->version,
                                              gfp->out_samplerate);
            if (gfc->bitrate_index < 0)
                return -1;
        }
    }

    /* for CBR, we will write an "info" tag. */
    /*    if ((gfp->VBR == vbr_off))  */
    /*  gfp->bWriteVbrTag = 0; */

#if defined(HAVE_GTK)
    if (gfp->analysis)
        gfp->bWriteVbrTag = 0;

    /* some file options not allowed if output is: not specified or stdout */
    if (gfc->pinfo != NULL)
        gfp->bWriteVbrTag = 0; /* disable Xing VBR tag */
#endif

    init_bit_stream_w(gfc);

    j = gfc->samplerate_index
	+ (3 * gfp->version) + 6 * (gfp->out_samplerate < 16000);
    for (i = 0; i < SBMAX_l + 1; i++)
        gfc->scalefac_band.l[i] = sfBandIndex[j].l[i];

    for (i = 0; i < PSFB21 + 1; i++){
        int size = (gfc->scalefac_band.l[ 22 ] - gfc->scalefac_band.l[ 21 ])/PSFB21;
        int start = gfc->scalefac_band.l[ 21 ] + i*size;
        gfc->scalefac_band.psfb21[i] = start;
    }
    gfc->scalefac_band.psfb21[PSFB21] = 576;

    for (i = 0; i < SBMAX_s + 1; i++)
        gfc->scalefac_band.s[i] = sfBandIndex[j].s[i];

    for (i = 0; i < PSFB12 + 1; i++){
        int size = (gfc->scalefac_band.s[ 13 ] - gfc->scalefac_band.s[ 12 ])/PSFB12;
        int start = gfc->scalefac_band.s[ 12 ] + i*size;
        gfc->scalefac_band.psfb12[i] = start;
    }
    gfc->scalefac_band.psfb12[PSFB12] = 192;

    /* determine the mean bitrate for main data */
    if (gfp->version == 1) /* MPEG 1 */
        gfc->sideinfo_len = (gfc->channels_out == 1) ? 4 + 17 : 4 + 32;
    else                /* MPEG 2 */
        gfc->sideinfo_len = (gfc->channels_out == 1) ? 4 + 9 : 4 + 17;

    if (gfp->error_protection)
        gfc->sideinfo_len += 2;

    lame_init_bitstream(gfp);

    gfc->Class_ID = LAME_ID;

    /*if (gfp->exp_nspsytune & 1)*/ {
        int     i;

        for (i = 0; i < 19; i++)
            gfc->nsPsy.pefirbuf[i] = 700*gfc->mode_gr*gfc->channels_out;

        if (gfp->ATHtype == -1)
            gfp->ATHtype = 4;

	if (gfp->exp_nspsytune2.pointer[0])
	    gfc->nsPsy.pass1fp = gfp->exp_nspsytune2.pointer[0];
	else
	    gfc->nsPsy.pass1fp = NULL;
    }

    assert( gfp->VBR_q <= 9 );
    assert( gfp->VBR_q >= 0 );
    
    gfc->PSY->tonalityPatch = 0;
  
    switch (gfp->VBR) {

    case vbr_mt:
        gfp->VBR = vbr_mtrh;
        
    case vbr_mtrh: {

        apply_preset(gfp, 500 - (gfp->VBR_q*10), 0);
        
        if (gfp->quality < 0) gfp->quality = LAME_DEFAULT_QUALITY;
        if (gfp->quality > 7) {
            gfp->quality = 7;     /* needs psymodel */
            ERRORF( gfc, "VBR needs a psymodel, switching to quality level 7\n");
        }

        /*  tonality
         */
        if (gfp->cwlimit <= 0) gfp->cwlimit = 0.42 * gfp->out_samplerate;
        gfc->PSY->tonalityPatch = 1;

        gfc->PSY->mask_adjust = gfp->maskingadjust;
        gfc->PSY->mask_adjust_short = gfp->maskingadjust_short;
        if (gfp->VBR_smooth < 0)
            gfp->VBR_smooth = 0;
        gfc->VBR->smooth = gfp->VBR_smooth;

        
        /*  sfb21 extra only with MPEG-1 at higher sampling rates
         */
        if ( gfp->experimentalY )
            gfc->sfb21_extra = 0;
        else 
            gfc->sfb21_extra = (gfp->out_samplerate > 44000);

        
        if ( gfp->athaa_type < 0 )
            gfc->ATH->use_adjust = 3;
        else
            gfc->ATH->use_adjust = gfp->athaa_type;
        
        break;
        
    }
    case vbr_rh: {

        apply_preset(gfp, 500 - (gfp->VBR_q*10), 0);
        
        gfc->PSY->mask_adjust = gfp->maskingadjust;
        gfc->PSY->mask_adjust_short = gfp->maskingadjust_short;

        if ( gfp->psymodel == PSY_GPSYCHO )
            gfc->PSY->tonalityPatch = 1;
        

        /*  automatic ATH adjustment on, VBR modes need it
         */
        if ( gfp->athaa_type < 0 )
            gfc->ATH->use_adjust = 3;
        else
            gfc->ATH->use_adjust = gfp->athaa_type;

        /*  sfb21 extra only with MPEG-1 at higher sampling rates
         */
        if ( gfp->experimentalY )
            gfc->sfb21_extra = 0;
        else 
            gfc->sfb21_extra = (gfp->out_samplerate > 44000);

        /*  VBR needs at least the output of GPSYCHO,
         *  so we have to garantee that by setting a minimum 
         *  quality level, actually level 6 does it.
         *  down to level 6
         */
        if (gfp->quality > 6)
            gfp->quality = 6;


        if (gfp->quality < 0)
            gfp->quality = LAME_DEFAULT_QUALITY;

        break;
    }

    default: /* cbr/abr */{
        vbr_mode vbrmode;

        /*  automatic ATH adjustment off by default
         *  not so important for CBR code?
         */
        if ( gfp->athaa_type < 0 )
            gfc->ATH->use_adjust = 0;
        else
            gfc->ATH->use_adjust = gfp->athaa_type;


        /*  no sfb21 extra with CBR code
         */
        gfc->sfb21_extra = 0;

        if (gfp->quality < 0)
            gfp->quality = LAME_DEFAULT_QUALITY;


        vbrmode = lame_get_VBR(gfp);
        if (vbrmode == vbr_off)
            lame_set_VBR_mean_bitrate_kbps(gfp, gfp->brate);
        /* second, set parameters depending on bitrate */
        apply_preset(gfp, gfp->VBR_mean_bitrate_kbps, 0);
        lame_set_VBR(gfp, vbrmode);

        break;
    }
    }

    /*initialize default values common for all modes*/


    if (lame_get_VBR(gfp) != vbr_off) {              /* choose a min/max bitrate for VBR */
        /* if the user didn't specify VBR_max_bitrate: */
        gfc->VBR_min_bitrate = 1; /* default: allow   8 kbps (MPEG-2) or  32 kbps (MPEG-1) */
        gfc->VBR_max_bitrate = 14; /* default: allow 160 kbps (MPEG-2) or 320 kbps (MPEG-1) */

        if (gfp->VBR_min_bitrate_kbps)
            if (
                (gfc->VBR_min_bitrate =
                 BitrateIndex(gfp->VBR_min_bitrate_kbps, gfp->version,
                              gfp->out_samplerate)) < 0) return -1;
        if (gfp->VBR_max_bitrate_kbps)
            if (
                (gfc->VBR_max_bitrate =
                 BitrateIndex(gfp->VBR_max_bitrate_kbps, gfp->version,
                              gfp->out_samplerate)) < 0) return -1;

        gfp->VBR_min_bitrate_kbps =
            bitrate_table[gfp->version][gfc->VBR_min_bitrate];
        gfp->VBR_max_bitrate_kbps =
            bitrate_table[gfp->version][gfc->VBR_max_bitrate];

        gfp->VBR_mean_bitrate_kbps =
            Min(bitrate_table[gfp->version][gfc->VBR_max_bitrate],
                gfp->VBR_mean_bitrate_kbps);
        gfp->VBR_mean_bitrate_kbps =
            Max(bitrate_table[gfp->version][gfc->VBR_min_bitrate],
                gfp->VBR_mean_bitrate_kbps);


    }


    /*  just another daily changing developer switch  */
    if ( gfp->tune ) gfc->PSY->mask_adjust = gfp->tune_value_a;

    /* initialize internal qval settings */
    lame_init_qval(gfp);


    /* initialize internal adaptive ATH settings  -jd */
    gfc->ATH->aa_sensitivity_p = pow( 10.0, gfp->athaa_sensitivity / -10.0 );


    gfc->PSY->cwlimit = gfp->cwlimit <= 0 ? 8871.7f : gfp->cwlimit;
    
    if (gfp->short_blocks == short_block_not_set) {
        gfp->short_blocks =  short_block_allowed;
    }

    /*Note Jan/2003: Many hardware decoders cannot handle short blocks in regular
      stereo mode unless they are coupled (same type in both channels)
      it is a rare event (1 frame per min. or so) that LAME would use
      uncoupled short blocks, so lets turn them off until we decide
      how to handle this.  No other encoders allow uncoupled short blocks,
      even though it is in the standard.  */
    /* rh 20040217: coupling makes no sense for mono and dual-mono streams
     */
    if (gfp->short_blocks == short_block_allowed && (gfp->mode == JOINT_STEREO || gfp->mode == STEREO) ) {
        gfp->short_blocks =  short_block_coupled;
    } 

    
    if (lame_get_quant_comp(gfp) < 0 ) lame_set_quant_comp(gfp, 1);
    if (lame_get_quant_comp_short(gfp) < 0 ) lame_set_quant_comp_short(gfp, 0);

    if (lame_get_msfix(gfp) < 0 ) lame_set_msfix(gfp, 0);

    /* select psychoacoustic model */
    if ((lame_get_psy_model(gfp) < 0) ||
        (lame_get_psy_model(gfp) == PSY_NSPSYTUNE)) {
        lame_set_psy_model(gfp, PSY_NSPSYTUNE);
        lame_set_exp_nspsytune(gfp, lame_get_exp_nspsytune(gfp) | 1);
    } else {
        lame_set_psy_model(gfp, PSY_GPSYCHO);
        lame_set_exp_nspsytune(gfp, (lame_get_exp_nspsytune(gfp) >> 1) << 1);
    }


    if (lame_get_short_threshold_lrm(gfp) < 0 ) lame_set_short_threshold_lrm(gfp, NSATTACKTHRE);
    if (lame_get_short_threshold_s(gfp) < 0 ) lame_set_short_threshold_s(gfp, NSATTACKTHRE_S);

    if ( gfp->scale < 0 ) gfp->scale = 1;
    
    if (gfp->ATHtype < 0) gfp->ATHtype = 4;

    if ( gfp->ATHcurve < 0 ) gfp->ATHcurve = 4;
    
    if ( gfp->athaa_loudapprox < 0 ) gfp->athaa_loudapprox = 2;
    
    if (gfp->interChRatio < 0 ) gfp->interChRatio = 0;

    if (gfp->useTemporal < 0 ) gfp->useTemporal = 1;  /* on by default */



    /* padding method as described in 
     * "MPEG-Layer3 / Bitstream Syntax and Decoding"
     * by Martin Sieler, Ralph Sperschneider
     *
     * note: there is no padding for the very first frame
     *
     * Robert Hegemann 2000-06-22
     */
    gfc->slot_lag = gfc->frac_SpF = 0;
    if (gfp->VBR == vbr_off && !gfp->disable_reservoir)
	gfc->slot_lag = gfc->frac_SpF
	    = ((gfp->version+1)*72000L*gfp->brate) % gfp->out_samplerate;

    /* mid side sparsing */
    gfc->sparsing = gfp->sparsing;
    gfc->sparseA = gfp->sparse_low;
    gfc->sparseB = gfp->sparse_low-gfp->sparse_high;
    if ( gfc->sparseA < 0 ) gfc->sparseA = 0;
    if ( gfc->sparseB < 0 ) gfc->sparseB = 0;
    if ( gfc->sparseB > gfc->sparseA ) gfc->sparseB = gfc->sparseA;

    iteration_init(gfp);
    psymodel_init(gfp);

    return 0;
}

/*
 *  print_config
 *
 *  Prints some selected information about the coding parameters via 
 *  the macro command MSGF(), which is currently mapped to lame_errorf 
 *  (reports via a error function?), which is a printf-like function 
 *  for <stderr>.
 */

void
lame_print_config(const lame_global_flags * gfp)
{
    lame_internal_flags *gfc = gfp->internal_flags;
    double  out_samplerate = gfp->out_samplerate;
    double  in_samplerate = gfp->out_samplerate * gfc->resample_ratio;

    MSGF(gfc, "LAME version %s (%s)\n", get_lame_version(), get_lame_url());
    if (LAME_ALPHA_VERSION)
        MSGF(gfc, "warning: alpha versions should be used for testing only\n");

    if (gfc->CPU_features.MMX
        || gfc->CPU_features.AMD_3DNow
        || gfc->CPU_features.SSE || gfc->CPU_features.SSE2) {
        MSGF(gfc, "CPU features: ");

        if (gfc->CPU_features.MMX)
#ifdef MMX_choose_table
            MSGF(gfc, "MMX (ASM used)");
#else
            MSGF(gfc, "MMX");
#endif
        if (gfc->CPU_features.AMD_3DNow)
#ifdef HAVE_NASM
            MSGF(gfc, ", 3DNow! (ASM used)");
#else
            MSGF(gfc, ", 3DNow!");
#endif
        if (gfc->CPU_features.SSE)
            MSGF(gfc, ", SSE");
        if (gfc->CPU_features.SSE2)
            MSGF(gfc, ", SSE2");
        MSGF(gfc, "\n");
    }

    if (gfp->num_channels == 2 && gfc->channels_out == 1 /* mono */ ) {
        MSGF
            (gfc,
             "Autoconverting from stereo to mono. Setting encoding to mono mode.\n");
    }

    if (gfc->resample_ratio != 1.) {
        MSGF(gfc, "Resampling:  input %g kHz  output %g kHz\n",
             1.e-3 * in_samplerate, 1.e-3 * out_samplerate);
    }

    if (gfc->filter_type == 0) {
        if (gfc->highpass2 > 0.)
            MSGF
                (gfc,
                 "Using polyphase highpass filter, transition band: %5.0f Hz - %5.0f Hz\n",
                 0.5 * gfc->highpass1 * out_samplerate,
                 0.5 * gfc->highpass2 * out_samplerate);
        if (0. < gfc->lowpass1 && gfc->lowpass1 < 1.) {
            MSGF
                (gfc,
                 "Using polyphase lowpass filter, transition band: %5.0f Hz - %5.0f Hz\n",
                 0.5 * gfc->lowpass1 * out_samplerate,
                 0.5 * gfc->lowpass2 * out_samplerate);
        }
        else {
            MSGF(gfc, "polyphase lowpass filter disabled\n");
        }
    }
    else {
        MSGF(gfc, "polyphase filters disabled\n");
    }

    if (gfp->free_format) {
        MSGF(gfc,
             "Warning: many decoders cannot handle free format bitstreams\n");
        if (gfp->brate > 320) {
            MSGF
                (gfc,
                 "Warning: many decoders cannot handle free format bitrates >320 kbps (see documentation)\n");
        }
    }
}


/**     rh:
 *      some pretty printing is very welcome at this point!
 *      so, if someone is willing to do so, please do it!
 *      add more, if you see more...
 */
void 
lame_print_internals( const lame_global_flags * gfp )
{
    lame_internal_flags *gfc = gfp->internal_flags;
    const char * pc = "";

    /*  compiler/processor optimizations, operational, etc.
     */
    MSGF( gfc, "\nmisc:\n\n" );
    
    MSGF( gfc, "\tscaling: %g\n", gfp->scale );
    MSGF( gfc, "\tch0 (left) scaling: %g\n", gfp->scale_left );
    MSGF( gfc, "\tch1 (right) scaling: %g\n", gfp->scale_right );
    MSGF( gfc, "\tfilter type: %d\n", gfc->filter_type );
    pc = gfc->quantization ? "xr^3/4" : "ISO";
    MSGF( gfc, "\tquantization: %s\n", pc );
    switch( gfc->use_best_huffman ) {
    default: pc = "normal"; break;
    case  1: pc = "best (outside loop)"; break;
    case  2: pc = "best (inside loop, slow)"; break;
    } 
    MSGF( gfc, "\thuffman search: %s\n", pc ); 
    MSGF( gfc, "\texperimental Y=%d\n", gfp->experimentalY);
    MSGF( gfc, "\t...\n" );

    /*  everything controlling the stream format 
     */
    MSGF( gfc, "\nstream format:\n\n" );
    switch ( gfp->version ) {
    case 0:  pc = "2.5"; break;
    case 1:  pc = "1";   break;
    case 2:  pc = "2";   break;
    default: pc = "?";   break;
    }
    MSGF( gfc, "\tMPEG-%s Layer 3\n", pc );
    switch ( gfp->mode ) {
    case JOINT_STEREO: pc = "joint stereo";   break;
    case STEREO      : pc = "stereo";         break;
    case DUAL_CHANNEL: pc = "dual channel";   break;
    case MONO        : pc = "mono";           break;
    case NOT_SET     : pc = "not set (error)"; break;
    default          : pc = "unknown (error)"; break;
    }
    MSGF( gfc, "\t%d channel - %s\n", gfc->channels_out, pc );
    
    switch (gfp->VBR) {
    case vbr_off   : pc = "off";    break;
    default        : pc = "all";    break;
    }
    MSGF( gfc, "\tpadding: %s\n", pc );
    
    if ( vbr_default == gfp->VBR )  pc = "(default)";
    else if ( gfp->free_format )    pc = "(free format)";
    else pc = "";
    switch ( gfp->VBR ) {
    case vbr_off : MSGF( gfc, "\tconstant bitrate - CBR %s\n",      pc ); break;
    case vbr_abr : MSGF( gfc, "\tvariable bitrate - ABR %s\n",      pc ); break;
    case vbr_rh  : MSGF( gfc, "\tvariable bitrate - VBR rh %s\n",   pc ); break;
    case vbr_mt  : MSGF( gfc, "\tvariable bitrate - VBR mt %s\n",   pc ); break;
    case vbr_mtrh: MSGF( gfc, "\tvariable bitrate - VBR mtrh %s\n", pc ); break; 
    default      : MSGF( gfc, "\t ?? oops, some new one ?? \n" );         break;
    }
    if (gfp->bWriteVbrTag) 
    MSGF( gfc, "\tusing LAME Tag\n" );
    MSGF( gfc, "\t...\n" );
    
    /*  everything controlling psychoacoustic settings, like ATH, etc.
     */
    MSGF( gfc, "\npsychoacoustic:\n\n" );
    
    MSGF( gfc, "\tusing psychoacoustic model: %d\n", gfc->psymodel);
    MSGF( gfc, "\tpsychoacoustic model: %s\n", (gfp->psymodel == PSY_NSPSYTUNE) ? "NSPsytune" : "GPsycho" );
    MSGF( gfc, "\ttonality estimation limit: %f Hz %s\n", gfc->PSY->cwlimit, (gfp->psymodel == PSY_NSPSYTUNE) ? "(not relevant)" : "");
    switch ( gfp->short_blocks ) {
    default:
    case short_block_not_set:   pc = "?";               break;
    case short_block_allowed:   pc = "allowed";         break;
    case short_block_coupled:   pc = "channel coupled"; break;
    case short_block_dispensed: pc = "dispensed";       break;
    case short_block_forced:    pc = "forced";          break;
    }
    MSGF( gfc, "\tusing short blocks: %s\n", pc );    
    MSGF( gfc, "\tsubblock gain: %d\n", gfc->subblock_gain );
    MSGF( gfc, "\tadjust masking: %g dB\n", gfp->maskingadjust );
    MSGF( gfc, "\tadjust masking short: %g dB\n", gfp->maskingadjust_short );
    MSGF( gfc, "\tquantization comparison: %d\n", gfp->quant_comp );
    MSGF( gfc, "\t ^ comparison short blocks: %d\n", gfp->quant_comp_short );
    MSGF( gfc, "\tnoise shaping: %d\n", gfc->noise_shaping );
    MSGF( gfc, "\t ^ amplification: %d\n", gfc->noise_shaping_amp );
    MSGF( gfc, "\t ^ stopping: %d\n", gfc->noise_shaping_stop );
    
    pc = "using";
    if ( gfp->ATHshort ) pc = "the only masking for short blocks";
    if ( gfp->ATHonly  ) pc = "the only masking";
    if ( gfp->noATH    ) pc = "not used";
    MSGF( gfc, "\tATH: %s\n", pc );
    MSGF( gfc, "\t ^ type: %d\n", gfp->ATHtype );
    MSGF( gfc, "\t ^ shape: %g%s\n", gfp->ATHcurve, " (only for type 4)" );
    MSGF( gfc, "\t ^ level adjustement: %g\n", gfp->ATHlower );
    MSGF( gfc, "\t ^ adjust type: %d\n", gfc->ATH->use_adjust );
    MSGF( gfc, "\t ^ adjust sensitivity power: %d\n", gfc->ATH->aa_sensitivity_p );
    MSGF( gfc, "\t ^ adapt threshold type: %d\n", gfp->athaa_loudapprox );
    
    if ( gfp->psymodel == PSY_NSPSYTUNE ) {
	MSGF(gfc, "\texperimental psy tunings by Naoki Shibata\n" );
	MSGF(gfc, "\t   adjust masking bass=%g dB, alto=%g dB, treble=%g dB, sfb21=%g dB\n", 
	     10*log10(gfc->nsPsy.longfact[ 0]),
	     10*log10(gfc->nsPsy.longfact[ 7]),
	     10*log10(gfc->nsPsy.longfact[14]),
	     10*log10(gfc->nsPsy.longfact[21]));
    }
    pc = gfp->useTemporal ? "yes" : "no";
    MSGF( gfc, "\tusing temporal masking effect: %s\n", pc );
    MSGF( gfc, "\tinterchannel masking ratio: %g\n", gfp->interChRatio );
    MSGF( gfc, "\t...\n" );
    
    /*  that's all ?
     */
    MSGF( gfc, "\n" );
    return;
}



/* routine to feed exactly one frame (gfp->framesize) worth of data to the 
encoding engine.  All buffering, resampling, etc, handled by calling
program.  
*/
int
lame_encode_frame(lame_global_flags * gfp,
                  sample_t inbuf_l[], sample_t inbuf_r[],
                  unsigned char *mp3buf, int mp3buf_size)
{
    int     ret;
    ret = lame_encode_mp3_frame(gfp, inbuf_l, inbuf_r, mp3buf, mp3buf_size);
    gfp->frameNum++;
    return ret;
}

/*
 * THE MAIN LAME ENCODING INTERFACE
 * mt 3/00
 *
 * input pcm data, output (maybe) mp3 frames.
 * This routine handles all buffering, resampling and filtering for you.
 * The required mp3buffer_size can be computed from num_samples,
 * samplerate and encoding rate, but here is a worst case estimate:
 *
 * mp3buffer_size in bytes = 1.25*num_samples + 7200
 *
 * return code = number of bytes output in mp3buffer.  can be 0
 *
 * NOTE: this routine uses LAME's internal PCM data representation,
 * 'sample_t'.  It should not be used by any application.  
 * applications should use lame_encode_buffer(), 
 *                         lame_encode_buffer_float()
 *                         lame_encode_buffer_int()
 * etc... depending on what type of data they are working with.  
*/
int
lame_encode_buffer_sample_t(lame_global_flags * gfp,
                   sample_t buffer_l[],
                   sample_t buffer_r[],
                   int nsamples, unsigned char *mp3buf, const int mp3buf_size)
{
    lame_internal_flags *gfc = gfp->internal_flags;
    int     mp3size = 0, ret, i, ch, mf_needed;
    int mp3out;
    sample_t *mfbuf[2];
    sample_t *in_buffer[2];

    if (gfc->Class_ID != LAME_ID)
        return -3;

    if (nsamples == 0)
        return 0;

    /* copy out any tags that may have been written into bitstream */
    mp3out = copy_buffer(gfc,mp3buf,mp3buf_size,0);
    if (mp3out<0) return mp3out;  /* not enough buffer space */
    mp3buf += mp3out;
    mp3size += mp3out;


    in_buffer[0]=buffer_l;
    in_buffer[1]=buffer_r;  


    /* Apply user defined re-scaling */

    /* user selected scaling of the samples */
    if (gfp->scale != 0 && gfp->scale != 1.0) {
	for (i=0 ; i<nsamples; ++i) {
	    in_buffer[0][i] *= gfp->scale;
	    if (gfc->channels_out == 2)
		in_buffer[1][i] *= gfp->scale;
	    }
    }

    /* user selected scaling of the channel 0 (left) samples */
    if (gfp->scale_left != 0 && gfp->scale_left != 1.0) {
	for (i=0 ; i<nsamples; ++i) {
	    in_buffer[0][i] *= gfp->scale_left;
	    }
    }

    /* user selected scaling of the channel 1 (right) samples */
	if (gfp->scale_right != 0 && gfp->scale_right != 1.0) {
	    for (i=0 ; i<nsamples; ++i) {
		in_buffer[1][i] *= gfp->scale_right;
	    }
	}

    /* Downsample to Mono if 2 channels in and 1 channel out */
	if (gfp->num_channels == 2 && gfc->channels_out == 1) {
		for (i=0; i<nsamples; ++i) {
			in_buffer[0][i] =
				0.5 * ((FLOAT8) in_buffer[0][i] + in_buffer[1][i]);
			in_buffer[1][i] = 0.0;
		}
	}


    /* some sanity checks */
#if ENCDELAY < MDCTDELAY
# error ENCDELAY is less than MDCTDELAY, see encoder.h
#endif
#if FFTOFFSET > BLKSIZE
# error FFTOFFSET is greater than BLKSIZE, see encoder.h
#endif

    mf_needed = BLKSIZE + gfp->framesize - FFTOFFSET; /* amount needed for FFT */
    /*mf_needed = Max(mf_needed, 286 + 576 * (1 + gfc->mode_gr)); */
    mf_needed = Max(mf_needed, 512+gfp->framesize-32 );

    assert(MFSIZE >= mf_needed);

    mfbuf[0] = gfc->mfbuf[0];
    mfbuf[1] = gfc->mfbuf[1];

    while (nsamples > 0) {
        int     n_in = 0;    /* number of input samples processed with fill_buffer */
        int     n_out = 0;   /* number of samples output with fill_buffer */
        /* n_in <> n_out if we are resampling */

        /* copy in new samples into mfbuf, with resampling */
        fill_buffer(gfp, mfbuf, in_buffer, nsamples, &n_in, &n_out);

        /* compute ReplayGain of resampled input if requested */
        if (gfp->findReplayGain && !gfp->decode_on_the_fly) 
            if (AnalyzeSamples(gfc->rgdata, &mfbuf[0][gfc->mf_size], &mfbuf[1][gfc->mf_size], n_out, gfc->channels_out) == GAIN_ANALYSIS_ERROR) 
                return -6;



        /* update in_buffer counters */
        nsamples -= n_in;
        in_buffer[0] += n_in;
        if (gfc->channels_out == 2)
            in_buffer[1] += n_in;

        /* update mfbuf[] counters */
        gfc->mf_size += n_out;
        assert(gfc->mf_size <= MFSIZE);
        gfc->mf_samples_to_encode += n_out;


        if (gfc->mf_size >= mf_needed) {
            /* encode the frame.  */
            /* mp3buf              = pointer to current location in buffer */
            /* mp3buf_size         = size of original mp3 output buffer */
            /*                     = 0 if we should not worry about the */
            /*                       buffer size because calling program is  */
            /*                       to lazy to compute it */
            /* mp3size             = size of data written to buffer so far */
            /* mp3buf_size-mp3size = amount of space avalable  */

            int buf_size=mp3buf_size - mp3size;
            if (mp3buf_size==0) buf_size=0;

            ret =
                lame_encode_frame(gfp, mfbuf[0], mfbuf[1], mp3buf,buf_size);

            if (ret < 0) return ret;
            mp3buf += ret;
            mp3size += ret;

            /* shift out old samples */
            gfc->mf_size -= gfp->framesize;
            gfc->mf_samples_to_encode -= gfp->framesize;
            for (ch = 0; ch < gfc->channels_out; ch++)
                for (i = 0; i < gfc->mf_size; i++)
                    mfbuf[ch][i] = mfbuf[ch][i + gfp->framesize];
        }
    }
    assert(nsamples == 0);

    return mp3size;
}


int
lame_encode_buffer(lame_global_flags * gfp,
                   const short int buffer_l[],
                   const short int buffer_r[],
                   const int nsamples, unsigned char *mp3buf, const int mp3buf_size)
{
    lame_internal_flags *gfc = gfp->internal_flags;
    int     ret, i;
    sample_t *in_buffer[2];

    if (gfc->Class_ID != LAME_ID)
        return -3;

    if (nsamples == 0)
        return 0;

    in_buffer[0] = calloc(sizeof(sample_t), nsamples);
    in_buffer[1] = calloc(sizeof(sample_t), nsamples);

    if (in_buffer[0] == NULL || in_buffer[1] == NULL) {
        ERRORF(gfc, "Error: can't allocate in_buffer buffer\n");
        return -2;
    }

    /* make a copy of input buffer, changing type to sample_t */
    for (i = 0; i < nsamples; i++) {
        in_buffer[0][i] = buffer_l[i];
	if (gfc->channels_in>1) in_buffer[1][i] = buffer_r[i];
    }

    ret = lame_encode_buffer_sample_t(gfp,in_buffer[0],in_buffer[1],
				      nsamples, mp3buf, mp3buf_size);
    
    free(in_buffer[0]);
    free(in_buffer[1]);
    return ret;
}


int
lame_encode_buffer_float(lame_global_flags * gfp,
                   const float buffer_l[],
                   const float buffer_r[],
                   const int nsamples, unsigned char *mp3buf, const int mp3buf_size)
{
    lame_internal_flags *gfc = gfp->internal_flags;
    int     ret, i;
    sample_t *in_buffer[2];

    if (gfc->Class_ID != LAME_ID)
        return -3;

    if (nsamples == 0)
        return 0;

    in_buffer[0] = calloc(sizeof(sample_t), nsamples);
    in_buffer[1] = calloc(sizeof(sample_t), nsamples);

    if (in_buffer[0] == NULL || in_buffer[1] == NULL) {
        ERRORF(gfc, "Error: can't allocate in_buffer buffer\n");
        return -2;
    }

    /* make a copy of input buffer, changing type to sample_t */
    for (i = 0; i < nsamples; i++) {
        in_buffer[0][i] = buffer_l[i];
        if (gfc->channels_in>1) in_buffer[1][i] = buffer_r[i];
    }

    ret = lame_encode_buffer_sample_t(gfp,in_buffer[0],in_buffer[1],
				      nsamples, mp3buf, mp3buf_size);
    
    free(in_buffer[0]);
    free(in_buffer[1]);
    return ret;
}



int
lame_encode_buffer_int(lame_global_flags * gfp,
                   const int buffer_l[],
                   const int buffer_r[],
                   const int nsamples, unsigned char *mp3buf, const int mp3buf_size)
{
    lame_internal_flags *gfc = gfp->internal_flags;
    int     ret, i;
    sample_t *in_buffer[2];

    if (gfc->Class_ID != LAME_ID)
        return -3;

    if (nsamples == 0)
        return 0;

    in_buffer[0] = calloc(sizeof(sample_t), nsamples);
    in_buffer[1] = calloc(sizeof(sample_t), nsamples);

    if (in_buffer[0] == NULL || in_buffer[1] == NULL) {
        ERRORF(gfc, "Error: can't allocate in_buffer buffer\n");
        return -2;
    }

    /* make a copy of input buffer, changing type to sample_t */
    for (i = 0; i < nsamples; i++) {
                                /* internal code expects +/- 32768.0 */
      in_buffer[0][i] = buffer_l[i] * (1.0 / ( 1L << (8 * sizeof(int) - 16)));
      if (gfc->channels_in>1)
	  in_buffer[1][i] = buffer_r[i] * (1.0 / ( 1L << (8 * sizeof(int) - 16)));
    }

    ret = lame_encode_buffer_sample_t(gfp,in_buffer[0],in_buffer[1],
				      nsamples, mp3buf, mp3buf_size);
    
    free(in_buffer[0]);
    free(in_buffer[1]);
    return ret;

}




int
lame_encode_buffer_long2(lame_global_flags * gfp,
                   const long buffer_l[],
                   const long buffer_r[],
                   const int nsamples, unsigned char *mp3buf, const int mp3buf_size)
{
    lame_internal_flags *gfc = gfp->internal_flags;
    int     ret, i;
    sample_t *in_buffer[2];

    if (gfc->Class_ID != LAME_ID)
        return -3;

    if (nsamples == 0)
        return 0;

    in_buffer[0] = calloc(sizeof(sample_t), nsamples);
    in_buffer[1] = calloc(sizeof(sample_t), nsamples);

    if (in_buffer[0] == NULL || in_buffer[1] == NULL) {
        ERRORF(gfc, "Error: can't allocate in_buffer buffer\n");
        return -2;
    }

    /* make a copy of input buffer, changing type to sample_t */
    for (i = 0; i < nsamples; i++) {
                                /* internal code expects +/- 32768.0 */
      in_buffer[0][i] = buffer_l[i] * (1.0 / ( 1L << (8 * sizeof(long) - 16)));
      if (gfc->channels_in>1)
	  in_buffer[1][i] = buffer_r[i] * (1.0 / ( 1L << (8 * sizeof(long) - 16)));
    }

    ret = lame_encode_buffer_sample_t(gfp,in_buffer[0],in_buffer[1],
				      nsamples, mp3buf, mp3buf_size);
    
    free(in_buffer[0]);
    free(in_buffer[1]);
    return ret;

}



int
lame_encode_buffer_long(lame_global_flags * gfp,
                   const long buffer_l[],
                   const long buffer_r[],
                   const int nsamples, unsigned char *mp3buf, const int mp3buf_size)
{
    lame_internal_flags *gfc = gfp->internal_flags;
    int     ret, i;
    sample_t *in_buffer[2];

    if (gfc->Class_ID != LAME_ID)
        return -3;

    if (nsamples == 0)
        return 0;

    in_buffer[0] = calloc(sizeof(sample_t), nsamples);
    in_buffer[1] = calloc(sizeof(sample_t), nsamples);

    if (in_buffer[0] == NULL || in_buffer[1] == NULL) {
        ERRORF(gfc, "Error: can't allocate in_buffer buffer\n");
        return -2;
    }

    /* make a copy of input buffer, changing type to sample_t */
    for (i = 0; i < nsamples; i++) {
        in_buffer[0][i] = buffer_l[i];
        if (gfc->channels_in>1)
	    in_buffer[1][i] = buffer_r[i];
    }

    ret = lame_encode_buffer_sample_t(gfp,in_buffer[0],in_buffer[1],
				      nsamples, mp3buf, mp3buf_size);
    
    free(in_buffer[0]);
    free(in_buffer[1]);
    return ret;
}











int
lame_encode_buffer_interleaved(lame_global_flags * gfp,
                               short int buffer[],
                               int nsamples,
                               unsigned char *mp3buf, int mp3buf_size)
{
    int     ret, i;
    sample_t *buffer_l;
    sample_t *buffer_r;

    buffer_l = calloc(sizeof(sample_t), nsamples);
    buffer_r = calloc(sizeof(sample_t), nsamples);
    if (buffer_l == NULL || buffer_r == NULL) {
        return -2;
    }
    for (i = 0; i < nsamples; i++) {
        buffer_l[i] = buffer[2 * i];
        buffer_r[i] = buffer[2 * i + 1];
    }
    ret =
        lame_encode_buffer_sample_t(gfp, buffer_l, buffer_r, nsamples, mp3buf,
                           mp3buf_size);
    free(buffer_l);
    free(buffer_r);
    return ret;

}


int
lame_encode(lame_global_flags * const gfp,
            const short int in_buffer[2][1152],
            unsigned char *const mp3buf, const int size)
{
    lame_internal_flags *gfc = gfp->internal_flags;

    if (gfc->Class_ID != LAME_ID)
        return -3;

    return lame_encode_buffer(gfp, in_buffer[0], in_buffer[1], gfp->framesize,
                              mp3buf, size);
}

/*****************************************************************
 Flush mp3 buffer, pad with ancillary data so last frame is complete.
 Reset reservoir size to 0                  
 but keep all PCM samples and MDCT data in memory             
 This option is used to break a large file into several mp3 files 
 that when concatenated together will decode with no gaps         
 Because we set the reservoir=0, they will also decode seperately 
 with no errors. 
*********************************************************************/
int
lame_encode_flush_nogap(lame_global_flags * gfp,
                  unsigned char *mp3buffer, int mp3buffer_size)
{
    lame_internal_flags *gfc = gfp->internal_flags;
    flush_bitstream(gfp);
    return copy_buffer(gfc,mp3buffer, mp3buffer_size,1);
}


/* called by lame_init_params.  You can also call this after flush_nogap 
   if you want to write new id3v2 and Xing VBR tags into the bitstream */
int
lame_init_bitstream(lame_global_flags * gfp)
{
    lame_internal_flags *gfc = gfp->internal_flags;
    gfp->frameNum=0;

    id3tag_write_v2(gfp);
#ifdef BRHIST
    /* initialize histogram data optionally used by frontend */
    memset(gfc->bitrate_stereoMode_Hist, 0,
	   sizeof(gfc->bitrate_stereoMode_Hist));
    memset(gfc->bitrate_blockType_Hist, 0,
	   sizeof(gfc->bitrate_blockType_Hist));
#endif

    gfc->PeakSample = 0.0;

    /* Write initial VBR Header to bitstream and init VBR data */
    if (gfp->bWriteVbrTag) 
        InitVbrTag(gfp);


    return 0;
}


/*****************************************************************/
/* flush internal PCM sample buffers, then mp3 buffers           */
/* then write id3 v1 tags into bitstream.                        */
/*****************************************************************/

int
lame_encode_flush(lame_global_flags * gfp,
                  unsigned char *mp3buffer, int mp3buffer_size)
{
    lame_internal_flags *gfc = gfp->internal_flags;
    short int buffer[2][1152];
    int     imp3 = 0, mp3count, mp3buffer_size_remaining;

    /* we always add POSTDELAY=288 padding to make sure granule with real
     * data can be complety decoded (because of 50% overlap with next granule */
    int end_padding=POSTDELAY;  

    memset(buffer, 0, sizeof(buffer));
    mp3count = 0;


    while (gfc->mf_samples_to_encode > 0) {

        mp3buffer_size_remaining = mp3buffer_size - mp3count;

        /* if user specifed buffer size = 0, dont check size */
        if (mp3buffer_size == 0)
            mp3buffer_size_remaining = 0;

        /* send in a frame of 0 padding until all internal sample buffers
         * are flushed 
         */
        imp3 = lame_encode_buffer(gfp, buffer[0], buffer[1], gfp->framesize,
                                  mp3buffer, mp3buffer_size_remaining);

        /* don't count the above padding: */
        gfc->mf_samples_to_encode -= gfp->framesize;
        if (gfc->mf_samples_to_encode < 0) {
            /* we added extra padding to the end */
            end_padding += -gfc->mf_samples_to_encode;  
        }


        if (imp3 < 0) {
            /* some type of fatal error */
            return imp3;
        }
        mp3buffer += imp3;
        mp3count += imp3;
    }

    mp3buffer_size_remaining = mp3buffer_size - mp3count;
    /* if user specifed buffer size = 0, dont check size */
    if (mp3buffer_size == 0)
        mp3buffer_size_remaining = 0;

    /* mp3 related stuff.  bit buffer might still contain some mp3 data */
    flush_bitstream(gfp);
    imp3 = copy_buffer(gfc,mp3buffer, mp3buffer_size_remaining, 1);
    if (imp3 < 0) {
	/* some type of fatal error */
	return imp3;
    }
    mp3buffer += imp3;
    mp3count += imp3;
    mp3buffer_size_remaining = mp3buffer_size - mp3count;
    /* if user specifed buffer size = 0, dont check size */
    if (mp3buffer_size == 0)
	mp3buffer_size_remaining = 0;

    /* write a id3 tag to the bitstream */
    id3tag_write_v1(gfp);
    imp3 = copy_buffer(gfc,mp3buffer, mp3buffer_size_remaining, 0);

    if (imp3 < 0) {
        return imp3;
    }
    mp3count += imp3;
    gfp->encoder_padding=end_padding;
    return mp3count;
}

/***********************************************************************
 *
 *      lame_close ()
 *
 *  frees internal buffers
 *
 ***********************************************************************/

int
lame_close(lame_global_flags * gfp)
{
    lame_internal_flags *gfc = gfp->internal_flags;

    if (gfc->Class_ID != LAME_ID)
        return -3;

    if (gfp->exp_nspsytune2.pointer[0]) {
      fclose((FILE *)gfp->exp_nspsytune2.pointer[0]);
      gfp->exp_nspsytune2.pointer[0] = NULL;
    }

    gfc->Class_ID = 0;

    /* this routien will free all malloc'd data in gfc, and then free gfc: */
    freegfc(gfc);

    gfp->internal_flags = NULL;

    if (gfp->lame_allocated_gfp) {
        gfp->lame_allocated_gfp = 0;
        free(gfp);
    }

    return 0;
}

/*****************************************************************/
/* flush internal mp3 buffers, and free internal buffers         */
/*****************************************************************/

int
lame_encode_finish(lame_global_flags * gfp,
                   unsigned char *mp3buffer, int mp3buffer_size)
{
    int     ret = lame_encode_flush(gfp, mp3buffer, mp3buffer_size);

    lame_close(gfp);

    return ret;
}

/*****************************************************************/
/* write VBR Xing header, and ID3 version 1 tag, if asked for    */
/*****************************************************************/
void
lame_mp3_tags_fid(lame_global_flags * gfp, FILE * fpStream)
{
    if (gfp->bWriteVbrTag) {
        /* Map VBR_q to Xing quality value: 0=worst, 100=best */
        int     nQuality = ((9-gfp->VBR_q) * 100) / 9;

        /* Write Xing header again */
        if (fpStream && !fseek(fpStream, 0, SEEK_SET))
            PutVbrTag(gfp, fpStream, nQuality);
    }
}
lame_global_flags *
lame_init(void)
{
    lame_global_flags *gfp;
    int     ret;

    init_log_table();

    gfp = calloc(1, sizeof(lame_global_flags));
    if (gfp == NULL)
        return NULL;

    ret = lame_init_old(gfp);
    if (ret != 0) {
        free(gfp);
        return NULL;
    }

    gfp->lame_allocated_gfp = 1;
    return gfp;
}

/* initialize mp3 encoder */
int
lame_init_old(lame_global_flags * gfp)
{
    lame_internal_flags *gfc;

    disable_FPE();      /* disable floating point exceptions */

    memset(gfp, 0, sizeof(lame_global_flags));

    if (NULL ==
        (gfc = gfp->internal_flags =
         calloc(1, sizeof(lame_internal_flags)))) return -1;

    /* Global flags.  set defaults here for non-zero values */
    /* see lame.h for description */
    /* set integer values to -1 to mean that LAME will compute the
     * best value, UNLESS the calling program as set it
     * (and the value is no longer -1)
     */


    gfp->mode = NOT_SET;
    gfp->original = 1;
    gfp->in_samplerate = 1000 * 44.1;
    gfp->num_channels = 2;
    gfp->num_samples = MAX_U_32_NUM;

    gfp->bWriteVbrTag = 1;
    gfp->quality = -1;
    gfp->short_blocks = short_block_not_set;
    gfc->subblock_gain = -1;

    gfp->lowpassfreq = 0;
    gfp->highpassfreq = 0;
    gfp->lowpasswidth = -1;
    gfp->highpasswidth = -1;

    gfp->VBR = vbr_off;
    gfp->VBR_q = 4;
    gfp->ATHcurve = -1;
    gfp->VBR_mean_bitrate_kbps = 128;
    gfp->VBR_min_bitrate_kbps = 0;
    gfp->VBR_max_bitrate_kbps = 0;
    gfp->VBR_hard_min = 0;
    gfc->VBR_min_bitrate = 1; /* not  0 ????? */
    gfc->VBR_max_bitrate = 13; /* not 14 ????? */

    gfp->VBR_smooth = -1;

    gfp->quant_comp = -1;
    gfp->quant_comp_short = -1;

    gfp->msfix = -1;

    gfc->resample_ratio = 1;

    gfc->OldValue[0] = 180;
    gfc->OldValue[1] = 180;
    gfc->CurrentStep[0] = 4;
    gfc->CurrentStep[1] = 4;
    gfc->masking_lower = 1;
    gfc->nsPsy.attackthre   = -1;
    gfc->nsPsy.attackthre_s = -1;

    gfp->scale = -1;

    gfp->athaa_type = -1;
    gfp->ATHtype = -1;  /* default = -1 = set in lame_init_params */
    gfp->athaa_loudapprox = -1;	/* 1 = flat loudness approx. (total energy) */
                                /* 2 = equal loudness curve */
    gfp->athaa_sensitivity = 0.0; /* no offset */
    gfp->useTemporal = -1;
    gfp->interChRatio = -1;

    /* The reason for
     *       int mf_samples_to_encode = ENCDELAY + POSTDELAY;
     * ENCDELAY = internal encoder delay.  And then we have to add POSTDELAY=288
     * because of the 50% MDCT overlap.  A 576 MDCT granule decodes to
     * 1152 samples.  To synthesize the 576 samples centered under this granule
     * we need the previous granule for the first 288 samples (no problem), and
     * the next granule for the next 288 samples (not possible if this is last
     * granule).  So we need to pad with 288 samples to make sure we can
     * encode the 576 samples we are interested in.
     */
    gfc->mf_samples_to_encode = ENCDELAY + POSTDELAY;
    gfp->encoder_padding = 0;
    gfc->mf_size = ENCDELAY - MDCTDELAY; /* we pad input with this many 0's */

    gfp->findReplayGain = 0;
    gfp->decode_on_the_fly = 0;

    gfc->findPeakSample = 0;

    gfc->RadioGain = 0.0;
    gfc->AudiophileGain = 0.0;
    gfc->noclipGainChange = 0.0;
    gfc->noclipScale = -1;

    gfp->asm_optimizations.mmx = 1;
    gfp->asm_optimizations.amd3dnow = 1;
    gfp->asm_optimizations.sse = 1;

    gfp->preset = 0;
    
    gfp->psymodel = -1;

    gfp->sparsing = 0;
    gfp->sparse_low = 9.0;
    gfp->sparse_high = 3.0;
    
    return 0;
}

/***********************************************************************
 *
 *  some simple statistics
 *
 *  Robert Hegemann 2000-10-11
 *
 ***********************************************************************/

/*  histogram of used bitrate indexes:
 *  One has to weight them to calculate the average bitrate in kbps
 *
 *  bitrate indices:
 *  there are 14 possible bitrate indices, 0 has the special meaning 
 *  "free format" which is not possible to mix with VBR and 15 is forbidden
 *  anyway.
 *
 *  stereo modes:
 *  0: LR   number of left-right encoded frames
 *  1: LR-I number of left-right and intensity encoded frames
 *  2: MS   number of mid-side encoded frames
 *  3: MS-I number of mid-side and intensity encoded frames
 *
 *  4: number of encoded frames
 *
 */

void
lame_bitrate_kbps(const lame_global_flags * const gfp, int bitrate_kbps[14])
{
    const lame_internal_flags *gfc;
    int     i;

    if (NULL == bitrate_kbps)
        return;
    if (NULL == gfp)
        return;
    gfc = gfp->internal_flags;
    if (NULL == gfc)
        return;

    for (i = 0; i < 14; i++)
        bitrate_kbps[i] = bitrate_table[gfp->version][i + 1];
}


#ifdef BRHIST
void
lame_bitrate_hist(const lame_global_flags * const gfp, int bitrate_count[14])
{
    const lame_internal_flags *gfc;
    int     i;

    if (NULL == bitrate_count)
        return;
    if (NULL == gfp)
        return;
    gfc = gfp->internal_flags;
    if (NULL == gfc)
        return;

    for (i = 0; i < 14; i++)
        bitrate_count[i] = gfc->bitrate_stereoMode_Hist[i + 1][4];
}


void
lame_stereo_mode_hist(const lame_global_flags * const gfp, int stmode_count[4])
{
    const lame_internal_flags *gfc;
    int     i;

    if (NULL == stmode_count)
        return;
    if (NULL == gfp)
        return;
    gfc = gfp->internal_flags;
    if (NULL == gfc)
        return;

    for (i = 0; i < 4; i++) {
        stmode_count[i] = gfc->bitrate_stereoMode_Hist[15][i];
    }
}



void
lame_bitrate_stereo_mode_hist(const lame_global_flags * const gfp,
                              int bitrate_stmode_count[14][4])
{
    const lame_internal_flags *gfc;
    int     i;
    int     j;

    if (NULL == bitrate_stmode_count)
        return;
    if (NULL == gfp)
        return;
    gfc = gfp->internal_flags;
    if (NULL == gfc)
        return;

    for (j = 0; j < 14; j++)
        for (i = 0; i < 4; i++)
            bitrate_stmode_count[j][i] = gfc->bitrate_stereoMode_Hist[j + 1][i];
}


void
lame_block_type_hist(const lame_global_flags * const gfp, int btype_count[6])
{
    const lame_internal_flags *gfc;
    int     i;

    if (NULL == btype_count)
        return;
    if (NULL == gfp)
        return;
    gfc = gfp->internal_flags;
    if (NULL == gfc)
        return;

    for (i = 0; i < 6; ++i) {
        btype_count[i] = gfc->bitrate_blockType_Hist[15][i];
    }
}



void 
lame_bitrate_block_type_hist(const lame_global_flags * const gfp, 
                             int bitrate_btype_count[14][6])
{
    const lame_internal_flags * gfc;
    int     i, j;
    
    if (NULL == bitrate_btype_count)
        return;
    if (NULL == gfp)
        return;
    gfc = gfp->internal_flags;
    if (NULL == gfc)
        return;
        
    for (j = 0; j < 14; ++j)
    for (i = 0; i <  6; ++i)
        bitrate_btype_count[j][i] = gfc->bitrate_blockType_Hist[j+1][i];
}

#endif

/* end of lame.c */

