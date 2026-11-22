/*
 * H.265 video codec.
 * Copyright (c) 2026 struktur AG
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

// Acceptance test for the error concealment policy API.
//
// The test media in testdata/error-concealment/ is generated once and
// committed to the repository, so every byte of the expected output below is
// deterministic:
//   sample.h265             45 frames, 176x144, IRAP pictures every 15 frames
//                           (frames 0, 15, 30)
//   frame10-corrupt.h265    same stream, but the slice NAL of frame 10
//                           (a TRAIL_R reference picture) is truncated to
//                           40% of its size, simulating packet loss
//   frame10-removed.h265    same stream with the damaged access unit removed
//
// Acceptance criteria checked here:
//   a) With DE265_CONCEALMENT_COPY_PREVIOUS_FRAME, the damaged frame 10 is
//      output pixel-identical to frame 9 of the clean stream (hard-coded MD5
//      below). From the next IRAP (frame 15) on, the output is identical to
//      the decode of the reference stream with the damaged access unit
//      removed, i.e. the concealed picture did not pollute any later frame.
//   b) With DE265_CONCEALMENT_OFF, the damaged stream decodes exactly as it
//      does without any of the concealment code (its output is byte-identical
//      to a build of the unmodified baseline; here we hard-code the baseline
//      MD5 of the damaged frame).
//   c) Switching the policy 1000 times while worker threads are active is
//      race-free. The policy is an std::atomic exchanged with
//      acquire/release ordering, and the test below is the TSan/helgrind
//      workload (run the whole executable under the race detector).
//   d) Existing decoder semantics are unchanged: the clean stream decodes to
//      identical hashes under every policy.
//
// Usage: error-concealment-test <path-to-testdata/error-concealment>
//
// Threading note: the hash-asserting tests run single-threaded. On a
// corrupted stream, the WPP worker path already exhibits worker-count
// dependent output in the unmodified baseline decoder (row tasks stop at
// different points of the truncated bitstream), so pixel hashes of damaged
// pictures and of the gray unavailable-reference pictures that follow are
// only deterministic for a fixed thread configuration. The multi-threaded
// behavior is covered separately by the concurrent policy-switch test [5].

#include "libde265/de265.h"
#include "libde265/md5.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kNumFrames = 45;
constexpr int kCorruptFrame = 10;
constexpr int kIrapFrame = 15;   // first IRAP after the damaged picture

// Hard-coded MD5 of the visible YUV420 planes (as exposed through the public
// de265_get_image_plane() API). Computed from the committed test media with
// the deterministic single-threaded decode path; any change in decoding
// behavior makes one of these mismatch.
const char* const kCleanFrame9Md5   = "6d77739ae1c9bd4e29a7868b0f701803";
const char* const kCleanFrame15Md5  = "98c6c6572d73809220fc114d6b730591";
const char* const kCleanLastFrameMd5 = "3d0df443388dee1f3ae2d4bb113327ae";
// Baseline (concealment OFF) MD5 of the damaged frame 10: the content
// produced by the normal error path, frozen here to prove the OFF policy
// keeps baseline behavior. Byte-identical output against an unmodified
// baseline build is additionally verified outside this program (cmp).
const char* const kBaselineCorruptFrame10Md5 = "701e30c269d7c1d6fff7178f8eee9686";
// Gray unavailable-reference content of frames 11..14 under COPY policy.
const char* const kGrayFrameMd5 = "ffccdf618878fe6e8a227e91d5028461";
int g_failures = 0;

void check(bool condition, const char* message)
{
  if (condition) {
    printf("  PASS: %s\n", message);
  }
  else {
    printf("  FAIL: %s\n", message);
    g_failures++;
  }
}

std::vector<uint8_t> read_file(const std::string& path)
{
  FILE* fh = fopen(path.c_str(), "rb");
  if (!fh) {
    fprintf(stderr, "cannot open %s\n", path.c_str());
    return {};
  }
  fseek(fh, 0, SEEK_END);
  long size = ftell(fh);
  fseek(fh, 0, SEEK_SET);
  std::vector<uint8_t> data(size);
  size_t got = fread(data.data(), 1, size, fh);
  fclose(fh);
  data.resize(got);
  return data;
}

// Hash the three visible planes of one output picture, in the exact layout a
// consumer reading de265_get_image_plane() row by row would observe.
std::string md5_of_picture(const de265_image* img)
{
  MD5_CTX ctx;
  MD5_Init(&ctx);

  for (int c = 0; c < 3; c++) {
    int stride_bytes = 0;
    const uint8_t* plane = de265_get_image_plane(img, c, &stride_bytes);
    int width = de265_get_image_width(img, c);
    int height = de265_get_image_height(img, c);
    for (int y = 0; y < height; y++) {
      // MD5_Update() predates const-correctness; it only reads the data.
      MD5_Update(&ctx, const_cast<uint8_t*>(plane) + y * stride_bytes, width);
    }
  }

  unsigned char digest[16];
  MD5_Final(digest, &ctx);

  char hex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  }
  return std::string(hex, 32);
}

std::vector<std::string> decode_stream(const std::vector<uint8_t>& bitstream,
                                       de265_error_concealment policy)
{
  de265_decoder_context* ctx = de265_new_decoder();
  de265_set_error_concealment(ctx, policy);

  de265_push_data(ctx, bitstream.data(), (int)bitstream.size(), 0, nullptr);
  de265_flush_data(ctx);

  std::vector<std::string> hashes;
  int more = 1;
  while (more) {
    more = 0;
    de265_error err = de265_decode(ctx, &more);
    if (err != DE265_OK && err != DE265_ERROR_WAITING_FOR_INPUT_DATA) {
      fprintf(stderr, "decode error: %s\n", de265_get_error_text(err));
      break;
    }

    const de265_image* img = de265_get_next_picture(ctx);
    if (img) {
      hashes.push_back(md5_of_picture(img));
    }
  }

  de265_free_decoder(ctx);
  return hashes;
}

void test_policy_api_basics()
{
  printf("[1] policy API defaults, get/set and rejection of bad values\n");

  de265_decoder_context* ctx = de265_new_decoder();
  check(de265_get_error_concealment(ctx) == DE265_CONCEALMENT_OFF,
        "default policy is DE265_CONCEALMENT_OFF");

  check(de265_set_error_concealment(ctx, DE265_CONCEALMENT_COPY_PREVIOUS_FRAME)
        == DE265_OK, "set COPY_PREVIOUS_FRAME succeeds");
  check(de265_get_error_concealment(ctx) == DE265_CONCEALMENT_COPY_PREVIOUS_FRAME,
        "get returns COPY_PREVIOUS_FRAME after set");

  check(de265_set_error_concealment(ctx, DE265_CONCEALMENT_OFF) == DE265_OK,
        "set OFF succeeds");
  check(de265_get_error_concealment(ctx) == DE265_CONCEALMENT_OFF,
        "get returns OFF after set");

  de265_error_concealment before = de265_get_error_concealment(ctx);
  de265_error err = de265_set_error_concealment(
      ctx, static_cast<de265_error_concealment>(9999));
  check(err == DE265_ERROR_INVALID_ARGUMENT,
        "unknown policy rejected with DE265_ERROR_INVALID_ARGUMENT");
  check(de265_get_error_concealment(ctx) == before,
        "policy unchanged after rejected value");

  de265_free_decoder(ctx);
}

void test_clean_stream_unchanged(const std::vector<uint8_t>& clean)
{
  printf("[2] clean stream decodes identically under both policies\n");

  auto off = decode_stream(clean, DE265_CONCEALMENT_OFF);
  auto copy = decode_stream(clean, DE265_CONCEALMENT_COPY_PREVIOUS_FRAME);

  check(off.size() == kNumFrames && copy.size() == kNumFrames,
        "all 45 frames decoded under both policies");
  check(off == copy,
        "clean-stream output hashes identical with OFF and COPY_PREVIOUS");
  check(off.size() > 9 && off[9] == kCleanFrame9Md5,
        "frame 9 matches hard-coded clean MD5");
  check(off.size() > kIrapFrame && off[kIrapFrame] == kCleanFrame15Md5,
        "IRAP frame 15 matches hard-coded clean MD5");
  check(off.back() == kCleanLastFrameMd5,
        "last frame matches hard-coded clean MD5");
}

void test_concealment_off_baseline(const std::vector<uint8_t>& corrupt)
{
  printf("[3] OFF policy on corrupt stream keeps baseline behavior\n");

  auto hashes = decode_stream(corrupt, DE265_CONCEALMENT_OFF);
  check(hashes.size() == kNumFrames, "all 45 frames still output");
  check(hashes[kCorruptFrame] == kBaselineCorruptFrame10Md5,
        "damaged frame 10 matches hard-coded baseline (OFF) MD5");
  check(hashes[9] == kCleanFrame9Md5,
        "frame 9 before the damage is unchanged");
  check(hashes[kCorruptFrame] != hashes[kCorruptFrame - 1],
        "damaged frame 10 is not silently replaced (no concealment)");
}

void test_copy_previous_concealment(const std::vector<uint8_t>& corrupt,
                                    const std::vector<uint8_t>& removed)
{
  printf("[4] COPY_PREVIOUS_FRAME: freeze + no reference pollution\n");

  auto copy = decode_stream(corrupt, DE265_CONCEALMENT_COPY_PREVIOUS_FRAME);
  auto reference = decode_stream(removed, DE265_CONCEALMENT_OFF);

  check(copy.size() == kNumFrames, "all 45 frames still output");

  // (a) damaged frame output is pixel-identical to the previous good frame.
  check(copy[kCorruptFrame] == kCleanFrame9Md5,
        "damaged frame 10 output is identical to clean frame 9 (hard-coded MD5)");
  check(copy[kCorruptFrame] == copy[kCorruptFrame - 1],
        "damaged frame 10 equals the immediately preceding output frame");

  // The concealed picture is marked UnusedForReference, so the pictures
  // decoded until the next IRAP fall back to the standard
  // unavailable-reference (gray) handling instead of borrowing its pixels.
  bool gray_between = true;
  for (int i = kCorruptFrame + 1; i < kIrapFrame; i++) {
    if (copy[i] != kGrayFrameMd5) {
      gray_between = false;
      break;
    }
  }
  check(gray_between,
        "frames 11..14 use gray unavailable-reference fallback, not the freeze");

  // (a, second half) from the next IRAP on, decoding is identical to the
  // stream with the damaged access unit removed. That reference stream has
  // one frame fewer, so its frame (i-1) aligns with our frame i.
  bool post_irap_matches_reference = true;
  for (int i = kIrapFrame; i < kNumFrames; i++) {
    if (i - 1 >= (int)reference.size() || copy[i] != reference[i - 1]) {
      post_irap_matches_reference = false;
      fprintf(stderr, "    divergence at output frame %d: %s vs ref %s\n",
              i, copy[i].c_str(),
              (i - 1 < (int)reference.size()) ? reference[i - 1].c_str()
                                              : "<missing>");
      break;
    }
  }
  check(post_irap_matches_reference,
        "frames 15..44 identical to reference stream with damaged AU removed");
  check(copy[kIrapFrame] == kCleanFrame15Md5,
        "IRAP frame 15 itself matches the hard-coded clean MD5");
  check(copy.back() == kCleanLastFrameMd5,
        "last frame matches hard-coded clean MD5 (no damage propagated)");
}

// Smoke test for the WPP worker path: the single-threaded hash tests cannot
// exercise it, and on damaged pictures WPP output is inherently
// worker-count dependent (a pre-existing baseline property). What must hold
// regardless of threading is that concealment engages on the damaged frame
// and that decoding completes with the full frame count and the deterministic
// post-IRAP hashes.
void test_worker_threads_smoke(const std::vector<uint8_t>& corrupt,
                               int worker_threads)
{
  printf("[4b] COPY_PREVIOUS_FRAME with %d WPP worker threads\n",
         worker_threads);

  de265_decoder_context* ctx = de265_new_decoder();
  de265_start_worker_threads(ctx, worker_threads);
  de265_set_error_concealment(ctx, DE265_CONCEALMENT_COPY_PREVIOUS_FRAME);
  de265_push_data(ctx, corrupt.data(), (int)corrupt.size(), 0, nullptr);
  de265_flush_data(ctx);

  std::vector<std::string> hashes;
  int more = 1;
  while (more) {
    more = 0;
    de265_error err = de265_decode(ctx, &more);
    if (err != DE265_OK && err != DE265_ERROR_WAITING_FOR_INPUT_DATA) {
      fprintf(stderr, "decode error: %s\n", de265_get_error_text(err));
      g_failures++;
      break;
    }
    const de265_image* img = de265_get_next_picture(ctx);
    if (img) {
      hashes.push_back(md5_of_picture(img));
    }
  }
  de265_free_decoder(ctx);

  check(hashes.size() == kNumFrames, "all 45 frames output under WPP");
  check(hashes[kCorruptFrame] == kCleanFrame9Md5,
        "damaged frame 10 concealed to previous-frame MD5 under WPP");
  check(hashes[kIrapFrame] == kCleanFrame15Md5,
        "IRAP frame 15 MD5 stable under WPP");
  check(hashes.back() == kCleanLastFrameMd5,
        "last frame MD5 stable under WPP (no damage propagated)");
}

// (c) Race-detector workload: a dedicated thread hammers the policy switch
// 1000 times while the owner thread decodes two full streams with WPP worker
// threads enabled. Only the atomic policy is touched cross-thread (the
// documented owner-thread rule still covers all other decoder calls), so TSan
// must report zero races here. The design rationale (atomic acquire/release
// policy; picture state touched only by the owner thread after all slice
// tasks are joined) is documented in de265.h and error-concealment.h.
void test_concurrent_policy_switch(const std::vector<uint8_t>& clean,
                                   const std::vector<uint8_t>& corrupt,
                                   int worker_threads, int switches)
{
  printf("[5] %d concurrent policy switches while decoding (worker threads=%d)\n",
         switches, worker_threads);

  de265_decoder_context* ctx = de265_new_decoder();
  if (worker_threads > 0) {
    de265_start_worker_threads(ctx, worker_threads);
  }

  std::atomic<bool> stop{false};
  std::atomic<int> observed_switches{0};

  std::thread switcher([&] {
    for (int i = 0; i < switches; i++) {
      de265_error_concealment p =
          (i & 1) ? DE265_CONCEALMENT_COPY_PREVIOUS_FRAME
                  : DE265_CONCEALMENT_OFF;
      de265_set_error_concealment(ctx, p);
      observed_switches.store(i, std::memory_order_release);
      if ((i & 7) == 0) {
        std::this_thread::yield();
      }
    }
    stop.store(true, std::memory_order_release);
  });

  // Decode both streams back-to-back on the owner thread; every picture
  // boundary reads the atomic policy, exercising concurrent set/get.
  std::vector<uint8_t> combined;
  combined.insert(combined.end(), clean.begin(), clean.end());
  combined.insert(combined.end(), corrupt.begin(), corrupt.end());
  de265_push_data(ctx, combined.data(), (int)combined.size(), 0, nullptr);
  de265_flush_data(ctx);

  int pictures = 0;
  int more = 1;
  while (more) {
    more = 0;
    de265_error err = de265_decode(ctx, &more);
    if (err != DE265_OK && err != DE265_ERROR_WAITING_FOR_INPUT_DATA) {
      fprintf(stderr, "decode error during switch stress: %s\n",
              de265_get_error_text(err));
      g_failures++;
      break;
    }
    while (de265_get_next_picture(ctx) != nullptr) {
      pictures++;
    }
  }

  switcher.join();
  de265_free_decoder(ctx);

  check(stop.load() && observed_switches.load() == switches - 1,
        "all policy switches completed");
  check(pictures == 2 * kNumFrames,
        "both streams decoded to completion despite concurrent switching");
}

}  // namespace

int main(int argc, char** argv)
{
  if (argc < 2) {
    fprintf(stderr,
            "usage: %s <testdata/error-concealment-dir> [stress-worker-threads]\n",
            argv[0]);
    return 2;
  }

  std::string dir = argv[1];
  int stress_worker_threads = (argc >= 3) ? atoi(argv[2]) : 2;

  std::vector<uint8_t> clean = read_file(dir + "/sample.h265");
  std::vector<uint8_t> corrupt = read_file(dir + "/frame10-corrupt.h265");
  std::vector<uint8_t> removed = read_file(dir + "/frame10-removed.h265");
  if (clean.empty() || corrupt.empty() || removed.empty()) {
    return 2;
  }

  test_policy_api_basics();
  test_clean_stream_unchanged(clean);
  test_concealment_off_baseline(corrupt);
  test_copy_previous_concealment(corrupt, removed);
  test_worker_threads_smoke(corrupt, stress_worker_threads);
  test_concurrent_policy_switch(clean, corrupt, stress_worker_threads, 1000);

  printf("\n%s: %d failure(s)\n",
         g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
