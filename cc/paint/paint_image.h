// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CC_PAINT_PAINT_IMAGE_H_
#define CC_PAINT_PAINT_IMAGE_H_

#include <vector>

#include "base/gtest_prod_util.h"
#include "base/logging.h"
#include "cc/paint/frame_metadata.h"
#include "cc/paint/image_animation_count.h"
#include "cc/paint/paint_export.h"
#include "cc/paint/skia_paint_image_generator.h"
#include "third_party/skia/include/core/SkImage.h"
#include "ui/gfx/geometry/rect.h"

namespace cc {

class PaintImageGenerator;
class PaintOpBuffer;
using PaintRecord = PaintOpBuffer;

// A representation of an image for the compositor.
// Note that aside from default construction, it can only be constructed using a
// PaintImageBuilder, or copied/moved into using operator=.
class CC_PAINT_EXPORT PaintImage {
 public:
  using Id = int;
  using AnimationSequenceId = uint32_t;

  // A ContentId is used to identify the content for which images which can be
  // lazily generated (generator/record backed images). As opposed to Id, which
  // stays constant for the same image, the content id can be updated when the
  // backing encoded data for this image changes. For instance, in the case of
  // images which can be progressively updated as more encoded data is received.
  using ContentId = int;

  // An id that can be used for all non-lazy images. Note that if an image is
  // not lazy, it does not mean that this id must be used; one can still use
  // GetNextId to generate a stable id for such images.
  static const Id kNonLazyStableId;

  // The default frame index to use if no index is provided. For multi-frame
  // images, this would imply the first frame of the animation.
  static const size_t kDefaultFrameIndex;

  static const Id kInvalidId;

  class CC_PAINT_EXPORT FrameKey {
   public:
    FrameKey(ContentId content_id, size_t frame_index, gfx::Rect subset_rect);
    bool operator==(const FrameKey& other) const;
    bool operator!=(const FrameKey& other) const;

    uint64_t hash() const { return hash_; }
    std::string ToString() const;
    size_t frame_index() const { return frame_index_; }

   private:
    ContentId content_id_;
    size_t frame_index_;
    // TODO(khushalsagar): Remove this when callers take care of subsetting.
    gfx::Rect subset_rect_;

    size_t hash_;
  };

  struct CC_PAINT_EXPORT FrameKeyHash {
    size_t operator()(const FrameKey& frame_key) const {
      return frame_key.hash();
    }
  };

  enum class AnimationType { ANIMATED, VIDEO, STATIC };
  enum class CompletionState { DONE, PARTIALLY_DONE };
  enum class DecodingMode {
    // No preference has been specified. The compositor may choose to use sync
    // or async decoding. See CheckerImageTracker for the default behaviour.
    kUnspecified,

    // It's preferred to display this image synchronously with the rest of the
    // content updates, skipping any heuristics.
    kSync,

    // Async is preferred. The compositor may decode async if it meets the
    // heuristics used to avoid flickering (for instance vetoing of multipart
    // response, animated, partially loaded images) and would be performant. See
    // CheckerImageTracker for all heuristics used.
    kAsync
  };

  // Returns the more conservative mode out of the two given ones.
  static DecodingMode GetConservative(DecodingMode one, DecodingMode two);

  static Id GetNextId();
  static ContentId GetNextContentId();

  PaintImage();
  PaintImage(const PaintImage& other);
  PaintImage(PaintImage&& other);
  ~PaintImage();

  PaintImage& operator=(const PaintImage& other);
  PaintImage& operator=(PaintImage&& other);

  bool operator==(const PaintImage& other) const;
  bool operator!=(const PaintImage& other) const { return !(*this == other); }

  // Returns the smallest size that is at least as big as the requested_size
  // such that we can decode to exactly that scale. If the requested size is
  // larger than the image, this returns the image size. Any returned value is
  // guaranteed to be stable. That is,
  // GetSupportedDecodeSize(GetSupportedDecodeSize(size)) is guaranteed to be
  // GetSupportedDecodeSize(size).
  SkISize GetSupportedDecodeSize(const SkISize& requested_size) const;

  // Returns SkImageInfo that should be used to decode this image to the given
  // size and color type. The size must be supported.
  SkImageInfo CreateDecodeImageInfo(const SkISize& size,
                                    SkColorType color_type) const;

