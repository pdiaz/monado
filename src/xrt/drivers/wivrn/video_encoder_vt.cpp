/*
 * WiVRn VR streaming
 * Copyright (C) 2022  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2022  Patrick Nicolas <patricknicolas@laposte.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CMSampleBuffer.h>
#include <CoreMedia/CMFormatDescription.h>
#include <CoreMedia/CMTime.h>
#include <VideoToolbox/VideoToolbox.h>
#include <VideoToolbox/VTSession.h>
#include <VideoToolbox/VTCompressionProperties.h>
#include <VideoToolbox/VTCompressionSession.h>
#include <VideoToolbox/VTDecompressionSession.h>
#include <VideoToolbox/VTErrors.h>

#include "h264bitstream/h264_stream.h"

typedef void* RROutput;
#define _RENDER_H_ // X11 HACK
#define _XRENDER_H_
#define _XRANDR_H_
#include "video_encoder_vt.h"

#include <stdexcept>

#define H264_NAL_UNSPECIFIED      (0)
#define H264_NAL_CODED_NON_IDR    (1)
#define H264_NAL_CODED_PART_A     (2)
#define H264_NAL_CODED_PART_B     (3)
#define H264_NAL_CODED_PART_C     (4)
#define H264_NAL_IDR              (5)
#define H264_NAL_SEI              (6)
#define H264_NAL_SPS              (7)
#define H264_NAL_PPS              (8)
#define H264_NAL_AUX              (9)
#define H264_NAL_END_SEQ          (10)
#define H264_NAL_END_STREAM       (11)
#define H264_NAL_FILLER           (12)
#define H264_NAL_SPS_EXT          (13)
#define H264_NAL_PREFIX           (14)
#define H264_NAL_SUBSET_SPS       (15)
#define H264_NAL_DEPTH            (16)
#define H264_NAL_CODED_AUX_NOPART (19)
#define H264_NAL_CODED_SLICE      (20)
#define H264_NAL_CODED_DEPTH      (21)

#define HEVC_NAL_TRAIL_N        (0)
#define HEVC_NAL_TRAIL_R        (1)
#define HEVC_NAL_TSA_N          (2)
#define HEVC_NAL_TSA_R          (3)
#define HEVC_NAL_STSA_N         (4)
#define HEVC_NAL_STSA_R         (5)
#define HEVC_NAL_RADL_N         (6)
#define HEVC_NAL_RADL_R         (7)
#define HEVC_NAL_RASL_N         (8)
#define HEVC_NAL_RASL_R         (9)
#define HEVC_NAL_BLA_W_LP       (16)
#define HEVC_NAL_BLA_W_RADL     (17)
#define HEVC_NAL_BLA_N_LP       (18)
#define HEVC_NAL_IDR_W_RADL     (19)
#define HEVC_NAL_IDR_N_LP       (20)
#define HEVC_NAL_CRA_NUT        (21)
#define HEVC_NAL_VPS            (32)
#define HEVC_NAL_SPS            (33)
#define HEVC_NAL_PPS            (34)
#define HEVC_NAL_AUD            (35)
#define HEVC_NAL_EOS_NUT        (36)
#define HEVC_NAL_EOB_NUT        (37)
#define HEVC_NAL_FD_NUT         (38)
#define HEVC_NAL_SEI_PREFIX     (39)
#define HEVC_NAL_SEI_SUFFIX     (40)

static void hex_dump(const uint8_t* b, size_t amt)
{
    for (size_t i = 0; i < amt; i++)
    {
        if (i && i % 16 == 0) {
            printf("\n");
        }
        printf("%02x ", b[i]);
    }
    printf("\n");
}

// Convenience function for creating a dictionary.
static CFDictionaryRef CreateCFTypeDictionary(CFTypeRef* keys,
                                              CFTypeRef* values,
                                              size_t size) {
  return CFDictionaryCreate(kCFAllocatorDefault, keys, values, size,
                            &kCFTypeDictionaryKeyCallBacks,
                            &kCFTypeDictionaryValueCallBacks);
}

void VideoEncoderVT::CopyNals(VideoEncoderVT* ctx,
				      char* avcc_buffer,
                      const size_t avcc_size,
                      size_t size_len,
                      int index) {
    size_t nal_size;

    size_t bytes_left = avcc_size;
    while (bytes_left > 0) {
        nal_size = 0;
        for (size_t i = 0; i < size_len; i++)
        {
            nal_size |= *(uint8_t*)avcc_buffer;

            bytes_left -= 1;
            avcc_buffer += 1;

            if (i != size_len-1) {
                nal_size <<= 8;
            }
        }

        int type = (avcc_buffer[0] & 0x7E) >> 1;
        //printf("Type: %u\n", type);
        if (type == HEVC_NAL_VPS || type == HEVC_NAL_SPS || type == HEVC_NAL_PPS) {
            bytes_left -= nal_size;
            avcc_buffer += nal_size;
            continue;
        }


        //hex_dump((uint8_t*)avcc_buffer, nal_size);
        
        std::vector<uint8_t> data(sizeof(kAnnexBHeaderBytes)+nal_size);

        memcpy(reinterpret_cast<char*>(data.data()), (void*)kAnnexBHeaderBytes, sizeof(kAnnexBHeaderBytes));
        memcpy(reinterpret_cast<char*>(data.data())+sizeof(kAnnexBHeaderBytes), (char*)avcc_buffer, nal_size);
        ctx->SendIDR(std::move(data), index);

        bytes_left -= nal_size;
        avcc_buffer += nal_size;
    }
}

void VideoEncoderVT::vtCallback(void *outputCallbackRefCon,
        void *sourceFrameRefCon,
        OSStatus status,
        VTEncodeInfoFlags infoFlags,
        CMSampleBufferRef sampleBuffer ) 
{
	bool keyframe;
	CFDictionaryRef sample_attachments;
	CMBlockBufferRef bb;
	CMFormatDescriptionRef fdesc;
	size_t bb_size, total_bytes, pset_count;
	int nal_size_field_bytes;

    VideoEncoderVT* ctx = (VideoEncoderVT*)outputCallbackRefCon;
    EncodeContext* encode_ctx = (EncodeContext*)sourceFrameRefCon;
    self_encode_params* encode_params = &ctx->encode_params;

    if (!encode_ctx)  {
    	printf("UHHHHHHHH???\n");
    	return; // shouldn't happen
    }

    // Frame skipped
    if (!sampleBuffer) {
    	goto cleanup;
    }

    sample_attachments = (CFDictionaryRef)CFArrayGetValueAtIndex(CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, true), 0);
    keyframe = !CFDictionaryContainsKey(sample_attachments, kCMSampleAttachmentKey_NotSync);

    // Get the sample buffer's block buffer and format description.
    bb = CMSampleBufferGetDataBuffer(sampleBuffer);
    fdesc = CMSampleBufferGetFormatDescription(sampleBuffer);
    bb_size = CMBlockBufferGetDataLength(bb);
    total_bytes = bb_size;
    pset_count;
    
    status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fdesc, 0, NULL, NULL, &pset_count, &nal_size_field_bytes);
    if (status == kCMFormatDescriptionBridgeError_InvalidParameter) 
    {
        pset_count = 2;
        nal_size_field_bytes = 4;
    } 
    else if (status != noErr) 
    {
        printf("CMVideoFormatDescriptionGetHEVCParameterSetAtIndex failed\n");
        goto cleanup;
    }
    
    // Get the total size of the parameter sets
    if (keyframe) {
        const uint8_t* pset;
        size_t pset_size;
        for (size_t pset_i = 0; pset_i < pset_count; ++pset_i) {
            status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fdesc, pset_i, &pset, &pset_size, NULL, NULL);
            if (status != noErr) {
              printf("CMVideoFormatDescriptionGetHEVCParameterSetAtIndex failed\n");
              goto cleanup;
            }
            total_bytes += pset_size + nal_size_field_bytes;
        }
    }

    // Copy all parameter sets separately
    if (keyframe) {
        const uint8_t* pset;
        size_t pset_size;

        for (size_t pset_i = 0; pset_i < pset_count; ++pset_i) {
            status = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fdesc, pset_i, &pset, &pset_size, NULL, NULL);
            if (status != noErr) {
                printf("CMVideoFormatDescriptionGetHEVCParameterSetAtIndex failed\n");
                goto cleanup;
            }

            int type = (pset[0] & 0x7E) >> 1;
            if (type != HEVC_NAL_VPS && type != HEVC_NAL_SPS && type != HEVC_NAL_PPS) continue;
            //printf("Type: %u\n", type);

            static const char startcode_4[4] = {0, 0, 0, 1};

            std::vector<uint8_t> data(sizeof(startcode_4)+pset_size);
            memcpy(reinterpret_cast<char*>(data.data()), startcode_4, sizeof(startcode_4));
            memcpy(reinterpret_cast<char*>(data.data())+sizeof(startcode_4), (char*)pset, pset_size);
            //hex_dump(data.data(), sizeof(startcode_4)+pset_size);
            ctx->SendCSD(std::move(data), encode_ctx->index);
        }
    }

    // Block buffers can be composed of non-contiguous chunks. For the sake of
    // keeping this code simple, flatten non-contiguous block buffers.
    CMBlockBufferRef contiguous_bb;
    if (!CMBlockBufferIsRangeContiguous(bb, 0, 0)) {
        //contiguous_bb.reset();
        status = CMBlockBufferCreateContiguous(
            kCFAllocatorDefault, (OpaqueCMBlockBuffer*)bb, kCFAllocatorDefault, NULL, 0, 0, 0,
            (OpaqueCMBlockBuffer**)&contiguous_bb);
        if (status != noErr) {
            printf("CMBlockBufferCreateContiguous failed\n");
            //DLOG(ERROR) << " CMBlockBufferCreateContiguous failed: " << status;
            goto cleanup;
        }
    }
    else {
        contiguous_bb = bb;
    }

    // Copy all the NAL units. In the process convert them from AVCC format
    // (length header) to AnnexB format (start code).
    int lengthAtOffset, totalLengthOut;
    char* bb_data;
    status = CMBlockBufferGetDataPointer(contiguous_bb, 0, NULL, NULL, &bb_data);
    if (status != noErr) {
        printf("CMBlockBufferGetDataPointer failed\n");
        goto cleanup;
    }

    CopyNals(ctx, bb_data, bb_size, nal_size_field_bytes, encode_ctx->index);

    ctx->FlushFrame(encode_ctx->display_ns, encode_ctx->index);
cleanup:
    os_mutex_unlock(&encode_ctx->wait_mutex);
}


namespace xrt::drivers::wivrn
{


VideoEncoderVT::VideoEncoderVT(
        vk_bundle * vk,
        encoder_settings & settings,
        int input_width,
        int input_height,
        int slice_idx,
        int num_slices,
        float fps) :
        vk(vk),
        fps(fps)
{
	if (settings.codec != h264)
	{
		U_LOG_W("requested vt encoder with codec != h264");
		settings.codec = h264;
	}

	//FILE* f = fopen("apple_h264.265", "wb");
    //fclose(f);

    settings.width += settings.width % 2;
	settings.height += settings.height % 2;

    encode_params.frameW = settings.width;
    encode_params.frameH = settings.height / num_slices;
    memset(encode_contexts, 0, sizeof(encode_contexts));

    this->slice_idx = slice_idx;
    this->num_slices = num_slices;
    frame_idx = 0;
    converter =
	        std::make_unique<YuvConverter>(vk, VkExtent3D{uint32_t(settings.width), uint32_t(settings.height / num_slices), 1}, settings.offset_x, settings.offset_y, input_width, input_height, slice_idx, num_slices);

    settings.range = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
	settings.color_model = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;

    const size_t attributesSize = 1;
    CFTypeRef keys[attributesSize] = {
        kCVPixelBufferPixelFormatTypeKey
    };
    CFDictionaryRef ioSurfaceValue = CreateCFTypeDictionary(NULL, NULL, 0);
    int64_t nv12type = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
    CFNumberRef pixelFormat = CFNumberCreate(NULL, kCFNumberLongType, &nv12type);
    CFTypeRef values[attributesSize] = {pixelFormat};

    CFDictionaryRef sourceAttributes = CreateCFTypeDictionary(keys, values, attributesSize);

    CFMutableDictionaryRef encoder_specs = CFDictionaryCreateMutable(NULL, 6, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    
    // we need the encoder to pick up the pace, so we lie and say we're at 4x framerate (but only ever feed the real fps)
    int32_t framerate = (int)fps;
    int32_t maxkeyframe = (int)fps * 5;
    int32_t bitrate = (int32_t)settings.bitrate;
    int32_t frames = 1;
    int32_t numslices = 1;
    CFNumberRef cfFPS = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &framerate);
    CFNumberRef cfMaxKeyframe = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &maxkeyframe);
    CFNumberRef cfBitrate = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &bitrate);
    CFNumberRef cfFrames = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &frames);
    CFNumberRef cfNumSlices = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &numslices);

    //CFNumberRef cfBaseFps = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &v);

    CFDictionarySetValue(encoder_specs, kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder, kCFBooleanTrue);


    // Create compression session
    int err = VTCompressionSessionCreate(kCFAllocatorDefault,
                                 encode_params.frameW,
                                 encode_params.frameH,
                                 kCMVideoCodecType_HEVC,
                                 encoder_specs, // use default encoder
                                 sourceAttributes,
                                 NULL, // use default compressed data allocator
                                 (VTCompressionOutputCallback)vtCallback,
                                 this,
                                 &compression_session);


    if(err == noErr) {
        //comp_session = session;
        //printf("Made session!\n");
    }

    err = VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_ExpectedFrameRate, cfFPS);
    if(err != noErr) {
        printf("ExpectedFrameRate fail?\n");
    }
    //VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_Quality, cfQuality);
    //VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_SourceFrameCount, cfFrames);
    err = VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_AverageBitRate, cfBitrate);
    if(err != noErr) {
        printf("kVTCompressionPropertyKey_AverageBitRate fail?\n");
    }
    err = VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_MaxFrameDelayCount, cfFrames);
    if(err != noErr) {
        printf("kVTCompressionPropertyKey_MaxFrameDelayCount fail?\n");
    }

    err = VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
    if(err != noErr) {
        printf("kVTCompressionPropertyKey_RealTime fail?\n");
    }
    err = VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    if(err != noErr) {
        printf("kVTCompressionPropertyKey_AllowFrameReordering fail?\n");
    }
    err = VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_AllowTemporalCompression, kCFBooleanTrue);
    if(err != noErr) {
        printf("kVTCompressionPropertyKey_AllowTemporalCompression fail?\n");
    }
    err = VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_AllowOpenGOP, kCFBooleanFalse);
    if(err != noErr) {
        printf("kVTCompressionPropertyKey_AllowOpenGOP fail?\n");
    }
    err = VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_MaxKeyFrameInterval, cfMaxKeyframe);
	if(err != noErr) {
        printf("kVTCompressionPropertyKey_MaxKeyFrameInterval fail?\n");
    }
	err = VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);
	if(err != noErr) {
        printf("kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality fail?\n");
    }

    err = VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
    if(err != noErr) {
        printf("kVTCompressionPropertyKey_ColorPrimaries fail?\n");
    }
    err = VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
    if(err != noErr) {
        printf("kVTCompressionPropertyKey_TransferFunction fail?\n");
    }
    err = VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    if(err != noErr) {
        printf("kVTCompressionPropertyKey_YCbCrMatrix fail?\n");
    }

    err = VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_HEVC_Main_AutoLevel);
    if(err != noErr) {
        printf("kVTCompressionPropertyKey_ProfileLevel fail?\n");
    }

    // Secret keys
    err = VTSessionSetProperty(compression_session, CFSTR("LowLatencyMode"), CFSTR("Minimum"));
    if(err != noErr) {
        printf("LowLatencyMode fail?\n");
    }
    err = VTSessionSetProperty(compression_session, CFSTR("NumberOfSlices"), cfNumSlices); 
    if(err != noErr) {
        printf("NumberOfSlices fail?\n");
    }

    VTCompressionSessionPrepareToEncodeFrames(compression_session);

    //VTSessionSetProperty(compression_session, kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_601_4);

    if(err == noErr) {
        //comp_session = session;
        //printf("Made session!\n");
    }

    void* planes[2] = {(void *)converter->y.mapped_memory, (void *)converter->uv.mapped_memory};
    size_t planes_w[2] = {size_t(settings.width), size_t(settings.width)};
    size_t planes_h[2] = {size_t(settings.height / num_slices), size_t(settings.height / num_slices)};
    size_t planes_stride[2] = {size_t(converter->y.stride), size_t(converter->uv.stride)};

    //CVPixelBufferCreate(kCFAllocatorDefault, encode_params.frameW, encode_params.frameH, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, NULL, &pixelBuffer);
    CVPixelBufferCreateWithPlanarBytes(kCFAllocatorDefault, encode_params.frameW, encode_params.frameH, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
    								   NULL, NULL, 2, planes, planes_w, planes_h, planes_stride, NULL, NULL, NULL, &pixelBuffer);
    

    {
    	CFTypeRef frameProperties_keys[1] = {
	        kVTEncodeFrameOptionKey_ForceKeyFrame
	    };
	    CFTypeRef frameProperties_values[1] = {kCFBooleanTrue};
	    CFDictionaryRef frameProperties = CreateCFTypeDictionary(frameProperties_keys, frameProperties_values, 1);

    	doIdrDict = (void*)frameProperties;
    }

    {
    	CFTypeRef frameProperties_keys[1] = {
	        kVTEncodeFrameOptionKey_ForceKeyFrame
	    };
	    CFTypeRef frameProperties_values[1] = {kCFBooleanFalse};
	    CFDictionaryRef frameProperties = CreateCFTypeDictionary(frameProperties_keys, frameProperties_values, 1);

    	doNoIdrDict = (void*)frameProperties;
    }

    CFRelease(cfFPS);
    CFRelease(cfMaxKeyframe);
    CFRelease(cfBitrate);
    CFRelease(cfFrames);

    for (int i = 0; i < 3; i++)
    {
    	int ret = os_mutex_init(&encode_contexts[i].wait_mutex);
	    if (ret != 0) {
	        printf("Failed to init wait mutex\n");
	    }
    }
}

void VideoEncoderVT::SetImages(int width, int height, VkFormat format, int num_images, VkImage * images, VkImageView * views, VkDeviceMemory * memory)
{
	converter->SetImages(num_images, images, views);
}

void VideoEncoderVT::PresentImage(int index, VkCommandBuffer * out_buffer)
{
	EncodeContext* ctx = &encode_contexts[index];
	os_mutex_lock(&ctx->wait_mutex);
	*out_buffer = converter->command_buffers[index];
	os_mutex_unlock(&ctx->wait_mutex);
}

void VideoEncoderVT::Encode(int index, bool idr, std::chrono::steady_clock::time_point pts)
{
	int64_t ns_display = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::time_point_cast<std::chrono::nanoseconds>(pts).time_since_epoch()).count();

	if (index >= 3 || index < 0) {
		printf("Weird index! %x\n", index);
		index = 0;
	}


	EncodeContext* ctx = &encode_contexts[index];
	os_mutex_lock(&ctx->wait_mutex);

	ctx->display_ns = ns_display;
	ctx->index = index;
	ctx->ctx = this;

	//idr = (frame_idx % (500) == 0);

	//printf("Frame number %u, idr=%x\n", frame_idx, idr);

	//CMTimeMake(1000*(index*4), (int)(fps * 4 * 1000));//
	CMTime pts_ = CMTimeMake(1000*(frame_idx), (int)(fps * 1000));//CMTimeMakeWithSeconds(std::chrono::duration<double>(pts.time_since_epoch()).count(), (int)(fps* 4 * 1000)); // (1.0/fps) * index
	//CMTime pts_ = CMTimeMake((double)(ns_display / 4000), (int)(fps* 4 * 1000)); // (1.0/fps) * index
    CMTime duration = CMTimeMake(1000, (int)(fps * 1000));//CMTimeMakeWithSeconds(1.0/fps, 1);

    VTCompressionSessionEncodeFrame(compression_session, pixelBuffer, pts_, duration, (CFDictionaryRef)(idr ? doIdrDict : doNoIdrDict), ctx, NULL);
    frame_idx++;
    
    // This causes stuttering if done for every frame
    if (slice_idx == num_slices-1) {
    	VTCompressionSessionCompleteFrames(compression_session, pts_);
    }
}

void VideoEncoderVT::ModifyBitrate(int amount)
{
	
}

VideoEncoderVT::~VideoEncoderVT()
{
	VTCompressionSessionInvalidate(compression_session);
	CFRelease(doIdrDict);
	CFRelease(doNoIdrDict);
}

} // namespace xrt::drivers::wivrn
