//
//  Copyright (c) 2009 VUDU Inc.
//  All rights reserved.
//

// libavformat does not work without this
#define __STDC_CONSTANT_MACROS
#include <stdint.h>

#include <vudu/VUDUAssert.h>
#include "FfmpegRemuxerDriver.h"
#include "vudu_proxy.h"
#include <algorithm>

#define AVFORMAT_VERBOSE
#define ARRAY_COUNT(x) (sizeof(x) / sizeof(x[0]))

using namespace VUDU;

static const int32_t AV_BUFFER_SIZE = 64*1024;
static const int32_t AV_PID_START = 0x100;

// AVFrame implementation
bool VUDU::AVFrame::getPts(const AVFormatContext *context, uint32_t &pts)
{
    if (m_packet.pts == (int64_t) AV_NOPTS_VALUE) {
        return false;
    }

    double dPts = m_packet.pts * av_q2d(context->streams[m_packet.stream_index]->time_base);
    pts = (uint32_t) (dPts * 1000);

    return true;
}

// Helper function to correct the pts when preping a packet for remux
void VUDU::AVFrame::correctPts(int64_t &ts, const AVFormatContext *source, 
                         const AVFormatContext *target, int tsid, bool fAdjustPts)
{
    if (ts != (int64_t) AV_NOPTS_VALUE) {
        // reduce ts by startTime of the input stream
        if (fAdjustPts) {
            ts -= av_rescale_q(source->start_time, AV_TIME_BASE_Q, 
                               source->streams[m_packet.stream_index]->time_base);
        }
        ts = av_rescale_q(ts, source->streams[m_packet.stream_index]->time_base,
                          target->streams[tsid]->time_base);
    }
}

bool VUDU::AVFrame::convertContext(const AVFormatContext *source, const AVFormatContext *target, 
                             int tsid, bool fAdjustPts)
{
    correctPts(m_packet.pts, source, target, tsid, fAdjustPts);
    correctPts(m_packet.dts, source, target, tsid, fAdjustPts);
    m_packet.stream_index = tsid;

    return true;
}

bool VUDU::AVFrame::applyFilter(AVBitStreamFilterContext *filter,
                          AVCodecContext *codec)
{
    uint8_t *newData = m_packet.data;
    int newSize = m_packet.size;

    if (m_packet.size == 0) {
        return true;
    }

    int st = av_bitstream_filter_filter(filter, codec, NULL,
                                        &newData, &newSize,
                                        m_packet.data, m_packet.size,
                                        m_packet.flags & PKT_FLAG_KEY);

    if (st < 0) {
        fprintf(stderr, "AVFrame::applyFilter() failed");
        return false;
    }

    if (st > 0) {
        AVPacket oldPacket = m_packet;

        // Safety check to make sure function pointers resolve to the same address from
        // everywhere
        // Look at this e-mail thread for a more detailed explanation of the problem
        // https://lists.mplayerhq.hu/pipermail/ffmpeg-devel/2008-March/043263.html
        VUDUDebugAssert((oldPacket.destruct == NULL ||
                         oldPacket.destruct == av_destruct_packet ||
                         oldPacket.destruct == av_destruct_packet_nofree) &&
                        "try building ffmpeg without -Bsymbolic");

        av_free_packet(&oldPacket);
        m_packet.destruct= av_destruct_packet;
    }

    m_packet.data = newData;
    m_packet.size = newSize;

    return true;
}

// AVDemuxer implementation

// static callbacks
/* static */ int FfmpegAVDemuxer::read(void *opaque, uint8_t *buf, int size)
{
    FfmpegAVDemuxer *dmux = (FfmpegAVDemuxer *) opaque;
    return dmux->m_listener->read(buf, size);
}

/* static */ int64_t FfmpegAVDemuxer::seek(void *opaque, int64_t offset, int whence)
{
    FfmpegAVDemuxer *dmux = (FfmpegAVDemuxer *) opaque;

    if (whence == AVSEEK_SIZE) {
        if (dmux->m_fileSize != 0) {
            return dmux->m_fileSize;
        } else {
            return -1;
        }
    } else {
        return dmux->m_listener->seek(offset, whence);
    }
}

#define AV_MAY_FAIL(_operation, _msg) { \
    st = _operation; \
    if (st < 0) { \
        fprintf(stderr, "%s: " #_msg " failed with st=%d", __FUNCTION__, st); \
        m_isValid = false; \
        return false; \
    } \
}

#define AV_CHECK_PTR(_ptr) ((_ptr == NULL) ? -1 :  0)

struct FormatMapping {
    const char *name;
    const char *ffName;
};

