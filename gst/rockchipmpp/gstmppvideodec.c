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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "gstmppdecbufferpool.h"
#include "gstmppvideodec.h"

GST_DEBUG_CATEGORY (mpp_video_dec_debug);
#define GST_CAT_DEFAULT mpp_video_dec_debug

#define parent_class gst_mpp_video_dec_parent_class
G_DEFINE_TYPE (GstMppVideoDec, gst_mpp_video_dec, GST_TYPE_VIDEO_DECODER);

#define NB_OUTPUT_BUFS 22       /* nb frames necessary for display pipeline */

/* GstVideoDecoder base class method */
static GstStaticPadTemplate gst_mpp_video_dec_sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264,"
        "stream-format = (string) { byte-stream },"
        "alignment = (string) { au }"
        ";"
        "video/x-h265,"
        "stream-format = (string) { byte-stream },"
        "alignment = (string) { au }"
        ";"
        "video/mpeg,"
        "mpegversion = (int) { 1, 2, 4 },"
        "systemstream = (boolean) false,"
        "parsed = (boolean) true" ";"
        "video/x-vp8" ";" "video/x-vp9" ";" "video/x-h263" ";")
    );

static GstStaticPadTemplate gst_mpp_video_dec_src_template =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, "
        "format = (string) NV12, "
        "width  = (int) [ 32, 4096 ], " "height =  (int) [ 32, 4096 ]"
        ";"
        "video/x-raw, "
        "format = (string) P010_10LEC, "
        "width  = (int) [ 32, 4096 ], " "height =  (int) [ 32, 4096 ]" ";")
    );

static MppCodingType
to_mpp_codec (GstStructure * s)
{
  if (gst_structure_has_name (s, "video/x-h264"))
    return MPP_VIDEO_CodingAVC;

  if (gst_structure_has_name (s, "video/x-h265"))
    return MPP_VIDEO_CodingHEVC;

  if (gst_structure_has_name (s, "video/x-h263"))
    return MPP_VIDEO_CodingH263;

  if (gst_structure_has_name (s, "video/mpeg")) {
    gint mpegversion = 0;
    if (gst_structure_get_int (s, "mpegversion", &mpegversion)) {
      switch (mpegversion) {
        case 2:
          return MPP_VIDEO_CodingMPEG2;
          break;
        case 4:
          return MPP_VIDEO_CodingMPEG4;
          break;
        default:
          break;
      }
    }
  }

  if (gst_structure_has_name (s, "video/x-vp8"))
    return MPP_VIDEO_CodingVP8;
  if (gst_structure_has_name (s, "video/x-vp9"))
    return MPP_VIDEO_CodingVP9;

  /* add more type here */
  return MPP_VIDEO_CodingUnused;
}

static GstVideoFormat
mpp_frame_type_to_gst_video_format (MppFrameFormat fmt)
{
  switch (fmt) {
    case MPP_FMT_YUV420SP:
      return GST_VIDEO_FORMAT_NV12;
      break;
    default:
      return GST_VIDEO_FORMAT_UNKNOWN;
      break;
  }
  return GST_VIDEO_FORMAT_UNKNOWN;
}

static GstVideoInterlaceMode
mpp_frame_mode_to_gst_interlace_mode (RK_U32 mode)
{
  switch (mode) {
    case MPP_FRAME_FLAG_DEINTERLACED:
      return GST_VIDEO_INTERLACE_MODE_MIXED;
    case MPP_FRAME_FLAG_BOT_FIRST:
    case MPP_FRAME_FLAG_TOP_FIRST:
      return GST_VIDEO_INTERLACE_MODE_INTERLEAVED;
    default:
      return GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;
  }
}

static void
gst_mpp_video_dec_unlock (GstMppVideoDec * self)
{
  if (self->pool && gst_buffer_pool_is_active (self->pool))
    gst_buffer_pool_set_flushing (self->pool, TRUE);
}

static void
gst_mpp_video_dec_unlock_stop (GstMppVideoDec * self)
{
  if (self->pool && gst_buffer_pool_is_active (self->pool))
    gst_buffer_pool_set_flushing (self->pool, FALSE);
}

static gboolean
gst_mpp_video_dec_close (GstMppVideoDec * self)
{
  if (self->mpp_ctx != NULL) {
    mpp_destroy (self->mpp_ctx);
    self->mpp_ctx = NULL;
  }

  GST_DEBUG_OBJECT (self, "Rockchip MPP context closed");

  return TRUE;
}

