/* PlayBackDev.cpp
   -- implement playback device for YPPL
   -- using FFMPEG first
   -- using streamplayer
   */
extern "C" {
#include "streaming_app.h"
}
#include <PlaybackDev.h>

static const int32_t AV_BUFFER_SIZE = 64 * 1024;
static const int32_t FIFOS_MIN_VALUE = 1024 * 3;
static const int32_t FIFOS_MAX_VALUE = 1024 * 512;
static const int32_t FORWARD_INTERVAL = 50;
static const UINT32  SLEEP_TICKS = 100;
static bool EOFlag = false;

//#define LOCALFILE_DEBUG

#define AV_MAY_FAIL(_operation, _msg) { \
    exitcode = _operation; \
    if (exitcode < 0) { \
        fprintf(stderr, "%s: " #_msg " failed with exitcode=%d\n", __FUNCTION__, exitcode); \
        if (mListener){ \
            mListener->SendPlaybackEvent(END_OF_STREAM); \
        } \
        return NF_FAIL; \
    } \
}

int PlayBackRCoreDev::read(void *opaque, uint8_t *buf, int size) {
    int realsize = 0;
    PlayBackRCoreDev * pdev = (PlayBackRCoreDev *) opaque;
    realsize = pdev->mVospStreamer->vosp_stream_read(buf, size, 1);
    //cout<<"start read to data from yppl  :"<<size << " and realsize " << realsize <<endl;
    EOFlag = pdev->mVospStreamer->vosp_is_media_end(realsize);
    return realsize;
}

int64_t PlayBackRCoreDev::seek(void *opaque, int64_t offset, int whence) {
    PlayBackRCoreDev * pdev = (PlayBackRCoreDev *) opaque;
    cout<<"seek to data from yppl  :"<<offset<<endl;
    return pdev->mVospStreamer->vosp_stream_seek(offset, whence);
}

void * PlayBackRCoreDev::feedDataThreadEntry(void * p) {
    PlayBackRCoreDev * pdev = (PlayBackRCoreDev *) p;
    while (pdev->running_) {
        if ((pdev->getDeviceStatus() == PLAYING)
                || (pdev->getDeviceStatus() == BUFFERING)) {
            if (NF_OK != pdev->feedData()) {
                continue;
            } else {
                /*
                   if send packet data succufully ,should free the buffer outside feedata function for resend
                   */
                av_free_packet(pdev->packet);
            }
        }
        usleep(SLEEP_TICKS);
    }
    pthread_detach(pthread_self());
    return NULL;
}

void * PlayBackRCoreDev::moniterFifoThreadEntry(void * p) {
    PlayBackRCoreDev * pdev = (PlayBackRCoreDev *) p;
    UINT32 videofull = 0, audiofull = 0 , unusedvalue = 0;
    bool fifoflag = false;
    int ratio = 0;
    while (pdev->running_) {
        if (pdev->getDeviceStatus() == BUFFERING || pdev->getDeviceStatus() == PLAYING){
            StreamingApp_getAVFifoStatus(pdev->videoStreamingSession,&videofull, &unusedvalue);
            StreamingApp_getAVFifoStatus(pdev->audioStreamingSession,&unusedvalue, &audiofull);
            pdev->mVospStreamer->vosp_stream_bufratio(&ratio);
        }
        if (pdev->getDeviceStatus() == BUFFERING ){
            if ( 100 == ratio && pdev->mListener){
                pdev->mListener->SendPlaybackEvent(LOADEND);
                pdev->setSpeed(NORMAL);
            }
        }
        if (pdev->getDeviceStatus() == PLAYING) {
            if (videofull > FIFOS_MAX_VALUE){
                fifoflag = true;
            }
            if ( ((videofull < FIFOS_MIN_VALUE) || (audiofull < FIFOS_MIN_VALUE)) && fifoflag && (100 != ratio) ) {
                    if (pdev->mListener){
                        pdev->mListener->SendPlaybackEvent(DATAEXHAUSTED);
                    }
                    StreamingApp_Pause(pdev->audioStreamingSession);
                    StreamingApp_Pause(pdev->videoStreamingSession);
                    pdev->curStatus = BUFFERING;
                    fifoflag = false;
                    cout << "go to buffering" << endl;
            }
        }
        usleep(SLEEP_TICKS);
    }
    pthread_detach(pthread_self());
    return NULL;
}

