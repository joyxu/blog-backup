// 
//  Copyright (c) 2009 VUDU Inc.
//  All rights reserved.
// 

// libavformat does not work without this
#define __STDC_CONSTANT_MACROS
#include <stdint.h>
#include <string.h>

#define ARRAY_COUNT(x) (sizeof(x) / sizeof(x[0]))

extern "C" {
#include <libavformat/avformat.h>
}
struct ADTSPrivData {
    int32_t sampleRateIndex;
    int32_t channelConf;
    bool fHaveInfo;
};


// Hackery to add ADTS header to audio packets
// Create a new bitstream filter that does this

static int32_t findArrayIndex(const int32_t toFind, 
                              const int32_t *array, 
                              const uint32_t size) 
{
    for (uint32_t i = 0; i < size; i++) {
        if (array[i] == toFind) {
            return i;
        }
    }
    return -1;
}

static int aac_adts_adder_filter(AVBitStreamFilterContext *bsfc,
                                 AVCodecContext *codec, const char *args,
                                 uint8_t  **pOutBuf, int *outBufSize,
                                 const uint8_t *buf, int bufSize,
                                 int keyframe) 
{
    static const int32_t aacSampleRates[12] = {
        96000, 88200, 64000, 48000, 44100, 32000,
        24000, 22050, 16000, 12000, 11025, 8000
    };
    static const int32_t aacChannelConfs[8] = {
        0, 1, 2, 3, 4, 5, 6, 8
    };

    static const uint32_t adtsHeaderSize = 7;

    static const uint8_t defaultAdtsHeader[7] = {0xff, 0xf1, 0x40, 0, 0, 0x1f, 0xfc};

    *outBufSize = bufSize + adtsHeaderSize;
    ADTSPrivData *pd = (ADTSPrivData *) bsfc->priv_data;
    if (!pd->fHaveInfo) {
        pd->sampleRateIndex = findArrayIndex(codec->sample_rate, aacSampleRates,
                                             ARRAY_COUNT(aacSampleRates));
        pd->channelConf = findArrayIndex(codec->channels, aacChannelConfs,
                                         ARRAY_COUNT(aacChannelConfs));

        if (pd->sampleRateIndex == -1 || pd->channelConf == -1) {
            return -1;
        }

        pd->fHaveInfo = true;
    }

    // Resize the buffer and copy the original data
    *pOutBuf = (uint8_t*) av_malloc(*outBufSize);
    if( NULL == pOutBuf ){
	return -1;		
    }
    uint8_t *outBuf = *pOutBuf;
    memcpy(outBuf + adtsHeaderSize, buf, bufSize);

    // Write the adts header
    memcpy(outBuf, defaultAdtsHeader, adtsHeaderSize);
    // sample_rate
    outBuf[2] |= (pd->sampleRateIndex << 2);

    // channel config
    outBuf[2] |= (pd->channelConf >> 2); // bit 0
    outBuf[3] |= (pd->channelConf << 6); // bits 1-2

    // size of packet
    outBuf[3] |= (*outBufSize >> 11); // bits 0-1
    outBuf[4] |= (*outBufSize >> 3);  // bits 2-9
    outBuf[5] |= (*outBufSize << 5);  // bits 10-12

    return 1;
}

AVBitStreamFilter aac_adts_adder_bsf = {
    "aac_adts_adder_bsf",
    sizeof(ADTSPrivData),
    aac_adts_adder_filter,
    NULL,
};



