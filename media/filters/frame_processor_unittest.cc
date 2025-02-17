// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "base/bind.h"
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "media/base/media_log.h"
#include "media/base/media_util.h"
#include "media/base/mock_filters.h"
#include "media/base/mock_media_log.h"
#include "media/base/test_helpers.h"
#include "media/base/timestamp_constants.h"
#include "media/filters/chunk_demuxer.h"
#include "media/filters/frame_processor.h"
#include "testing/gtest/include/gtest/gtest.h"

using ::testing::InSequence;
using ::testing::StrictMock;
using ::testing::Values;

namespace {

struct FrameProcessorTestParams {
 public:
  FrameProcessorTestParams(const bool use_sequence_mode,
                           const media::ChunkDemuxerStream::RangeApi range_api)
      : use_sequence_mode(use_sequence_mode), range_api(range_api) {}

  // Test will use 'sequence' append mode if true, or 'segments' if false.
  const bool use_sequence_mode;

  // Determines if media::kMseBufferByPts feature should be forced on or off for
  // the test, and is also used in tests' ChunkDemuxerStream constructions.
  const media::ChunkDemuxerStream::RangeApi range_api;
};

// Helper to shorten "base::TimeDelta::FromMilliseconds(...)" in these test
// cases for integer milliseconds.
constexpr base::TimeDelta Milliseconds(int64_t milliseconds) {
  return base::TimeDelta::FromMilliseconds(milliseconds);
}

}  // namespace

namespace media {

typedef StreamParser::BufferQueue BufferQueue;
typedef StreamParser::TrackId TrackId;

// Used for setting expectations on callbacks. Using a StrictMock also lets us
// test for missing or extra callbacks.
class FrameProcessorTestCallbackHelper {
 public:
  FrameProcessorTestCallbackHelper() {}
  virtual ~FrameProcessorTestCallbackHelper() {}

  MOCK_METHOD1(OnParseWarning, void(const SourceBufferParseWarning));
  MOCK_METHOD1(PossibleDurationIncrease, void(base::TimeDelta new_duration));