static BOOLOP HandleEosEvent(UINT32 userParam) {
    PlayBackRCoreDev * pdev = (PlayBackRCoreDev *) userParam;
    cout << "receive EOS from RT side " << endl;
    if (pdev && !pdev->mEosHandledFlag)
        pdev->close();
}

Perror PlayBackRCoreDev::initStreamer(uint32_t scr)
{
    S_CODEC_PARAMS audioCodecParams, videoCodecParams;
    S_PRIVATE_DATA codecPrivate;
    /*
    if (TRUE
            != StreamingApp_construct(CONTROL_TYPE_VIDEO, TRUE, scr,
                                      &videoStreamingSession)) {
                                      */
    if (TRUE
            != StreamingApp_construct(CONTROL_TYPE_VIDEO, FALSE, 0,
                                      &videoStreamingSession)) {    
        cout << "Video streaming player construct fail" << endl;
        return NF_FAIL;
    }
    if (TRUE
            != StreamingApp_construct(CONTROL_TYPE_AUDIO, FALSE, 0,
                                      &audioStreamingSession)) {
        cout << "Video streaming player construct fail" << endl;
        return NF_FAIL;
    }

    videoCodecParams.data.VIDEO.videoType = CONTROL_VIDEO_CODING_TYPE_AVC;
    videoCodecParams.data.VIDEO.isVC1Advanced = FALSE;
    videoCodecParams.data.VIDEO.fallbackFrameRateValid = FALSE;

    audioCodecParams.data.AUDIO.audioParams.type = AV_CTRL_AUDIO_CODING_TYPE_AAC_E;
    audioCodecParams.data.AUDIO.audioSamplingRate = 48000;
    audioCodecParams.data.AUDIO.audioParams.data.aac.frameLength = 0xE00;

    for (int i = 0; i < m_context->nb_streams; i++) {
        AVCodecContext *enc = m_context->streams[i]->codec;
        switch (enc->codec_type) {
        case CODEC_TYPE_AUDIO: {
            audioStream = i;
            StreamingApp_setAudioPid(audioStreamingSession,
                                     m_context->streams[i]->id);
            cout <<"audio id :"<< m_context->streams[i]->id << " Audio num:" << m_context->streams[i]->time_base.num
                 << " ; den:" << m_context->streams[i]->time_base.den << endl;
        }
        break;
        case CODEC_TYPE_VIDEO: {
            videoStream = i;

            codecPrivate.isVideo = TRUE;
            codecPrivate.dataArr = (BYTE *) enc->extradata;
            codecPrivate.sizeBytes = enc->extradata_size;

            StreamingApp_setCodecPrivateData(videoStreamingSession,
                                             &codecPrivate);
            StreamingApp_setVideoPid(videoStreamingSession,
                                     m_context->streams[i]->id);
            StreamingApp_setPcrPid(videoStreamingSession,
                                   m_context->streams[i]->id);
            cout  <<"video id :"<<m_context->streams[i]->id << " Video num:" << m_context->streams[i]->time_base.num
                  << " ; den:" << m_context->streams[i]->time_base.den << endl;
        }
        break;
        default:
            break;
        }
    }

    //set video codecparams
    if (TRUE
            != StreamingApp_setCodecParams(videoStreamingSession,
                                           &videoCodecParams)) {
        cout << "Video streaming player SetCodec fail" << endl;
        return NF_FAIL;
    }

    //set audio codecparams
    if (TRUE
            != StreamingApp_setCodecParams(audioStreamingSession,
                                           &audioCodecParams)) {
        cout << "Audio streaming player SetCodec fail" << endl;
        return NF_FAIL;
    }

    if (TRUE
            != StreamingApp_subscribeEosEvt(audioStreamingSession,
                                            HandleEosEvent, (UINT32) this)) {
        cout << "Register Eos event fail" << endl;
        return NF_FAIL;
    }

    if (TRUE
            != StreamingApp_subscribeEosEvt(videoStreamingSession,
                                            HandleEosEvent, (UINT32) this)) {
        cout << "Register Eos event fail" << endl;
        return NF_FAIL;
    }
    return NF_OK;
}