static const FormatMapping mappings[] = {
    {"audio/mpeg", "mp3"},
    {"video/mp4", "mov,mp4,m4a,3gp,3g2,mj2"},
    {"video/ts", "mpegts"},
};

static const char *getFFMpegName(const char *name)
{
    for (uint32_t i = 0; i < ARRAY_COUNT(mappings); i++) {
        if (strncmp(mappings[i].name, name, strlen(mappings[i].name)) == 0) {
            return mappings[i].ffName;
        }
    }
    return NULL;
}

// Constructor. Can block for a while since it reads and parses the header
// Dont call while holding locks, blocking something else etc.
FfmpegAVDemuxer::FfmpegAVDemuxer(const char *fmt, Listener *listener, const int64_t fileSize) :
    m_context(NULL),
    m_listener(listener),
    m_fileSize(fileSize),
    m_buffer(NULL),
    m_isValid(true)
{
    const char *ffFmt = getFFMpegName(fmt);
    if (!init(ffFmt != NULL ? ffFmt : fmt)) {
        return;
    }
    VUDU_SetMovieType(0);
    #ifdef AVFORMAT_VERBOSE
    dump_format(m_context, 0, "bioInStream", 0);
    #endif
}

bool FfmpegAVDemuxer::init(const char *fmt)
{
    int st;
    m_buffer = new uint8_t[AV_BUFFER_SIZE];
    AV_MAY_FAIL(AV_CHECK_PTR(m_buffer), "allocate buffer")

    AV_MAY_FAIL(init_put_byte(&m_bioContext, m_buffer, AV_BUFFER_SIZE, 0,
                             (void*) this, read, NULL, seek),
                "init_put_byte");

    AVInputFormat *iFmt = av_find_input_format(fmt); 
    printf("%s\n",iFmt->name);
    if(strncmp(iFmt->name, "mp3", strlen("mp3")) == 0) {
    	m_bioContext.is_streamed = 1; // we get a lot fewer seek call from
                                  // libavformat when we set this flag
    }

    AV_MAY_FAIL(AV_CHECK_PTR(iFmt), "finding input format");

    AV_MAY_FAIL(av_open_input_stream(&m_context, &m_bioContext, "",
                                     iFmt, NULL),
                "av_open_input_stream");

    AV_MAY_FAIL(av_find_stream_info(m_context),
                "av_find_stream_info");
    return true;
}


FfmpegAVDemuxer::~FfmpegAVDemuxer()
{
    if (m_context != NULL) {
        av_close_input_stream(m_context);
    }

    delete[] m_buffer;
}

bool FfmpegAVDemuxer::readFrame(IAVFrame frame)
{
    if (!isValid()) {
        return false;
    }

    int st = av_read_frame(m_context, frame->getPacket());

    // TODO: Do we need a separate way to notify EOF? Hard to do
    // beacause avformat sometimes returns AVERROR(EIO), AVERROR(EPERM)
    // or AVERROR(EAGAIN) on EOF. So right now I am going with everything
    // being a fatal error. It all means the same for the caller anyway -
    // they cant read any more packets

    if (st < 0) {
        fprintf(stderr, "av_read_frame() failed with %d", st);
        m_isValid = false;
        return false;
    }

    return true;
}

bool FfmpegAVDemuxer::seekToTime(uint32_t time)
{
    int64_t target = ((int64_t) time) * AV_TIME_BASE / 1000LL;

    return seekWithFlags(target, 0);
}

bool FfmpegAVDemuxer::seekToBytes(uint64_t pos)
{
    return seekWithFlags(pos, AVSEEK_FLAG_BYTE);
}

bool FfmpegAVDemuxer::seekWithFlags(int64_t pos, int flags)
{
    if (!isValid()) {
        return false;
    }

    int st;
    AV_MAY_FAIL(av_seek_frame(m_context, -1, pos, flags),
                "av_seek_frame");

    return true;
}



// AVMuxer implementation

// static callbacks
/* static */ int FfmpegAVMuxer::write(void *opaque, uint8_t *buf, int size)
{
    FfmpegAVMuxer *mux = (FfmpegAVMuxer *) opaque;

    if (mux->isValid()) {
        int rv = mux->m_listener->write(buf, size);

        if (rv != size) {
            mux->m_isValid = false;
        }
    }

    // We NEVER fail a write called by ffmpeg. Failing writes prevents
    // a whole bunch of clean up code from executing => memory leaks
    return size;
}

// the muxers we use for output
extern AVOutputFormat mpegts_muxer;
extern AVOutputFormat mp3_muxer;