  // Helper that calls the mock method as well as does basic sanity checks on
  // |new_duration|.
  void OnPossibleDurationIncrease(base::TimeDelta new_duration) {
    PossibleDurationIncrease(new_duration);
    ASSERT_NE(kNoTimestamp, new_duration);
    ASSERT_NE(kInfiniteDuration, new_duration);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(FrameProcessorTestCallbackHelper);
};

class FrameProcessorTest
    : public ::testing::TestWithParam<FrameProcessorTestParams> {
 protected:
  FrameProcessorTest()
      : append_window_end_(kInfiniteDuration),
        frame_duration_(Milliseconds(10)),
        audio_id_(1),
        video_id_(2) {
    const FrameProcessorTestParams& params = GetParam();
    use_sequence_mode_ = params.use_sequence_mode;
    range_api_ = params.range_api;

    frame_processor_ = base::MakeUnique<FrameProcessor>(
        base::Bind(
            &FrameProcessorTestCallbackHelper::OnPossibleDurationIncrease,
            base::Unretained(&callbacks_)),
        &media_log_, range_api_);
    frame_processor_->SetParseWarningCallback(
        base::Bind(&FrameProcessorTestCallbackHelper::OnParseWarning,
                   base::Unretained(&callbacks_)));
  }

  enum StreamFlags {
    HAS_AUDIO = 1 << 0,
    HAS_VIDEO = 1 << 1
  };

  void AddTestTracks(int stream_flags) {
    const bool has_audio = (stream_flags & HAS_AUDIO) != 0;
    const bool has_video = (stream_flags & HAS_VIDEO) != 0;
    ASSERT_TRUE(has_audio || has_video);

    if (has_audio) {
      CreateAndConfigureStream(DemuxerStream::AUDIO);
      ASSERT_TRUE(audio_);
      EXPECT_TRUE(frame_processor_->AddTrack(audio_id_, audio_.get()));
      SeekStream(audio_.get(), Milliseconds(0));
    }
    if (has_video) {
      CreateAndConfigureStream(DemuxerStream::VIDEO);
      ASSERT_TRUE(video_);
      EXPECT_TRUE(frame_processor_->AddTrack(video_id_, video_.get()));
      SeekStream(video_.get(), Milliseconds(0));
    }
  }

  void SetTimestampOffset(base::TimeDelta new_offset) {
    timestamp_offset_ = new_offset;
    frame_processor_->SetGroupStartTimestampIfInSequenceMode(timestamp_offset_);
  }

  BufferQueue StringToBufferQueue(const std::string& buffers_to_append,
                                  const TrackId track_id,
                                  const DemuxerStream::Type type) {
    std::vector<std::string> timestamps = base::SplitString(
        buffers_to_append, " ", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);

    BufferQueue buffers;
    for (size_t i = 0; i < timestamps.size(); i++) {
      bool is_keyframe = false;
      if (base::EndsWith(timestamps[i], "K", base::CompareCase::SENSITIVE)) {
        is_keyframe = true;
        // Remove the "K" off of the token.
        timestamps[i] = timestamps[i].substr(0, timestamps[i].length() - 1);
      }

      // Use custom decode timestamp if included.
      std::vector<std::string> buffer_timestamps = base::SplitString(
          timestamps[i], "|", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
      if (buffer_timestamps.size() == 1)
        buffer_timestamps.push_back(buffer_timestamps[0]);
      CHECK_EQ(2u, buffer_timestamps.size());

      double time_in_ms, decode_time_in_ms;
      CHECK(base::StringToDouble(buffer_timestamps[0], &time_in_ms));
      CHECK(base::StringToDouble(buffer_timestamps[1], &decode_time_in_ms));

      // Create buffer. Encode the original time_in_ms as the buffer's data to
      // enable later verification of possible buffer relocation in presentation
      // timeline due to coded frame processing.
      const uint8_t* timestamp_as_data =
          reinterpret_cast<uint8_t*>(&time_in_ms);
      scoped_refptr<StreamParserBuffer> buffer =
          StreamParserBuffer::CopyFrom(timestamp_as_data, sizeof(time_in_ms),
                                       is_keyframe, type, track_id);
      buffer->set_timestamp(base::TimeDelta::FromMillisecondsD(time_in_ms));
      if (time_in_ms != decode_time_in_ms) {
        buffer->SetDecodeTimestamp(DecodeTimestamp::FromPresentationTime(
            base::TimeDelta::FromMillisecondsD(decode_time_in_ms)));
      }

      buffer->set_duration(frame_duration_);
      buffers.push_back(buffer);
    }
    return buffers;
  }

  bool ProcessFrames(const std::string& audio_timestamps,
                     const std::string& video_timestamps) {
    StreamParser::BufferQueueMap buffer_queue_map;
    const auto& audio_buffers =
        StringToBufferQueue(audio_timestamps, audio_id_, DemuxerStream::AUDIO);
    if (!audio_buffers.empty())
      buffer_queue_map.insert(std::make_pair(audio_id_, audio_buffers));
    const auto& video_buffers =
        StringToBufferQueue(video_timestamps, video_id_, DemuxerStream::VIDEO);
    if (!video_buffers.empty())
      buffer_queue_map.insert(std::make_pair(video_id_, video_buffers));
    return frame_processor_->ProcessFrames(
        buffer_queue_map, append_window_start_, append_window_end_,
        &timestamp_offset_);
  }

  // Compares |expected| to the buffered ranges of |stream| formatted into a
  // string as follows:
  //
  // If no ranges: "{ }"
  // If one range: "{ [start1,end1) }"
  // If multiple ranges, they are added space-delimited in sequence, like:
  // "{ [start1,end1) [start2,end2) }"
  //
  // startN and endN are the respective buffered start and end times of the
  // range in integer milliseconds.
  void CheckExpectedRangesByTimestamp(ChunkDemuxerStream* stream,
                                      const std::string& expected) {
    // Note, DemuxerStream::TEXT streams return [0,duration (==infinity here))
    Ranges<base::TimeDelta> r = stream->GetBufferedRanges(kInfiniteDuration);

    std::stringstream ss;
    ss << "{ ";
    for (size_t i = 0; i < r.size(); ++i) {
      int64_t start = r.start(i).InMilliseconds();
      int64_t end = r.end(i).InMilliseconds();
      ss << "[" << start << "," << end << ") ";
    }
    ss << "}";
    EXPECT_EQ(expected, ss.str());
  }

  void CheckReadStalls(ChunkDemuxerStream* stream) {
    int loop_count = 0;

    do {
      read_callback_called_ = false;
      stream->Read(base::Bind(&FrameProcessorTest::StoreStatusAndBuffer,
                              base::Unretained(this)));
      base::RunLoop().RunUntilIdle();
    } while (++loop_count < 2 && read_callback_called_ &&
             last_read_status_ == DemuxerStream::kAborted);

    ASSERT_FALSE(read_callback_called_ &&
                 last_read_status_ == DemuxerStream::kAborted)
        << "2 kAborted reads in a row. Giving up.";
    EXPECT_FALSE(read_callback_called_);
  }

  // Format of |expected| is a space-delimited sequence of
  // timestamp_in_ms:original_timestamp_in_ms
  // original_timestamp_in_ms (and the colon) must be omitted if it is the same
  // as timestamp_in_ms.
  void CheckReadsThenReadStalls(ChunkDemuxerStream* stream,
                                const std::string& expected) {
    std::vector<std::string> timestamps = base::SplitString(
        expected, " ", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
    std::stringstream ss;
    for (size_t i = 0; i < timestamps.size(); ++i) {
      int loop_count = 0;

      do {
        read_callback_called_ = false;
        stream->Read(base::Bind(&FrameProcessorTest::StoreStatusAndBuffer,
                                base::Unretained(this)));
        base::RunLoop().RunUntilIdle();
        EXPECT_TRUE(read_callback_called_);
      } while (++loop_count < 2 &&
               last_read_status_ == DemuxerStream::kAborted);

      ASSERT_FALSE(last_read_status_ == DemuxerStream::kAborted)
          << "2 kAborted reads in a row. Giving up.";
      EXPECT_EQ(DemuxerStream::kOk, last_read_status_);
      EXPECT_FALSE(last_read_buffer_->end_of_stream());

      if (i > 0)
        ss << " ";

      int time_in_ms = last_read_buffer_->timestamp().InMilliseconds();
      ss << time_in_ms;

      // Decode the original_time_in_ms from the buffer's data.
      double original_time_in_ms;
      ASSERT_EQ(sizeof(original_time_in_ms), last_read_buffer_->data_size());
      original_time_in_ms = *(reinterpret_cast<const double*>(
          last_read_buffer_->data()));
      if (original_time_in_ms != time_in_ms)
        ss << ":" << original_time_in_ms;

      // Detect full-discard preroll buffer.
      if (last_read_buffer_->discard_padding().first == kInfiniteDuration &&
          last_read_buffer_->discard_padding().second.is_zero()) {
        ss << "P";
      }
    }

    EXPECT_EQ(expected, ss.str());
    CheckReadStalls(stream);
  }

  // TODO(wolenetz): Refactor to instead verify the expected signalling or lack
  // thereof of new coded frame group by the FrameProcessor. See
  // https://crbug.com/580613.
  bool in_coded_frame_group() {
    return !frame_processor_->pending_notify_all_group_start_;
  }

  void SeekStream(ChunkDemuxerStream* stream, base::TimeDelta seek_time) {
    stream->AbortReads();
    stream->Seek(seek_time);
    stream->StartReturningData();
  }

  base::MessageLoop message_loop_;
  StrictMock<MockMediaLog> media_log_;
  StrictMock<FrameProcessorTestCallbackHelper> callbacks_;

  bool use_sequence_mode_;
  ChunkDemuxerStream::RangeApi range_api_;

  std::unique_ptr<FrameProcessor> frame_processor_;
  base::TimeDelta append_window_start_;
  base::TimeDelta append_window_end_;
  base::TimeDelta timestamp_offset_;
  base::TimeDelta frame_duration_;
  std::unique_ptr<ChunkDemuxerStream> audio_;
  std::unique_ptr<ChunkDemuxerStream> video_;
  const TrackId audio_id_;
  const TrackId video_id_;
  const BufferQueue empty_queue_;

  // StoreStatusAndBuffer's most recent result.
  DemuxerStream::Status last_read_status_;
  scoped_refptr<DecoderBuffer> last_read_buffer_;
  bool read_callback_called_;

 private:
  void StoreStatusAndBuffer(DemuxerStream::Status status,
                            const scoped_refptr<DecoderBuffer>& buffer) {
    if (status == DemuxerStream::kOk && buffer.get()) {
      DVLOG(3) << __func__ << "status: " << status
               << " ts: " << buffer->timestamp().InSecondsF();
    } else {
      DVLOG(3) << __func__ << "status: " << status << " ts: n/a";
    }

    read_callback_called_ = true;
    last_read_status_ = status;
    last_read_buffer_ = buffer;
  }

  void CreateAndConfigureStream(DemuxerStream::Type type) {
    // TODO(wolenetz/dalecurtis): Also test with splicing disabled?
    switch (type) {
      case DemuxerStream::AUDIO: {
        ASSERT_FALSE(audio_);
        audio_.reset(
            new ChunkDemuxerStream(DemuxerStream::AUDIO, "1", range_api_));
        AudioDecoderConfig decoder_config(kCodecVorbis, kSampleFormatPlanarF32,
                                          CHANNEL_LAYOUT_STEREO, 1000,
                                          EmptyExtraData(), Unencrypted());
        frame_processor_->OnPossibleAudioConfigUpdate(decoder_config);
        ASSERT_TRUE(audio_->UpdateAudioConfig(decoder_config, &media_log_));
        break;
      }
      case DemuxerStream::VIDEO: {
        ASSERT_FALSE(video_);
        video_.reset(
            new ChunkDemuxerStream(DemuxerStream::VIDEO, "2", range_api_));
        ASSERT_TRUE(
            video_->UpdateVideoConfig(TestVideoConfig::Normal(), &media_log_));
        break;
      }
      // TODO(wolenetz): Test text coded frame processing.
      case DemuxerStream::TEXT:
      case DemuxerStream::UNKNOWN: {
        ASSERT_FALSE(true);
      }
    }
  }

  DISALLOW_COPY_AND_ASSIGN(FrameProcessorTest);
};

TEST_P(FrameProcessorTest, WrongTypeInAppendedBuffer) {
  AddTestTracks(HAS_AUDIO);
  EXPECT_FALSE(in_coded_frame_group());

  StreamParser::BufferQueueMap buffer_queue_map;
  const auto& audio_buffers =
      StringToBufferQueue("0K", audio_id_, DemuxerStream::VIDEO);
  buffer_queue_map.insert(std::make_pair(audio_id_, audio_buffers));
  EXPECT_MEDIA_LOG(FrameTypeMismatchesTrackType("video", "1"));
  ASSERT_FALSE(
      frame_processor_->ProcessFrames(buffer_queue_map, append_window_start_,
                                      append_window_end_, &timestamp_offset_));
  EXPECT_FALSE(in_coded_frame_group());
  EXPECT_EQ(Milliseconds(0), timestamp_offset_);
  CheckExpectedRangesByTimestamp(audio_.get(), "{ }");
  CheckReadStalls(audio_.get());
}

TEST_P(FrameProcessorTest, NonMonotonicallyIncreasingTimestampInOneCall) {
  AddTestTracks(HAS_AUDIO);

  EXPECT_MEDIA_LOG(ParsedBuffersNotInDTSSequence());
  EXPECT_FALSE(ProcessFrames("10K 0K", ""));
  EXPECT_FALSE(in_coded_frame_group());
  EXPECT_EQ(Milliseconds(0), timestamp_offset_);
  CheckExpectedRangesByTimestamp(audio_.get(), "{ }");
  CheckReadStalls(audio_.get());
}

TEST_P(FrameProcessorTest, AudioOnly_SingleFrame) {
  // Tests A: P(A) -> (a)
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  if (use_sequence_mode_)
    frame_processor_->SetSequenceMode(true);

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(10)));
  EXPECT_TRUE(ProcessFrames("0K", ""));
  EXPECT_TRUE(in_coded_frame_group());
  EXPECT_EQ(Milliseconds(0), timestamp_offset_);
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,10) }");
  CheckReadsThenReadStalls(audio_.get(), "0");
}

