//
//  video-capture.c
//  
//
//  Created by Ivan Sergunin on 21/06/16.
//
//

#include "video-capture.h"
#include <stdio.h>
#include <stdbool.h>

#include "qemu/osdep.h"
#include "qemu/timer.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define INBUF_SIZE 4096
#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096

typedef struct {
    AVCodecContext *codecContext; // Codec context for encoding video.
    AVPacket packet;              // Packet for encoded frame.
    FILE *outputFile;             // Result video file.
    int fps;                      // FPS for result video.
    QEMUTimer *captureTimer;      // Timer which fires video capture callback according to FPS.
} VideoCaptureContext;

static bool shouldCaptureVideo = false;
static VideoCaptureContext *currentVideoCaptureContext = NULL;

AVFrame* image_decode_example(const char *filename);
void start_capture_video(const char *filename, int fps);
void stop_capture_video();
void video_capture_callback(void *opaque);

AVFrame* image_decode_example(const char *filename)
{
    // Open input file.
    AVFormatContext *iFormatContext = avformat_alloc_context();
    int err = avformat_open_input(&iFormatContext, filename, NULL, NULL);
    if (err != 0) {
        printf("Error in opening input file: (%s), error code: (%d)\n", filename, err);
        return NULL;
    }
    
    // Finding stream information.
    if (avformat_find_stream_info(iFormatContext, NULL)<0) {
        printf("Unable to find stream info.\n");
        avformat_close_input(&iFormatContext); // Release AVFormatContext memory.
        return NULL;
    }
    
    // Finding video stream from number of streams.
    int videoStreamIndex = -1;
    
    for (int a = 0; a < iFormatContext->nb_streams; a++) {
        if (iFormatContext->streams[a]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex=a;
            break;
        }
    }
    if (videoStreamIndex == -1) {
        printf("Couldn't find video stream.\n");
        avformat_close_input(&iFormatContext);
        return NULL;
    }
    
    // Finding decoder for video stream of image.
    AVCodecContext *pCodecCtx = iFormatContext->streams[videoStreamIndex]->codec;
    AVCodec *pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
    if (pCodec == NULL) {
        printf("Cannot find decoder.\n");
        avformat_close_input(&iFormatContext);
        return NULL;
    }
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
        printf("Cannot open decoder.\n");
        avformat_close_input(&iFormatContext);
        return NULL;
    }
    
    // Reading video frame.
    AVPacket encodedPacket;
    av_init_packet(&encodedPacket);
    encodedPacket.data = NULL;
    encodedPacket.size = 0;
    
    // Now read a frame into this AVPacket.
    if (av_read_frame(iFormatContext,&encodedPacket) < 0) {
        printf("Cannot read frame.\n");
        av_packet_unref(&encodedPacket);
        av_free(pCodecCtx);
        av_free(iFormatContext);
        return NULL;
    }
    
    // Decoding the encoded video frame.
    AVFrame *decodedFrame = av_frame_alloc();
    avcodec_send_packet(pCodecCtx, &encodedPacket);
    int frameFinished = avcodec_receive_frame(pCodecCtx, decodedFrame);
    
    if (frameFinished == 0) {
        
        // Convert decoded Frame to AV_PIX_FMT_YUV420P
        struct SwsContext *img_convert_ctx;
        img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
                                         pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC,
                                         NULL, NULL, NULL);
        
        AVFrame *convertedFrame = av_frame_alloc();
        convertedFrame->height = pCodecCtx->height;
        convertedFrame->width = pCodecCtx->width;
        convertedFrame->format = AV_PIX_FMT_YUV420P;
        
        int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 32);
        uint8_t* convertedFrame_buffer = (uint8_t *)av_malloc(num_bytes*sizeof(uint8_t));
        
        // av_image_fill_arrays(convertedFrame->data, convertedFrame->linesize, convertedFrame_buffer, AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 32);
        av_image_alloc(convertedFrame->data, convertedFrame->linesize, pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, 32);
        
        sws_scale(img_convert_ctx, decodedFrame->data, decodedFrame->linesize, 0, pCodecCtx->height, convertedFrame->data, convertedFrame->linesize);
        sws_freeContext(img_convert_ctx);
        
        return convertedFrame;
    }
    else {
        printf("Unable to decode AVFrame.\n");
        return NULL;
    }
}