static AVOutputFormat *getMuxer(VUDU::Containers container)
{
    switch (container) {
    case VUDU::CONTAINER_TS:
        return &mpegts_muxer;
    case VUDU::CONTAINER_MP3:
        return &mp3_muxer;
    default:
        return NULL;
    }
}

FfmpegAVMuxer::FfmpegAVMuxer(AVDemuxer * const demux,
                             Listener *listener, VUDU::Containers container,
                             bool fApplyFilters) :
    m_context(NULL),
    m_inputContext(((FfmpegAVDemuxer *) demux)->getContext()),
    m_outputFormat(container),
    m_listener(listener),
    m_bsfContext(NULL),
    m_adtsWrapper(NULL),
    m_audioSid(-1),
    m_videoSid(-1),
    m_audioPid(-1),
    m_videoPid(-1),
    m_buffer(NULL),
    m_isValid(true),
    m_fTrailerWriteNeeded(false)
{
    if (!init(fApplyFilters))  {
        return;
    }

    #ifdef AVFORMAT_VERBOSE
    dump_format(m_context, 0, "bioOutStream", 1);
    #endif
}

bool FfmpegAVMuxer::init(bool fApplyFilters)
{
    int st;
    m_buffer = new uint8_t[AV_BUFFER_SIZE];
    AV_MAY_FAIL(AV_CHECK_PTR(m_buffer), "allocate buffer")

    AV_MAY_FAIL(init_put_byte(&m_bioContext, m_buffer, AV_BUFFER_SIZE, 1,
                             (void*) this, NULL, write, NULL),
                "init_put_byte");

    m_bioContext.is_streamed = 1; // we get a lot fewer seek calls from
                                  // libavformat when we set this flag

    // Create the output context
    m_context = avformat_alloc_context();
    AV_MAY_FAIL(AV_CHECK_PTR(m_context), "allocate output context");
    strcpy(m_context->filename, "");

    m_context->oformat = getMuxer(m_outputFormat);
    AV_MAY_FAIL(AV_CHECK_PTR(m_context->oformat), "finding muxer");

    m_context->pb = &m_bioContext;
    m_context->bit_rate = m_inputContext->bit_rate;

    // set the format params to default values
    AVFormatParameters fmtParams;
    memset(&fmtParams, 0, sizeof(AVFormatParameters));
    AV_MAY_FAIL(av_set_parameters(m_context, &fmtParams),
                "av_set_parameters");

    uint32_t streamsSoFar = 0; 
    // Look at streams in the input to set up our output stream
    for (uint32_t i=0; i < m_inputContext->nb_streams; i++) {
        AVStream *inputStream = m_inputContext->streams[i];
        CodecType codecType = inputStream->codec->codec_type;

        if (codecType == CODEC_TYPE_VIDEO && m_videoSid == -1) {
            m_videoSid = inputStream->index;
            m_videoPid = AV_PID_START + streamsSoFar;
        } else if (codecType == CODEC_TYPE_AUDIO && m_audioSid == -1) {
            m_audioSid = inputStream->index;
            m_audioPid = AV_PID_START + streamsSoFar;
        } else {
            continue;
        }

        streamsSoFar++;

        // Allocate the new stream
        AVStream *newStream = av_new_stream(m_context, inputStream->id);
        AV_MAY_FAIL(AV_CHECK_PTR(newStream), "allocate new stream");

        // probably not needed but just to be safe
        memcpy(newStream->language, inputStream->language, sizeof(inputStream->language));

        if (codecType == CODEC_TYPE_VIDEO) {
            avcodec_get_context_defaults2(newStream->codec, CODEC_TYPE_VIDEO);
            newStream->codec->codec_type = CODEC_TYPE_VIDEO;
            newStream->codec->pix_fmt = inputStream->codec->pix_fmt;
            newStream->codec->width = inputStream->codec->width;
            newStream->codec->height = inputStream->codec->height;
            newStream->codec->has_b_frames = inputStream->codec->has_b_frames;

            if (inputStream->codec->codec_id == CODEC_ID_H264 && fApplyFilters) {
                m_bsfContext = av_bitstream_filter_init("h264_mp4toannexb");
                AV_MAY_FAIL(AV_CHECK_PTR(m_bsfContext), "get h264_mp4toannexb");
            }
        } else {
            avcodec_get_context_defaults2(newStream->codec, CODEC_TYPE_AUDIO);
            newStream->codec->codec_type = CODEC_TYPE_AUDIO;
            newStream->codec->channel_layout = inputStream->codec->channel_layout;
            newStream->codec->channels = inputStream->codec->channels;
            newStream->codec->sample_rate = inputStream->codec->sample_rate;
            newStream->codec->frame_size = inputStream->codec->frame_size;
            newStream->codec->block_align = inputStream->codec->block_align;
            if (inputStream->codec->codec_id == CODEC_ID_AAC && fApplyFilters) {
                m_adtsWrapper = av_bitstream_filter_init("aac_adts_adder_bsf");
                AV_MAY_FAIL(AV_CHECK_PTR(m_adtsWrapper), "get aac_adts_adder_bsf");
            }
        }

        newStream->codec->codec_id = inputStream->codec->codec_id;

        // Fix broken block_align if needed
        if (newStream->codec->codec_id == CODEC_ID_MP3 ||
            newStream->codec->codec_id == CODEC_ID_AC3) {
            newStream->codec->block_align= 0;
        }

        // Copy over more fields from the input
        newStream->codec->codec_tag = inputStream->codec->codec_tag;
        newStream->codec->bit_rate = inputStream->codec->bit_rate;
        newStream->codec->extradata = inputStream->codec->extradata;
        newStream->codec->extradata_size = inputStream->codec->extradata_size;
        newStream->codec->priv_data = inputStream->codec->priv_data;

        // Check the input codec time_base for sanity and do some some workarounds
        if (av_q2d(inputStream->codec->time_base) *
               inputStream->codec->ticks_per_frame > av_q2d(inputStream->time_base) &&
            av_q2d(inputStream->time_base) < 1.0/1000) {
            newStream->codec->time_base = inputStream->codec->time_base;
            newStream->codec->time_base.num *= inputStream->codec->ticks_per_frame;
        } else {
            newStream->codec->time_base = inputStream->time_base;
        }
    }

    return true;
}

