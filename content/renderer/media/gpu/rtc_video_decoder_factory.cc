// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/media/gpu/rtc_video_decoder_factory.h"

#include <memory>

#include "base/memory/ptr_util.h"
#include "content/renderer/media/gpu/rtc_video_decoder.h"
#include "media/video/gpu_video_accelerator_factories.h"
#include "third_party/webrtc/api/video_codecs/sdp_video_format.h"

namespace content {

namespace {

// This extra indirection is needed so that we can delete the decoder on the
// correct thread.
class ScopedVideoDecoder : public webrtc::VideoDecoder {
 public:
  ScopedVideoDecoder(
      const scoped_refptr<base::SingleThreadTaskRunner>& task_runner,
      std::unique_ptr<webrtc::VideoDecoder> decoder)
      : task_runner_(task_runner), decoder_(std::move(decoder)) {}

  int32_t InitDecode(const webrtc::VideoCodec* codec_settings,
                     int32_t number_of_cores) override {
    return decoder_->InitDecode(codec_settings, number_of_cores);
  }
  int32_t RegisterDecodeCompleteCallback(
      webrtc::DecodedImageCallback* callback) override {
    return decoder_->RegisterDecodeCompleteCallback(callback);
  }
  int32_t Release() override { return decoder_->Release(); }
  int32_t Decode(const webrtc::EncodedImage& input_image,
                 bool missing_frames,
                 const webrtc::RTPFragmentationHeader* fragmentation,
                 const webrtc::CodecSpecificInfo* codec_specific_info,
                 int64_t render_time_ms) override {
    return decoder_->Decode(input_image, missing_frames, fragmentation,
                            codec_specific_info, render_time_ms);
  }
  bool PrefersLateDecoding() const override {
    return decoder_->PrefersLateDecoding();
  }
  const char* ImplementationName() const override {
    return decoder_->ImplementationName();
  }

  // Runs on Chrome_libJingle_WorkerThread. The child thread is blocked while
  // this runs.
  ~ScopedVideoDecoder() override {
    task_runner_->DeleteSoon(FROM_HERE, decoder_.release());
  }

 private:
  scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
  std::unique_ptr<webrtc::VideoDecoder> decoder_;
};

}  // namespace

RTCVideoDecoderFactory::RTCVideoDecoderFactory(
    media::GpuVideoAcceleratorFactories* gpu_factories)
    : gpu_factories_(gpu_factories) {
  DVLOG(2) << __func__;
}

std::vector<webrtc::SdpVideoFormat>
RTCVideoDecoderFactory::GetSupportedFormats() const {
  // TODO(magjed): Implement this based on
  // gpu_factories_->GetVideoDecodeAcceleratorSupportedProfiles(). This function
  // is currently unused by WebRTC.
  return std::vector<webrtc::SdpVideoFormat>();
}

RTCVideoDecoderFactory::~RTCVideoDecoderFactory() {
  DVLOG(2) << __func__;
}

std::unique_ptr<webrtc::VideoDecoder>
RTCVideoDecoderFactory::CreateVideoDecoder(
    const webrtc::SdpVideoFormat& format) {
  DVLOG(2) << __func__;
  std::unique_ptr<webrtc::VideoDecoder> decoder = RTCVideoDecoder::Create(
      webrtc::PayloadStringToCodecType(format.name), gpu_factories_);
  // Make sure the decoder is destructed on the correct thread.
  return decoder ? base::MakeUnique<ScopedVideoDecoder>(
                       gpu_factories_->GetTaskRunner(), std::move(decoder))
                 : nullptr;
}

}  // namespace content