/* Open the device */
static gboolean
gst_mpp_video_dec_start (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Starting");
  gst_mpp_video_dec_unlock (self);
  g_atomic_int_set (&self->active, TRUE);
  self->output_flow = GST_FLOW_OK;

  return TRUE;
}

static gboolean
gst_mpp_video_dec_sendeos (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  MppPacket mpkt;

  mpp_packet_init (&mpkt, NULL, 0);
  mpp_packet_set_eos (mpkt);

  self->mpi->decode_put_packet (self->mpp_ctx, mpkt);
  mpp_packet_deinit (&mpkt);

  return TRUE;
}

static gboolean
gst_mpp_video_to_frame_format (MppFrame mframe, GstVideoInfo * info,
    GstVideoAlignment * align)
{
  gint hor_stride, ver_stride, mv_size;
  GstVideoFormat format;
  GstVideoInterlaceMode mode;

  if (NULL == mframe || NULL == info || NULL == align)
    return FALSE;
  gst_video_info_init (info);
  gst_video_alignment_reset (align);

  format = mpp_frame_type_to_gst_video_format (mpp_frame_get_fmt (mframe));
  if (format == GST_VIDEO_FORMAT_UNKNOWN)
    return FALSE;
  mode = mpp_frame_get_mode (mframe);

  info->finfo = gst_video_format_get_info (format);
  info->width = mpp_frame_get_width (mframe);
  info->height = mpp_frame_get_height (mframe);
  info->interlace_mode = mpp_frame_mode_to_gst_interlace_mode (mode);
  /* FIXME only work for the buffer with only two planes */
  hor_stride = mpp_frame_get_hor_stride (mframe);
  info->stride[0] = hor_stride;
  info->stride[1] = hor_stride;
  ver_stride = mpp_frame_get_ver_stride (mframe);
  info->offset[0] = 0;
  info->offset[1] = info->stride[0] * ver_stride;
  /* FIXME only work for YUV 4:2:0 */
  info->size = (info->stride[0] * ver_stride) * 3 / 2;
  mv_size = info->size * 2 / 6;
  info->size += mv_size;

  align->padding_right = hor_stride - info->width;
  align->padding_bottom = ver_stride - info->height;

  return TRUE;
}

static gboolean
gst_mpp_video_acquire_frame_format (GstMppVideoDec * self)
{
  MPP_RET mret;
  MppFrame mframe = NULL;
  mret = self->mpi->decode_get_frame (self->mpp_ctx, &mframe);
  if (mret || NULL == mframe) {
	GST_ERROR_OBJECT (self, "can't get valid info %d", mret);
	return FALSE;
  }

  return gst_mpp_video_to_frame_format (mframe, &self->info, &self->align);
}

static gboolean
gst_mpp_video_dec_finish (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  GstFlowReturn ret = GST_FLOW_OK;

  if (gst_pad_get_task_state (decoder->srcpad) != GST_TASK_STARTED)
    goto done;

  GST_DEBUG_OBJECT (self, "Finishing decoding");

  GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
  if (gst_mpp_video_dec_sendeos (decoder)) {
    /* Wait for mpp output thread to stop */
    GstTask *task = decoder->srcpad->task;
    GST_OBJECT_LOCK (task);
    while (GST_TASK_STATE (task) == GST_TASK_STARTED)
      GST_TASK_WAIT (task);
    GST_OBJECT_UNLOCK (task);
    ret = GST_FLOW_FLUSHING;
  }
  GST_VIDEO_DECODER_STREAM_LOCK (decoder);

  if (ret == GST_FLOW_FLUSHING)
    ret = self->output_flow;

  GST_DEBUG_OBJECT (decoder, "Done draining buffers");

done:
  return ret;
}

static gboolean
gst_mpp_video_dec_stop (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Stopping");

  /* Kill mpp output thread to stop */
  gst_mpp_video_dec_unlock (self);
  gst_mpp_video_dec_sendeos (decoder);
  gst_pad_stop_task (decoder->srcpad);

  GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  self->output_flow = GST_FLOW_OK;
  GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);

  /* Should have been flushed already */
  g_assert (g_atomic_int_get (&self->active) == FALSE);

  /* Release all the internal references of the buffer */
  self->mpi->reset (self->mpp_ctx);
  if (self->pool) {
    gst_object_unref (self->pool);
    self->pool = NULL;
  }
  gst_mpp_video_dec_close (self);

  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);

  GST_DEBUG_OBJECT (self, "Stopped");

  return TRUE;
}

