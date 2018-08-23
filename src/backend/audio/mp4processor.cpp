#
/*
 *    Copyright (C) 2013
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *    Lazy Chair Computing
 *
 *    This file is part of the SDR-J (JSDR).
 *    SDR-J is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    SDR-J is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with SDR-J; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * 	superframer for the SDR-J DAB+ receiver
 * 	This processor handles the whole DAB+ specific part
 ************************************************************************
 *	may 15 2015. A real improvement on the code
 *	is the addition from Stefan Poeschel to create a
 *	header for the aac that matches, really a big help!!!!
 ************************************************************************
 */
#include	"mp4processor.h"
#include	<cstring>
#include	"gui.h"
//
#include	"charsets.h"
#include	"pad-handler.h"
#include	"rs1.h"

#include 	<iostream>
#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#include <arpa/inet.h>
#include <sys/socket.h>

/**
  *	\brief simple, inline coded, crc checker
  */
bool	dabPlus_crc (uint8_t *msg, int16_t len) {
int i, j;
uint16_t	accumulator	= 0xFFFF;
uint16_t	crc;
uint16_t	genpoly		= 0x1021;

	for (i = 0; i < len; i ++) {
	   int16_t data = msg [i] << 8;
	   for (j = 8; j > 0; j--) {
	      if ((data ^ accumulator) & 0x8000)
	         accumulator = ((accumulator << 1) ^ genpoly) & 0xFFFF;
	      else
	         accumulator = (accumulator << 1) & 0xFFFF;
	      data = (data << 1) & 0xFFFF;
	   }
	}
//
//	ok, now check with the crc that is contained
//	in the au
	crc	= ~((msg [len] << 8) | msg [len + 1]) & 0xFFFF;
	return (crc ^ accumulator) == 0;
}

/**
  *	\class mp4Processor is the main handler for the aac frames
  *	the class proper processes input and extracts the aac frames
  *	that are processed by the "faadDecoder" class
  */
	mp4Processor::mp4Processor (RadioInterface	*mr,
	                            int16_t	bitRate,
	                            RingBuffer<int16_t> *b)
	                            :my_padhandler (mr),
	                             the_rsDecoder (8, 0435, 0, 1, 10),
	                             aacDecoder (mr, b) {

	myRadioInterface	= mr;
	connect (this, SIGNAL (show_successRate (int)),
	         mr, SLOT (show_successRate (int)));
	connect (this, SIGNAL (showLabel (QString)),
	         mr, SLOT (showLabel (QString)));
	connect (this, SIGNAL (isStereo (bool)),
	         mr, SLOT (setStereo (bool)));
	this	-> bitRate	= bitRate;	// input rate

	superFramesize		= 110 * (bitRate / 8);
	RSDims			= bitRate / 8;
	frameBytes		= new uint8_t [RSDims * 120];	// input
	outVector		= new uint8_t [RSDims * 110];
	blockFillIndex	= 0;
	blocksInBuffer	= 0;
	frameCount	= 0;
	frameErrors	= 0;
//
//	error display
	au_count	= 0;
	au_errors	= 0;
}

	mp4Processor::~mp4Processor (void) {
	delete[]	frameBytes;
	delete[]	outVector;
}

/**
  *	\brief addtoFrame
  *
  *	a DAB+ superframe consists of 5 consecutive DAB frames 
  *	we add vector for vector to the superframe. Once we have
  *	5 lengths of "old" frames, we check
  *	Note that the packing in the entry vector is still one bit
  *	per Byte, nbits is the number of Bits (i.e. containing bytes)
  *	the function adds nbits bits, packed in bytes, to the frame
  */