TEST_P(FrameProcessorTest, VideoOnly_SingleFrame) {
  // Tests V: P(V) -> (v)
  InSequence s;
  AddTestTracks(HAS_VIDEO);
  if (use_sequence_mode_)
    frame_processor_->SetSequenceMode(true);

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(10)));
  EXPECT_TRUE(ProcessFrames("", "0K"));
  EXPECT_TRUE(in_coded_frame_group());
  EXPECT_EQ(Milliseconds(0), timestamp_offset_);
  CheckExpectedRangesByTimestamp(video_.get(), "{ [0,10) }");
  CheckReadsThenReadStalls(video_.get(), "0");
}

TEST_P(FrameProcessorTest, AudioOnly_TwoFrames) {
  // Tests A: P(A0, A10) -> (a0, a10)
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  if (use_sequence_mode_)
    frame_processor_->SetSequenceMode(true);

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(20)));
  EXPECT_TRUE(ProcessFrames("0K 10K", ""));
  EXPECT_TRUE(in_coded_frame_group());
  EXPECT_EQ(Milliseconds(0), timestamp_offset_);
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,20) }");
  CheckReadsThenReadStalls(audio_.get(), "0 10");
}

TEST_P(FrameProcessorTest, AudioOnly_SetOffsetThenSingleFrame) {
  // Tests A: STSO(50)+P(A0) -> TSO==50,(a0@50)
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  if (use_sequence_mode_)
    frame_processor_->SetSequenceMode(true);

  SetTimestampOffset(Milliseconds(50));
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(60)));
  EXPECT_TRUE(ProcessFrames("0K", ""));
  EXPECT_TRUE(in_coded_frame_group());
  EXPECT_EQ(Milliseconds(50), timestamp_offset_);
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [50,60) }");

  // We do not stall on reading without seeking to 50ms due to
  // SourceBufferStream::kSeekToStartFudgeRoom().
  CheckReadsThenReadStalls(audio_.get(), "50:0");
}

TEST_P(FrameProcessorTest, AudioOnly_SetOffsetThenFrameTimestampBelowOffset) {
  // Tests A: STSO(50)+P(A20) ->
  //   if sequence mode: TSO==30,(a20@50)
  //   if segments mode: TSO==50,(a20@70)
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  if (use_sequence_mode_)
    frame_processor_->SetSequenceMode(true);

  SetTimestampOffset(Milliseconds(50));

  if (use_sequence_mode_) {
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(60)));
  } else {
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(80)));
  }

  EXPECT_TRUE(ProcessFrames("20K", ""));
  EXPECT_TRUE(in_coded_frame_group());

  // We do not stall on reading without seeking to 50ms / 70ms due to
  // SourceBufferStream::kSeekToStartFudgeRoom().
  if (use_sequence_mode_) {
    EXPECT_EQ(Milliseconds(30), timestamp_offset_);
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [50,60) }");
    CheckReadsThenReadStalls(audio_.get(), "50:20");
  } else {
    EXPECT_EQ(Milliseconds(50), timestamp_offset_);
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [70,80) }");
    CheckReadsThenReadStalls(audio_.get(), "70:20");
  }
}

TEST_P(FrameProcessorTest, AudioOnly_SequentialProcessFrames) {
  // Tests A: P(A0,A10)+P(A20,A30) -> (a0,a10,a20,a30)
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  if (use_sequence_mode_)
    frame_processor_->SetSequenceMode(true);

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(20)));
  EXPECT_TRUE(ProcessFrames("0K 10K", ""));
  EXPECT_TRUE(in_coded_frame_group());
  EXPECT_EQ(Milliseconds(0), timestamp_offset_);
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,20) }");

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(40)));
  EXPECT_TRUE(ProcessFrames("20K 30K", ""));
  EXPECT_TRUE(in_coded_frame_group());
  EXPECT_EQ(Milliseconds(0), timestamp_offset_);
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,40) }");

  CheckReadsThenReadStalls(audio_.get(), "0 10 20 30");
}

TEST_P(FrameProcessorTest, AudioOnly_NonSequentialProcessFrames) {
  // Tests A: P(A20,A30)+P(A0,A10) ->
  //   if sequence mode: TSO==-20 after first P(), 20 after second P(), and
  //                     a(20@0,a30@10,a0@20,a10@30)
  //   if segments mode: TSO==0,(a0,a10,a20,a30)
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  if (use_sequence_mode_) {
    frame_processor_->SetSequenceMode(true);
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(20)));
  } else {
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(40)));
  }

  EXPECT_TRUE(ProcessFrames("20K 30K", ""));
  EXPECT_TRUE(in_coded_frame_group());

  if (use_sequence_mode_) {
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,20) }");
    EXPECT_EQ(Milliseconds(-20), timestamp_offset_);
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(40)));
  } else {
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [20,40) }");
    EXPECT_EQ(Milliseconds(0), timestamp_offset_);
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(20)));
  }

  EXPECT_TRUE(ProcessFrames("0K 10K", ""));
  EXPECT_TRUE(in_coded_frame_group());

  if (use_sequence_mode_) {
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,40) }");
    EXPECT_EQ(Milliseconds(20), timestamp_offset_);
    CheckReadsThenReadStalls(audio_.get(), "0:20 10:30 20:0 30:10");
  } else {
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,40) }");
    EXPECT_EQ(Milliseconds(0), timestamp_offset_);
    // Re-seek to 0ms now that we've appended data earlier than what has already
    // satisfied our initial seek to start, above.
    SeekStream(audio_.get(), Milliseconds(0));
    CheckReadsThenReadStalls(audio_.get(), "0 10 20 30");
  }
}

