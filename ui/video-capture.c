//
//  video-capture.c
//  
//
//  Created by Ivan Sergunin on 21/06/16.
//
//

#include "video-capture.h"

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

/*
 * Image decoding example
 */

AVFrame* image_decode_example(const char *filename)
{
//    printf("Decoding image.\n");
    
//    printf("Try to open file.\n");
    // Open input file.
    AVFormatContext *iFormatContext = avformat_alloc_context();
    int err = avformat_open_input(&iFormatContext, filename, NULL, NULL);
    if (err != 0) {
        printf("Error in opening input file: (%s), error code: (%d)\n", filename, err);
        return NULL;
    }
//    printf("File opened.\n");
    
//    printf("Try to find stream information.\n");
    // Finding stream information.
    if (avformat_find_stream_info(iFormatContext, NULL)<0) {
        printf("Unable to find stream info.\n");
        avformat_close_input(&iFormatContext); // Release AVFormatContext memory.
        return NULL;
    }
//    printf("Stream information finded.\n");
    
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
//    printf("Video stream of image finded.\n");
    
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
//    printf("Decoder finded.\n");
    
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
//    printf("Frame successful read.\n");
    
    // Decoding the encoded video frame.
    AVFrame *decodedFrame = av_frame_alloc();
    avcodec_send_packet(pCodecCtx, &encodedPacket);
    int frameFinished = avcodec_receive_frame(pCodecCtx, decodedFrame);
    
    if (frameFinished == 0) {
//        printf("AVFrame decoded. Frame information - width: (%d) Height: (%d).\n", decodedFrame->width, decodedFrame->height);
        
        // Convert decoded Frame to AV_PIX_FMT_YUV420P
//        printf("Prepare to scale.\n");
        
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
        
        //        av_image_fill_arrays(convertedFrame->data, convertedFrame->linesize, convertedFrame_buffer, AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 32);
        av_image_alloc(convertedFrame->data, convertedFrame->linesize, pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, 32);
        
//        printf("Scaling.\n");
        sws_scale(img_convert_ctx, decodedFrame->data, decodedFrame->linesize, 0, pCodecCtx->height, convertedFrame->data, convertedFrame->linesize);
        
//        printf("AVFrame converted. Frame information - width: (%d) Height: (%d).\n", convertedFrame->width, convertedFrame->height);
        
        sws_freeContext(img_convert_ctx);
        
        return convertedFrame;
    }
    else {
//        printf("Unable to decode AVFrame.\n");
        return NULL;
    }
}

/*
 * Video encoding example
 */
void video_encode_example(const char *filename, int fps)
{
    avcodec_register_all();
    av_register_all();
    
    AVCodec *codec;
    AVCodecContext *c= NULL;
    int i, ret, got_output;
    FILE *f;
    AVFrame *picture;
    AVPacket pkt;
    uint8_t endcode[] = { 0, 0, 1, 0xb7 };
    
    printf("Video encoding\n");
    
    /* find the mpeg1 video encoder */
    codec = avcodec_find_encoder(AV_CODEC_ID_MPEG1VIDEO);
    if (!codec) {
        fprintf(stderr, "codec not found\n");
        exit(1);
    }
    
    c = avcodec_alloc_context3(codec);
    picture = av_frame_alloc();
    
    // Take first screenshot to determine demension.
    qmp_screendump("image228.ppm", NULL);
    picture = image_decode_example("image228.ppm");
    
    /* put sample parameters */
    c->bit_rate = 400000;
    /* resolution must be a multiple of two */
    c->width = picture->width;
    c->height = picture->height;
    /* frames per second */
    c->time_base= (AVRational){1,fps};
    c->gop_size = 600; /* emit one intra frame every ten frames */
    c->max_b_frames=1;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    
    /* open it */
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "could not open codec\n");
        exit(1);
    }
    
    f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "could not open %s\n", filename);
        exit(1);
    }
    
    /* encode 3 seconds of video */
    for(i = 0; i < 75; i++) {
        av_init_packet(&pkt);
        pkt.data = NULL;    // packet data will be allocated by the encoder
        pkt.size = 0;
        
        fflush(stdout);
        
        qmp_screendump("image228.ppm", NULL);
//        printf("Getting decoded image!\n");
        picture = image_decode_example("image228.ppm");
//        printf("Got decoded image: (%p)\n", picture);
        
        
//        printf("Encoding frame!\n");
        /* encode the image */
        ret = avcodec_encode_video2(c, &pkt, picture, &got_output);
        if (ret < 0) {
            fprintf(stderr, "error encoding frame\n");
            exit(1);
        }
//        printf("Success!\n");
        
        if (got_output) {
//            printf("encoding frame %3d (size=%5d)\n", i, pkt.size);
            fwrite(pkt.data, 1, pkt.size, f);
            av_packet_unref(&pkt);
        }
        usleep(1000000/30);
    }
    
    /* get the delayed frames */
    for (got_output = 1; got_output; i++) {
        fflush(stdout);
        
        ret = avcodec_encode_video2(c, &pkt, NULL, &got_output);
        if (ret < 0) {
            fprintf(stderr, "error encoding frame\n");
            exit(1);
        }
//        printf("Success!\n");
        
        if (got_output) {
//            printf("encoding frame %3d (size=%5d)\n", i, pkt.size);
            fwrite(pkt.data, 1, pkt.size, f);
            av_packet_unref(&pkt);
        }
    }
    
    /* add sequence end code to have a real mpeg file */
    fwrite(endcode, 1, sizeof(endcode), f);
    fclose(f);
    
    avcodec_close(c);
    av_free(c);
    av_freep(&picture->data[0]);
    av_frame_free(&picture);
    printf("Video captured!\n");
}