void	mp4Processor::addtoFrame (uint8_t *V, int16_t nbits) {
int16_t	i, j;
uint8_t	temp	= 0;

	for (i = 0; i < nbits / 8; i ++) {	// in bytes
	   temp = 0;
	   for (j = 0; j < 8; j ++)
	      temp = (temp << 1) | (V [i * 8 + j] & 01);
	   frameBytes [blockFillIndex * nbits / 8 + i] = temp;
	}
//
	blocksInBuffer ++;
	blockFillIndex = (blockFillIndex + 1) % 5;
//
/**
  *	we take the last five blocks to look at
  */
	if (blocksInBuffer >= 5) {
///	first, we show the "successrate"
	   if (++frameCount >= 25) {
	      frameCount = 0;
	      show_successRate (4 * (25 - frameErrors));
	      frameErrors = 0;
	   }

/**
  *	starting for real: check the fire code
  *	if the firecode is OK, we handle the frame
  *	and adjust the buffer here for the next round
  */
	   if (fc. check (&frameBytes [blockFillIndex * nbits / 8]) &&
	       (processSuperframe (frameBytes,
	                           blockFillIndex * nbits / 8))) {
//	since we processed a full cycle of 5 blocks, we just start a
//	new sequence, beginning with block blockFillIndex
	      blocksInBuffer	= 0;
	   }
	   else {
/**
  *	we were wrong, virtual shift to left in block sizes
  */
	      blocksInBuffer  = 4;
	      frameErrors ++;
	   }
	}
}

/**
  *	\brief processSuperframe
  *
  *	First, we know the firecode checker gave green light
  *	We correct the errors using RS
  */