FfmpegAVMuxer::~FfmpegAVMuxer()
{
    m_isValid = false;

    if (m_context != NULL && m_fTrailerWriteNeeded) {
        // Writing the trailer frees up a whole bunch of memory
        // allocted during write_header, write_frame etc.
        // So we MUST call it here if it hasn't been called
        // before
        av_write_trailer(m_context);
    }

    if (m_bsfContext != NULL) {
        av_bitstream_filter_close(m_bsfContext);
    }

    if (m_adtsWrapper != NULL) {
        av_bitstream_filter_close(m_adtsWrapper);
    }

    if (m_context != NULL) {
        for (uint32_t i=0; i < m_context->nb_streams; i++) {
            AVStream *st = m_context->streams[i];
            av_metadata_free(&st->metadata);
            av_freep(&st->index_entries);
            av_freep(&st->codec);
            av_freep(&m_context->streams[i]);
        }
        av_metadata_free(&m_context->metadata);
        av_freep(&m_context->priv_data);
    }
    av_freep(&m_context);

    delete[] m_buffer;
}

bool FfmpegAVMuxer::writeHeader()
{
    if (!isValid()) {
        return false;
    }

    m_fTrailerWriteNeeded = true;

    int st;
    AV_MAY_FAIL(av_write_header(m_context),
                "av_write_header");

    return isValid();
}

bool FfmpegAVMuxer::writeTrailer()
{
    if (!isValid()) {
        return false;
    }

    m_fTrailerWriteNeeded = false;

    int st;
    AV_MAY_FAIL(av_write_trailer(m_context),
                "av_write_trailer");

    return isValid();
}

bool FfmpegAVMuxer::convertFrame(IAVFrame frame, bool fAdjustPts)
{
    if (!isValid()) {
        return false;
    }

    int inputStreamIndex = frame->getPacket()->stream_index;
    int outputStreamIndex = 0;

    // find if we are interested in this frame and do the right thing
    if (inputStreamIndex == m_videoSid) {
        if(m_bsfContext != NULL) {
            if (!frame->applyFilter(m_bsfContext,
                           m_inputContext->streams[inputStreamIndex]->codec)) {
                return false;
            }
        }

        outputStreamIndex = (m_videoPid - AV_PID_START);
    } else if (inputStreamIndex == m_audioSid) {
        if(m_adtsWrapper != NULL) {
            if (!frame->applyFilter(m_adtsWrapper,
                           m_inputContext->streams[inputStreamIndex]->codec)) {
                return false;
            }
        }

        outputStreamIndex = (m_audioPid - AV_PID_START);
    } else {
        return false;
    }

    return frame->convertContext(m_inputContext, m_context, outputStreamIndex, fAdjustPts);
}