void PlayBackRCoreDev::destroyStreamer(void)
{
    StreamingApp_unsubscribeEosEvt(audioStreamingSession);
    StreamingApp_unsubscribeEosEvt(videoStreamingSession);
    StreamingApp_stop(audioStreamingSession);
    StreamingApp_stop(videoStreamingSession);
    StreamingApp_destruct(&audioStreamingSession);
    StreamingApp_destruct(&videoStreamingSession);
}

void PlayBackRCoreDev::startFeedDataAndFifoMoniterThread(void)
{
    running_ = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&thread, &attr, feedDataThreadEntry, (void*) this);
    pthread_create(&fifothread, &attr, moniterFifoThreadEntry, (void*) this);
    pthread_attr_destroy(&attr);
}

void PlayBackRCoreDev::stopFeedDataAndFifoMoniterThread(void)
{
    running_ = false;
    pthread_join(thread, NULL);
    pthread_join(fifothread, NULL);
}

Perror PlayBackRCoreDev::open() {

    int exitcode = 0;

    if (curStatus != IDLE) {
        cout << "can't open the busy device!" << endl;
        return NF_FAIL;
    }

#ifndef LOCALFILE_DEBUG
    if (0 != mVospStreamer->vosp_stream_open(currentUrl.c_str(), NULL)) {
        cout << "Can't open the remote stream file" << endl;
        return NF_OK;
    }
    cout << "-->Get the size of stream is:"
         << mVospStreamer->vosp_stream_length() << endl;
    cout << "-->Get the size of index file is:"
         << mVospStreamer->vosp_stream_idx_size() << endl;

    mVospStreamer->vosp_stream_buffering(4 * 1024 * 1024);

    for (int ratio = 0; ratio < 100;) {
        mVospStreamer->vosp_stream_bufratio(&ratio);
        cout << "===== buffering ratio=" << ratio << endl;
        if (ratio < 100)
            usleep(SLEEP_TICKS);
    }
#endif

    /*  start ffmpeg initial here  */
    av_register_all();

#ifndef LOCALFILE_DEBUG
    if(m_buffer){
        delete[] m_buffer;
        m_buffer = NULL;
    }
    
    m_buffer = new uint8_t[AV_BUFFER_SIZE];

    init_put_byte(&m_bioContext, m_buffer, AV_BUFFER_SIZE, 0, (void*) this,
                  read, NULL, seek);

    AVInputFormat *iFmt = av_find_input_format("mp4");

    AV_MAY_FAIL(av_open_input_stream(&m_context, &m_bioContext, "", iFmt, NULL), "av_open_input_stream");

    AV_MAY_FAIL(av_find_stream_info(m_context), "av_find_stream_info");
#else

    if(av_open_input_file(&m_context, "/media/sda/movie/zhizhuren.mp4", NULL, 0, NULL)!=0)
    {
        cout<<"can't open this file "<<endl;
        return NF_FAIL;
    }
    av_find_stream_info(m_context);

#endif
    //dump_format(m_context, 0, "bioInStream", 0);
    if (packet){
        free(packet);
        packet = NULL;
    }
    packet = (AVPacket *) malloc(sizeof(AVPacket));
    av_init_packet(packet);

    /*   start streamer proxy */
    cout << "=====>     Try to initial the streamer Player  <======" << endl;
    if(NF_OK != initStreamer()){
        return NF_FAIL;
    }

    /* Start one pthread to feeddata*/
    startFeedDataAndFifoMoniterThread();

    curStatus = READY;
    mEosHandledFlag = false;
    EOFlag = false;
    mKeyFramePts = 0;
    if (mListener)
        mListener->SendPlaybackEvent(LOADEND);

    setSpeed(NORMAL);
    return NF_OK;
}