bool	mp4Processor::processSuperframe (uint8_t frameBytes [], int16_t base) {
uint8_t		num_aus;
int16_t		i, j, k;
int16_t		nErrors	= 0;
uint8_t		rsIn	[120];
uint8_t		rsOut	[110];
uint8_t		dacRate;
uint8_t		sbrFlag;
uint8_t		aacChannelMode;
uint8_t		psFlag;
uint16_t	mpegSurround;
int32_t		outSamples	= 0;
int32_t		tmp;

/**
  *	apply reed-solomon error repar
  *	OK, what we now have is a vector with RSDims * 120 uint8_t's
  *	the superframe, containing parity bytes for error repair
  *	take into account the interleaving that is applied.
  */
	for (j = 0; j < RSDims; j ++) {
	   int16_t ler	= 0;
	   for (k = 0; k < 120; k ++) 
	      rsIn [k] = frameBytes [(base + j + k * RSDims) % (RSDims * 120)];
	   ler = the_rsDecoder. dec (rsIn, rsOut, 135);
	   if (ler > 0) {		// corrected errors
	      nErrors += ler;
	   }
	   else
	   if (ler < 0) {
	      return false;
	   }

	   for (k = 0; k < 110; k ++) 
	      outVector [j + k * RSDims] = rsOut [k];
	}

//	bits 0 .. 15 is firecode
//	bit 16 is unused
	dacRate		= (outVector [2] >> 6) & 01;	// bit 17
	sbrFlag		= (outVector [2] >> 5) & 01;	// bit 18
	aacChannelMode	= (outVector [2] >> 4) & 01;	// bit 19
	psFlag		= (outVector [2] >> 3) & 01;	// bit 20
	mpegSurround	= (outVector [2] & 07);		// bits 21 .. 23

	switch (2 * dacRate + sbrFlag) {
	  default:		// cannot happen
	   case 0:
	      num_aus = 4;
	      au_start [0] = 8;
	      au_start [1] = outVector [3] * 16 + (outVector [4] >> 4);
	      au_start [2] = (outVector [4] & 0xf) * 256 +
	                      outVector [5];
	      au_start [3] = outVector [6] * 16 +
	                     (outVector [7] >> 4);
	      au_start [4] = 110 *  (bitRate / 8);
	      break;
//
	   case 1:
	      num_aus = 2;
	      au_start [0] = 5;
	      au_start [1] = outVector [3] * 16 +
	                     (outVector [4] >> 4);
	      au_start [2] = 110 *  (bitRate / 8);
	      break;
//
	   case 2:
	      num_aus = 6;
	      au_start [0] = 11;
	      au_start [1] = outVector [3] * 16 + (outVector [4] >> 4);
	      au_start [2] = (outVector [4] & 0xf) * 256 + outVector [ 5];
	      au_start [3] = outVector [6] * 16 + (outVector [7] >> 4);
	      au_start [4] = (outVector [7] & 0xf) * 256 + outVector [8];
	      au_start [5] = outVector [9] * 16 + (outVector [10] >> 4);
	      au_start [6] = 110 *  (bitRate / 8);
	      break;
//
	   case 3:
	      num_aus = 3;
	      au_start [0] = 6;
	      au_start [1] = outVector [3] * 16 + (outVector [4] >> 4);
	      au_start [2] = (outVector [4] & 0xf) * 256 + outVector [5];
	      au_start [3] = 110 * (bitRate / 8);
	      break;
	}
/**
  *	OK, the result is N * 110 * 8 bits (still single bit per byte!!!)
  *	extract the AU's, and prepare a buffer,  with the sufficient
  *	lengthy for conversion to PCM samples
  */
	for (i = 0; i < num_aus; i ++) {
	   int16_t	aac_frame_length;
	   au_count ++;
	   uint8_t theAU [2 * 960 + 10];	// sure, large enough
	   memset (theAU, 0, sizeof (theAU));

		///	sanity check 1
	   if (au_start [i + 1] < au_start [i]) {
		//	cannot happen, all errors were corrected
	      fprintf (stderr, "%d %d\n", au_start [i + 1], au_start [i]);
	      return false;
	   }

	   aac_frame_length = au_start [i + 1] - au_start [i] - 2;
	   if ((aac_frame_length >= 2 * 960) || (aac_frame_length < 0)) {

		//	cannot happen, all errors were corrected
	      fprintf (stderr, "serious error in frame 6 (%d) (%d) frame_length = %d\n",
	                                        ++au_errors,
	                                        au_count, aac_frame_length);
	      return false;
	   }
	   ///	but first the crc check
	   if (dabPlus_crc (&outVector [au_start [i]],
	                    aac_frame_length)) {
	      memcpy (theAU,
	              &outVector [au_start [i]],
	              aac_frame_length * sizeof (uint8_t));
/**
  *	see if we have a PAD
  */
#ifndef	GUI_3
	      if (((theAU [0] >> 5) & 07) == 4)
	         my_padhandler. processPAD (theAU);
#endif
	      emit isStereo (aacChannelMode);
/**
  *	just a few bytes extra, such that the decoder can look
  *	beyond the last byte
  */
	      for (j = aac_frame_length;
	           j < aac_frame_length + 10; j ++)
	         theAU [j] = 0;
if(bfirstrun) {
	bfirstrun = false;
	fprintf(stderr,"opening a cheeky /tmp/mp4-dump with the bois\n");
	if ((bdumpfd = open("/tmp/mp4-dump", O_WRONLY)) < 0) {
			fprintf(stderr, "Failed to open MP4 dump FIFO\n");
	}
}

bdumping = true;
if (bdumpfd == 0) {
bdumping = false;
	fprintf(stderr,"Attempting to open /tmp/mp4-dump\n");

if ((bdumpfd = open("/tmp/mp4-dump", O_WRONLY)) < 0) {
	fprintf(stderr, "Failed to open MP4 dump FIFO\n");
}

}
// fprintf(stderr, "Am I working?? %d\n",aac_frame_length);
	
	if(budpsockfd == 0) {
		budpsockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	}
	      tmp = aacDecoder. MP42PCM (dacRate,
	                                 sbrFlag,
	                                 mpegSurround,
	                                 aacChannelMode,
	                                 theAU,
	                                 aac_frame_length);
	if (bdumping) {

	      uint8_t fileBuffer [1024];

	      buildHeader (aac_frame_length, dacRate,sbrFlag,aacChannelMode,psFlag, fileBuffer);
	      memcpy (&fileBuffer [7], 
	              &outVector [au_start [i]],
	              aac_frame_length);



              write(bdumpfd, theAU, aac_frame_length);
	}

	if (budpsockfd != -1 && budpsockfd != 0) {
		struct sockaddr_in si_other;
		memset((char *) &si_other, 0, sizeof(si_other));
		si_other.sin_family = AF_INET;
		si_other.sin_port = htons(1337);

		inet_aton("127.0.0.1" , &si_other.sin_addr);
		int slen = sizeof(si_other);


		uint8_t fileBuffer [1024];


				struct adts_fixed_header {
					unsigned                     : 4;
					unsigned home                : 1;
					unsigned orig                : 1;
					unsigned channel_config      : 3;
					unsigned private_bit         : 1;
					unsigned sampling_freq_index : 4;
					unsigned profile             : 2;
					unsigned protection_absent   : 1;
					unsigned layer               : 2;
					unsigned id                  : 1;
					unsigned syncword            : 12;
				} fh;
				struct adts_variable_header {
					unsigned                            : 4;
					unsigned num_raw_data_blks_in_frame : 2;
					unsigned adts_buffer_fullness       : 11;
					unsigned frame_length               : 13;
					unsigned copyright_id_start         : 1;
					unsigned copyright_id_bit           : 1;
				} vh;
				/* 32k 16k 48k 24k */
				const unsigned short samptab[] = {0x5, 0x8, 0x3, 0x6};

				fh. syncword = 0xfff;
				fh. id = 0;
				fh. layer = 0;
				fh. protection_absent = 1;
				fh. profile = 0;
				fh. sampling_freq_index = samptab [dacRate << 1 | sbrFlag];

				fh. private_bit = 0;
				switch (0) {
				default:
					fprintf (stderr, "Unrecognized mpeg_surround_config ignored\n");
			//	not nice, but deliberate: fall through
				case 0:
					if (sbrFlag && !aacChannelMode && psFlag)
						fh. channel_config = 2; /* Parametric stereo */
					else
						fh. channel_config = 1 << aacChannelMode ;
					break;

				case 1:
					fh. channel_config = 6;
					break;
				}

				fh. orig = 0;
				fh. home = 0;
				vh. copyright_id_bit = 0;
				vh. copyright_id_start = 0;
				vh. frame_length = aac_frame_length + 7;  /* Includes header length */
				vh. adts_buffer_fullness = 1999;
				vh. num_raw_data_blks_in_frame = 0;
				fileBuffer [0]	= fh. syncword >> 4;
				fileBuffer [1]	= (fh. syncword & 0xf) << 4;
				fileBuffer [1]	|= fh. id << 3;
				fileBuffer [1]	|= fh. layer << 1;
				fileBuffer [1]	|= fh. protection_absent;
					fileBuffer [2]	= fh. profile << 6;
				fileBuffer [2]	|= fh. sampling_freq_index << 2;
				fileBuffer [2]	|= fh. private_bit << 1;
				fileBuffer [2]	|= (fh. channel_config & 0x4);
				fileBuffer [3]	= (fh. channel_config & 0x3) << 6;
				fileBuffer [3]	|= fh. orig << 5;
				fileBuffer [3]	|= fh. home << 4;
				fileBuffer [3]	|= vh. copyright_id_bit << 3;
				fileBuffer [3]	|= vh. copyright_id_start << 2;
				fileBuffer [3]	|= (vh. frame_length >> 11) & 0x3;
				fileBuffer [4]	= (vh. frame_length >> 3) & 0xff;
				fileBuffer [5]	= (vh. frame_length & 0x7) << 5;
				fileBuffer [5]	|= vh. adts_buffer_fullness >> 6;
				fileBuffer [6]	= (vh. adts_buffer_fullness & 0x3f) << 2;
				fileBuffer [6]	|= vh. num_raw_data_blks_in_frame;


		// buildHeader (aac_frame_length, dacRate,sbrFlag,aacChannelMode,psFlag, &fileBuffer[0]);
		memcpy (&fileBuffer [7],
				&outVector [au_start [i]],
				aac_frame_length);

		// fileBuffer[0] = 0xDE;
		// fileBuffer[1] = 0xAD;
		// fileBuffer[2] = 0xBE;
		// fileBuffer[3] = 0xEF;
		sendto(budpsockfd, &fileBuffer[0], aac_frame_length + 7 , 0 , (struct sockaddr *) &si_other, slen);
	}


	      if (tmp == 0)
	         frameErrors ++;
	      else
	         outSamples += tmp;
	   }
	   else {
	      au_errors ++;
	      fprintf (stderr, "CRC failure with dab+ frame (error %d) %d (%d)\n",
	                                          au_errors, i, num_aus);
	   }
	}
//	fprintf (stderr, "%d samples good for %d nsec of music\n",
//	                 outSamples, outSamples * 1000 / 48);
//
	return true;
}