TEST_P(FrameProcessorTest, AudioVideo_SequentialProcessFrames) {
  // Tests AV: P(A0,A10;V0k,V10,V20)+P(A20,A30,A40,V30) ->
  //   (a0,a10,a20,a30,a40);(v0,v10,v20,v30)
  InSequence s;
  AddTestTracks(HAS_AUDIO | HAS_VIDEO);
  if (use_sequence_mode_) {
    frame_processor_->SetSequenceMode(true);
    EXPECT_CALL(callbacks_,
                OnParseWarning(SourceBufferParseWarning::kMuxedSequenceMode));
    EXPECT_MEDIA_LOG(MuxedSequenceModeWarning());
  }

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(30)));
  EXPECT_TRUE(ProcessFrames("0K 10K", "0K 10 20"));
  EXPECT_TRUE(in_coded_frame_group());
  EXPECT_EQ(Milliseconds(0), timestamp_offset_);
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,20) }");
  CheckExpectedRangesByTimestamp(video_.get(), "{ [0,30) }");

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(50)));
  EXPECT_TRUE(ProcessFrames("20K 30K 40K", "30"));
  EXPECT_TRUE(in_coded_frame_group());
  EXPECT_EQ(Milliseconds(0), timestamp_offset_);
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,50) }");
  CheckExpectedRangesByTimestamp(video_.get(), "{ [0,40) }");

  CheckReadsThenReadStalls(audio_.get(), "0 10 20 30 40");
  CheckReadsThenReadStalls(video_.get(), "0 10 20 30");
}

TEST_P(FrameProcessorTest, AudioVideo_Discontinuity) {
  // Tests AV: P(A0,A10,A30,A40,A50;V0key,V10,V40,V50key) ->
  //   if sequence mode: TSO==10,(a0,a10,a30,a40,a50@60);(v0,v10,v50@60)
  //   if segments mode: TSO==0,(a0,a10,a30,a40,a50);(v0,v10,v50)
  // This assumes A40K is processed before V40, which depends currently on
  // MergeBufferQueues() behavior.
  InSequence s;
  AddTestTracks(HAS_AUDIO | HAS_VIDEO);
  if (use_sequence_mode_) {
    frame_processor_->SetSequenceMode(true);
    EXPECT_CALL(callbacks_,
                OnParseWarning(SourceBufferParseWarning::kMuxedSequenceMode));
    EXPECT_MEDIA_LOG(MuxedSequenceModeWarning());
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(70)));
  } else {
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(60)));
  }

  EXPECT_TRUE(ProcessFrames("0K 10K 30K 40K 50K", "0K 10 40 50K"));
  EXPECT_TRUE(in_coded_frame_group());

  if (use_sequence_mode_) {
    EXPECT_EQ(Milliseconds(10), timestamp_offset_);
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,70) }");
    CheckExpectedRangesByTimestamp(video_.get(), "{ [0,20) [60,70) }");
    CheckReadsThenReadStalls(audio_.get(), "0 10 30 40 60:50");
    CheckReadsThenReadStalls(video_.get(), "0 10");
    SeekStream(video_.get(), Milliseconds(60));
    CheckReadsThenReadStalls(video_.get(), "60:50");
  } else {
    EXPECT_EQ(Milliseconds(0), timestamp_offset_);
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,60) }");
    CheckExpectedRangesByTimestamp(video_.get(), "{ [0,20) [50,60) }");
    CheckReadsThenReadStalls(audio_.get(), "0 10 30 40 50");
    CheckReadsThenReadStalls(video_.get(), "0 10");
    SeekStream(video_.get(), Milliseconds(50));
    CheckReadsThenReadStalls(video_.get(), "50");
  }
}

TEST_P(FrameProcessorTest, AudioVideo_Discontinuity_TimestampOffset) {
  InSequence s;
  AddTestTracks(HAS_AUDIO | HAS_VIDEO);
  frame_processor_->SetSequenceMode(use_sequence_mode_);
  if (use_sequence_mode_) {
    EXPECT_CALL(callbacks_,
                OnParseWarning(SourceBufferParseWarning::kMuxedSequenceMode));
    EXPECT_MEDIA_LOG(MuxedSequenceModeWarning());
  }

  // Start a coded frame group at time 100ms. Note the jagged start still uses
  // the coded frame group's start time as the range start for both streams.
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(140)));
  SetTimestampOffset(Milliseconds(100));
  EXPECT_TRUE(ProcessFrames("0K 10K 20K", "10K 20K 30K"));
  EXPECT_EQ(Milliseconds(100), timestamp_offset_);
  EXPECT_TRUE(in_coded_frame_group());
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [100,130) }");
  CheckExpectedRangesByTimestamp(video_.get(), "{ [100,140) }");

  // Test the behavior of both 'sequence' and 'segments' mode if the coded frame
  // sequence jumps forward beyond the normal discontinuity threshold.
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(240)));
  SetTimestampOffset(Milliseconds(200));
  EXPECT_TRUE(ProcessFrames("0K 10K 20K", "10K 20K 30K"));
  EXPECT_EQ(Milliseconds(200), timestamp_offset_);
  EXPECT_TRUE(in_coded_frame_group());
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [100,130) [200,230) }");
  CheckExpectedRangesByTimestamp(video_.get(), "{ [100,140) [200,240) }");

  // Test the behavior when timestampOffset adjustment causes next frames to be
  // in the past relative to the previously processed frame and triggers a new
  // coded frame group.
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(95)));
  SetTimestampOffset(Milliseconds(55));
  EXPECT_TRUE(ProcessFrames("0K 10K 20K", "10K 20K 30K"));
  EXPECT_EQ(Milliseconds(55), timestamp_offset_);
  EXPECT_TRUE(in_coded_frame_group());
  // The new audio range is not within SourceBufferStream's coalescing threshold
  // relative to the next range, but the new video range is within the
  // threshold.
  CheckExpectedRangesByTimestamp(audio_.get(),
                                 "{ [55,85) [100,130) [200,230) }");
  // Note that the range adjacency logic used in this case is doesn't consider
  // DTS 85 to be close enough to [100,140), since the first DTS in video
  // range [100,140) is actually 110. The muxed data started a coded frame
  // group at time 100, but actual DTS is used for adjacency checks while
  // appending.
  CheckExpectedRangesByTimestamp(video_.get(),
                                 "{ [55,95) [100,140) [200,240) }");

  // Verify the buffers.
  // Re-seek now that we've appended data earlier than what already satisfied
  // our initial seek to start.
  SeekStream(audio_.get(), Milliseconds(55));
  SeekStream(video_.get(), Milliseconds(55));
  CheckReadsThenReadStalls(audio_.get(), "55:0 65:10 75:20");
  CheckReadsThenReadStalls(video_.get(), "65:10 75:20 85:30");
  SeekStream(audio_.get(), Milliseconds(100));
  SeekStream(video_.get(), Milliseconds(100));
  CheckReadsThenReadStalls(audio_.get(), "100:0 110:10 120:20");
  CheckReadsThenReadStalls(video_.get(), "110:10 120:20 130:30");
  SeekStream(audio_.get(), Milliseconds(200));
  SeekStream(video_.get(), Milliseconds(200));
  CheckReadsThenReadStalls(audio_.get(), "200:0 210:10 220:20");
  CheckReadsThenReadStalls(video_.get(), "210:10 220:20 230:30");
}