Perror PlayBackRCoreDev::feedData() {

    void* streamingSession;
    int st;

    if ((true == resend) && (packet != NULL)) {
        resend = false;
        usleep(SLEEP_TICKS);
    } else {
        st = av_read_frame(m_context, packet);

        // TODO: Do we need a separate way to notify EOF? Hard to do
        // beacause avformat sometimes returns AVERROR(EIO), AVERROR(EPERM)
        // or AVERROR(EAGAIN) on EOF. So right now I am going with everything
        // being a fatal error. It all means the same for the caller anyway -
        // they cant read any more packets

        if (st < 0) {
            if (EOFlag) {
                cout << "receive EOF from file parser" << endl;
                StreamingApp_setEOS(audioStreamingSession);
                StreamingApp_setEOS(videoStreamingSession);
                EOFlag = false;

            }

            cout << "av_read_frame() failed with " << st << endl;
            return NF_FAIL;
        }
    }

    if (packet->stream_index == videoStream) {
        streamingSession = videoStreamingSession;
    } else if(packet->stream_index == audioStream) {
        streamingSession = audioStreamingSession;
    } else {
        return NF_OK;
    }
    
    if(packet->size > 1000000){
        return NF_OK;
    }

    UINT32 pts = 0;
    if (0 != m_context->streams[packet->stream_index]->time_base.den)
        pts =
            (UINT32)(
                packet->pts * 1000
                * m_context->streams[packet->stream_index]->time_base.num
                / m_context->streams[packet->stream_index]->time_base.den);

    if(packet->stream_index == videoStream){
        mLastPts = pts; 
        //cout << "feedData : packet->pts: " << packet->pts << " pts:" << pts << " flags: " << packet->flags<< endl;
        if(mKeyFramePts == 0 || mKeyFramePts == m_context->duration){
            if (mListener){
                mListener->SendPlaybackEvent(NOTIFY_PLAY);
            }            
            mKeyFramePts = mLastPts;
        }
    }
    if(packet->stream_index == audioStream){
        //cout << "pts " << pts << " mKeyFramePts " << mKeyFramePts << endl;
        if( (pts < mKeyFramePts) && (mKeyFramePts > 0) && (mKeyFramePts < m_context->duration) && (pts *10 > mKeyFramePts)){
                return NF_OK;
        }
    }
    /*
    if (OK != StreamingApp_feedData(streamingSession, packet->data,
                                    packet->size, pts, TRUE, 0)) {
                                    */
    if (OK != StreamingApp_feedData(streamingSession, packet->data,
                                    packet->size, 0, FALSE, 0)) {
        resend = true;
        //cout << "StreamingApp_feedData failed,should resend:" << packet->size << endl;
        return NF_FAIL;
    }

    return NF_OK;
}

Perror PlayBackRCoreDev::flush() {
    return NF_OK;
}

Perror PlayBackRCoreDev::close() {

    cout << "========Try to stop the device!=======" << endl;

    if (getDeviceStatus() == IDLE)
        return NF_OK;

    curStatus = IDLE;
    stopFeedDataAndFifoMoniterThread();
    destroyStreamer();

    if (m_context != NULL) {
        av_close_input_stream(m_context);
        m_context = NULL;

        delete[] m_buffer;
        m_buffer = NULL;
    }

    if (packet){
        free(packet);
        packet = NULL;
    }

#ifndef LOCALFILE_DEBUG
    mVospStreamer->vosp_stream_close();
#endif

    if (mListener)
        mListener->SendPlaybackEvent(END_OF_STREAM);

    return NF_OK;
}

Perror PlayBackRCoreDev::setPlayUrl(string & pUrl) {
    if (curStatus != IDLE) {
        cout << "can't open the busy device!" << endl;
        return NF_FAIL;
    }
    currentUrl = pUrl;
    cout << "Set the url is " << pUrl << endl;
    return NF_OK;
}