static gboolean
gst_mpp_video_dec_open (GstMppVideoDec * self, MppCodingType codec_format)
{
  mpp_create (&self->mpp_ctx, &self->mpi);

  GST_DEBUG_OBJECT (self, "created mpp context %p", self->mpp_ctx);

  if (mpp_init (self->mpp_ctx, MPP_CTX_DEC, codec_format))
    goto mpp_init_error;

  return TRUE;

mpp_init_error:
  {
    GST_ERROR_OBJECT (self, "rockchip context init failed");
    if (!self->mpp_ctx)
      mpp_destroy (self->mpp_ctx);
    return FALSE;
  }
}

static gboolean
gst_mpp_video_dec_flush (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  /* Ensure the processing thread has stopped for the reverse playback
   * discount case */
  if (gst_pad_get_task_state (decoder->srcpad) == GST_TASK_STARTED) {
    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    gst_mpp_video_dec_unlock (self);
    gst_pad_stop_task (decoder->srcpad);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  }
  self->output_flow = GST_FLOW_OK;

  gst_mpp_video_dec_unlock_stop (self);
  return !self->mpi->reset (self->mpp_ctx);
}

static gboolean
gst_mpp_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  GstStructure *structure;

  GST_DEBUG_OBJECT (self, "Setting format: %" GST_PTR_FORMAT, state->caps);

  structure = gst_caps_get_structure (state->caps, 0);

  if (self->input_state) {
    GstQuery *query = gst_query_new_drain ();

    if (gst_caps_is_strictly_equal (self->input_state->caps, state->caps))
      goto done;

    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;

    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    if (gst_mpp_video_dec_sendeos (decoder)) {
      /* Wait for mpp output thread to stop */
      GstTask *task = decoder->srcpad->task;
      if (task != NULL) {
        GST_OBJECT_LOCK (task);
        while (GST_TASK_STATE (task) == GST_TASK_STARTED)
          GST_TASK_WAIT (task);
        GST_OBJECT_UNLOCK (task);
      }
    }
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);
    /* Query the downstream to release buffers from buffer pool */
    if (!gst_pad_peer_query (GST_VIDEO_DECODER_SRC_PAD (self), query))
      GST_DEBUG_OBJECT (self, "drain query failed");
    gst_query_unref (query);

    gst_pad_stop_task (decoder->srcpad);

    if (self->pool) {
      self->mpi->reset (self->mpp_ctx);
      gst_object_unref (self->pool);
      self->pool = NULL;
    }

    self->output_flow = GST_FLOW_OK;
  } else {
    MppCodingType codingtype;
    codingtype = to_mpp_codec (structure);
    if (MPP_VIDEO_CodingUnused == codingtype)
      goto format_error;

    if (!gst_mpp_video_dec_open (self, codingtype))
      goto device_error;
  }

  self->input_state = gst_video_codec_state_ref (state);

done:
  return TRUE;

  /* Errors */
format_error:
  {
    GST_ERROR_OBJECT (self, "Unsupported format in caps: %" GST_PTR_FORMAT,
        state->caps);
    return FALSE;
  }
device_error:
  {
    GST_ERROR_OBJECT (self, "Failed to open the device");
    return FALSE;
  }
}

static void
gst_mpp_video_dec_loop (GstVideoDecoder * decoder)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  GstBuffer *buffer;
  GstVideoCodecFrame *frame;
  GstFlowReturn ret = GST_FLOW_OK;

  ret = gst_buffer_pool_acquire_buffer (self->pool, &buffer, NULL);
  if (ret != GST_FLOW_OK && ret != GST_FLOW_CUSTOM_ERROR_1)
    goto beach;

  frame = gst_video_decoder_get_oldest_frame (decoder);
  if (frame) {
    if (ret == GST_FLOW_CUSTOM_ERROR_1) {
      gst_video_decoder_drop_frame (decoder, frame);
      return;
    }
    frame->output_buffer = buffer;

    buffer = NULL;
    ret = gst_video_decoder_finish_frame (decoder, frame);

    GST_TRACE_OBJECT (self, "finish buffer ts=%" GST_TIME_FORMAT,
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (frame->output_buffer)));

    if (ret != GST_FLOW_OK)
      goto beach;
  } else {
    GST_WARNING_OBJECT (self, "Decoder is producing too many buffers");
    gst_buffer_unref (buffer);
  }

  return;