TEST_P(FrameProcessorTest, AudioVideo_OutOfSequence_After_Discontinuity) {
  // Once a discontinuity is detected (and all tracks drop everything until the
  // next keyframe per each track), we should gracefully handle the case where
  // some tracks' first keyframe after the discontinuity are appended after, but
  // end up earlier in timeline than some other track(s). In particular, we
  // shouldn't notify all tracks that a new coded frame group is starting and
  // begin dropping leading non-keyframes from all tracks.  Rather, we should
  // notify just the track encountering this new type of discontinuity.  Since
  // MSE doesn't require all media segments to contain media from every track,
  // these append sequences can occur.
  InSequence s;
  AddTestTracks(HAS_AUDIO | HAS_VIDEO);
  frame_processor_->SetSequenceMode(use_sequence_mode_);

  // Begin with a simple set of appends for all tracks.
  if (use_sequence_mode_) {
    // Allow room in the timeline for the last audio append (50K, below) in this
    // test to remain within default append window [0, +Infinity]. Moving the
    // sequence mode appends to begin at time 100ms, the same time as the first
    // append, below, results in a -20ms offset (instead of a -120ms offset)
    // applied to frames beginning at the first frame after the discontinuity
    // caused by the video append at 160K, below.
    SetTimestampOffset(Milliseconds(100));
    EXPECT_CALL(callbacks_,
                OnParseWarning(SourceBufferParseWarning::kMuxedSequenceMode));
    EXPECT_MEDIA_LOG(MuxedSequenceModeWarning());
  }
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(140)));
  EXPECT_TRUE(ProcessFrames("100K 110K 120K", "110K 120K 130K"));
  EXPECT_TRUE(in_coded_frame_group());
  EXPECT_EQ(Milliseconds(0), timestamp_offset_);
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [100,130) }");
  CheckExpectedRangesByTimestamp(video_.get(), "{ [100,140) }");

  // Trigger (normal) discontinuity with one track (video).
  if (use_sequence_mode_)
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(150)));
  else
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(170)));

  EXPECT_TRUE(ProcessFrames("", "160K"));
  EXPECT_TRUE(in_coded_frame_group());

  if (use_sequence_mode_) {
    // The new video buffer is relocated into [140,150).
    EXPECT_EQ(Milliseconds(-20), timestamp_offset_);
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [100,130) }");
    CheckExpectedRangesByTimestamp(video_.get(), "{ [100,150) }");
  } else {
    // The new video buffer is at [160,170).
    EXPECT_EQ(Milliseconds(0), timestamp_offset_);
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [100,130) }");
    CheckExpectedRangesByTimestamp(video_.get(), "{ [100,140) [160,170) }");
  }

  // Append to the other track (audio) with lower time than the video frame we
  // just appended. Append with a timestamp such that segments mode demonstrates
  // we don't retroactively extend the new video buffer appended above's range
  // start back to this audio start time.
  if (use_sequence_mode_)
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(150)));
  else
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(170)));

  EXPECT_TRUE(ProcessFrames("50K", ""));
  EXPECT_TRUE(in_coded_frame_group());

  // Because this is the first audio buffer appended following the discontinuity
  // detected while appending the video frame, above, a new coded frame group
  // for video is not triggered.
  if (use_sequence_mode_) {
    // The new audio buffer is relocated into [30,40). Note the muxed 'sequence'
    // mode append mode results in a buffered range gap in this case.
    EXPECT_EQ(Milliseconds(-20), timestamp_offset_);
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [30,40) [100,130) }");
    CheckExpectedRangesByTimestamp(video_.get(), "{ [100,150) }");
  } else {
    EXPECT_EQ(Milliseconds(0), timestamp_offset_);
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [50,60) [100,130) }");
    CheckExpectedRangesByTimestamp(video_.get(), "{ [100,140) [160,170) }");
  }

  // Finally, append a non-keyframe to the first track (video), to continue the
  // GOP that started the normal discontinuity on the previous video append.
  if (use_sequence_mode_)
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(160)));
  else
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(180)));

  EXPECT_TRUE(ProcessFrames("", "170"));
  EXPECT_TRUE(in_coded_frame_group());

  // Verify the final buffers. First, re-seek audio since we appended data
  // earlier than what already satisfied our initial seek to start. We satisfy
  // the seek with the first buffer in [0,1000).
  SeekStream(audio_.get(), Milliseconds(0));
  if (use_sequence_mode_) {
    // The new video buffer is relocated into [150,160).
    EXPECT_EQ(Milliseconds(-20), timestamp_offset_);
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [30,40) [100,130) }");
    CheckReadsThenReadStalls(audio_.get(), "30:50");
    SeekStream(audio_.get(), Milliseconds(100));
    CheckReadsThenReadStalls(audio_.get(), "100 110 120");

    CheckExpectedRangesByTimestamp(video_.get(), "{ [100,160) }");
    CheckReadsThenReadStalls(video_.get(), "110 120 130 140:160 150:170");
  } else {
    EXPECT_EQ(Milliseconds(0), timestamp_offset_);
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [50,60) [100,130) }");
    CheckReadsThenReadStalls(audio_.get(), "50");
    SeekStream(audio_.get(), Milliseconds(100));
    CheckReadsThenReadStalls(audio_.get(), "100 110 120");

    CheckExpectedRangesByTimestamp(video_.get(), "{ [100,140) [160,180) }");
    CheckReadsThenReadStalls(video_.get(), "110 120 130");
    SeekStream(video_.get(), Milliseconds(160));
    CheckReadsThenReadStalls(video_.get(), "160 170");
  }
}

TEST_P(FrameProcessorTest,
       AppendWindowFilterOfNegativeBufferTimestampsWithPrerollDiscard) {
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  if (use_sequence_mode_)
    frame_processor_->SetSequenceMode(true);

  SetTimestampOffset(Milliseconds(-20));
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(10)));
  EXPECT_TRUE(ProcessFrames("0K 10K 20K", ""));
  EXPECT_TRUE(in_coded_frame_group());
  EXPECT_EQ(Milliseconds(-20), timestamp_offset_);
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,10) }");
  CheckReadsThenReadStalls(audio_.get(), "0:10P 0:20");
}

TEST_P(FrameProcessorTest, AppendWindowFilterWithInexactPreroll) {
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  if (use_sequence_mode_)
    frame_processor_->SetSequenceMode(true);
  SetTimestampOffset(Milliseconds(-10));
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(20)));
  EXPECT_TRUE(ProcessFrames("0K 9.75K 20K", ""));
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,20) }");
  CheckReadsThenReadStalls(audio_.get(), "0P 0:9.75 10:20");
}