void start_capture_video(const char *filename, int fps)
{
    avcodec_register_all();
    av_register_all();
    
    AVCodec *codec;
    AVCodecContext *c= NULL;
    int i, ret, got_output;
    FILE *f;
    AVFrame *picture;
    AVPacket pkt;
//    uint8_t endcode[] = { 0, 0, 1, 0xb7 };
    
    printf("Video encoding\n");
    
    // Find the mpeg1 video encoder.
    codec = avcodec_find_encoder(AV_CODEC_ID_MPEG1VIDEO);
    if (!codec) {
        fprintf(stderr, "codec not found\n");
        exit(1);
    }
    
    c = avcodec_alloc_context3(codec);
    picture = av_frame_alloc();
    
    // Take first screenshot to determine video resolution.
    qmp_screendump("image228.ppm", NULL);
    picture = image_decode_example("image228.ppm");
    
    c->bit_rate = 400000;
    c->width = picture->width;
    c->height = picture->height;
    c->time_base= (AVRational){1,fps};
    c->gop_size = 600; // Emit one intra frame every 600 frames.
    c->max_b_frames=1;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    
    // Open codec.
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "could not open codec\n");
        exit(1);
    }
    
    f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Could not open %s\n", filename);
        exit(1);
    }
    
    // Start video capture.
    shouldCaptureVideo = true;
    
    currentVideoCaptureContext = g_new0(VideoCaptureContext, 1);
    currentVideoCaptureContext->codecContext = c;
    currentVideoCaptureContext->packet = pkt;
    currentVideoCaptureContext->outputFile = f;
    currentVideoCaptureContext->fps = fps;
    currentVideoCaptureContext->captureTimer = timer_new_ms(QEMU_CLOCK_REALTIME, video_capture_callback, currentVideoCaptureContext);
    // Arm timer.
    timer_mod(currentVideoCaptureContext->captureTimer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000 / fps);
}

void stop_capture_video()
{
    shouldCaptureVideo = false;
}

void video_capture_callback(void *opaque)
{
    VideoCaptureContext *videoCaptureContext = opaque;
    
    av_init_packet(&videoCaptureContext->packet);
    videoCaptureContext->packet.data = NULL;
    videoCaptureContext->packet.size = 0;
    
    fflush(stdout);
    
    qmp_screendump("image228.ppm", NULL);
    AVFrame *picture = image_decode_example("image228.ppm");
    
    int encodeResult = -1;
    int outputResult = 0;
    
    encodeResult = avcodec_encode_video2(videoCaptureContext->codecContext, &videoCaptureContext->packet, picture, &outputResult);
    if (encodeResult < 0) {
        fprintf(stderr, "Error when encoding frame\n");
        exit(1);
    }
    
    if (outputResult) {
        fwrite(videoCaptureContext->packet.data, 1, videoCaptureContext->packet.size, videoCaptureContext->outputFile);
        av_packet_unref(&videoCaptureContext->packet);
    }
    
    if (!shouldCaptureVideo) {
        timer_del(videoCaptureContext->captureTimer);
        timer_free(videoCaptureContext->captureTimer);
        
        int got_output = 1;
        
        // Get the delayed frames.
        while (got_output) {
            fflush(stdout);
            
            int ret = -1;
            
            ret = avcodec_encode_video2(videoCaptureContext->codecContext, &videoCaptureContext->packet, NULL, &got_output);
            if (ret < 0) {
                fprintf(stderr, "Error when encoding frame\n");
                exit(1);
            }
            
            if (got_output) {
                fwrite(videoCaptureContext->packet.data, 1, videoCaptureContext->packet.size, videoCaptureContext->outputFile);
                av_packet_unref(&videoCaptureContext->packet);
            }
        }
        
        // Add sequence end code to have a real mpeg file.
        uint8_t endcode[] = { 0, 0, 1, 0xb7 };
        
        fwrite(endcode, 1, sizeof(endcode), videoCaptureContext->outputFile);
        fclose(videoCaptureContext->outputFile);
        
        avcodec_close(videoCaptureContext->codecContext);
        av_free(videoCaptureContext->codecContext);
        
        currentVideoCaptureContext = NULL;
        
        printf("Video captured!\n");
    }
    else {
        // Rearm timer.
        timer_mod(currentVideoCaptureContext->captureTimer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1000 / videoCaptureContext->fps);
    }
}