  // Decode the image into the given memory for the given SkImageInfo.
  // - Size in |info| must be supported.
  // - The amount of memory allocated must be at least
  //   |info|.minRowBytes() * |info|.height().
  // Returns true on success and false on failure. Updates |info| to match the
  // requested color space, if provided.
  // Note that for non-lazy images this will do a copy or readback if the image
  // is texture backed.
  bool Decode(void* memory,
              SkImageInfo* info,
              sk_sp<SkColorSpace> color_space,
              size_t frame_index) const;

  Id stable_id() const { return id_; }
  const sk_sp<SkImage>& GetSkImage() const;
  AnimationType animation_type() const { return animation_type_; }
  CompletionState completion_state() const { return completion_state_; }
  bool is_multipart() const { return is_multipart_; }
  int repetition_count() const { return repetition_count_; }
  bool ShouldAnimate() const;
  size_t frame_index() const { return frame_index_; }
  AnimationSequenceId reset_animation_sequence_id() const {
    return reset_animation_sequence_id_;
  }
  DecodingMode decoding_mode() const { return decoding_mode_; }

  // TODO(vmpstr): Don't get the SkImage here if you don't need to.
  uint32_t unique_id() const { return GetSkImage()->uniqueID(); }
  explicit operator bool() const { return !!GetSkImage(); }
  bool IsLazyGenerated() const { return GetSkImage()->isLazyGenerated(); }
  int width() const { return GetSkImage()->width(); }
  int height() const { return GetSkImage()->height(); }
  SkColorSpace* color_space() const { return GetSkImage()->colorSpace(); }

  // Returns a unique id for the pixel data for the frame at |frame_index|. Used
  // only for lazy-generated images.
  FrameKey GetKeyForFrame(size_t frame_index) const;

  // Returns the metadata for each frame of a multi-frame image. Should only be
  // used with animated images.
  const std::vector<FrameMetadata>& GetFrameMetadata() const;

  // Returns the total number of frames known to exist in this image.
  size_t FrameCount() const;

  std::string ToString() const;

 private:
  static const ContentId kInvalidContentId = -1;
  friend class PaintImageBuilder;
  FRIEND_TEST_ALL_PREFIXES(PaintImageTest, Subsetting);

  bool DecodeFromGenerator(void* memory,
                           SkImageInfo* info,
                           sk_sp<SkColorSpace> color_space,
                           size_t frame_index) const;
  bool DecodeFromSkImage(void* memory,
                         SkImageInfo* info,
                         sk_sp<SkColorSpace> color_space,
                         size_t frame_index) const;
  void CreateSkImage();
  PaintImage MakeSubset(const gfx::Rect& subset) const;

  // Returns an SkImage for the frame at |index|.
  sk_sp<SkImage> GetSkImageForFrame(size_t index) const;

  sk_sp<SkImage> sk_image_;

  sk_sp<PaintRecord> paint_record_;
  gfx::Rect paint_record_rect_;
  ContentId paint_record_content_id_ = kInvalidContentId;

  sk_sp<PaintImageGenerator> paint_image_generator_;

  Id id_ = 0;
  AnimationType animation_type_ = AnimationType::STATIC;
  CompletionState completion_state_ = CompletionState::DONE;
  int repetition_count_ = kAnimationNone;

  // If non-empty, holds the subset of this image relative to the original image
  // at the origin.
  gfx::Rect subset_rect_;

  // The frame index to use when rasterizing this image.
  size_t frame_index_ = kDefaultFrameIndex;

  // Whether the data fetched for this image is a part of a multpart response.
  bool is_multipart_ = false;

  // An incrementing sequence number maintained by the painter to indicate if
  // this animation should be reset in the compositor. Incrementing this number
  // will reset this animation in the compositor for the first frame which has a
  // recording with a PaintImage storing the updated sequence id.
  AnimationSequenceId reset_animation_sequence_id_ = 0u;

  DecodingMode decoding_mode_ = DecodingMode::kSync;

  // The |cached_sk_image_| can be derived/created from other inputs present in
  // the PaintImage but we always construct it at creation time for 2 reasons:
  // 1) This ensures that the underlying SkImage is shared across PaintImage
  //    copies, which is necessary to allow reuse of decodes from this image in
  //    skia's cache.
  // 2) Ensures that accesses to it are thread-safe.
  sk_sp<SkImage> cached_sk_image_;
};

}  // namespace cc

#endif  // CC_PAINT_PAINT_IMAGE_H_