beach:
  GST_DEBUG_OBJECT (self, "Leaving output thread: %s", gst_flow_get_name (ret));

  gst_buffer_replace (&buffer, NULL);
  self->output_flow = ret;
  gst_pad_pause_task (decoder->srcpad);
}

static GstFlowReturn
gst_mpp_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  GstBufferPool *pool = NULL;
  GstMapInfo mapinfo = { 0, };
  GstFlowReturn ret = GST_FLOW_OK;
  MppPacket mpkt = NULL;
  gboolean processed = FALSE;
  MPP_RET mret = 0;

  GST_DEBUG_OBJECT (self, "Handling frame %d", frame->system_frame_number);

  if (G_UNLIKELY (!g_atomic_int_get (&self->active)))
    goto flushing;

  if (self->pool == NULL) {
    if (!self->input_state)
      goto not_negotiated;

    self->pool = gst_mpp_dec_buffer_pool_new (self, NULL);
    if (!self->pool)
      goto not_negotiated;
  }

  pool = GST_BUFFER_POOL (self->pool);
  if (!gst_buffer_pool_is_active (pool)) {
    GstBuffer *codec_data;
    gint block_flag = MPP_POLL_BLOCK;

    codec_data = self->input_state->codec_data;
    if (codec_data) {
      gst_buffer_ref (codec_data);
    } else {
      codec_data = gst_buffer_ref (frame->input_buffer);
      processed = TRUE;
    }

    gst_buffer_map (codec_data, &mapinfo, GST_MAP_READ);
    mpp_packet_init (&mpkt, mapinfo.data, mapinfo.size);
    /* For those DMA buffers, the mapping would be destroy at once */
    gst_buffer_unmap (codec_data, &mapinfo);

    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    do {
      mret = self->mpi->decode_put_packet (self->mpp_ctx, mpkt);
    } while (MPP_ERR_BUFFER_FULL == mret);
    if (mret != 0)
      goto send_stream_error;

    mpp_packet_deinit (&mpkt);
    gst_buffer_unref (codec_data);

    self->mpi->control (self->mpp_ctx, MPP_SET_OUTPUT_BLOCK,
        (gpointer) & block_flag);

	if (gst_mpp_video_acquire_frame_format (self)) {
      GstVideoCodecState *output_state;
      GstVideoInfo *info = &self->info;
      GstStructure *config = gst_buffer_pool_get_config (pool);

      output_state =
          gst_video_decoder_set_output_state (decoder,
          info->finfo->format, info->width, info->height, self->input_state);
      output_state->info.interlace_mode = info->interlace_mode;
      gst_video_codec_state_unref (output_state);

      /* NOTE: if you suffer from the reconstruction of buffer pool which slows
       * down the decoding, then don't allocate more than 10 buffers here */
      gst_buffer_pool_config_set_params (config, output_state->caps,
          self->info.size, NB_OUTPUT_BUFS, NB_OUTPUT_BUFS);
      gst_buffer_pool_config_set_video_alignment (config, &self->align);

      if (!gst_buffer_pool_set_config (pool, config))
        goto error_activate_pool;
      /* activate the pool: the buffers are allocated */
      if (gst_buffer_pool_set_active (self->pool, TRUE) == FALSE)
        goto error_activate_pool;

      self->mpi->control (self->mpp_ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
    } else {
      goto not_negotiated;
    }
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  }

  /* Start the output thread if it is not started before */
  if (gst_pad_get_task_state (GST_VIDEO_DECODER_SRC_PAD (self)) ==
      GST_TASK_STOPPED) {
    /* It's possible that the processing thread stopped due to an error */
    if (self->output_flow != GST_FLOW_OK &&
        self->output_flow != GST_FLOW_FLUSHING) {
      GST_DEBUG_OBJECT (self, "Processing loop stopped with error, leaving");
      ret = self->output_flow;
      goto drop;
    }

    GST_DEBUG_OBJECT (self, "Starting decoding thread");

    self->output_flow = GST_FLOW_FLUSHING;
    if (!gst_pad_start_task (decoder->srcpad,
            (GstTaskFunction) gst_mpp_video_dec_loop, self, NULL))
      goto start_task_failed;
  }

  if (!processed) {
    gst_buffer_map (frame->input_buffer, &mapinfo, GST_MAP_READ);
    mpp_packet_init (&mpkt, mapinfo.data, mapinfo.size);
    gst_buffer_unmap (frame->input_buffer, &mapinfo);

    GST_VIDEO_DECODER_STREAM_UNLOCK (decoder);
    do {
      mret = self->mpi->decode_put_packet (self->mpp_ctx, mpkt);
    } while (MPP_ERR_BUFFER_FULL == mret);
    if (mret != 0)
      goto send_stream_error;

    mpp_packet_deinit (&mpkt);
    /* No need to keep input arround */
    gst_buffer_replace (&frame->input_buffer, NULL);
    GST_VIDEO_DECODER_STREAM_LOCK (decoder);
  }

  gst_video_codec_frame_unref (frame);

  return ret;

  /* ERRORS */
error_activate_pool:
  {
    GST_ERROR_OBJECT (self, "Unable to activate the pool");
    ret = GST_FLOW_ERROR;
    goto drop;
  }
flushing:
  {
    ret = GST_FLOW_FLUSHING;
    goto drop;
  }
start_task_failed:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED,
        ("Failed to start decoding thread."), (NULL));
    ret = GST_FLOW_ERROR;
    goto drop;
  }
