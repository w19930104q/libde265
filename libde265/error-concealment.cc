/*
 * H.265 video codec.
 * Copyright (c) 2026 struktur AG
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "error-concealment.h"
#include "decctx.h"
#include "image.h"
#include "sps.h"

#include <assert.h>


error_concealment::~error_concealment()
{
  clear();
}


void error_concealment::clear()
{
  if (mLastCorrectImage) {
    delete mLastCorrectImage;
    mLastCorrectImage = nullptr;
  }
}


static bool same_geometry(const de265_image* a, const de265_image* b)
{
  return a->get_width(0)  == b->get_width(0) &&
         a->get_height(0) == b->get_height(0) &&
         a->get_chroma_format() == b->get_chroma_format() &&
         a->BitDepth_Y == b->BitDepth_Y &&
         a->BitDepth_C == b->BitDepth_C;
}


de265_error error_concealment::remember_correct_picture(decoder_context* ctx,
                                                        const de265_image* img)
{
  // Pictures that are not output (e.g. RASL pictures with
  // NoRaslOutputFlag) cannot become the concealment source: the concealed
  // output must itself be outputtable, and only output pictures establish
  // what the application actually saw as the previous frame.
  if (!img->PicOutputFlag) {
    return DE265_OK;
  }

  de265_image* cache = mLastCorrectImage;

  // (Re)allocate the internal cache when it does not exist yet or when the
  // stream geometry changed. alloc_image() releases the old pixel buffers on
  // reuse, so keeping one de265_image around avoids an allocation per frame.
  if (cache == nullptr) {
    cache = new de265_image;
    mLastCorrectImage = cache;
  }

  if (!cache->is_allocated() || !same_geometry(cache, img)) {
    // Allocated without decoding metadata: the cache stores pixels only.
    // The default (built-in) allocation functions are always used, so this
    // buffer is independent of any application-provided image allocator.
    // get_shared_sps() has no const overload in the existing API; the cast
    // only reads the shared pointer and does not modify the image.
    auto sps = const_cast<de265_image*>(img)->get_shared_sps();
    de265_error err = cache->alloc_image(img->get_width(0), img->get_height(0),
                                         img->get_chroma_format(),
                                         sps,
                                         /*allocMetadata=*/false,
                                         ctx, img->pts, img->user_data,
                                         /*useCustomAllocFunc=*/false);
    if (err != DE265_OK) {
      delete cache;
      mLastCorrectImage = nullptr;
      return err;
    }
  }

  cache->pts = img->pts;
  cache->user_data = img->user_data;
  cache->PicOutputFlag = true;
  cache->integrity.store(INTEGRITY_CORRECT, std::memory_order_release);

  cache->copy_lines_from(img, 0, img->get_height(0));
  return DE265_OK;
}


bool error_concealment::apply(decoder_context* ctx, de265_image* img)
{
  if (get_policy() != DE265_CONCEALMENT_COPY_PREVIOUS_FRAME) {
    return false;
  }

  de265_image* src = mLastCorrectImage;
  if (src == nullptr || !src->is_allocated() || !same_geometry(src, img)) {
    // No usable previous frame (stream start, or geometry change): leave the
    // picture untouched so that the baseline output behavior is preserved.
    return false;
  }

  // Whole-frame replacement. The destination already owns buffers with the
  // same geometry (it was allocated by the normal DPB path), so only the
  // pixel planes have to be copied.
  img->copy_lines_from(src, 0, img->get_height(0));

  // The output is now pixel-identical to a verified picture.
  img->integrity.store(INTEGRITY_CORRECT, std::memory_order_release);

  // Keep this picture's own PTS/user data/NAL header (they describe the
  // damaged access unit), but do not let the borrowed pixels enter the
  // reference pool: the caller marks the picture UnusedForReference after
  // this function returns.
  (void)ctx;
  return true;
}
