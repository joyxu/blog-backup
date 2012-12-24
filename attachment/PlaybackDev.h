#ifndef PLAYBACKDEV_H
#define PLAYBACKDEV_H

#include <IPlaybackDev.h>
#include <iostream>
#include <VospStreamFileSystem.h>

extern "C" {
#include <libavformat/avformat.h>
}

class PlayBackRCoreDev: public IPlayBackDev {
public:
    /** Pre-condition: the device is currently closed. */
    virtual ~PlayBackRCoreDev() {
        delete mVospStreamer;
    }
    Perror open();
    Perror close();
    Perror feedData();
    Perror flush();
    Perror endofstream();
    Perror setPlayUrl(string & pUrl);
    Perror setSpeed(Speed playback);
    Perror fastForward();
    Perror fastBackward();
    DevStatus getDeviceStatus() {
        return curStatus;
    }
    void setEventListener(YpplEventListener * listener);

public:
    PlayBackRCoreDev() :
        curStatus(IDLE), resend(false), mLastPts(0), mKeyFramePts(0), mVospStreamer(new VospStreamer()), m_buffer(NULL), packet(NULL){
    }

private:
    string currentUrl;
    VospStreamer * mVospStreamer;

private:
    // FFMPEG
    uint8_t *m_buffer;
    DevStatus curStatus;
    ByteIOContext m_bioContext;
    AVFormatContext *m_context;
    AVPacket *packet;

    bool resend;
    int videoStream;
    int audioStream;
    pthread_t thread;
    pthread_t fifothread;
    
    Perror initStreamer(uint32_t scr = 0);
    void destroyStreamer(void);

    void startFeedDataAndFifoMoniterThread(void);
    void stopFeedDataAndFifoMoniterThread(void);
    Perror searchTime(bool direction);

    // Callbacks for libavformat
    static int read(void *opaque, uint8_t *buf, int size);
    static int64_t seek(void *opaque, int64_t offset, int whence);
    static void * feedDataThreadEntry(void *p);
    static void * moniterFifoThreadEntry(void *p);

public:
    // playbackThreadEntry
    volatile bool running_;
    bool mEosHandledFlag; 
private:
    //streamer player variables
    void* audioStreamingSession;
    void * videoStreamingSession;
    YpplEventListener * mListener;
    int64_t mLastPts;
    int64_t mKeyFramePts;
};

#endif