TEST_P(FrameProcessorTest, AppendWindowFilterWithInexactPreroll_2) {
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  if (use_sequence_mode_)
    frame_processor_->SetSequenceMode(true);
  SetTimestampOffset(Milliseconds(-10));
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(20)));
  EXPECT_TRUE(ProcessFrames("0K 10.25K 20K", ""));
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,20) }");
  CheckReadsThenReadStalls(audio_.get(), "0P 0:10.25 10:20");
}

TEST_P(FrameProcessorTest, AllowNegativeFramePTSAndDTSBeforeOffsetAdjustment) {
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  if (use_sequence_mode_) {
    frame_processor_->SetSequenceMode(true);
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(30)));
  } else {
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(25)));
  }

  EXPECT_TRUE(ProcessFrames("-5K 5K 15K", ""));

  if (use_sequence_mode_) {
    EXPECT_EQ(Milliseconds(5), timestamp_offset_);
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,30) }");
    CheckReadsThenReadStalls(audio_.get(), "0:-5 10:5 20:15");
  } else {
    EXPECT_EQ(Milliseconds(0), timestamp_offset_);
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,25) }");
    CheckReadsThenReadStalls(audio_.get(), "0:-5 5 15");
  }
}

TEST_P(FrameProcessorTest, PartialAppendWindowFilterNoDiscontinuity) {
  // Tests that spurious discontinuity is not introduced by a partially
  // trimmed frame.
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  if (use_sequence_mode_)
    frame_processor_->SetSequenceMode(true);
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(29)));

  append_window_start_ = Milliseconds(7);
  EXPECT_TRUE(ProcessFrames("0K 19K", ""));

  EXPECT_EQ(Milliseconds(0), timestamp_offset_);
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [7,29) }");
  CheckReadsThenReadStalls(audio_.get(), "7:0 19");
}

TEST_P(FrameProcessorTest,
       PartialAppendWindowFilterNoDiscontinuity_DtsAfterPts) {
  // Tests that spurious discontinuity is not introduced by a partially trimmed
  // frame that originally had DTS > PTS.
  InSequence s;
  AddTestTracks(HAS_AUDIO);

  EXPECT_MEDIA_LOG(ParsedDTSGreaterThanPTS()).Times(2);
  if (use_sequence_mode_) {
    frame_processor_->SetSequenceMode(true);
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(20)));
  } else {
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(13)));
  }

  // Process a sequence of two audio frames:
  // A: PTS -7ms, DTS 10ms, duration 10ms, keyframe
  // B: PTS  3ms, DTS 20ms, duration 10ms, keyframe
  EXPECT_TRUE(ProcessFrames("-7|10K 3|20K", ""));

  if (use_sequence_mode_) {
    // Sequence mode detected that frame A needs to be relocated 7ms into the
    // future to begin the sequence at time 0. There is no append window
    // filtering because the PTS result of the relocation is within the append
    // window of [0,+Infinity).
    // Frame A is relocated by 7 to PTS 0, DTS 17, duration 10.
    // Frame B is relocated by 7 to PTS 10, DTS 27, duration 10.
    EXPECT_EQ(Milliseconds(7), timestamp_offset_);

    if (range_api_ == ChunkDemuxerStream::RangeApi::kLegacyByDts) {
      // By DTS, start of frame A (17) through end of frame B (27+10).
      CheckExpectedRangesByTimestamp(audio_.get(), "{ [17,37) }");
    } else {
      // By PTS, start of frame A (0) through end of frame B (10+10).
      CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,20) }");
    }

    // Frame A is now at PTS 0 (originally at PTS -7)
    // Frame B is now at PTS 10 (originally at PTS 3)
    CheckReadsThenReadStalls(audio_.get(), "0:-7 10:3");
  } else {
    // Segments mode does not update timestampOffset automatically, so it
    // remained 0 and neither frame was relocated by timestampOffset.
    // Frame A's start *was* relocated by append window partial audio cropping:
    // Append window filtering (done by PTS, regardless of range buffering API)
    // did a partial crop of the first 7ms of frame A which was before
    // the default append window start time 0, and moved both the PTS and DTS of
    // frame A forward by 7 and reduced its duration by 7. Frame B was fully
    // inside the append window and remained uncropped and unrelocated.
    // Frame A is buffered at PTS -7+7=0, DTS 10+7=17, duration 10-7=3.
    // Frame B is buffered at PTS 3, DTS 20, duration 10.
    EXPECT_EQ(Milliseconds(0), timestamp_offset_);

    if (range_api_ == ChunkDemuxerStream::RangeApi::kLegacyByDts) {
      // By DTS, start of frame A (17) through end of frame B (20+10).
      CheckExpectedRangesByTimestamp(audio_.get(), "{ [17,30) }");
    } else {
      // By PTS, start of frame A (0) through end of frame B (3+10).
      CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,13) }");
    }

    // Frame A is now at PTS 0 (originally at PTS -7)
    // Frame B is now at PTS 3 (same as it was originally)
    CheckReadsThenReadStalls(audio_.get(), "0:-7 3");
  }
}

TEST_P(FrameProcessorTest, PartialAppendWindowFilterNoNewMediaSegment) {
  // Tests that a new media segment is not forcibly signalled for audio frame
  // partial front trim, to prevent incorrect introduction of a discontinuity
  // and potentially a non-keyframe video frame to be processed next after the
  // discontinuity.
  InSequence s;
  AddTestTracks(HAS_AUDIO | HAS_VIDEO);
  frame_processor_->SetSequenceMode(use_sequence_mode_);
  if (use_sequence_mode_) {
    EXPECT_CALL(callbacks_,
                OnParseWarning(SourceBufferParseWarning::kMuxedSequenceMode));
    EXPECT_MEDIA_LOG(MuxedSequenceModeWarning());
  }
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(10)));
  EXPECT_TRUE(ProcessFrames("", "0K"));
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(10)));
  EXPECT_TRUE(ProcessFrames("-5K", ""));
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(20)));
  EXPECT_TRUE(ProcessFrames("", "10"));

  EXPECT_EQ(Milliseconds(0), timestamp_offset_);
  EXPECT_TRUE(in_coded_frame_group());
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,5) }");
  CheckExpectedRangesByTimestamp(video_.get(), "{ [0,20) }");
  CheckReadsThenReadStalls(audio_.get(), "0:-5");
  CheckReadsThenReadStalls(video_.get(), "0 10");
}

TEST_P(FrameProcessorTest, AudioOnly_SequenceModeContinuityAcrossReset) {
  if (!use_sequence_mode_) {
    DVLOG(1) << "Skipping segments mode variant; inapplicable to this case.";
    return;
  }

  InSequence s;
  AddTestTracks(HAS_AUDIO);
  frame_processor_->SetSequenceMode(true);
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(10)));
  EXPECT_TRUE(ProcessFrames("0K", ""));
  frame_processor_->Reset();
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(20)));
  EXPECT_TRUE(ProcessFrames("100K", ""));

  EXPECT_EQ(Milliseconds(-90), timestamp_offset_);
  EXPECT_TRUE(in_coded_frame_group());
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,20) }");
  CheckReadsThenReadStalls(audio_.get(), "0 10:100");
}

