/* processaudio.h */

// High level functions to process audio, received from an ALSA device
// The actual audio is received from the ringbuffer, fed by the "capture" function


// version 20111107: initial release
// version 20111110: read from file, dump bits, dump amplitude
// version 20111112: support for stereo files
// version 20111130: end stream on signal drop. do not process noise data,
//                   accept bits at startsync and descrable slow data
// version 20120105: raw mode audio output
// version 20120529: codec2 version
// version 20120611: bitslip code added to codec2 version

/*
 *      Copyright (C) 2011 by Kristoff Bonne, ON1ARF
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; version 2 of the License.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
*/


void * funct_r_processaudio_codec2 (void * c_globaldatain ) {


// vars

int16_t audioin;

int nextbuffer;
int thisbuffer;
int thisbuffersize;
int thisfileend;
int thisaudioaverage;

int sampleloop;
int16_t * audiobuffer;

int bit, state;

int codec2_bitcount;
int codec2_octetcount;
int countnoiseframe=0; // number of frames not processed due to average audio level > noise

int hadtowait;

int syncmask;

uint16_t last2octets;
int syncreceived;
uint32_t last4octets=0;

unsigned char codec2frame[7]; // 40 ms @ 1400 bps = 56 bits = 7 octets
unsigned char codec2inframe[24]; // 40 ms @ 4800 bps = 192 bits = 24 octets

unsigned char codec2versionid[3]; // 3 octets after header containing versionid (3 times)

int retval, retval2=0;
char retmsg[ISALRETMSGSIZE];
int outputopen=0;

int endfound=0;
int bitmatch;
int framecount=0;

// flags that indicate certainty of good stream
int missedsync;
int syncfound=0;

// auto audioinvert detection
int inaudioinvert=0;
int syncpattern;
int syncpattern_inverted;


char marker=0; // 'S' (sync) , 'M' (missed) of 'E' (end) put at end of line 

int16_t *samplepointer;
int channel;
int buffermask;

// tempory data
int totalerror;
int8_t error_8bit;
int loop;

// vars to calculate "maximum audio level" for a valid stream (i.e. no noise)
int maxaudiolevelvalid=0;
int framesnoise=0;
// ringbuffer to store last 32 values of average audio level
// used to detect false-positive start-of-stream for raw streams
int averageleveltable[32];
int averagelevelindex;
int averagevalid;
uint64_t averagetotal;

// vars only used for alsa capture
#ifdef _USEALSA
int maxlevel;
#endif


// temp values to calculate maximum audio level
int64_t maxaudiolevelvalid_total=0;
int maxaudiolevelvalid_numbersample=0;

// output format:
int thisformat;

c_globaldatastr * p_c_global;
r_globaldatastr * p_r_global;
g_globaldatastr * p_g_global;

p_c_global=(c_globaldatastr *) c_globaldatain;

p_r_global=p_c_global->p_r_global;
p_g_global=p_c_global->p_g_global;



// init vars
state=0;
syncreceived=0; 
last2octets=0;

missedsync=0;
codec2_bitcount=0; codec2_octetcount=0;

for (loop=0;loop<32;loop++) {
	averageleveltable[loop]=0;
}; // end if
averagelevelindex=0;




// thisformat: 1: dvtool-file format, 2: DSTAR stream, 10: raw, 20: codec2
if (p_r_global->recformat) {
	thisformat=p_r_global->recformat;
} else {
	thisformat=p_g_global->format;
}; // end if

if (thisformat != 20) {
	fprintf(stderr,"Error in processaudio_codec2, invalid format. Expected 20, got %d\n",thisformat);
	exit(-1);
}; // end if


// init OUTPUT DESTINATION ABSTRACTION LAYER
retval=output_dal(IDAL_CMD_INIT,p_c_global,0,&retval2,retmsg);

if (retval != IDAL_RET_SUCCESS) {
	// init failed
	fprintf(stderr,"%s",retmsg);
	exit(-1);
}; // end if


// sync mask: depends on syncsize
// should contain valid between 1 and 16.
// check just to be sure
if ((p_r_global->syncsize < 1) || (p_r_global->syncsize > 16)) {
	fprintf(stderr,"Error in processaudio, synsize should be between 1 and 16, got %d. Exiting!\n",p_r_global->syncsize);
	exit(-1);
}; // end if
syncmask=size2mask[p_r_global->syncsize-1];


// precalculate values to avoid having to do the calculation every
// itteration
if (p_r_global->stereo) {
	channel=2;
	buffermask=0x7f;
} else {
	channel=1;
	buffermask=0xff;
}; // end if




// init var
countnoiseframe=0;
hadtowait=0;
syncpattern=p_r_global->syncpattern;
syncpattern_inverted= syncpattern ^ 0xffffffff;

// init thisfileend. For ALSA capturing, there is no file ending; so we
// just thread this as a endless file
thisfileend=0;


// endless loop;
while (!(thisfileend)) {

	// is there something to process?

	// check the next buffer slot. If the "capture" process is using it, we do not have any
	// audio available. -> wait and try again
	nextbuffer=(p_r_global->pntr_process + 1) & buffermask;

	if (p_r_global->pntr_capture == nextbuffer) {
		// nothing to process: sleep for 1/4 of ms (= 5 ms)
		usleep((useconds_t) 250);

		// set "had to wait" marker
		hadtowait=1;
		continue;
	};

	// OK, we have a frame with audio-information, process it, sample
	// per sample
	thisbuffer=nextbuffer;
	audiobuffer= (int16_t *) p_r_global->buffer[thisbuffer];
	thisbuffersize=p_r_global->buffersize[thisbuffer];
	thisaudioaverage=p_r_global->audioaverage[thisbuffer];

	#ifdef _USEALSA
	if (p_r_global->fileorcapture) {
		thisfileend=p_r_global->fileend[thisbuffer];
	}; // end if
	#endif



	// PART1:
	// things to do at the beginning of every block of data

	// dump average if requested
	if (p_r_global->dumpaverage) {
		printf("(%04X)",thisaudioaverage);
	}; // end if


	// sample was received when modem was transmitting. Ignore it
	if (p_r_global->sending[thisbuffer]) {
		// sleep for 10 ms (half of one audiosample), only when reading from ALSA device
		// and we have he "had to wait" marker set

		#ifdef _USEALSA
		if ((hadtowait) && (p_r_global->fileorcapture == 0)) {	
			usleep((useconds_t) 10000);
		}; // end if
		#endif

		// reset "had to wait"
		hadtowait=0;
	
		// go to next audio frame
		p_r_global->pntr_process = nextbuffer;
		continue;
	}; // end if


	// store average audio-level values in  ringbuffer; to be used
	// later to detect false-positive start-of-stream
	averagelevelindex++;
	averagelevelindex &= 0x1f; // wrap from 0 to 31
	averageleveltable[averagelevelindex]=thisaudioaverage;
	

	// do we process the audio ?
	// if state is 0 (waiting for sync), and input audio-level is to high
	// (i.e. received signal is noise), we skip the complete frame
	if ((state == 0) && (maxaudiolevelvalid) && (p_r_global->audioaverage[thisbuffer] > maxaudiolevelvalid)) {
		// skip audio frame

		// how many frames did we already skip?
		countnoiseframe++;

		// if to much (more then 900 seconds), clear "maxaudiolevel", just to be sure
		if (countnoiseframe > MAXNOISEREJECT) {
			maxaudiolevelvalid=0;
		}; // end if

		// sleep for 10 ms (half of one audiosample), only when reading from ALSA device
		// and we have he "had to wait" marker set

		#ifdef _USEALSA
		if ((hadtowait) && (p_r_global->fileorcapture == 0)) {	
			usleep((useconds_t) 10000);
		}; // end if
		#endif

		// reset "had to wait"
		hadtowait=0;
	
		// go to next audio frame
		p_r_global->pntr_process = nextbuffer;
		continue;
	};


	// at this point, we have a valid frame without noise

	// reset countnoiseframe
	countnoiseframe=0;

	// reset "had to wait" flag
	hadtowait=0;


	// when receiving header (state is 1), add up audio-level data to calculate
	// maxaudiolevel when header completely received
	if (state == 1) {
		maxaudiolevelvalid_total += p_r_global->audioaverage[thisbuffer];
		maxaudiolevelvalid_numbersample++;
	}; // end if

	// when receiving data frame (state is 20), just count number of consequent audiolevels
	if (state == 20) {
		if ((maxaudiolevelvalid) && (p_r_global->audioaverage[thisbuffer] > maxaudiolevelvalid)) {
			framesnoise++;
		} else {
			framesnoise=0;
		}; // end else - if
	}; // end if



	// PART 2: 
	// process individual samples in buffer

	samplepointer=audiobuffer;
	for (sampleloop=0; sampleloop <thisbuffersize; sampleloop++) {
		// read audio:

		// For mono: read mono channel
		// For stereo: read left channel
		audioin=*samplepointer;

		//move up pointer one (mono) or two (stereo). 
		samplepointer += channel;


		bit=demodulate(audioin);

		// the demodulate function returns three possible values:
		// -1: not a valid bit
		// 0 or 1, valid bit

		if (bit < 0) {
			// not a valid bit
			continue;
		}; // end if

		// "State" variable:
		// state 0: waiting for sync "10101010 1010101" (bit sync) or
		// 	"1110110 01010000" (frame sync)
		// state 20: receiving codec2 speed/version id
		// state 21: receiving codec2 main part of bitstream

		// state 0
		if (state == 0) {
			if (p_r_global->dumpstream >= 2) {
				printbit(bit,6,4,0);
			}; // end if

			// keep track of last 16 bits
			last2octets<<=1;
			if (bit) {
				last2octets |= 0x01;
			}; // end if

			// the syncronisation pattern is at least 64 times "0101"
			if ((last2octets == 0x5555) || (last2octets == 0xaaaa)) {
				syncreceived += 3;
			} else {
				if (syncreceived > 0) {
					syncreceived--;
				}; // end if
			}; 

			if (syncreceived > 20) {
			// start looking for frame sync if we have received sufficient bitsync

				// we accept up to "BITERROR START SYN" penality points for errors
				// (note: syncmask is initialised above, based on syncsize)

				// syncpattern is initialised above
				bitmatch=countdiff16_fromlsb(last2octets,syncmask,p_r_global->syncsize, syncpattern,BITERRORSSTARTSYN);

				if (bitmatch) {
					inaudioinvert=0;
				} else {

					// try again with inverted bit pattern
					bitmatch=countdiff16_fromlsb(last2octets,syncmask,p_r_global->syncsize, syncpattern_inverted, BITERRORSSTARTSYN);

					if (bitmatch) {
						inaudioinvert=1;
						if ((p_g_global->verboselevel >= 1) || (p_r_global->dumpstream >= 1)) {
							fprintf(stderr,"WARNING: audio invertion detected!\n");
						}; // end if
					}; // end if
				}; // end if


				if (bitmatch) {
					int p;
					// OK, we have a valid frame sync, go to state 20 (codec2 versionid)

					// Additional check: compair average audio level with average of 16 up to 31 samples
					// ago (when the signal should contain noise). The current audio-level should be at
					// least 18.75 % (13/16) below that "noise level"

					// init vars
					averagevalid=1;
					averagetotal=0;

					// the averagelevel table is a 32 wide ringbuffer
					// the index points (N) to the last value added in the buffer; so the value N+1
					// is actually the value N-31
					p=averagelevelindex;

					for (loop=0; loop<16;loop++) {
						int thisaverage;
						p++;
						p &= 0x1f;

						thisaverage=averageleveltable[p];

						// average value = 0? Not valid
						if (!thisaverage) {
							averagevalid=0;
						} else {
							averagetotal += thisaverage;
						}; // end if
					}; // end for

					
					#ifdef _USEALSA
					if (DISABLE_AUDIOLEVELCHECK != 1) {
						// test only makes sence for capture.
						if (!p_r_global->fileorcapture) {
							if (averagevalid) {
								maxlevel = (averagetotal >> 8) * 13; // >>8=/256: divide by 16 (for 16 samples) and
																			// then a 2nd time for get 13/16 of average total
	
								if (thisaudioaverage < maxlevel) {
									if (p_g_global->verboselevel >= 1) {
										fprintf(stderr,"START STREAM: Average audio level: %04X, max: %04X\n",thisaudioaverage,maxlevel);
									}; // end if
								} else {
									if (p_g_global->verboselevel >= 1) {
											fprintf(stderr,"START STREAM CANCELED - noiselevel %04X to high (max %04X)\n",thisaudioaverage, maxlevel);
											continue;
									}; // end if
								}; // end if
							} else {
								if (p_g_global->verboselevel >= 1) {
										fprintf(stderr,"START STREAM CANCELED - not yet enough data for noiselevel test\n");
										continue;
								}; // end if
							}; // end if
						}; // end if (capture)
					}; // DISABLE_AUDIOLEVELCHECK
					#endif


					// new state: 20 codec2 version id
					state=20;

					// init some vars
					codec2_bitcount=0;
					codec2_octetcount=0;
					memset(codec2versionid,0,3); // clear codec2versionid;

					// reset print-bit to beginning of stream
					if (p_r_global->dumpstream >= 2) {
						putchar(0x0a); // ASCII 0x0a = linefeed
					}; // end if
					printbit(-1,0,0,0);

					// store first data of audiolevel to calculate average
					maxaudiolevelvalid_total=p_r_global->audioaverage[thisbuffer];
					maxaudiolevelvalid_numbersample=1;

					// go to next bit
					continue;
				}; // end if
			}; 

			// end of checking, 
			// go to next bit
			continue;
		}; // end if (state 0)


		// state 20: codec2 versionid
		if (state == 20) {
			unsigned char thisversionid;

			if (p_r_global->dumpstream >= 2) {
				printbit(bit,6,4,0);
			}; // end if

			// read up to 24 bits
			if (bit) {
				codec2versionid[codec2_octetcount] |= bit2octet[codec2_bitcount];
			}; // end if

			codec2_bitcount++;

			if (codec2_bitcount >= 8) {
				codec2_octetcount++;
				codec2_bitcount=0;
			}; // end if


			// if not received all 24 bits, get next
			if (codec2_octetcount < 3) {
				continue;
			}; // end if

			// invert value if audio is inverted
			if (inaudioinvert) {
				codec2versionid[0] ^= 0xff; codec2versionid[1] ^= 0xff; codec2versionid[2] ^= 0xff;
			}; // end if

			// apply 1/3 FEC ("best of 3" FEC decoding on versionid fields)
			error_8bit=fec13decode_8bit(codec2versionid[0],codec2versionid[1],codec2versionid[2],&thisversionid);


			// print individual version id fields 
			if ((error_8bit) && (p_g_global->verboselevel >= 2)) {
				fprintf(stderr,"WARNING: error detected in codec2 versionid fields: %X %X %X\n",codec2versionid[0],codec2versionid[1],codec2versionid[2]);
			}; // end if

			// print version id
			if (p_g_global->verboselevel >= 1) {
				fprintf(stderr,"codec2 versionid = %X \n",thisversionid);
			}; // end if


			// check version
			// currently only versionid "0x11" is accepted (speed "1", mode "1");
			if (thisversionid != 0x11) {
				// unknown version id
				fprintf(stderr,"Error: processaudio_codec2: unsupported version id %x\n",thisversionid);

				// re-init vars
				syncreceived=0;
				last2octets=0;

				state=0;

				// next bit
				continue;
			}; // end if


			// OK, we have a valid versionid.

			// open output, so we can send out "begin" marker if needed

			// open Output Destination Abstraction layer
			if (!(outputopen)) {
				retval=output_dal(IDAL_CMD_OPEN,NULL,0,&retval2,retmsg);

				if (retval == IDAL_RET_SUCCESS) {
					outputopen=1;
				} else {
					// open failed
					fprintf(stderr,"%s",retmsg);
				}; // end if
			}; // end if


			// write marker if requested
			if ((outputopen) && (p_r_global->sendmarker)) {
				// send marker for begin DSTAR stream
				retval=output_dal(IDAL_CMD_WRITE,"CODEC2STREAMBEGIN.\n",16,&retval2,retmsg);

				if (retval != IDAL_RET_SUCCESS) {
					// open failed
					fprintf(stderr,"%s",retmsg);
				}; // end if

			}; // end if
				

			// new state: 21 (codec2 data frame)
			state=21;

			// init some vars
			last4octets=0;

			codec2_octetcount=0;
			codec2_bitcount=0;

			memset(codec2inframe,0,24); // clear codec2inframe

			framesnoise=0;
			framecount=0;

			missedsync=0;

			syncfound=0;

			// reset print-bit to beginning of stream
			if (p_r_global->dumpstream >= 2) {
				putchar(0x0a); // ASCII 0x0a = linefeed
			}; // end if
			printbit(-1,0,0,0);


			// go to next bit
			continue;
		}; // end if (state 20)

		// state 21: codec2: main part of stream
		if (state == 21) {


			// READING the main part of the stream
			// the first 32 bits are read into the "last4octets" var
			// this is easier to process bitslips (after reading the 24-bit header) as this is mainly
			// "bit" operations
			// after reading/processing these 32 bits, they are copied into "codec2inframe" memory
			// the last 160 bits are read directly into the "codec2inframe" structure
			// keep track of last 32 bits (used to determine bitslips)
			last4octets>>=1;
			if (bit) {
				last4octets |= 0x80000000;
			}; // end if

//printf("bitcount %d octetcount %d marker %X (%c) missedsync %d\n",codec2_bitcount,codec2_octetcount, marker,marker, missedsync);
			if (codec2_octetcount < 4) {
				codec2_bitcount++;

				if (codec2_bitcount < 32) {
					// bitslip tests

					uint32_t framesync, endsync; 

					if (inaudioinvert == 0) {
						framesync=0x5c08e700;
						endsync=0xc5807e00;
					} else {
						// only left 24 bits are used
						framesync=0xa3f71800;
						endsync=0x3a7f8100;
					}; // end else - if

					// only do test if sync not yet found and no resync yet done
					if (!syncfound) {
						if (codec2_bitcount == 24) {
							// check sync pattern (first 24 bits), allow up to 3 biterrors
							if (countdiff32_frommsb(last4octets, 0xffffff00, 24, framesync, 3)) {
								marker='S';
								missedsync=0;
								endfound=0;
								syncfound=1; // no further checks needed;
							} else if (countdiff32_frommsb(last4octets, 0xffffff00, 24, endsync, 3)) {
								marker='E';
								endfound=1;
								syncfound=1; // no further checks needed;
							 }; // end elsif - if

						} else if ((codec2_bitcount == 23) || (codec2_bitcount == 25)) {
							// bitslip +1 or -1 bit
							// check sync pattern (first 24 bits), allow up to 1 biterror
							// if found, correct "bitcount" value

							if (countdiff32_frommsb(last4octets, 0xffffff00, 24, framesync, 1)) {
								marker='T';
								missedsync=0;
								endfound=0;
								syncfound=1; // no further checks needed
								codec2_bitcount=24; // correct bit position
							} else  if (countdiff32_frommsb(last4octets, 0xffffff00, 24, endsync, 1)) {
								marker='F';
								endfound=1;
								syncfound=1; // no further checks needed
								codec2_bitcount=24; // correct bit position
							 }; // end elsif - if
						} else if ((codec2_bitcount == 22) || (codec2_bitcount == 26)) {
							// bitslip +2 or -2 bit
							// check sync pattern (first 24 bits), allow no biterror
							// if found, correct "bitcount" value

							if (countdiff32_frommsb(last4octets, 0xffffff00, 24, framesync, 0)) {
								marker='U';
								missedsync=0;
								endfound=0;
								syncfound=1; // no further checks needed
								codec2_bitcount=24; // correct bit position
							} else  if (countdiff32_frommsb(last4octets, 0xffffff00, 24, endsync, 0)) {
								marker='G';
								endfound=1;
								syncfound=1; // no further checks needed
								codec2_bitcount=24; // correct bit position
							}; // end elseif - if
						}; // end elsif - elsif - if
					}; // end if (! sync found)
				} else {
					// 32 bits received. Copy them over to the codec2inframe structure
					// note that the octet order is reversed

					// copy octet per octet as we are not sure how a 32bit integer structure is stored in memory
					codec2inframe[0]=(unsigned char)last4octets & 0xff;
					codec2inframe[1]=(unsigned char)(last4octets >> 8) & 0xff;
					codec2inframe[2]=(unsigned char)(last4octets >> 16) & 0xff;
					codec2inframe[3]=(unsigned char)(last4octets >> 24) & 0xff;

					codec2_octetcount = 4;
					codec2_bitcount=0;
				}; // end if
			} else {
			// after the first 32 bits, read up to 192 bits

				if (codec2_octetcount <= 23) {
					if (bit) {
						codec2inframe[codec2_octetcount] |= bit2octet[codec2_bitcount];
					}; // end if
				} else  {
					fprintf(stderr,"Warning: codec2inframe Boundary protection: %d \n",codec2_octetcount);
				}; // end else - if

				codec2_bitcount++;

				if (codec2_bitcount >= 8) {
					codec2_octetcount++;
					codec2_bitcount=0;
				}; // end if

			}; // end else - if


			// go to next bit if not yet 192 bits received
			if (codec2_octetcount <= 23) {
				// print bits if requested
				if (p_r_global->dumpstream >= 2) {
					printbit(bit,12,2,0);
				}; // end if

				continue;
			}; // end if

			// if no sync found
			if (!syncfound) {
				marker='M';
				missedsync++;
			}; // end if


			if (p_r_global->dumpstream >= 2) {
				printbit(bit,12,2,marker);
			}; // end if

			// we have received a full frame:

			// 1: apply scrambling

			// we have up to 8 lines of known exor-patterns
			framecount &= 0x7;


			{
				unsigned char *c1, *c2;

				c1=&scramble_exor[framecount][0];
				// descramblink chars 3 up to 24 (starting at 0)
				c2=&codec2inframe[3];

				// descramble 21 chars
				for (loop=0; loop < 21; loop++) {
					*c2 ^= *c1;
					c2++; c1++;
				}; // end for

			}; 

			// increase framecounter; no need to do boundary check, happens above
			framecount++;


			// 2: apply FEC, including interleaving
			{
				unsigned char *p1, *d;

				totalerror=0;

				p1=&codec2inframe[3];
				// destination is codec2frame. Point to beginning of struct, then go up one position per loop
				d=codec2frame;

				if (p_g_global->verboselevel >= 3) {
					for (loop=0; loop < 7; loop++) {
						error_8bit = fec13decode_8bit(*p1,codec2inframe[interl2[loop]],codec2inframe[interl3[loop]],d);
						if (error_8bit) {
							totalerror += count1s_8bit(error_8bit);
						}; // end if
						p1++; d++;
					}; // end for

					printf("Number of bit errors: %d\n\n",totalerror);
				} else {
					for (loop=0; loop < 7; loop++) {
						fec13decode_8bit(*p1,codec2inframe[interl2[loop]],codec2inframe[interl3[loop]],d);
						p1++; d++;
					}; // end for
				}; // end if
			}; // FEC + interleaving


			// frame received, now invert if needed
			if (inaudioinvert) {
				for (loop=0; loop<7; loop++) {
					codec2frame[loop] ^= 0xff;
				}; // end for
			}; // end if


			// 3 possible senario:
			// - to many missed sync: just stop
			// - endfound: send final frame and stop
			// other: send frame

			if (missedsync < 20) {
				// send frame
				if (outputopen) {
					retval=output_dal(IDAL_CMD_WRITE,codec2frame,7,&retval2,retmsg);

					if (retval != IDAL_RET_SUCCESS) {
						// open failed
						fprintf(stderr,"%s",retmsg);
					}; // end if
				}; // end if
			}; // end if

			// print out messages (if needed)

			if (endfound)  {
				if (p_g_global->verboselevel >= 1) {
					fprintf(stderr,"END.\n\n\n");
				}; // end if
			}; // end if

			if (missedsync >= 20) {
				if (p_g_global->verboselevel >= 1) {
					fprintf(stderr,"TOMANYMISSEDSYNC.\n\n\n");
				}; // end if
				endfound=1;
			}; // end if


			
			// things to do at end of frame

			// re-init vars
			codec2_octetcount=0;
			codec2_bitcount=0;
			syncfound=0;
			// clear memory for new frame
			memset(codec2inframe,0,24);


			// reset counters of printbit function
			printbit(-1,0,0,0);


			// write marker if requested
			if ((outputopen) && (p_r_global->sendmarker)) {
				retval=output_dal(IDAL_CMD_WRITE,"DSTARSTREAMEND.",15,&retval2,retmsg);
				if (retval != IDAL_RET_SUCCESS) {
					// open failed
					fprintf(stderr,"%s",retmsg);
				}; // end if
			}; // end if


			// if not end, go to next frame
			if (!endfound) {
				continue;
			}; // end if

			// END FOUND!!!
			// things to do at the end of the stream


			// close output
			if (outputopen){
				retval=output_dal(IDAL_CMD_CLOSE,NULL,0,&retval2,retmsg);
				if (retval != IDAL_RET_SUCCESS) {
					// open failed
					fprintf(stderr,"%s",retmsg);
				} else {
					// success
					outputopen=0;
				}; // end if
			}; // end if


			// reinit vars
			last2octets=0;
			syncreceived=0; 
			inaudioinvert=0;
			state=0; // new state is 0, "waiting for sync"


			// get next audio-frame to look for new stream

			continue;
		}; // end if (state 21)


	}; // end for (sampleloop)

	p_r_global->pntr_process = nextbuffer;

}; // end while (for capture: endless loop; for file: EOF)


#ifdef _USEALSA
if (p_r_global->fileorcapture == 0) {
	// capture
	fprintf(stderr,"Error: CAPTURE-THREAD TERMINATED for capture. Should not happen! \n");
}; // end if
#endif

/// end program
exit(0);
}; // end thread "process audio"