not_negotiated:
  {
    GST_ERROR_OBJECT (self, "not negotiated");
    gst_video_decoder_drop_frame (decoder, frame);
    return GST_FLOW_NOT_NEGOTIATED;
  }
send_stream_error:
  {
    GST_ERROR_OBJECT (self, "send packet failed %d", mret);
    mpp_packet_deinit (&mpkt);
    return GST_FLOW_ERROR;
  }
drop:
  {
    gst_video_decoder_drop_frame (decoder, frame);
    return ret;
  }
}

static gboolean
gst_mpp_video_dec_sink_event (GstVideoDecoder * decoder, GstEvent * event)
{
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);
  gboolean ret;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      GST_DEBUG_OBJECT (self, "flush start");
      gst_mpp_video_dec_unlock (self);
      break;
    default:
      break;
  }

  ret = GST_VIDEO_DECODER_CLASS (parent_class)->sink_event (decoder, event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
      gst_pad_stop_task (decoder->srcpad);
      GST_DEBUG_OBJECT (self, "flush done");
      break;
    default:
      break;
  }

  return ret;
}

static GstStateChangeReturn
gst_mpp_video_dec_change_state (GstElement * element, GstStateChange transition)
{
  GstVideoDecoder *decoder = GST_VIDEO_DECODER (element);
  GstMppVideoDec *self = GST_MPP_VIDEO_DEC (decoder);

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    g_atomic_int_set (&self->active, FALSE);
    gst_mpp_video_dec_sendeos (decoder);
    gst_mpp_video_dec_unlock (self);
    gst_pad_stop_task (decoder->srcpad);
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_mpp_video_dec_class_init (GstMppVideoDecClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *video_decoder_class = GST_VIDEO_DECODER_CLASS (klass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mpp_video_dec_src_template));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mpp_video_dec_sink_template));

  video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_start);
  video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_stop);
  video_decoder_class->finish = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_finish);
  video_decoder_class->set_format = GST_DEBUG_FUNCPTR
      (gst_mpp_video_dec_set_format);
  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_mpp_video_dec_handle_frame);
  video_decoder_class->flush = GST_DEBUG_FUNCPTR (gst_mpp_video_dec_flush);
  video_decoder_class->sink_event =
      GST_DEBUG_FUNCPTR (gst_mpp_video_dec_sink_event);

  element_class->change_state = GST_DEBUG_FUNCPTR
      (gst_mpp_video_dec_change_state);

  GST_DEBUG_CATEGORY_INIT (mpp_video_dec_debug, "mppvideodec", 0,
      "mpp video decoder");

  gst_element_class_set_static_metadata (element_class,
      "Rockchip's MPP video decoder", "Decoder/Video",
      "Multicodec (MPEG-2/4 / AVC / VP8 / HEVC) hardware decoder",
      "Randy Li <randy.li@rock-chips.com>");
}

static void
gst_mpp_video_dec_init (GstMppVideoDec * self)
{
  GstVideoDecoder *decoder = (GstVideoDecoder *) self;

  gst_video_decoder_set_packetized (decoder, TRUE);

  self->active = FALSE;

  self->input_state = NULL;
}