void	mp4Processor::buildHeader (int16_t framelen,
uint8_t		dacRate,
uint8_t		sbrFlag,
uint8_t		aacChannelMode,
uint8_t		psFlag,
	                           uint8_t *header) {
	struct adts_fixed_header {
		unsigned                     : 4;
		unsigned home                : 1;
		unsigned orig                : 1;
		unsigned channel_config      : 3;
		unsigned private_bit         : 1;
		unsigned sampling_freq_index : 4;
		unsigned profile             : 2;
		unsigned protection_absent   : 1;
		unsigned layer               : 2;
		unsigned id                  : 1;
		unsigned syncword            : 12;
	} fh;
	struct adts_variable_header {
		unsigned                            : 4;
		unsigned num_raw_data_blks_in_frame : 2;
		unsigned adts_buffer_fullness       : 11;
		unsigned frame_length               : 13;
		unsigned copyright_id_start         : 1;
		unsigned copyright_id_bit           : 1;
	} vh;
	/* 32k 16k 48k 24k */
	const unsigned short samptab[] = {0x5, 0x8, 0x3, 0x6};

	fh. syncword = 0xfff;
	fh. id = 0;
	fh. layer = 0;
	fh. protection_absent = 1;
	fh. profile = 0;
	fh. sampling_freq_index = samptab [dacRate << 1 | sbrFlag];

	fh. private_bit = 0;
	switch (0) {
	   default:
	      fprintf (stderr, "Unrecognized mpeg_surround_config ignored\n");
//	not nice, but deliberate: fall through
	   case 0:
	      if (sbrFlag && !aacChannelMode && psFlag)
	         fh. channel_config = 2; /* Parametric stereo */
	      else
	         fh. channel_config = 1 << aacChannelMode ;
	      break;

	   case 1:
	      fh. channel_config = 6;
	      break;
	}

	fh. orig = 0;
	fh. home = 0;
	vh. copyright_id_bit = 0;
	vh. copyright_id_start = 0;
	vh. frame_length = framelen + 7;  /* Includes header length */
	vh. adts_buffer_fullness = 1999;
	vh. num_raw_data_blks_in_frame = 0;
	header [0]	= fh. syncword >> 4;
	header [1]	= (fh. syncword & 0xf) << 4;
	header [1]	|= fh. id << 3;
	header [1]	|= fh. layer << 1;
	header [1]	|= fh. protection_absent;
        header [2]	= fh. profile << 6;
	header [2]	|= fh. sampling_freq_index << 2;
	header [2]	|= fh. private_bit << 1;
	header [2]	|= (fh. channel_config & 0x4);
	header [3]	= (fh. channel_config & 0x3) << 6;
	header [3]	|= fh. orig << 5;
	header [3]	|= fh. home << 4;
	header [3]	|= vh. copyright_id_bit << 3;
	header [3]	|= vh. copyright_id_start << 2;
	header [3]	|= (vh. frame_length >> 11) & 0x3;
	header [4]	= (vh. frame_length >> 3) & 0xff;
	header [5]	= (vh. frame_length & 0x7) << 5;
	header [5]	|= vh. adts_buffer_fullness >> 6;
	header [6]	= (vh. adts_buffer_fullness & 0x3f) << 2;
	header [6]	|= vh. num_raw_data_blks_in_frame;
}
