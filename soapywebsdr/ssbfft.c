/* Web based SDR Client for SDRplay
* =============================================================
* Author: DJ0ABR
*
*   (c) DJ0ABR
*   www.dj0abr.de
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
* 
* 
* ssbfft.c ... 
* 
* this function gets samples with a rate of 600.000 kS/s (300kHz bandwidth)
* we need an FFT with a resolution of 10 Hz per bin
* so we do the FFT every 60.000 samples (10 times per second)
* 
* 
*/

#include <fftw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "fifo.h"
#include "wf_univ.h"
#include "websdr.h"
#include "setqrg.h"
#include "audio_bandpass.h"
#include "downmixer.h"
#include "hilbert90.h"
#include "antialiasing.h"
#include "timing.h"

#define FSSB_SRATE          600000
#define FSSB_RESOLUTION     10
#define FSSB_FFT_LENGTH     (FSSB_SRATE / FSSB_RESOLUTION)  // = 60.000
#define FSSB_NUM_BINS       (FSSB_FFT_LENGTH / 2)           // = 30.000 (the last one is on nyquist freq, there for it should not be used)

fftw_complex *din = NULL;				// input data for  fft, output data from ifft
fftw_complex *cpout = NULL;	            // ouput data from fft, input data to ifft
fftw_complex *cpssb = NULL;             // converted cpout, used as input for the inverse fft
fftw_plan plan = NULL, iplan = NULL;
int din_idx = 0;
int rflock = 0;


void init_fssb()
{
    char fn[300];
	sprintf(fn, "fssb_wisdom");	// wisdom file for each capture rate

	fftw_import_wisdom_from_filename(fn);
  
    din   = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * FSSB_FFT_LENGTH);
	cpout = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * FSSB_FFT_LENGTH);
    cpssb = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * FSSB_FFT_LENGTH);

    plan = fftw_plan_dft_1d(FSSB_FFT_LENGTH, din, cpout, FFTW_FORWARD, FFTW_MEASURE);
	iplan = fftw_plan_dft_1d(FSSB_FFT_LENGTH, cpssb, din, FFTW_BACKWARD, FFTW_MEASURE);

    fftw_export_wisdom_to_filename(fn);
}

void fssb_sample_processing(short *xi, short *xq, int numSamples)
{
    // copy the samples into the FFT input buffer
    for(int i=0; i<numSamples; i++)
    {
        din[din_idx][0] = xi[i];
		din[din_idx][1] = xq[i];
        din_idx++;
        
        // check if the fft input buffer is filled
        if(din_idx >= FSSB_FFT_LENGTH)
        {
            din_idx = 0;
            
            // the buffer is full, now lets execute the fft
            fftw_execute(plan);
            
            // this fft has generated FSSB_NUM_BINS (= 30.000) bins in cpout
            double wfsamp[WF_WIDTH];
            int idx = 0;
            double real,imag;
            int wfbins;

            // for the BIG waterfall we need 1500 (WF_WIDTH) bins, so take every 20th
            // take the loudest of the ten bins
            // create waterfall line and send to the client through a web socket
            int picture_div = 20;
            for(wfbins=0; wfbins<FSSB_NUM_BINS; wfbins+=picture_div)
            {
                wfsamp[idx] = -99999;
                for(int bin10=0; bin10<picture_div; bin10++)
                {
                    real = cpout[wfbins+bin10][0];
                    imag = cpout[wfbins+bin10][1];
                    double v = sqrt((real * real) + (imag * imag));
                    if(v > wfsamp[idx]) wfsamp[idx] = v;
                    // correct level TODO ???
                    //wfsamp[idx] /= 50;
                 }
                 
                 wfsamp[idx] /= 100;

                idx++;
            }
            
            unsigned int realrf = TUNED_FREQUENCY - newrf;
            
            drawWF(WFID_BIG,wfsamp, WF_WIDTH, WF_WIDTH, 1, realrf, FSSB_SRATE/2, FSSB_RESOLUTION*picture_div, DISPLAYED_FREQUENCY_KHZ, "\0");
            
            // for the SMALL waterfall we need 1500 (WF_WIDTH) bins
            // in a range of 15.000 Hz, so every single bin (one bin is 10 Hz)
            // starting at the current RX frequency - 15kHz/2 (so the RX qrg is in the middle)
            // foffset is the RX qrg in Hz
            // so we need the bins from (foffset/10) - 750 to (foffset/10) + 750
            int start = ((foffset+0)/10) - 750;
            int end = ((foffset+0)/10) + 750;

			if(start < 0) start = 0;
			if(end >= FSSB_FFT_LENGTH) end = FSSB_FFT_LENGTH-1;
            
            double wfsampsmall[WF_WIDTH];
            idx = 0;

            for(wfbins=start; wfbins<end; wfbins++)
            {
                real = cpout[wfbins][0];
                imag = cpout[wfbins][1];
                wfsampsmall[idx] = sqrt((real * real) + (imag * imag));
                
                // correct level TODO ???
                wfsampsmall[idx] /= 50;
                
                idx++;
            }

			// void drawWF(int id, double *fdata, int cnt, int wpix, int hpix, unsigned int _realqrg, int _rightqrg, int res, int _tunedQRG, char *_fn)
            drawWF(WFID_SMALL,wfsampsmall, WF_WIDTH, WF_WIDTH, 1, realrf, 15000, FSSB_RESOLUTION, DISPLAYED_FREQUENCY_KHZ + (foffset)/1000, "\0");

        }
        
        // SSB Decoding and Audio Output
        // shift the needed frequency to baseband
        downmixer_process(xi+i,xq+i);
        
        // here we have the samples with rate of 600.000
        // we need 8.000 for the sound card
        // lets decimate by 75 (FSSB_SRATE / 8000)
        static int audio_cnt = 0;
        static short b16samples[AUDIO_RATE];
        static int audio_idx = 0;
        if(++audio_cnt >= 75)
        {
            audio_cnt = 0;
            
            xi[i] = fir_filter_i_ssb(xi[i]);
            xq[i] = fir_filter_q_ssb(xq[i]);
    
            // USB demodulation
            double dsamp = BandPassm45deg(xi[i]) - BandPass45deg(xq[i]);
            
            if(dsamp > 32767 || dsamp < -32767)
                printf("Hilbert Output too loud: %.0f\n",dsamp);
            
            b16samples[audio_idx] = audio_filter((short)dsamp);
            
            if(++audio_idx >= AUDIO_RATE)
            {
                audio_idx = 0;
                // we have AUDIO_RATE (8000) samples, this is one second
                #ifdef SOUNDLOCAL
                    // play it to the local soundcard
                    write_pipe(FIFO_AUDIO, (unsigned char *)b16samples, AUDIO_RATE*2);
                #else
                    // lets pipe it to the browser through the web socket
                    write_pipe(FIFO_AUDIOWEBSOCKET, (unsigned char *)b16samples, AUDIO_RATE*2);
                #endif
            }
        }
        else
        {
            fir_filter_i_ssb_inc(xi[i]);
            fir_filter_q_ssb_inc(xq[i]);
        }
    }
}