TEST_P(FrameProcessorTest, PartialAppendWindowZeroDurationPreroll) {
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  frame_processor_->SetSequenceMode(use_sequence_mode_);

  append_window_start_ = Milliseconds(5);

  // Append a 0 duration frame that falls just before the append window.
  frame_duration_ = Milliseconds(0);
  EXPECT_FALSE(in_coded_frame_group());
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(0)));
  EXPECT_TRUE(ProcessFrames("4K", ""));
  // Verify buffer is not part of ranges. It should be silently saved for
  // preroll for future append.
  CheckExpectedRangesByTimestamp(audio_.get(), "{ }");
  CheckReadsThenReadStalls(audio_.get(), "");
  EXPECT_FALSE(in_coded_frame_group());

  // Abort the reads from last stall. We don't want those reads to "complete"
  // when we append below. We will initiate new reads to confirm the buffer
  // looks as we expect.
  SeekStream(audio_.get(), Milliseconds(0));

  // Append a frame with 10ms duration, with 9ms falling after the window start.
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(
                              Milliseconds(use_sequence_mode_ ? 10 : 14)));
  frame_duration_ = Milliseconds(10);
  EXPECT_TRUE(ProcessFrames("4K", ""));
  EXPECT_TRUE(in_coded_frame_group());

  // Verify range updated to reflect last append was processed and trimmed, and
  // also that zero duration buffer was saved and attached as preroll.
  if (use_sequence_mode_) {
    // For sequence mode, append window trimming is applied after the append
    // is adjusted for timestampOffset. Basically, everything gets rebased to 0
    // and trimming then removes 5 seconds from the front.
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [5,10) }");
    CheckReadsThenReadStalls(audio_.get(), "5:4P 5:4");
  } else {  // segments mode
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [5,14) }");
    CheckReadsThenReadStalls(audio_.get(), "5:4P 5:4");
  }

  // Verify the preroll buffer still has zero duration.
  StreamParserBuffer* last_read_parser_buffer =
      static_cast<StreamParserBuffer*>(last_read_buffer_.get());
  ASSERT_EQ(Milliseconds(0),
            last_read_parser_buffer->preroll_buffer()->duration());
}

TEST_P(FrameProcessorTest,
       OOOKeyframePrecededByDependantNonKeyframeShouldWarn) {
  InSequence s;
  AddTestTracks(HAS_VIDEO);
  frame_processor_->SetSequenceMode(use_sequence_mode_);

  if (use_sequence_mode_) {
    // Allow room in the timeline for the last video append (40|70, below) in
    // this test to remain within default append window [0, +Infinity]. Moving
    // the sequence mode appends to begin at time 50ms, the same time as the
    // first append, below, also results in identical expectation checks for
    // buffered ranges and buffer reads for both segments and sequence modes.
    SetTimestampOffset(Milliseconds(50));
  }

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(70)));
  EXPECT_TRUE(ProcessFrames("", "50K 60"));

  CheckExpectedRangesByTimestamp(video_.get(), "{ [50,70) }");

  EXPECT_MEDIA_LOG(ParsedDTSGreaterThanPTS());
  EXPECT_CALL(callbacks_,
              OnParseWarning(
                  SourceBufferParseWarning::kKeyframeTimeGreaterThanDependant));
  EXPECT_MEDIA_LOG(KeyframeTimeGreaterThanDependant("0.05", "0.04"));
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(70)));
  EXPECT_TRUE(ProcessFrames("", "40|70"));  // PTS=40, DTS=70

  if (range_api_ == ChunkDemuxerStream::RangeApi::kLegacyByDts) {
    // Verify DTS-based range is increased.
    CheckExpectedRangesByTimestamp(video_.get(), "{ [50,80) }");
  } else {
    // This reflects the expectation that PTS start is not "pulled backward" for
    // the new frame at PTS=40 because current spec text doesn't support SAP
    // Type 2; it has no steps in the coded frame processing algorithm that
    // would do that "pulling backward". See https://crbug.com/718641 and
    // https://github.com/w3c/media-source/issues/187.
    CheckExpectedRangesByTimestamp(video_.get(), "{ [50,70) }");
  }

  SeekStream(video_.get(), Milliseconds(0));
  CheckReadsThenReadStalls(video_.get(), "50 60 40");
}

TEST_P(FrameProcessorTest, OOOKeyframePts_1) {
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  frame_processor_->SetSequenceMode(use_sequence_mode_);

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(1010)));
  // Note that the following does not contain a DTS continuity, but *does*
  // contain a PTS discontinuity (keyframe at 0.1s after keyframe at 1s).
  EXPECT_TRUE(ProcessFrames("0K 1000|10K 100|20K", ""));

  // Force sequence mode to place the next frames where segments mode would put
  // them, to simplify this test case.
  if (use_sequence_mode_)
    SetTimestampOffset(Milliseconds(500));

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(510)));
  EXPECT_TRUE(ProcessFrames("500|100K", ""));
  EXPECT_EQ(Milliseconds(0), timestamp_offset_);

  if (range_api_ == ChunkDemuxerStream::RangeApi::kLegacyByDts) {
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,30) [100,110) }");
    CheckReadsThenReadStalls(audio_.get(), "0 1000 100");  // Verifies PTS
    SeekStream(audio_.get(), Milliseconds(100));
    CheckReadsThenReadStalls(audio_.get(), "500");  // Verifies PTS
  } else {
    CheckExpectedRangesByTimestamp(
        audio_.get(), "{ [0,10) [100,110) [500,510) [1000,1010) }");
    CheckReadsThenReadStalls(audio_.get(), "0");
    SeekStream(audio_.get(), Milliseconds(100));
    CheckReadsThenReadStalls(audio_.get(), "100");
    SeekStream(audio_.get(), Milliseconds(500));
    CheckReadsThenReadStalls(audio_.get(), "500");
    SeekStream(audio_.get(), Milliseconds(1000));
    CheckReadsThenReadStalls(audio_.get(), "1000");
  }
}

TEST_P(FrameProcessorTest, OOOKeyframePts_2) {
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  frame_processor_->SetSequenceMode(use_sequence_mode_);

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(1010)));
  EXPECT_TRUE(ProcessFrames("0K 1000|10K", ""));

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(1010)));
  EXPECT_TRUE(ProcessFrames("100|20K", ""));

  if (range_api_ == ChunkDemuxerStream::RangeApi::kLegacyByDts) {
    CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,30) }");
    CheckReadsThenReadStalls(audio_.get(), "0 1000 100");  // Verifies PTS
  } else {
    CheckExpectedRangesByTimestamp(audio_.get(),
                                   "{ [0,10) [100,110) [1000,1010) }");
    CheckReadsThenReadStalls(audio_.get(), "0");
    SeekStream(audio_.get(), Milliseconds(100));
    CheckReadsThenReadStalls(audio_.get(), "100");
    SeekStream(audio_.get(), Milliseconds(1000));
    CheckReadsThenReadStalls(audio_.get(), "1000");
  }
}

