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

#ifndef LIBDE265_ERROR_CONCEALMENT_H
#define LIBDE265_ERROR_CONCEALMENT_H

#include "libde265/de265.h"
#include <atomic>
#include <memory>

struct de265_image;
class decoder_context;

/* Output-side error concealment.
 *
 * All methods run on the decoder owner thread (the thread that calls
 * de265_decode()). de265_start_worker_threads() only adds internal WPP/tile
 * worker threads, whose tasks are fully joined before any of these methods is
 * entered, so no additional locking is required around the cached picture.
 * The only cross-thread state is the policy itself, which is an atomic and
 * therefore safe to switch while worker threads are active.
 */
class error_concealment {
public:
  error_concealment() = default;
  ~error_concealment();

  error_concealment(const error_concealment&) = delete;
  error_concealment& operator=(const error_concealment&) = delete;

  // Thread-safe policy access. The enum fits in an int and is exchanged with
  // acquire/release ordering so a switched policy becomes visible immediately
  // at the next picture boundary.
  void set_policy(de265_error_concealment policy) {
    mPolicy.store(static_cast<int>(policy), std::memory_order_release);
  }

  de265_error_concealment get_policy() const {
    return static_cast<de265_error_concealment>(
        mPolicy.load(std::memory_order_acquire));
  }

  /* Remember the pixels of a correctly decoded output picture as the
     concealment source for later damaged pictures. A copy of the pixel planes
     is kept in a decoder-owned buffer whose lifetime is independent of the
     DPB/output-queue lifetime of 'img'. No-op for pictures without output
     (PicOutputFlag == false). */
  de265_error remember_correct_picture(decoder_context* ctx,
                                       const de265_image* img);

  /* Replace the output pixels of the damaged picture 'img' according to the
     currently configured policy. Returns true when concealment was applied
     (the picture now contains the previous good picture), false when no
     concealment was possible and the caller must keep the baseline behavior.
     On success the picture's integrity is set to INTEGRITY_CORRECT and its
     reference state is handled by the caller (it is marked
     UnusedForReference so the concealed pixels never pollute later frames). */
  bool apply(decoder_context* ctx, de265_image* img);

  /* Drop all state (e.g. on de265_reset()). A previously cached picture is
     released, so a geometry change after a reset cannot leak stale pixels. */
  void clear();

private:
  std::atomic<int> mPolicy{DE265_CONCEALMENT_OFF};

  // Decoder-owned copy of the last correct output picture. nullptr when no
  // concealment source is available. Owned exclusively by this object.
  de265_image* mLastCorrectImage = nullptr;
};

#endif