bool FfmpegAVMuxer::writeFrame(IAVFrame frame)
{
    VUDUDebugAssert(frame->getPacket()->stream_index < (int32_t) m_context->nb_streams);
    if (!isValid()) {
        return false;
    }

    int st;

    if (frame->getPacket()->size == 0) {
        return true;
    }

    AV_MAY_FAIL(av_interleaved_write_frame(m_context, frame->getPacket()),
                "av_interleaved_write_frame");

    return isValid();
}

uint32_t FfmpegAVMuxer::getDuration() const
{
    if (m_inputContext == NULL) {
        return 0;
    }

    return (uint32_t) (m_inputContext->duration*1000 / AV_TIME_BASE);
}

bool FfmpegAVMuxer::getAudioCodec(VUDU::AudioCodecs & aCodec) const
{
    if (m_audioSid != -1) {
        switch (m_inputContext->streams[m_audioSid]->codec->codec_id) {
        case CODEC_ID_MP3:
            aCodec = VUDU::AUDIO_CODEC_MP3;
            return true;
        case CODEC_ID_AC3:
        case CODEC_ID_EAC3:
            aCodec = VUDU::AUDIO_CODEC_DDP;
            return true;
        case CODEC_ID_AAC:
            aCodec = VUDU::AUDIO_CODEC_AAC;
            return true;
        default:
            fprintf(stderr, "FfmpegAVMuxer::getAudioCodec() has unhandled codec id %u",
                    m_inputContext->streams[m_audioSid]->codec->codec_id);
            aCodec = VUDU::AUDIO_CODEC_UNKNOWN;
            // fall though
        }
    }

    return false;
}

bool FfmpegAVMuxer::getVideoCodec(VUDU::VideoCodecs & vCodec) const
{
    if (m_videoSid != -1) {
        switch (m_inputContext->streams[m_videoSid]->codec->codec_id) {
        case CODEC_ID_MPEG2VIDEO:
            vCodec = VUDU::VIDEO_CODEC_MPEG2;
            return true;
        case CODEC_ID_H264:
            vCodec = VUDU::VIDEO_CODEC_H264;
            return true;
        default:
            fprintf(stderr, "FfmpegAVMuxer::getVideoCodec() has unhandled codec id %u",
                    m_inputContext->streams[m_videoSid]->codec->codec_id);
            vCodec = VUDU::VIDEO_CODEC_UNKNOWN;
            // fall though
        }
    }
    return false;
}

bool FfmpegAVMuxer::getContentInfo(VUDU::MediaBuffer::ContentInfo *ci) const
{
    if (m_videoSid != -1) {
        AVStream *st = m_inputContext->streams[m_videoSid];

        ci->width = st->codec->width;
        ci->height = st->codec->height;

        // XXX no clue how to find these from libavformat
        ci->pixelAspectNumerator = 1;
        ci->pixelAspectDenominator = 1;
        ci->framesPerKiloSeconds = (uint32_t) (av_q2d(st->r_frame_rate) * 1000);
        ci->dimensionality = MediaBuffer::DIM_2D;
    } else {
        ci->width = 0;
        ci->height = 0;
        ci->pixelAspectNumerator = 0;
        ci->pixelAspectDenominator = 0;
        ci->framesPerKiloSeconds = 0;
        ci->dimensionality = MediaBuffer::DIM_2D;
    }

    if (m_audioSid != -1) {
        AVStream *st = m_inputContext->streams[m_audioSid];
        ci->numAudioChannels = st->codec->channels;
        ci->audioSampleRate = st->codec->sample_rate;
    } else {
        ci->numAudioChannels = 0;
        ci->audioSampleRate = 0;
    }

    ci->duration = getDuration();

    ci->videoPid = std::max(m_videoPid, 0);
    ci->audioPid = std::max(m_audioPid, 0);

    getVideoCodec(ci->videoCodec);
    getAudioCodec(ci->audioCodec);

    ci->isEncryped = false;
    ci->containerType = m_outputFormat;
    ci->pingPeriod = 0;
    ci->pingPeriodFast = 0;

    return true;
}

extern AVBitStreamFilter aac_adts_adder_bsf;

FfmpegRemuxerDriver::FfmpegRemuxerDriver()
{
    static bool fAvRegisterAllDone = false;
    if (fAvRegisterAllDone == false) {
        av_register_all();
        av_register_bitstream_filter(&aac_adts_adder_bsf);
        fAvRegisterAllDone = true;
    }
}

static FfmpegRemuxerDriver *driver = NULL;

VUDU::IRemuxerDriver CreateFfmpegRemuxerDriver()
{
    driver = new FfmpegRemuxerDriver();

    return driver;
}

void DestroyFfmpegRemuxerDriver()
{
    delete driver;
    driver = NULL;
}