TEST_P(FrameProcessorTest, AudioNonKeyframeChangedToKeyframe) {
  // Verifies that an audio non-keyframe is changed to a keyframe with a media
  // log warning. An exact overlap append of the preceding keyframe is also done
  // to ensure that the (original non-keyframe) survives (because it was changed
  // to a keyframe, so no longer depends on the original preceding keyframe).
  // The sequence mode test version uses SetTimestampOffset to make it behave
  // like segments mode to simplify the tests.
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  frame_processor_->SetSequenceMode(use_sequence_mode_);

  EXPECT_MEDIA_LOG(AudioNonKeyframe(10000, 10000));
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(30)));
  EXPECT_TRUE(ProcessFrames("0K 10 20K", ""));

  if (use_sequence_mode_)
    SetTimestampOffset(Milliseconds(0));

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(10)));
  EXPECT_TRUE(ProcessFrames("0K", ""));

  CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,30) }");
  SeekStream(audio_.get(), Milliseconds(0));
  CheckReadsThenReadStalls(audio_.get(), "0 10 20");
}

TEST_P(FrameProcessorTest, TimestampOffsetNegativeDts) {
  // Shift a GOP earlier using timestampOffset such that the GOP has
  // starts with negative DTS, but PTS 0. Expect ByDts parse error, ByPts
  // success.
  InSequence s;
  AddTestTracks(HAS_VIDEO);
  frame_processor_->SetSequenceMode(use_sequence_mode_);

  if (!use_sequence_mode_) {
    // Simulate the offset that sequence mode would apply, to make the results
    // the same regardless of sequence vs segments mode.
    SetTimestampOffset(Milliseconds(-100));
  }

  if (range_api_ == ChunkDemuxerStream::RangeApi::kLegacyByDts) {
    EXPECT_MEDIA_LOG(NegativeDtsFailureWhenByDts("video", 0, -30000));
    EXPECT_FALSE(ProcessFrames("", "100|70K 130|80"));
  } else {
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(40)));
    EXPECT_TRUE(ProcessFrames("", "100|70K 130|80"));
    EXPECT_EQ(Milliseconds(-100), timestamp_offset_);
    CheckExpectedRangesByTimestamp(video_.get(), "{ [0,40) }");
    SeekStream(video_.get(), Milliseconds(0));
    CheckReadsThenReadStalls(video_.get(), "0:100 30:130");
  }
}

TEST_P(FrameProcessorTest, LargeTimestampOffsetJumpForward) {
  // Verifies that jumps forward in buffers emitted from the coded frame
  // processing algorithm can create discontinuous buffered ranges if those
  // jumps are large enough, in both kinds of AppendMode, and in both kinds of
  // RangeApi.
  InSequence s;
  AddTestTracks(HAS_AUDIO);
  frame_processor_->SetSequenceMode(use_sequence_mode_);

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(10)));
  EXPECT_TRUE(ProcessFrames("0K", ""));

  SetTimestampOffset(Milliseconds(5000));

  // Along with the new timestampOffset set above, this should cause a large
  // jump forward in both PTS and DTS for both sequence and segments append
  // modes.
  if (use_sequence_mode_) {
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(5010)));
  } else {
    EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(10010)));
  }
  EXPECT_TRUE(ProcessFrames("5000|100K", ""));
  if (use_sequence_mode_) {
    EXPECT_EQ(Milliseconds(0), timestamp_offset_);
  } else {
    EXPECT_EQ(Milliseconds(5000), timestamp_offset_);
  }

  if (range_api_ == ChunkDemuxerStream::RangeApi::kLegacyByDts) {
    if (use_sequence_mode_) {
      CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,10) [100,110) }");
      CheckReadsThenReadStalls(audio_.get(), "0");
      SeekStream(audio_.get(), Milliseconds(100));
      CheckReadsThenReadStalls(audio_.get(), "5000");  // Util verifies PTS.
    } else {
      CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,10) [5100,5110) }");
      CheckReadsThenReadStalls(audio_.get(), "0");
      SeekStream(audio_.get(), Milliseconds(5100));
      CheckReadsThenReadStalls(audio_.get(),
                               "10000:5000");  // Util verifies PTS.
    }
  } else {
    if (use_sequence_mode_) {
      CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,10) [5000,5010) }");
      CheckReadsThenReadStalls(audio_.get(), "0");
      SeekStream(audio_.get(), Milliseconds(5000));
      CheckReadsThenReadStalls(audio_.get(), "5000");
    } else {
      CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,10) [10000,10010) }");
      CheckReadsThenReadStalls(audio_.get(), "0");
      SeekStream(audio_.get(), Milliseconds(10000));
      CheckReadsThenReadStalls(audio_.get(), "10000:5000");
    }
  }
}

TEST_P(FrameProcessorTest,
       SegmentsMode_BufferingByPts_InitialZeroDurationBuffers) {
  // When buffering ByPts in segments append mode, verifies that initial
  // zero-duration keyframes that are not adjacent in PTS, but may be adjacent
  // if the default fudge room is large enough, doesn't result in inconsistent
  // range adjacency determination on a subsequent overlapping append of a
  // non-zero duration keyframe with a duration smaller than half the default
  // initial fudge room.
  if (range_api_ == ChunkDemuxerStream::RangeApi::kLegacyByDts ||
      use_sequence_mode_) {
    DVLOG(1) << "Skipping kLegacyByDts and sequence mode versions of this test";
    return;
  }

  InSequence s;
  AddTestTracks(HAS_AUDIO);
  frame_processor_->SetSequenceMode(use_sequence_mode_);

  frame_duration_ = Milliseconds(0);
  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(10)));
  EXPECT_TRUE(ProcessFrames("0K 10|1K", ""));

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(Milliseconds(3)));
  EXPECT_TRUE(ProcessFrames("3|2K", ""));

  EXPECT_CALL(callbacks_, PossibleDurationIncrease(
                              base::TimeDelta::FromMicroseconds(6001)));
  frame_duration_ = base::TimeDelta::FromMicroseconds(1);
  EXPECT_TRUE(ProcessFrames("6|3K", ""));

  // Note the following includes a truncated 6001 microseconds end time for the
  // third range.
  CheckExpectedRangesByTimestamp(audio_.get(), "{ [0,1) [3,4) [6,6) [10,11) }");
}

INSTANTIATE_TEST_CASE_P(SequenceModeLegacyByDts,
                        FrameProcessorTest,
                        Values(FrameProcessorTestParams(
                            true,
                            ChunkDemuxerStream::RangeApi::kLegacyByDts)));
INSTANTIATE_TEST_CASE_P(SegmentsModeLegacyByDts,
                        FrameProcessorTest,
                        Values(FrameProcessorTestParams(
                            false,
                            ChunkDemuxerStream::RangeApi::kLegacyByDts)));
INSTANTIATE_TEST_CASE_P(
    SequenceModeNewByPts,
    FrameProcessorTest,
    Values(FrameProcessorTestParams(true,
                                    ChunkDemuxerStream::RangeApi::kNewByPts)));
INSTANTIATE_TEST_CASE_P(
    SegmentsModeNewByPts,
    FrameProcessorTest,
    Values(FrameProcessorTestParams(false,
                                    ChunkDemuxerStream::RangeApi::kNewByPts)));

}  // namespace media