Perror PlayBackRCoreDev::setSpeed(Speed playspeed) {

    if (getDeviceStatus() == IDLE) {
        cout << "Unaviable playback device can be used" << endl;
        return NF_OK;
    }
    switch (playspeed) {
    case NORMAL: {
        if (getDeviceStatus() == READY) {
            cout << "start video" << endl;
            if (TRUE != StreamingApp_start(videoStreamingSession))
                cout << "Unaviable playback device can be used" << endl;

            cout << "start audio" << endl;
            if (TRUE != StreamingApp_start(audioStreamingSession))
                cout << "Unaviable playback device can be used" << endl;

            if (mListener)
                mListener->SendPlaybackEvent(NOTIFY_PLAY);

            curStatus = PLAYING;

        } else if (getDeviceStatus() == SLEEPING
                   || getDeviceStatus() == BUFFERING) {
            if (TRUE != StreamingApp_Resume(audioStreamingSession))
                cout << "Unaviable playback device can be used" << endl;

            if (TRUE != StreamingApp_Resume(videoStreamingSession))
                cout << "Unaviable playback device can be used" << endl;

            if (mListener)
                mListener->SendPlaybackEvent(NOTIFY_PLAY);

            curStatus = PLAYING;
        }
    }
    break;
    case PAUSE: {
        if (getDeviceStatus() == PLAYING) {
            if (TRUE != StreamingApp_Pause(audioStreamingSession))
                cout << "Unaviable playback device can be used" << endl;

            if (TRUE != StreamingApp_Pause(videoStreamingSession))
                cout << "Unaviable playback device can be used" << endl;

            if (mListener)
                mListener->SendPlaybackEvent(NOTIFY_PAUSE);
        }
        curStatus = SLEEPING;
        cout << "go to SLEEPING" << endl;
    }
    break;
    default:
        return NF_OK;
    }
    return NF_OK;
}
void PlayBackRCoreDev::setEventListener(YpplEventListener * listener) {
    mListener = listener;
}

Perror PlayBackRCoreDev::searchTime(bool direction)
{
    if (getDeviceStatus() == IDLE)
        return NF_OK;

    int64_t seektime = 0;
    if(direction){
        seektime = av_rescale_q(mLastPts * 1000 + FORWARD_INTERVAL*AV_TIME_BASE, AV_TIME_BASE_Q,m_context->streams[videoStream]->time_base);   
        if(seektime >= m_context->duration){
            mKeyFramePts = m_context->duration;
            if (mListener){
                mListener->SendPlaybackEvent(ERRORMSG);
            }
            return NF_OK;
        }
        if (mListener){
            mListener->SendPlaybackEvent(NOTIFY_FF);
        }
    }else{
        seektime = av_rescale_q(mLastPts * 1000 - FORWARD_INTERVAL*AV_TIME_BASE, AV_TIME_BASE_Q,m_context->streams[videoStream]->time_base);        cout << "seektime " << seektime << "duration " << m_context->duration << endl;
        if(seektime < 0 || seektime >= m_context->duration ){
            mKeyFramePts = 0;
            if (mListener){
                mListener->SendPlaybackEvent(ERRORMSG);
            }
            return NF_OK;
        }
        if (mListener){
            mListener->SendPlaybackEvent(NOTIFY_FB);
        }
    }

    stopFeedDataAndFifoMoniterThread();
    destroyStreamer();
    cout << "searchTime:" << seektime << " mLastPts :"<< mLastPts <<endl;
    if( av_seek_frame(m_context, videoStream, seektime, 0) < 0){
        cout << "searchTime failed " << endl;
        curStatus = IDLE;
        if (m_context != NULL) {
            av_close_input_stream(m_context);
            m_context = NULL;
            delete[] m_buffer;
            m_buffer = NULL;
        }
        if (packet){
            free(packet);
            packet = NULL;
        }
        #ifndef LOCALFILE_DEBUG
            mVospStreamer->vosp_stream_close();
        #endif
        if (mListener){
            mListener->SendPlaybackEvent(END_OF_STREAM);
        }
        return NF_OK;
    }
    for (int ratio = 0; ratio < 100;) {
        mVospStreamer->vosp_stream_bufratio(&ratio);
        cout << "===== buffering ratio=" << ratio << endl;
        if (ratio < 100)
            usleep(SLEEP_TICKS);
    }
    curStatus = READY;
    mEosHandledFlag = false;
    EOFlag = false;
    resend = true;
    av_read_frame(m_context, packet);
    mKeyFramePts =(UINT32)(packet->pts * 1000 * m_context->streams[packet->stream_index]->time_base.num/ m_context->streams[packet->stream_index]->time_base.den);
    initStreamer(mKeyFramePts);
    startFeedDataAndFifoMoniterThread();
    setSpeed(NORMAL);
    return NF_OK;
}

Perror PlayBackRCoreDev::fastForward()
{
    searchTime(true);
}

Perror PlayBackRCoreDev::fastBackward()
{
    searchTime(false);
}
