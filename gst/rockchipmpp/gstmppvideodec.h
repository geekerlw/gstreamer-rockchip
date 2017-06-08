/*
 * Copyright 2017 Rockchip Electronics Co., Ltd
 *     Author: Randy Li <randy.li@rock-chips.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifndef  _GST_MPP_VIDEO_DEC_H_
#define  _GST_MPP_VIDEO_DEC_H_

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideodecoder.h>
#include <gst/video/gstvideopool.h>

#include <rockchip/rk_mpi.h>

/* Begin Declaration */
G_BEGIN_DECLS
#define GST_TYPE_MPP_VIDEO_DEC	(gst_mpp_video_dec_get_type())
#define GST_MPP_VIDEO_DEC(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_MPP_VIDEO_DEC, GstMppVideoDec))
#define GST_MPP_VIDEO_DEC_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_MPP_VIDEO_DEC, GstMppVideoDecClass))
#define GST_IS_MPP_VIDEO_DEC(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_MPP_VIDEO_DEC))
#define GST_IS_MPP_VIDEO_DEC_CLASS(obj) \
	(G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_MPP_VIDEO_DEC))

typedef struct _GstMppVideoDec GstMppVideoDec;
typedef struct _GstMppVideoDecClass GstMppVideoDecClass;

struct _GstMppVideoDec
{
  GstVideoDecoder parent;

  /* add private members here */
  gint width;
  gint height;
  gint framesize;

  GstVideoCodecState *input_state;

  /* the currently format */
  GstVideoInfo info;
  GstVideoAlignment align;

  /* State */
  gboolean active;
  GstFlowReturn output_flow;

  /* Rockchip Mpp definitions */
  MppCtx mpp_ctx;
  MppApi *mpi;

  GstBufferPool *pool;          /* Pool of output frames */
};

struct _GstMppVideoDecClass
{
  GstVideoDecoderClass parent_class;
};

GType gst_mpp_video_dec_get_type (void);

G_END_DECLS
#endif /* _GST_MPP_VIDEO_DEC_H_ */
