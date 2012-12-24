//
//  Copyright (c) 2009 VUDU Inc.
//  All rights reserved.
//

#ifndef __FFMPEGREMUXERDRIVER_DOT_H_
#define __FFMPEGREMUXERDRIVER_DOT_H_

#include <vudu/VUDUMediaPlayer.h>
#include <vudu/VUDURemuxerDriver.h>
extern "C" {
#include <libavformat/avformat.h>
}

// NOTE: This wrapper is *NOT* thread safe. If using from more that one thread, the
// caller has to do all the necessary locking


namespace VUDU {

// Very basic wrapper for libavformat AVPacket
class AVFrame
{
public:
    AVFrame() {}
    ~AVFrame() { av_free_packet(getPacket()); }

    bool getPts(const AVFormatContext *context,
                uint32_t &pts /* output, in milliseconds */);
    
    bool convertContext(const AVFormatContext *source, 
                        const AVFormatContext *target, 
                        int tsid, bool fAdjustPts);

    // apply a bitstream filter
    bool applyFilter(AVBitStreamFilterContext *filter,
                     AVCodecContext *codec);

    AVPacket *getPacket() { return &m_packet; }
private:
    AVPacket m_packet;

    // Helper function for updating pts/dts values
    void correctPts(int64_t &ts, const AVFormatContext *source,
                    const AVFormatContext *target, int tsid, bool fAdjustPts);
};

} // namespace VUDU

// This is the class that implements adding ADTS headers to packets
// Setup libavformat stuff for reading and demuxing a container
class FfmpegAVDemuxer : public VUDU::AVDemuxer
{
public:

    // Create a demuxer
    // Params:
    //    const char *fmt - short name of the input container to be
    //                      passed on to ffmpeg.
    //    Listener *listener - callback interface
    //    int64_t fileSize - size of the file. Can be set to zero if not known
    FfmpegAVDemuxer(const char *fmt, Listener *listener, const int64_t fileSize);

    bool isValid() const { return m_isValid; }

    // Read an av packet from the input stream
    bool readFrame(VUDU::IAVFrame frame);

    // Seek a a specific time/bytes
    // Bytes does not really work properly for most containers, but I am
    // exposing it anyway
    bool seekToTime(uint32_t time); // time in milliseconds
    bool seekToBytes(uint64_t pos);

    AVFormatContext *getContext()
    {
        return isValid() ? m_context : NULL;
    }

    ~FfmpegAVDemuxer();

private:
    AVFormatContext *m_context;
    ByteIOContext m_bioContext;
    Listener * const m_listener;
    const int64_t m_fileSize;

    uint8_t *m_buffer;

    bool m_isValid;

    // helper for constructor
    bool init(const char *fmt);

    // Callbacks for libavformat
    static int read(void *opaque, uint8_t *buf, int size);
    static int64_t seek(void *opaque, int64_t offset, int whence);

    bool seekWithFlags(int64_t pos, int flags);
};


// Create an output ts stream
class FfmpegAVMuxer : public VUDU::AVMuxer
{
public:
    FfmpegAVMuxer(VUDU::AVDemuxer * const demux,
                  Listener *listener, VUDU::Containers container, 
                  bool fApplyFilters);

    ~FfmpegAVMuxer();

    bool isValid() const { return m_isValid; }

    AVFormatContext *getContext() { return m_context; }

    bool getPts(VUDU::IAVFrame fr, uint32_t & pts) const
    {
       return fr->getPts(m_context, pts);
    }

    bool writeHeader();
    bool writeTrailer();

    // conver the frame to output format
    // does not write it out
    bool convertFrame(VUDU::IAVFrame frame, bool fAdjustPts);

    // write a converted frame
    bool writeFrame(VUDU::IAVFrame frame);

    bool getContentInfo(VUDU::MediaBuffer::ContentInfo *ci) const;

private:
    AVFormatContext *m_context;
    const AVFormatContext *m_inputContext;
    const VUDU::Containers m_outputFormat;

    ByteIOContext m_bioContext;
    Listener * const m_listener;

    AVBitStreamFilterContext *m_bsfContext;
    AVBitStreamFilterContext *m_adtsWrapper;

    int32_t m_audioSid;
    int32_t m_videoSid;
    int m_audioPid;
    int m_videoPid;
    uint8_t *m_buffer;

    bool m_isValid;
    bool m_fTrailerWriteNeeded;

    bool init(bool fApplyFilters);

    uint32_t getDuration() const; // in milliseconds
    bool getAudioCodec(VUDU::AudioCodecs & aCodec) const;
    bool getVideoCodec(VUDU::VideoCodecs & vCodec) const;

    // Callbacks for libavformat
    static int write(void *opaque, uint8_t *buf, int size);
};

class FfmpegRemuxerDriver : public VUDU::RemuxerDriver
{
public:
    // Constructor - initialize global stuff
    FfmpegRemuxerDriver();

    // Create and destroy a packet
    VUDU::IAVFrame createNewFrame() { return new VUDU::AVFrame(); }
    void destroyFrame(VUDU::IAVFrame frame) { delete frame; }

    /// Create a demuxer
    VUDU::IAVDemuxer createDemuxer(const char *format,
                                   VUDU::AVDemuxer::Listener *listener,
                                   const int64_t fileSize)
    {
        return new FfmpegAVDemuxer(format, listener, fileSize);
    }


    /// Destroy a demuxer
    void destroyDemuxer(VUDU::IAVDemuxer demux) { delete demux; }

    /// Create a ts muxer
    VUDU::IAVMuxer createMuxer(VUDU::Containers container,
                               VUDU::IAVDemuxer demux,
                               VUDU::AVMuxer::Listener *listener,
                               bool fApplyFilters)
    {
        return new FfmpegAVMuxer(demux, listener, container, fApplyFilters);
    }


    /// Destroy a ts muxer
    void destroyMuxer(VUDU::IAVMuxer remux) { delete remux; }
};

VUDU::IRemuxerDriver CreateFfmpegRemuxerDriver();
void DestroyFfmpegRemuxerDriver();

#endif

