/**************************************************************************
 *
 * Copyright 2022 Red Hat
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
#include "pipe/p_video_codec.h"

#include "vl_codec.h"

bool vl_codec_supported(struct pipe_screen *screen,
                        enum pipe_video_profile profile,
                        bool encode)
{
   static_assert(PIPE_VIDEO_PROFILE_MAX == 30, "Update table below when adding new video profiles");
   if (profile == PIPE_VIDEO_PROFILE_AV1_MAIN ||
       profile == PIPE_VIDEO_PROFILE_AV1_PROFILE2) {
      if (encode) {
         if (!VIDEO_CODEC_AV1ENC)
            return false;
      } else if (!VIDEO_CODEC_AV1DEC) {
         return false;
      }
   }
   if (profile == PIPE_VIDEO_PROFILE_VP9_PROFILE0 ||
       profile == PIPE_VIDEO_PROFILE_VP9_PROFILE2) {
      if (!VIDEO_CODEC_VP9DEC)
         return false;
   }
   if (profile == PIPE_VIDEO_PROFILE_VC1_SIMPLE ||
       profile == PIPE_VIDEO_PROFILE_VC1_MAIN ||
       profile == PIPE_VIDEO_PROFILE_VC1_ADVANCED) {
      if (!VIDEO_CODEC_VC1DEC)
         return false;
   }
   if (profile >= PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE &&
       profile <= PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH444) {
      if (encode) {
         if (!VIDEO_CODEC_H264ENC)
            return false;
      } else if (!VIDEO_CODEC_H264DEC) {
         return false;
      }
   }
   if (profile >= PIPE_VIDEO_PROFILE_HEVC_MAIN &&
       profile <= PIPE_VIDEO_PROFILE_HEVC_MAIN_444) {
      if (encode) {
         if (!VIDEO_CODEC_H265ENC)
            return false;
      } else if (!VIDEO_CODEC_H265DEC) {
         return false;
      }
   }

   return screen->get_video_param(screen, profile, encode ? PIPE_VIDEO_ENTRYPOINT_ENCODE : PIPE_VIDEO_ENTRYPOINT_BITSTREAM, PIPE_VIDEO_CAP_SUPPORTED);
}
