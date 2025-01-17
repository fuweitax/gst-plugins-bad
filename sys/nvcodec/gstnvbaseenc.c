/* GStreamer NVENC plugin
 * Copyright (C) 2015 Centricular Ltd
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
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstnvbaseenc.h"
#include "gstcudautils.h"

#include <gst/pbutils/codec-utils.h>

#include <string.h>

#define GST_CAT_DEFAULT gst_nvenc_debug

#if HAVE_NVCODEC_GST_GL
#include <gst/gl/gl.h>
#endif

/* TODO:
 *  - reset last_flow on FLUSH_STOP (seeking)
 */

/* This currently supports both 5.x and 6.x versions of the NvEncodeAPI.h
 * header which are mostly API compatible. */

#define N_BUFFERS_PER_FRAME 1
#define SUPPORTED_GL_APIS GST_GL_API_OPENGL3

/* magic pointer value we can put in the async queue to signal shut down */
#define SHUTDOWN_COOKIE ((gpointer)GINT_TO_POINTER (1))

#define parent_class gst_nv_base_enc_parent_class
G_DEFINE_ABSTRACT_TYPE (GstNvBaseEnc, gst_nv_base_enc, GST_TYPE_VIDEO_ENCODER);

#define GST_TYPE_NV_PRESET (gst_nv_preset_get_type())
static GType
gst_nv_preset_get_type (void)
{
  static GType nv_preset_type = 0;

  static const GEnumValue presets[] = {
    {GST_NV_PRESET_DEFAULT, "Default", "default"},
    {GST_NV_PRESET_HP, "High Performance", "hp"},
    {GST_NV_PRESET_HQ, "High Quality", "hq"},
/*    {GST_NV_PRESET_BD, "BD", "bd"}, */
    {GST_NV_PRESET_LOW_LATENCY_DEFAULT, "Low Latency", "low-latency"},
    {GST_NV_PRESET_LOW_LATENCY_HQ, "Low Latency, High Quality",
        "low-latency-hq"},
    {GST_NV_PRESET_LOW_LATENCY_HP, "Low Latency, High Performance",
        "low-latency-hp"},
    {GST_NV_PRESET_LOSSLESS_DEFAULT, "Lossless", "lossless"},
    {GST_NV_PRESET_LOSSLESS_HP, "Lossless, High Performance", "lossless-hp"},
    {0, NULL, NULL},
  };

  if (!nv_preset_type) {
    nv_preset_type = g_enum_register_static ("GstNvPreset", presets);
  }
  return nv_preset_type;
}

static GUID
_nv_preset_to_guid (GstNvPreset preset)
{
  GUID null = { 0, };

  switch (preset) {
#define CASE(gst,nv) case G_PASTE(GST_NV_PRESET_,gst): return G_PASTE(G_PASTE(NV_ENC_PRESET_,nv),_GUID)
      CASE (DEFAULT, DEFAULT);
      CASE (HP, HP);
      CASE (HQ, HQ);
/*    CASE (BD, BD);*/
      CASE (LOW_LATENCY_DEFAULT, LOW_LATENCY_DEFAULT);
      CASE (LOW_LATENCY_HQ, LOW_LATENCY_HQ);
      CASE (LOW_LATENCY_HP, LOW_LATENCY_HQ);
      CASE (LOSSLESS_DEFAULT, LOSSLESS_DEFAULT);
      CASE (LOSSLESS_HP, LOSSLESS_HP);
#undef CASE
    default:
      return null;
  }
}

#define GST_TYPE_NV_RC_MODE (gst_nv_rc_mode_get_type())
static GType
gst_nv_rc_mode_get_type (void)
{
  static GType nv_rc_mode_type = 0;

  static const GEnumValue modes[] = {
    {GST_NV_RC_MODE_DEFAULT, "Default (from NVENC preset)", "default"},
    {GST_NV_RC_MODE_CONSTQP, "Constant Quantization", "constqp"},
    {GST_NV_RC_MODE_CBR, "Constant Bit Rate", "cbr"},
    {GST_NV_RC_MODE_VBR, "Variable Bit Rate", "vbr"},
    {GST_NV_RC_MODE_VBR_MINQP,
          "Variable Bit Rate (with minimum quantization parameter)",
        "vbr-minqp"},
    {0, NULL, NULL},
  };

  if (!nv_rc_mode_type) {
    nv_rc_mode_type = g_enum_register_static ("GstNvRCMode", modes);
  }
  return nv_rc_mode_type;
}

static NV_ENC_PARAMS_RC_MODE
_rc_mode_to_nv (GstNvRCMode mode)
{
  switch (mode) {
    case GST_NV_RC_MODE_DEFAULT:
      return -1;
#define CASE(gst,nv) case G_PASTE(GST_NV_RC_MODE_,gst): return G_PASTE(NV_ENC_PARAMS_RC_,nv)
      CASE (CONSTQP, CONSTQP);
      CASE (CBR, CBR);
      CASE (VBR, VBR);
      CASE (VBR_MINQP, VBR_MINQP);
#undef CASE
    default:
      return -1;
  }
}

enum
{
  PROP_0,
  PROP_DEVICE_ID,
  PROP_PRESET,
  PROP_BITRATE,
  PROP_RC_MODE,
  PROP_QP_MIN,
  PROP_QP_MAX,
  PROP_QP_CONST,
  PROP_GOP_SIZE,
};

#define DEFAULT_PRESET GST_NV_PRESET_DEFAULT
#define DEFAULT_BITRATE 0
#define DEFAULT_RC_MODE GST_NV_RC_MODE_DEFAULT
#define DEFAULT_QP_MIN -1
#define DEFAULT_QP_MAX -1
#define DEFAULT_QP_CONST -1
#define DEFAULT_GOP_SIZE 75

/* This lock is needed to prevent the situation where multiple encoders are
 * initialised at the same time which appears to cause excessive CPU usage over
 * some period of time. */
G_LOCK_DEFINE_STATIC (initialization_lock);

#if HAVE_NVCODEC_GST_GL
struct gl_input_resource
{
  GstGLMemory *gl_mem[GST_VIDEO_MAX_PLANES];
  CUgraphicsResource cuda_texture;
  CUdeviceptr cuda_plane_pointers[GST_VIDEO_MAX_PLANES];
  gpointer cuda_pointer;
  gsize cuda_stride;
  gsize cuda_num_bytes;
  NV_ENC_REGISTER_RESOURCE nv_resource;
  NV_ENC_MAP_INPUT_RESOURCE nv_mapped_resource;

  /* whether nv_mapped_resource was mapped via NvEncMapInputResource()
   * and therefore should unmap via NvEncUnmapInputResource or not */
  gboolean mapped;
};
#endif

struct frame_state
{
  gint n_buffers;
  gpointer in_bufs[N_BUFFERS_PER_FRAME];
  gpointer out_bufs[N_BUFFERS_PER_FRAME];
};

static gboolean gst_nv_base_enc_open (GstVideoEncoder * enc);
static gboolean gst_nv_base_enc_close (GstVideoEncoder * enc);
static gboolean gst_nv_base_enc_start (GstVideoEncoder * enc);
static gboolean gst_nv_base_enc_stop (GstVideoEncoder * enc);
static void gst_nv_base_enc_set_context (GstElement * element,
    GstContext * context);
static gboolean gst_nv_base_enc_sink_query (GstVideoEncoder * enc,
    GstQuery * query);
static gboolean gst_nv_base_enc_set_format (GstVideoEncoder * enc,
    GstVideoCodecState * state);
static GstFlowReturn gst_nv_base_enc_handle_frame (GstVideoEncoder * enc,
    GstVideoCodecFrame * frame);
static void gst_nv_base_enc_free_buffers (GstNvBaseEnc * nvenc);
static GstFlowReturn gst_nv_base_enc_finish (GstVideoEncoder * enc);
static void gst_nv_base_enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_nv_base_enc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void gst_nv_base_enc_finalize (GObject * obj);
static GstCaps *gst_nv_base_enc_getcaps (GstVideoEncoder * enc,
    GstCaps * filter);
static gboolean gst_nv_base_enc_stop_bitstream_thread (GstNvBaseEnc * nvenc,
    gboolean force);
static gboolean gst_nv_base_enc_drain_encoder (GstNvBaseEnc * nvenc);

static void
gst_nv_base_enc_class_init (GstNvBaseEncClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoEncoderClass *videoenc_class = GST_VIDEO_ENCODER_CLASS (klass);

  gobject_class->set_property = gst_nv_base_enc_set_property;
  gobject_class->get_property = gst_nv_base_enc_get_property;
  gobject_class->finalize = gst_nv_base_enc_finalize;

  element_class->set_context = GST_DEBUG_FUNCPTR (gst_nv_base_enc_set_context);

  videoenc_class->open = GST_DEBUG_FUNCPTR (gst_nv_base_enc_open);
  videoenc_class->close = GST_DEBUG_FUNCPTR (gst_nv_base_enc_close);

  videoenc_class->start = GST_DEBUG_FUNCPTR (gst_nv_base_enc_start);
  videoenc_class->stop = GST_DEBUG_FUNCPTR (gst_nv_base_enc_stop);

  videoenc_class->set_format = GST_DEBUG_FUNCPTR (gst_nv_base_enc_set_format);
  videoenc_class->getcaps = GST_DEBUG_FUNCPTR (gst_nv_base_enc_getcaps);
  videoenc_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_nv_base_enc_handle_frame);
  videoenc_class->finish = GST_DEBUG_FUNCPTR (gst_nv_base_enc_finish);
  videoenc_class->sink_query = GST_DEBUG_FUNCPTR (gst_nv_base_enc_sink_query);

  g_object_class_install_property (gobject_class, PROP_DEVICE_ID,
      g_param_spec_uint ("cuda-device-id",
          "Cuda Device ID",
          "Get the GPU device to use for operations",
          0, G_MAXUINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_PRESET,
      g_param_spec_enum ("preset", "Encoding Preset",
          "Encoding Preset",
          GST_TYPE_NV_PRESET, DEFAULT_PRESET,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_RC_MODE,
      g_param_spec_enum ("rc-mode", "RC Mode", "Rate Control Mode",
          GST_TYPE_NV_RC_MODE, DEFAULT_RC_MODE,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_QP_MIN,
      g_param_spec_int ("qp-min", "Minimum Quantizer",
          "Minimum quantizer (-1 = from NVENC preset)", -1, 51, DEFAULT_QP_MIN,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_QP_MAX,
      g_param_spec_int ("qp-max", "Maximum Quantizer",
          "Maximum quantizer (-1 = from NVENC preset)", -1, 51, DEFAULT_QP_MAX,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_QP_CONST,
      g_param_spec_int ("qp-const", "Constant Quantizer",
          "Constant quantizer (-1 = from NVENC preset)", -1, 51,
          DEFAULT_QP_CONST,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (gobject_class, PROP_GOP_SIZE,
      g_param_spec_int ("gop-size", "GOP size",
          "Number of frames between intra frames (-1 = infinite)",
          -1, G_MAXINT, DEFAULT_GOP_SIZE,
          (GParamFlags) (G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
              G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_BITRATE,
      g_param_spec_uint ("bitrate", "Bitrate",
          "Bitrate in kbit/sec (0 = from NVENC preset)", 0, 2000 * 1024,
          DEFAULT_BITRATE,
          G_PARAM_READWRITE | GST_PARAM_MUTABLE_PLAYING |
          G_PARAM_STATIC_STRINGS));
}

static gboolean
gst_nv_base_enc_open_encode_session (GstNvBaseEnc * nvenc)
{
  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = { 0, };
  NVENCSTATUS nv_ret;

  params.version = gst_nvenc_get_open_encode_session_ex_params_version ();
  params.apiVersion = gst_nvenc_get_api_version ();
  params.device = gst_cuda_context_get_handle (nvenc->cuda_ctx);
  params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
  nv_ret = NvEncOpenEncodeSessionEx (&params, &nvenc->encoder);

  return nv_ret == NV_ENC_SUCCESS;
}

static gboolean
gst_nv_base_enc_open (GstVideoEncoder * enc)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);
  GstNvBaseEncClass *klass = GST_NV_BASE_ENC_GET_CLASS (enc);
  GValue *formats = NULL;
  CUresult cuda_ret;

  if (!gst_cuda_ensure_element_context (GST_ELEMENT_CAST (enc),
          klass->cuda_device_id, &nvenc->cuda_ctx)) {
    GST_ERROR_OBJECT (nvenc, "failed to create CUDA context");
    return FALSE;
  }

  if (gst_cuda_context_push (nvenc->cuda_ctx)) {
    cuda_ret = CuStreamCreate (&nvenc->cuda_stream, CU_STREAM_DEFAULT);
    if (!gst_cuda_result (cuda_ret)) {
      GST_WARNING_OBJECT (nvenc,
          "Could not create cuda stream, will use default stream");
      nvenc->cuda_stream = NULL;
    }
    gst_cuda_context_pop (NULL);
  }

  if (!gst_nv_base_enc_open_encode_session (nvenc)) {
    GST_ERROR ("Failed to create NVENC encoder session");
    gst_clear_object (&nvenc->cuda_ctx);
    return FALSE;
  }

  GST_INFO ("created NVENC encoder %p", nvenc->encoder);

  /* query supported input formats */
  if (!gst_nvenc_get_supported_input_formats (nvenc->encoder, klass->codec_id,
          &formats)) {
    GST_WARNING_OBJECT (nvenc, "No supported input formats");
    gst_nv_base_enc_close (enc);
    return FALSE;
  }

  nvenc->input_formats = formats;

  return TRUE;
}

static void
gst_nv_base_enc_set_context (GstElement * element, GstContext * context)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (element);
  GstNvBaseEncClass *klass = GST_NV_BASE_ENC_GET_CLASS (nvenc);

  if (gst_cuda_handle_set_context (element, context, klass->cuda_device_id,
          &nvenc->cuda_ctx)) {
    goto done;
  }
#if HAVE_NVCODEC_GST_GL
  gst_gl_handle_set_context (element, context,
      (GstGLDisplay **) & nvenc->display,
      (GstGLContext **) & nvenc->other_context);
  if (nvenc->display)
    gst_gl_display_filter_gl_api (GST_GL_DISPLAY (nvenc->display),
        SUPPORTED_GL_APIS);
#endif

done:
  GST_ELEMENT_CLASS (parent_class)->set_context (element, context);
}

static gboolean
gst_nv_base_enc_sink_query (GstVideoEncoder * enc, GstQuery * query)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CONTEXT:{
      if (gst_cuda_handle_context_query (GST_ELEMENT (nvenc),
              query, nvenc->cuda_ctx))
        return TRUE;

#if HAVE_NVCODEC_GST_GL
      {
        gboolean ret;

        ret = gst_gl_handle_context_query ((GstElement *) nvenc, query,
            nvenc->display, NULL, nvenc->other_context);
        if (nvenc->display) {
          gst_gl_display_filter_gl_api (GST_GL_DISPLAY (nvenc->display),
              SUPPORTED_GL_APIS);
        }

        if (ret)
          return ret;
      }
#endif
      break;
    }
    default:
      break;
  }

  return GST_VIDEO_ENCODER_CLASS (parent_class)->sink_query (enc, query);
}

static gboolean
gst_nv_base_enc_start (GstVideoEncoder * enc)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);

  nvenc->bitstream_pool = g_async_queue_new ();
  nvenc->bitstream_queue = g_async_queue_new ();
  nvenc->in_bufs_pool = g_async_queue_new ();

  nvenc->last_flow = GST_FLOW_OK;
  memset (&nvenc->init_params, 0, sizeof (NV_ENC_INITIALIZE_PARAMS));
  memset (&nvenc->config, 0, sizeof (NV_ENC_CONFIG));

#if HAVE_NVCODEC_GST_GL
  {
    gst_gl_ensure_element_data (GST_ELEMENT (nvenc),
        (GstGLDisplay **) & nvenc->display,
        (GstGLContext **) & nvenc->other_context);
    if (nvenc->display)
      gst_gl_display_filter_gl_api (GST_GL_DISPLAY (nvenc->display),
          SUPPORTED_GL_APIS);
  }
#endif

  return TRUE;
}

static gboolean
gst_nv_base_enc_stop (GstVideoEncoder * enc)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);

  gst_nv_base_enc_stop_bitstream_thread (nvenc, TRUE);

  gst_nv_base_enc_free_buffers (nvenc);

  if (nvenc->input_state) {
    gst_video_codec_state_unref (nvenc->input_state);
    nvenc->input_state = NULL;
  }

  if (nvenc->bitstream_pool) {
    g_async_queue_unref (nvenc->bitstream_pool);
    nvenc->bitstream_pool = NULL;
  }
  if (nvenc->bitstream_queue) {
    g_async_queue_unref (nvenc->bitstream_queue);
    nvenc->bitstream_queue = NULL;
  }
  if (nvenc->in_bufs_pool) {
    g_async_queue_unref (nvenc->in_bufs_pool);
    nvenc->in_bufs_pool = NULL;
  }
  if (nvenc->display) {
    gst_object_unref (nvenc->display);
    nvenc->display = NULL;
  }
  if (nvenc->other_context) {
    gst_object_unref (nvenc->other_context);
    nvenc->other_context = NULL;
  }

  return TRUE;
}

static void
check_formats (const gchar * str, guint * max_chroma, guint * max_bit_minus8)
{
  if (!str)
    return;

  if (g_strrstr (str, "-444") || g_strrstr (str, "-4:4:4"))
    *max_chroma = 2;
  else if ((g_strrstr (str, "-4:2:2") || g_strrstr (str, "-422"))
      && *max_chroma < 1)
    *max_chroma = 1;

  if (g_strrstr (str, "-12"))
    *max_bit_minus8 = 4;
  else if (g_strrstr (str, "-10") && *max_bit_minus8 < 2)
    *max_bit_minus8 = 2;
}

static gboolean
gst_nv_base_enc_set_filtered_input_formats (GstNvBaseEnc * nvenc,
    GstCaps * caps, const GValue * input_formats, guint max_chroma,
    guint max_bit_minus8)
{
  gint i;
  GValue supported_format = G_VALUE_INIT;
  gint num_format = 0;
  const GValue *last_format = NULL;

  g_value_init (&supported_format, GST_TYPE_LIST);

  for (i = 0; i < gst_value_list_get_size (input_formats); i++) {
    const GValue *val;
    GstVideoFormat format;

    val = gst_value_list_get_value (input_formats, i);
    format = gst_video_format_from_string (g_value_get_string (val));

    switch (format) {
      case GST_VIDEO_FORMAT_NV12:
      case GST_VIDEO_FORMAT_YV12:
      case GST_VIDEO_FORMAT_I420:
        /* 8bits 4:2:0 formats are always supported */
      case GST_VIDEO_FORMAT_BGRA:
      case GST_VIDEO_FORMAT_RGBA:
        /* NOTE: RGB formats seems to also supported format, which are
         * encoded to 4:2:0 formats */
        gst_value_list_append_value (&supported_format, val);
        last_format = val;
        num_format++;
        break;
      case GST_VIDEO_FORMAT_Y444:
        if (max_chroma >= 2) {
          gst_value_list_append_value (&supported_format, val);
          last_format = val;
          num_format++;
        }
        break;
      case GST_VIDEO_FORMAT_P010_10LE:
      case GST_VIDEO_FORMAT_P010_10BE:
      case GST_VIDEO_FORMAT_BGR10A2_LE:
      case GST_VIDEO_FORMAT_RGB10A2_LE:
      case GST_VIDEO_FORMAT_Y444_16LE:
      case GST_VIDEO_FORMAT_Y444_16BE:
        if (max_bit_minus8 >= 2) {
          gst_value_list_append_value (&supported_format, val);
          last_format = val;
          num_format++;
        }
        break;
      default:
        break;
    }
  }

  if (num_format == 0) {
    g_value_unset (&supported_format);
    GST_WARNING_OBJECT (nvenc, "Cannot find matching input format");
    return FALSE;
  }

  if (num_format > 1)
    gst_caps_set_value (caps, "format", &supported_format);
  else
    gst_caps_set_value (caps, "format", last_format);

  g_value_unset (&supported_format);

  return TRUE;
}

static GstCaps *
gst_nv_base_enc_getcaps (GstVideoEncoder * enc, GstCaps * filter)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);
  GstNvBaseEncClass *klass = GST_NV_BASE_ENC_GET_CLASS (enc);
  GstCaps *supported_incaps = NULL;
  GstCaps *template_caps, *caps, *allowed;

  template_caps = gst_pad_get_pad_template_caps (enc->sinkpad);
  allowed = gst_pad_get_allowed_caps (enc->srcpad);

  GST_LOG_OBJECT (enc, "template caps %" GST_PTR_FORMAT, template_caps);
  GST_LOG_OBJECT (enc, "allowed caps %" GST_PTR_FORMAT, allowed);

  if (!allowed) {
    /* no peer */
    supported_incaps = template_caps;
    template_caps = NULL;
    goto done;
  } else if (gst_caps_is_empty (allowed)) {
    /* couldn't be negotiated, just return empty caps */
    gst_caps_unref (template_caps);
    return allowed;
  }

  GST_OBJECT_LOCK (nvenc);

  if (nvenc->input_formats != NULL) {
    GValue *val;
    gboolean has_profile = FALSE;
    guint max_chroma_index = 0;
    guint max_bit_minus8 = 0;
    gint i, j;

    for (i = 0; i < gst_caps_get_size (allowed); i++) {
      const GstStructure *allowed_s = gst_caps_get_structure (allowed, i);
      const GValue *val;

      if ((val = gst_structure_get_value (allowed_s, "profile"))) {
        if (G_VALUE_HOLDS_STRING (val)) {
          check_formats (g_value_get_string (val), &max_chroma_index,
              &max_bit_minus8);
          has_profile = TRUE;
        } else if (GST_VALUE_HOLDS_LIST (val)) {
          for (j = 0; j < gst_value_list_get_size (val); j++) {
            const GValue *vlist = gst_value_list_get_value (val, j);

            if (G_VALUE_HOLDS_STRING (vlist)) {
              check_formats (g_value_get_string (vlist), &max_chroma_index,
                  &max_bit_minus8);
              has_profile = TRUE;
            }
          }
        }
      }
    }

    GST_LOG_OBJECT (enc,
        "downstream requested profile %d, max bitdepth %d, max chroma %d",
        has_profile, max_bit_minus8 + 8, max_chroma_index);

    supported_incaps = gst_caps_copy (template_caps);
    if (!has_profile ||
        !gst_nv_base_enc_set_filtered_input_formats (nvenc, supported_incaps,
            nvenc->input_formats, max_chroma_index, max_bit_minus8)) {
      gst_caps_set_value (supported_incaps, "format", nvenc->input_formats);
    }

    val = gst_nvenc_get_interlace_modes (nvenc->encoder, klass->codec_id);
    gst_caps_set_value (supported_incaps, "interlace-mode", val);
    g_value_unset (val);
    g_free (val);

    GST_LOG_OBJECT (enc, "codec input caps %" GST_PTR_FORMAT, supported_incaps);
    GST_LOG_OBJECT (enc, "   template caps %" GST_PTR_FORMAT, template_caps);
    caps = gst_caps_intersect (template_caps, supported_incaps);
    gst_caps_unref (supported_incaps);
    supported_incaps = caps;
    GST_LOG_OBJECT (enc, "  supported caps %" GST_PTR_FORMAT, supported_incaps);
  }

  GST_OBJECT_UNLOCK (nvenc);

done:
  caps = gst_video_encoder_proxy_getcaps (enc, supported_incaps, filter);

  if (supported_incaps)
    gst_caps_unref (supported_incaps);
  gst_clear_caps (&allowed);
  gst_clear_caps (&template_caps);

  GST_DEBUG_OBJECT (nvenc, "  returning caps %" GST_PTR_FORMAT, caps);

  return caps;
}

static gboolean
gst_nv_base_enc_close (GstVideoEncoder * enc)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);
  gboolean ret = TRUE;

  if (nvenc->encoder) {
    if (NvEncDestroyEncoder (nvenc->encoder) != NV_ENC_SUCCESS)
      ret = FALSE;

    nvenc->encoder = NULL;
  }

  if (nvenc->cuda_ctx && nvenc->cuda_stream) {
    if (gst_cuda_context_push (nvenc->cuda_ctx)) {
      gst_cuda_result (CuStreamDestroy (nvenc->cuda_stream));
      gst_cuda_context_pop (NULL);
    }
  }

  gst_clear_object (&nvenc->cuda_ctx);
  nvenc->cuda_stream = NULL;

  GST_OBJECT_LOCK (nvenc);
  if (nvenc->input_formats)
    g_value_unset (nvenc->input_formats);
  g_free (nvenc->input_formats);
  nvenc->input_formats = NULL;
  GST_OBJECT_UNLOCK (nvenc);

  if (nvenc->input_state) {
    gst_video_codec_state_unref (nvenc->input_state);
    nvenc->input_state = NULL;
  }

  if (nvenc->bitstream_pool != NULL) {
    g_assert (g_async_queue_length (nvenc->bitstream_pool) == 0);
    g_async_queue_unref (nvenc->bitstream_pool);
    nvenc->bitstream_pool = NULL;
  }

  return ret;
}

static void
gst_nv_base_enc_init (GstNvBaseEnc * nvenc)
{
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (nvenc);

  nvenc->preset_enum = DEFAULT_PRESET;
  nvenc->selected_preset = _nv_preset_to_guid (nvenc->preset_enum);
  nvenc->rate_control_mode = DEFAULT_RC_MODE;
  nvenc->qp_min = DEFAULT_QP_MIN;
  nvenc->qp_max = DEFAULT_QP_MAX;
  nvenc->qp_const = DEFAULT_QP_CONST;
  nvenc->bitrate = DEFAULT_BITRATE;
  nvenc->gop_size = DEFAULT_GOP_SIZE;

  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);

  GST_PAD_SET_ACCEPT_INTERSECT (GST_VIDEO_ENCODER_SINK_PAD (encoder));
}

static void
gst_nv_base_enc_finalize (GObject * obj)
{
  G_OBJECT_CLASS (gst_nv_base_enc_parent_class)->finalize (obj);
}

static GstVideoCodecFrame *
_find_frame_with_output_buffer (GstNvBaseEnc * nvenc, NV_ENC_OUTPUT_PTR out_buf)
{
  GList *l, *walk = gst_video_encoder_get_frames (GST_VIDEO_ENCODER (nvenc));
  GstVideoCodecFrame *ret = NULL;
  gint i;

  for (l = walk; l; l = l->next) {
    GstVideoCodecFrame *frame = (GstVideoCodecFrame *) l->data;
    struct frame_state *state = frame->user_data;

    if (!state)
      continue;

    for (i = 0; i < N_BUFFERS_PER_FRAME; i++) {

      if (!state->out_bufs[i])
        break;

      if (state->out_bufs[i] == out_buf)
        ret = frame;
    }
  }

  if (ret)
    gst_video_codec_frame_ref (ret);

  g_list_free_full (walk, (GDestroyNotify) gst_video_codec_frame_unref);

  return ret;
}

static gpointer
gst_nv_base_enc_bitstream_thread (gpointer user_data)
{
  GstVideoEncoder *enc = user_data;
  GstNvBaseEnc *nvenc = user_data;

  /* overview of operation:
   * 1. retreive the next buffer submitted to the bitstream pool
   * 2. wait for that buffer to be ready from nvenc (LockBitsream)
   * 3. retreive the GstVideoCodecFrame associated with that buffer
   * 4. for each buffer in the frame
   * 4.1 (step 2): wait for that buffer to be ready from nvenc (LockBitsream)
   * 4.2 create an output GstBuffer from the nvenc buffers
   * 4.3 unlock the nvenc bitstream buffers UnlockBitsream
   * 5. finish_frame()
   * 6. cleanup
   */
  do {
    GstBuffer *buffers[N_BUFFERS_PER_FRAME];
    struct frame_state *state = NULL;
    GstVideoCodecFrame *frame = NULL;
    NVENCSTATUS nv_ret;
    GstFlowReturn flow = GST_FLOW_OK;
    gint i;

    {
      NV_ENC_LOCK_BITSTREAM lock_bs = { 0, };
      NV_ENC_OUTPUT_PTR out_buf;

      for (i = 0; i < N_BUFFERS_PER_FRAME; i++) {
        /* get and lock bitstream buffers */
        GstVideoCodecFrame *tmp_frame;

        if (state && i >= state->n_buffers)
          break;

        GST_LOG_OBJECT (enc, "wait for bitstream buffer..");

        /* assumes buffers are submitted in order */
        out_buf = g_async_queue_pop (nvenc->bitstream_queue);
        if ((gpointer) out_buf == SHUTDOWN_COOKIE)
          break;

        GST_LOG_OBJECT (nvenc, "waiting for output buffer %p to be ready",
            out_buf);

        lock_bs.version = gst_nvenc_get_lock_bitstream_version ();
        lock_bs.outputBitstream = out_buf;
        lock_bs.doNotWait = 0;

        /* FIXME: this would need to be updated for other slice modes */
        lock_bs.sliceOffsets = NULL;

        nv_ret = NvEncLockBitstream (nvenc->encoder, &lock_bs);
        if (nv_ret != NV_ENC_SUCCESS) {
          /* FIXME: what to do here? */
          GST_ELEMENT_ERROR (nvenc, STREAM, ENCODE, (NULL),
              ("Failed to lock bitstream buffer %p, ret %d",
                  lock_bs.outputBitstream, nv_ret));
          out_buf = SHUTDOWN_COOKIE;
          break;
        }

        GST_LOG_OBJECT (nvenc, "picture type %d", lock_bs.pictureType);

        tmp_frame = _find_frame_with_output_buffer (nvenc, out_buf);
        g_assert (tmp_frame != NULL);
        if (frame)
          g_assert (frame == tmp_frame);
        frame = tmp_frame;

        state = frame->user_data;
        g_assert (state->out_bufs[i] == out_buf);

        /* copy into output buffer */
        buffers[i] =
            gst_buffer_new_allocate (NULL, lock_bs.bitstreamSizeInBytes, NULL);
        gst_buffer_fill (buffers[i], 0, lock_bs.bitstreamBufferPtr,
            lock_bs.bitstreamSizeInBytes);

        if (lock_bs.pictureType == NV_ENC_PIC_TYPE_IDR) {
          GST_DEBUG_OBJECT (nvenc, "This is a keyframe");
          GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT (frame);
        }

        /* TODO: use lock_bs.outputTimeStamp and lock_bs.outputDuration */
        /* TODO: check pts/dts is handled properly if there are B-frames */

        nv_ret = NvEncUnlockBitstream (nvenc->encoder, state->out_bufs[i]);
        if (nv_ret != NV_ENC_SUCCESS) {
          /* FIXME: what to do here? */
          GST_ELEMENT_ERROR (nvenc, STREAM, ENCODE, (NULL),
              ("Failed to unlock bitstream buffer %p, ret %d",
                  lock_bs.outputBitstream, nv_ret));
          state->out_bufs[i] = SHUTDOWN_COOKIE;
          break;
        }

        GST_LOG_OBJECT (nvenc, "returning bitstream buffer %p to pool",
            state->out_bufs[i]);
        g_async_queue_push (nvenc->bitstream_pool, state->out_bufs[i]);
      }

      if (out_buf == SHUTDOWN_COOKIE)
        break;
    }

    {
      GstBuffer *output_buffer = gst_buffer_new ();

      for (i = 0; i < state->n_buffers; i++)
        output_buffer = gst_buffer_append (output_buffer, buffers[i]);

      frame->output_buffer = output_buffer;
    }

    for (i = 0; i < state->n_buffers; i++) {
      void *in_buf = state->in_bufs[i];
      g_assert (in_buf != NULL);

#if HAVE_NVCODEC_GST_GL
      if (nvenc->gl_input) {
        struct gl_input_resource *in_gl_resource = in_buf;

        nv_ret =
            NvEncUnmapInputResource (nvenc->encoder,
            in_gl_resource->nv_mapped_resource.mappedResource);
        in_gl_resource->mapped = FALSE;

        if (nv_ret != NV_ENC_SUCCESS) {
          GST_ERROR_OBJECT (nvenc, "Failed to unmap input resource %p, ret %d",
              in_gl_resource, nv_ret);
          break;
        }

        memset (&in_gl_resource->nv_mapped_resource, 0,
            sizeof (in_gl_resource->nv_mapped_resource));
      }
#endif

      g_async_queue_push (nvenc->in_bufs_pool, in_buf);
    }

    flow = gst_video_encoder_finish_frame (enc, frame);
    frame = NULL;

    if (flow != GST_FLOW_OK) {
      GST_INFO_OBJECT (enc, "got flow %s", gst_flow_get_name (flow));
      g_atomic_int_set (&nvenc->last_flow, flow);
      g_async_queue_push (nvenc->in_bufs_pool, SHUTDOWN_COOKIE);
      break;
    }
  }
  while (TRUE);

  GST_INFO_OBJECT (nvenc, "exiting thread");

  return NULL;
}

static gboolean
gst_nv_base_enc_start_bitstream_thread (GstNvBaseEnc * nvenc)
{
  gchar *name = g_strdup_printf ("%s-read-bits", GST_OBJECT_NAME (nvenc));

  g_assert (nvenc->bitstream_thread == NULL);

  g_assert (g_async_queue_length (nvenc->bitstream_queue) == 0);

  nvenc->bitstream_thread =
      g_thread_try_new (name, gst_nv_base_enc_bitstream_thread, nvenc, NULL);

  g_free (name);

  if (nvenc->bitstream_thread == NULL)
    return FALSE;

  GST_INFO_OBJECT (nvenc, "started thread to read bitstream");
  return TRUE;
}

static gboolean
gst_nv_base_enc_stop_bitstream_thread (GstNvBaseEnc * nvenc, gboolean force)
{
  gpointer out_buf;

  if (nvenc->bitstream_thread == NULL)
    return TRUE;

  if (force) {
    g_async_queue_lock (nvenc->bitstream_queue);
    g_async_queue_lock (nvenc->bitstream_pool);
    while ((out_buf = g_async_queue_try_pop_unlocked (nvenc->bitstream_queue))) {
      GST_INFO_OBJECT (nvenc, "stole bitstream buffer %p from queue", out_buf);
      g_async_queue_push_unlocked (nvenc->bitstream_pool, out_buf);
    }
    g_async_queue_push_unlocked (nvenc->bitstream_queue, SHUTDOWN_COOKIE);
    g_async_queue_unlock (nvenc->bitstream_pool);
    g_async_queue_unlock (nvenc->bitstream_queue);
  } else {
    /* wait for encoder to drain the remaining buffers */
    g_async_queue_push (nvenc->bitstream_queue, SHUTDOWN_COOKIE);
  }

  if (!force) {
    /* temporary unlock during finish, so other thread can find and push frame */
    GST_VIDEO_ENCODER_STREAM_UNLOCK (nvenc);
  }

  g_thread_join (nvenc->bitstream_thread);

  if (!force)
    GST_VIDEO_ENCODER_STREAM_LOCK (nvenc);

  nvenc->bitstream_thread = NULL;
  return TRUE;
}

static void
gst_nv_base_enc_reset_queues (GstNvBaseEnc * nvenc, gboolean refill)
{
  gpointer ptr;
  gint i;

  GST_INFO_OBJECT (nvenc, "clearing queues");

  while ((ptr = g_async_queue_try_pop (nvenc->bitstream_queue))) {
    /* do nothing */
  }
  while ((ptr = g_async_queue_try_pop (nvenc->bitstream_pool))) {
    /* do nothing */
  }
  while ((ptr = g_async_queue_try_pop (nvenc->in_bufs_pool))) {
    /* do nothing */
  }

  if (refill) {
    GST_INFO_OBJECT (nvenc, "refilling buffer pools");
    for (i = 0; i < nvenc->n_bufs; ++i) {
      g_async_queue_push (nvenc->bitstream_pool, nvenc->input_bufs[i]);
      g_async_queue_push (nvenc->in_bufs_pool, nvenc->output_bufs[i]);
    }
  }
}

static void
gst_nv_base_enc_free_buffers (GstNvBaseEnc * nvenc)
{
  NVENCSTATUS nv_ret;
  CUresult cuda_ret;
  guint i;

  if (nvenc->encoder == NULL)
    return;

  gst_nv_base_enc_reset_queues (nvenc, FALSE);

  for (i = 0; i < nvenc->n_bufs; ++i) {
    NV_ENC_OUTPUT_PTR out_buf = nvenc->output_bufs[i];

#if HAVE_NVCODEC_GST_GL
    if (nvenc->gl_input) {
      struct gl_input_resource *in_gl_resource = nvenc->input_bufs[i];

      gst_cuda_context_push (nvenc->cuda_ctx);

      if (in_gl_resource->mapped) {
        GST_LOG_OBJECT (nvenc, "Unmap resource %p", in_gl_resource);

        nv_ret =
            NvEncUnmapInputResource (nvenc->encoder,
            in_gl_resource->nv_mapped_resource.mappedResource);

        if (nv_ret != NV_ENC_SUCCESS) {
          GST_ERROR_OBJECT (nvenc, "Failed to unmap input resource %p, ret %d",
              in_gl_resource, nv_ret);
        }
      }

      nv_ret =
          NvEncUnregisterResource (nvenc->encoder,
          in_gl_resource->nv_resource.registeredResource);
      if (nv_ret != NV_ENC_SUCCESS)
        GST_ERROR_OBJECT (nvenc, "Failed to unregister resource %p, ret %d",
            in_gl_resource, nv_ret);

      cuda_ret = CuMemFree ((CUdeviceptr) in_gl_resource->cuda_pointer);
      if (!gst_cuda_result (cuda_ret)) {
        GST_ERROR_OBJECT (nvenc, "Failed to free CUDA device memory, ret %d",
            cuda_ret);
      }

      g_free (in_gl_resource);
      gst_cuda_context_pop (NULL);
    } else
#endif
    {
      NV_ENC_INPUT_PTR in_buf = (NV_ENC_INPUT_PTR) nvenc->input_bufs[i];

      GST_DEBUG_OBJECT (nvenc, "Destroying input buffer %p", in_buf);
      nv_ret = NvEncDestroyInputBuffer (nvenc->encoder, in_buf);
      if (nv_ret != NV_ENC_SUCCESS) {
        GST_ERROR_OBJECT (nvenc, "Failed to destroy input buffer %p, ret %d",
            in_buf, nv_ret);
      }
    }

    GST_DEBUG_OBJECT (nvenc, "Destroying output bitstream buffer %p", out_buf);
    nv_ret = NvEncDestroyBitstreamBuffer (nvenc->encoder, out_buf);
    if (nv_ret != NV_ENC_SUCCESS) {
      GST_ERROR_OBJECT (nvenc, "Failed to destroy output buffer %p, ret %d",
          out_buf, nv_ret);
    }
  }

  nvenc->n_bufs = 0;
  g_free (nvenc->output_bufs);
  nvenc->output_bufs = NULL;
  g_free (nvenc->input_bufs);
  nvenc->input_bufs = NULL;
}

static inline guint
_get_plane_width (GstVideoInfo * info, guint plane)
{
  return GST_VIDEO_INFO_COMP_WIDTH (info, plane)
      * GST_VIDEO_INFO_COMP_PSTRIDE (info, plane);
}

static inline guint
_get_plane_height (GstVideoInfo * info, guint plane)
{
  if (GST_VIDEO_INFO_IS_YUV (info))
    /* For now component width and plane width are the same and the
     * plane-component mapping matches
     */
    return GST_VIDEO_INFO_COMP_HEIGHT (info, plane);
  else                          /* RGB, GRAY */
    return GST_VIDEO_INFO_HEIGHT (info);
}

static inline gsize
_get_frame_data_height (GstVideoInfo * info)
{
  gsize ret = 0;
  gint i;

  for (i = 0; i < GST_VIDEO_INFO_N_PLANES (info); i++) {
    ret += _get_plane_height (info, i);
  }

  return ret;
}

/* GstVideoEncoder::set_format or by nvenc self if new properties were set.
 *
 * NvEncReconfigureEncoder with following conditions are not allowed
 * 1) GOP structure change
 * 2) sync-Async mode change (Async mode is Windows only and we didn't support it)
 * 3) MaxWidth, MaxHeight
 * 4) PTDmode (Picture Type Decision mode)
 *
 * So we will force to re-init the encode session if
 * 1) New resolution is larger than previous config
 * 2) GOP size changed
 * 3) Input pixel format change
 *    pre-allocated CUDA memory could not ensure stride, width and height
 *
 * TODO: bframe also considered as force re-init case
 */
static gboolean
gst_nv_base_enc_set_format (GstVideoEncoder * enc, GstVideoCodecState * state)
{
  GstNvBaseEncClass *nvenc_class = GST_NV_BASE_ENC_GET_CLASS (enc);
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);
  GstVideoInfo *info = &state->info;
  GstVideoCodecState *old_state = nvenc->input_state;
  NV_ENC_RECONFIGURE_PARAMS reconfigure_params = { 0, };
  NV_ENC_INITIALIZE_PARAMS *params = &nvenc->init_params;
  NV_ENC_PRESET_CONFIG preset_config = { 0, };
  NVENCSTATUS nv_ret;
  gint dar_n, dar_d;
  gboolean reconfigure = FALSE;

  g_atomic_int_set (&nvenc->reconfig, FALSE);

  if (old_state) {
    gboolean larger_resolution;
    gboolean format_changed;
    gboolean gop_size_changed;

    larger_resolution =
        (GST_VIDEO_INFO_WIDTH (info) > nvenc->init_params.maxEncodeWidth ||
        GST_VIDEO_INFO_HEIGHT (info) > nvenc->init_params.maxEncodeHeight);
    format_changed =
        GST_VIDEO_INFO_FORMAT (info) !=
        GST_VIDEO_INFO_FORMAT (&old_state->info);

    if (nvenc->config.gopLength == NVENC_INFINITE_GOPLENGTH
        && nvenc->gop_size == -1) {
      gop_size_changed = FALSE;
    } else if (nvenc->config.gopLength != nvenc->gop_size) {
      gop_size_changed = TRUE;
    } else {
      gop_size_changed = FALSE;
    }

    if (larger_resolution || format_changed || gop_size_changed) {
      GST_DEBUG_OBJECT (nvenc,
          "resolution %dx%d -> %dx%d, format %s -> %s, re-init",
          nvenc->init_params.maxEncodeWidth, nvenc->init_params.maxEncodeHeight,
          GST_VIDEO_INFO_WIDTH (info), GST_VIDEO_INFO_HEIGHT (info),
          gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&old_state->info)),
          gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (info)));

      gst_nv_base_enc_drain_encoder (nvenc);
      gst_nv_base_enc_stop_bitstream_thread (nvenc, FALSE);
      gst_nv_base_enc_free_buffers (nvenc);
      NvEncDestroyEncoder (nvenc->encoder);
      nvenc->encoder = NULL;

      if (!gst_nv_base_enc_open_encode_session (nvenc)) {
        GST_ERROR_OBJECT (nvenc, "Failed to open encode session");
        return FALSE;
      }
    } else {
      reconfigure_params.version = gst_nvenc_get_reconfigure_params_version ();
      /* reset rate control state and start from IDR */
      reconfigure_params.resetEncoder = TRUE;
      reconfigure_params.forceIDR = TRUE;
      reconfigure = TRUE;
    }
  }

  params->version = gst_nvenc_get_initialize_params_version ();
  params->encodeGUID = nvenc_class->codec_id;
  params->encodeWidth = GST_VIDEO_INFO_WIDTH (info);
  params->encodeHeight = GST_VIDEO_INFO_HEIGHT (info);

  {
    guint32 n_presets;
    GUID *presets;
    guint32 i;

    nv_ret =
        NvEncGetEncodePresetCount (nvenc->encoder,
        params->encodeGUID, &n_presets);
    if (nv_ret != NV_ENC_SUCCESS) {
      GST_ELEMENT_ERROR (nvenc, LIBRARY, SETTINGS, (NULL),
          ("Failed to get encoder presets"));
      return FALSE;
    }

    presets = g_new0 (GUID, n_presets);
    nv_ret =
        NvEncGetEncodePresetGUIDs (nvenc->encoder,
        params->encodeGUID, presets, n_presets, &n_presets);
    if (nv_ret != NV_ENC_SUCCESS) {
      GST_ELEMENT_ERROR (nvenc, LIBRARY, SETTINGS, (NULL),
          ("Failed to get encoder presets"));
      g_free (presets);
      return FALSE;
    }

    for (i = 0; i < n_presets; i++) {
      if (gst_nvenc_cmp_guid (presets[i], nvenc->selected_preset))
        break;
    }
    g_free (presets);
    if (i >= n_presets) {
      GST_ELEMENT_ERROR (nvenc, LIBRARY, SETTINGS, (NULL),
          ("Selected preset not supported"));
      return FALSE;
    }

    params->presetGUID = nvenc->selected_preset;
  }

  params->enablePTD = 1;
  if (!reconfigure) {
    /* this sets the required buffer size and the maximum allowed size on
     * subsequent reconfigures */
    params->maxEncodeWidth = GST_VIDEO_INFO_WIDTH (info);
    params->maxEncodeHeight = GST_VIDEO_INFO_HEIGHT (info);
  }

  preset_config.version = gst_nvenc_get_preset_config_version ();
  preset_config.presetCfg.version = gst_nvenc_get_config_version ();

  nv_ret =
      NvEncGetEncodePresetConfig (nvenc->encoder,
      params->encodeGUID, params->presetGUID, &preset_config);
  if (nv_ret != NV_ENC_SUCCESS) {
    GST_ELEMENT_ERROR (nvenc, LIBRARY, SETTINGS, (NULL),
        ("Failed to get encode preset configuration: %d", nv_ret));
    return FALSE;
  }

  params->encodeConfig = &preset_config.presetCfg;

  if (GST_VIDEO_INFO_IS_INTERLACED (info)) {
    if (GST_VIDEO_INFO_INTERLACE_MODE (info) ==
        GST_VIDEO_INTERLACE_MODE_INTERLEAVED
        || GST_VIDEO_INFO_INTERLACE_MODE (info) ==
        GST_VIDEO_INTERLACE_MODE_MIXED) {
      preset_config.presetCfg.frameFieldMode =
          NV_ENC_PARAMS_FRAME_FIELD_MODE_FIELD;
    }
  }

  if (info->fps_d > 0 && info->fps_n > 0) {
    params->frameRateNum = info->fps_n;
    params->frameRateDen = info->fps_d;
  } else {
    params->frameRateNum = 0;
    params->frameRateDen = 1;
  }

  if (gst_util_fraction_multiply (GST_VIDEO_INFO_WIDTH (info),
          GST_VIDEO_INFO_HEIGHT (info), GST_VIDEO_INFO_PAR_N (info),
          GST_VIDEO_INFO_PAR_D (info), &dar_n, &dar_d) && dar_n > 0
      && dar_d > 0) {
    params->darWidth = dar_n;
    params->darHeight = dar_d;
  }

  if (nvenc->rate_control_mode != GST_NV_RC_MODE_DEFAULT) {
    params->encodeConfig->rcParams.rateControlMode =
        _rc_mode_to_nv (nvenc->rate_control_mode);
    if (nvenc->bitrate > 0) {
      /* FIXME: this produces larger bitrates?! */
      params->encodeConfig->rcParams.averageBitRate = nvenc->bitrate * 1024;
      params->encodeConfig->rcParams.maxBitRate = nvenc->bitrate * 1024;
    }
    if (nvenc->qp_const > 0) {
      params->encodeConfig->rcParams.constQP.qpInterB = nvenc->qp_const;
      params->encodeConfig->rcParams.constQP.qpInterP = nvenc->qp_const;
      params->encodeConfig->rcParams.constQP.qpIntra = nvenc->qp_const;
    }
    if (nvenc->qp_min >= 0) {
      params->encodeConfig->rcParams.enableMinQP = 1;
      params->encodeConfig->rcParams.minQP.qpInterB = nvenc->qp_min;
      params->encodeConfig->rcParams.minQP.qpInterP = nvenc->qp_min;
      params->encodeConfig->rcParams.minQP.qpIntra = nvenc->qp_min;
    }
    if (nvenc->qp_max >= 0) {
      params->encodeConfig->rcParams.enableMaxQP = 1;
      params->encodeConfig->rcParams.maxQP.qpInterB = nvenc->qp_max;
      params->encodeConfig->rcParams.maxQP.qpInterP = nvenc->qp_max;
      params->encodeConfig->rcParams.maxQP.qpIntra = nvenc->qp_max;
    }
  }

  if (nvenc->gop_size < 0) {
    params->encodeConfig->gopLength = NVENC_INFINITE_GOPLENGTH;
    params->encodeConfig->frameIntervalP = 1;
  } else if (nvenc->gop_size > 0) {
    params->encodeConfig->gopLength = nvenc->gop_size;
  }

  g_assert (nvenc_class->set_encoder_config);
  if (!nvenc_class->set_encoder_config (nvenc, state, params->encodeConfig)) {
    GST_ERROR_OBJECT (enc, "Subclass failed to set encoder configuration");
    return FALSE;
  }

  /* store the last config to reconfig/re-init decision in the next time */
  nvenc->config = *params->encodeConfig;

  G_LOCK (initialization_lock);
  if (reconfigure) {
    reconfigure_params.reInitEncodeParams = nvenc->init_params;
    nv_ret = NvEncReconfigureEncoder (nvenc->encoder, &reconfigure_params);
  } else {
    nv_ret = NvEncInitializeEncoder (nvenc->encoder, params);
  }
  G_UNLOCK (initialization_lock);

  if (nv_ret != NV_ENC_SUCCESS) {
    GST_ELEMENT_ERROR (nvenc, LIBRARY, SETTINGS, (NULL),
        ("Failed to %sinit encoder: %d", reconfigure ? "re" : "", nv_ret));
    return FALSE;
  }

  if (!reconfigure) {
    nvenc->input_info = *info;
    nvenc->gl_input = FALSE;
  }

  if (nvenc->input_state)
    gst_video_codec_state_unref (nvenc->input_state);
  nvenc->input_state = gst_video_codec_state_ref (state);
  GST_INFO_OBJECT (nvenc, "%sconfigured encoder", reconfigure ? "re" : "");

  /* now allocate some buffers only on first configuration */
  if (!reconfigure) {
#if HAVE_NVCODEC_GST_GL
    GstCapsFeatures *features;
#endif
    guint num_macroblocks, i;
    guint input_width, input_height;

    input_width = GST_VIDEO_INFO_WIDTH (info);
    input_height = GST_VIDEO_INFO_HEIGHT (info);

    num_macroblocks = (GST_ROUND_UP_16 (input_width) >> 4)
        * (GST_ROUND_UP_16 (input_height) >> 4);
    nvenc->n_bufs = (num_macroblocks >= 8160) ? 32 : 48;

    /* input buffers */
    nvenc->input_bufs = g_new0 (gpointer, nvenc->n_bufs);

#if HAVE_NVCODEC_GST_GL
    features = gst_caps_get_features (state->caps, 0);
    if (gst_caps_features_contains (features,
            GST_CAPS_FEATURE_MEMORY_GL_MEMORY)) {
      guint pixel_depth = 0;
      nvenc->gl_input = TRUE;

      for (i = 0; i < GST_VIDEO_INFO_N_COMPONENTS (info); i++) {
        pixel_depth += GST_VIDEO_INFO_COMP_DEPTH (info, i);
      }

      gst_cuda_context_push (nvenc->cuda_ctx);
      for (i = 0; i < nvenc->n_bufs; ++i) {
        struct gl_input_resource *in_gl_resource =
            g_new0 (struct gl_input_resource, 1);
        CUresult cu_ret;

        memset (&in_gl_resource->nv_resource, 0,
            sizeof (in_gl_resource->nv_resource));
        memset (&in_gl_resource->nv_mapped_resource, 0,
            sizeof (in_gl_resource->nv_mapped_resource));

        /* scratch buffer for non-contigious planer into a contigious buffer */
        cu_ret =
            CuMemAllocPitch ((CUdeviceptr *) & in_gl_resource->cuda_pointer,
            &in_gl_resource->cuda_stride, _get_plane_width (info, 0),
            _get_frame_data_height (info), 16);
        if (!gst_cuda_result (CUDA_SUCCESS)) {
          GST_ERROR_OBJECT (nvenc, "failed to alocate cuda scratch buffer "
              "ret %d", cu_ret);
          g_assert_not_reached ();
        }

        in_gl_resource->nv_resource.version =
            gst_nvenc_get_registure_resource_version ();
        in_gl_resource->nv_resource.resourceType =
            NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
        in_gl_resource->nv_resource.width = input_width;
        in_gl_resource->nv_resource.height = input_height;
        in_gl_resource->nv_resource.pitch = in_gl_resource->cuda_stride;
        in_gl_resource->nv_resource.bufferFormat =
            gst_nvenc_get_nv_buffer_format (GST_VIDEO_INFO_FORMAT (info));
        in_gl_resource->nv_resource.resourceToRegister =
            in_gl_resource->cuda_pointer;

        nv_ret =
            NvEncRegisterResource (nvenc->encoder,
            &in_gl_resource->nv_resource);
        if (nv_ret != NV_ENC_SUCCESS)
          GST_ERROR_OBJECT (nvenc, "Failed to register resource %p, ret %d",
              in_gl_resource, nv_ret);

        nvenc->input_bufs[i] = in_gl_resource;
        g_async_queue_push (nvenc->in_bufs_pool, nvenc->input_bufs[i]);
      }

      gst_cuda_context_pop (NULL);
    } else
#endif
    {
      for (i = 0; i < nvenc->n_bufs; ++i) {
        NV_ENC_CREATE_INPUT_BUFFER cin_buf = { 0, };

        cin_buf.version = gst_nvenc_get_create_input_buffer_version ();

        cin_buf.width = GST_ROUND_UP_32 (input_width);
        cin_buf.height = GST_ROUND_UP_32 (input_height);

        cin_buf.memoryHeap = NV_ENC_MEMORY_HEAP_SYSMEM_CACHED;
        cin_buf.bufferFmt =
            gst_nvenc_get_nv_buffer_format (GST_VIDEO_INFO_FORMAT (info));

        nv_ret = NvEncCreateInputBuffer (nvenc->encoder, &cin_buf);

        if (nv_ret != NV_ENC_SUCCESS) {
          GST_WARNING_OBJECT (enc, "Failed to allocate input buffer: %d",
              nv_ret);
          /* FIXME: clean up */
          return FALSE;
        }

        nvenc->input_bufs[i] = cin_buf.inputBuffer;

        GST_INFO_OBJECT (nvenc, "allocated  input buffer %2d: %p", i,
            nvenc->input_bufs[i]);

        g_async_queue_push (nvenc->in_bufs_pool, nvenc->input_bufs[i]);
      }
    }

    /* output buffers */
    nvenc->output_bufs = g_new0 (NV_ENC_OUTPUT_PTR, nvenc->n_bufs);
    for (i = 0; i < nvenc->n_bufs; ++i) {
      NV_ENC_CREATE_BITSTREAM_BUFFER cout_buf = { 0, };

      cout_buf.version = gst_nvenc_get_create_bitstream_buffer_version ();

      /* 1 MB should be large enough to hold most output frames.
       * NVENC will automatically increase this if it's not enough. */
      cout_buf.size = 1024 * 1024;
      cout_buf.memoryHeap = NV_ENC_MEMORY_HEAP_SYSMEM_CACHED;

      G_LOCK (initialization_lock);
      nv_ret = NvEncCreateBitstreamBuffer (nvenc->encoder, &cout_buf);
      G_UNLOCK (initialization_lock);

      if (nv_ret != NV_ENC_SUCCESS) {
        GST_WARNING_OBJECT (enc, "Failed to allocate input buffer: %d", nv_ret);
        /* FIXME: clean up */
        return FALSE;
      }

      nvenc->output_bufs[i] = cout_buf.bitstreamBuffer;

      GST_INFO_OBJECT (nvenc, "allocated output buffer %2d: %p", i,
          nvenc->output_bufs[i]);

      g_async_queue_push (nvenc->bitstream_pool, nvenc->output_bufs[i]);
    }

#if 0
    /* Get SPS/PPS */
    {
      NV_ENC_SEQUENCE_PARAM_PAYLOAD seq_param = { 0 };
      uint32_t seq_size = 0;

      seq_param.version = gst_nvenc_get_sequence_param_payload_version ();
      seq_param.spsppsBuffer = g_alloca (1024);
      seq_param.inBufferSize = 1024;
      seq_param.outSPSPPSPayloadSize = &seq_size;

      nv_ret = NvEncGetSequenceParams (nvenc->encoder, &seq_param);
      if (nv_ret != NV_ENC_SUCCESS) {
        GST_WARNING_OBJECT (enc, "Failed to retrieve SPS/PPS: %d", nv_ret);
        return FALSE;
      }

      /* FIXME: use SPS/PPS */
      GST_MEMDUMP_OBJECT (enc, "SPS/PPS", seq_param.spsppsBuffer, seq_size);
    }
#endif
  }

  g_assert (nvenc_class->set_src_caps);
  if (!nvenc_class->set_src_caps (nvenc, state)) {
    GST_ERROR_OBJECT (nvenc, "Subclass failed to set output caps");
    /* FIXME: clean up */
    return FALSE;
  }

  return TRUE;
}

#if HAVE_NVCODEC_GST_GL
static guint
_get_cuda_device_stride (GstVideoInfo * info, guint plane, gsize cuda_stride)
{
  switch (GST_VIDEO_INFO_FORMAT (info)) {
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_NV21:
    case GST_VIDEO_FORMAT_P010_10LE:
    case GST_VIDEO_FORMAT_P010_10BE:
    case GST_VIDEO_FORMAT_Y444:
    case GST_VIDEO_FORMAT_BGRA:
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_BGR10A2_LE:
    case GST_VIDEO_FORMAT_RGB10A2_LE:
    case GST_VIDEO_FORMAT_Y444_16LE:
    case GST_VIDEO_FORMAT_Y444_16BE:
      return cuda_stride;
    case GST_VIDEO_FORMAT_I420:
    case GST_VIDEO_FORMAT_YV12:
      return plane == 0 ? cuda_stride : (GST_ROUND_UP_2 (cuda_stride) / 2);
    default:
      g_assert_not_reached ();
      return cuda_stride;
  }
}

typedef struct _GstNvEncRegisterResourceData
{
  GstMemory *mem;
  GstCudaGraphicsResource *resource;
  GstNvBaseEnc *nvenc;
  gboolean ret;
} GstNvEncRegisterResourceData;

static void
register_cuda_resource (GstGLContext * context,
    GstNvEncRegisterResourceData * data)
{
  GstMemory *mem = data->mem;
  GstCudaGraphicsResource *resource = data->resource;
  GstNvBaseEnc *nvenc = data->nvenc;
  GstMapInfo map_info = GST_MAP_INFO_INIT;
  GstGLBuffer *gl_buf_obj;

  data->ret = FALSE;

  if (!gst_cuda_context_push (nvenc->cuda_ctx)) {
    GST_WARNING_OBJECT (nvenc, "failed to push CUDA context");
    return;
  }

  if (gst_memory_map (mem, &map_info, GST_MAP_READ | GST_MAP_GL)) {
    GstGLMemoryPBO *gl_mem = (GstGLMemoryPBO *) data->mem;
    gl_buf_obj = gl_mem->pbo;

    GST_LOG_OBJECT (nvenc,
        "registure glbuffer %d to CUDA resource", gl_buf_obj->id);

    if (gst_cuda_graphics_resource_register_gl_buffer (resource,
            gl_buf_obj->id, CU_GRAPHICS_REGISTER_FLAGS_NONE)) {
      data->ret = TRUE;
    } else {
      GST_WARNING_OBJECT (nvenc, "failed to register memory");
    }

    gst_memory_unmap (mem, &map_info);
  } else {
    GST_WARNING_OBJECT (nvenc, "failed to map memory");
  }

  if (!gst_cuda_context_pop (NULL))
    GST_WARNING_OBJECT (nvenc, "failed to unlock CUDA context");
}

static GstCudaGraphicsResource *
ensure_cuda_graphics_resource (GstMemory * mem, GstNvBaseEnc * nvenc)
{
  GQuark quark;
  GstCudaGraphicsResource *cgr_info;
  GstNvEncRegisterResourceData data;

  if (!gst_is_gl_memory_pbo (mem)) {
    GST_WARNING_OBJECT (nvenc, "memory is not GL PBO memory, %s",
        mem->allocator->mem_type);
    return NULL;
  }

  quark = gst_cuda_quark_from_id (GST_CUDA_QUARK_GRAPHICS_RESOURCE);

  cgr_info = gst_mini_object_get_qdata (GST_MINI_OBJECT (mem), quark);
  if (!cgr_info) {
    cgr_info = gst_cuda_graphics_resource_new (nvenc->cuda_ctx,
        GST_OBJECT (GST_GL_BASE_MEMORY_CAST (mem)->context),
        GST_CUDA_GRAPHICS_RESOURCE_GL_BUFFER);
    data.mem = mem;
    data.resource = cgr_info;
    data.nvenc = nvenc;
    gst_gl_context_thread_add ((GstGLContext *) cgr_info->graphics_context,
        (GstGLContextThreadFunc) register_cuda_resource, &data);
    if (!data.ret) {
      GST_WARNING_OBJECT (nvenc, "could not register resource");
      gst_cuda_graphics_resource_free (cgr_info);

      return NULL;
    }

    gst_mini_object_set_qdata (GST_MINI_OBJECT (mem), quark, cgr_info,
        (GDestroyNotify) gst_cuda_graphics_resource_free);
  }

  return cgr_info;
}

typedef struct _GstNvEncGLMapData
{
  GstNvBaseEnc *nvenc;
  GstBuffer *buffer;
  GstVideoInfo *info;
  struct gl_input_resource *in_gl_resource;

  gboolean ret;
} GstNvEncGLMapData;

static void
_map_gl_input_buffer (GstGLContext * context, GstNvEncGLMapData * data)
{
  CUresult cuda_ret;
  guint8 *data_pointer;
  guint i;
  CUDA_MEMCPY2D param;
  GstCudaGraphicsResource **resources;
  guint num_resources;

  data->ret = FALSE;

  num_resources = gst_buffer_n_memory (data->buffer);
  resources = g_newa (GstCudaGraphicsResource *, num_resources);

  for (i = 0; i < num_resources; i++) {
    GstMemory *mem;

    mem = gst_buffer_peek_memory (data->buffer, i);
    resources[i] = ensure_cuda_graphics_resource (mem, data->nvenc);
    if (!resources[i]) {
      GST_ERROR_OBJECT (data->nvenc, "could not register %dth memory", i);
      return;
    }
  }

  gst_cuda_context_push (data->nvenc->cuda_ctx);
  data_pointer = data->in_gl_resource->cuda_pointer;
  for (i = 0; i < GST_VIDEO_INFO_N_PLANES (data->info); i++) {
    GstGLBuffer *gl_buf_obj;
    GstGLMemoryPBO *gl_mem;
    guint src_stride, dest_stride;
    CUgraphicsResource cuda_resource;

    gl_mem = (GstGLMemoryPBO *) gst_buffer_peek_memory (data->buffer, i);
    g_return_if_fail (gst_is_gl_memory_pbo ((GstMemory *) gl_mem));
    data->in_gl_resource->gl_mem[i] = GST_GL_MEMORY_CAST (gl_mem);

    gl_buf_obj = (GstGLBuffer *) gl_mem->pbo;
    g_return_if_fail (gl_buf_obj != NULL);

    /* get the texture into the PBO */
    gst_gl_memory_pbo_upload_transfer (gl_mem);
    gst_gl_memory_pbo_download_transfer (gl_mem);

    GST_LOG_OBJECT (data->nvenc, "attempting to copy texture %u into cuda",
        gl_mem->mem.tex_id);

    cuda_resource =
        gst_cuda_graphics_resource_map (resources[i], data->nvenc->cuda_stream,
        CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);

    if (!cuda_resource) {
      GST_ERROR_OBJECT (data->nvenc, "failed to map GL texture %u into cuda",
          gl_mem->mem.tex_id);
      g_assert_not_reached ();
    }

    cuda_ret =
        CuGraphicsResourceGetMappedPointer (&data->in_gl_resource->
        cuda_plane_pointers[i], &data->in_gl_resource->cuda_num_bytes,
        cuda_resource);
    if (!gst_cuda_result (cuda_ret)) {
      GST_ERROR_OBJECT (data->nvenc, "failed to get mapped pointer of map GL "
          "texture %u in cuda ret :%d", gl_mem->mem.tex_id, cuda_ret);
      g_assert_not_reached ();
    }

    src_stride = GST_VIDEO_INFO_PLANE_STRIDE (data->info, i);
    dest_stride =
        _get_cuda_device_stride (data->info, i,
        data->in_gl_resource->cuda_stride);

    /* copy into scratch buffer */
    param.srcXInBytes = 0;
    param.srcY = 0;
    param.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    param.srcDevice = data->in_gl_resource->cuda_plane_pointers[i];
    param.srcPitch = src_stride;

    param.dstXInBytes = 0;
    param.dstY = 0;
    param.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    param.dstDevice = (CUdeviceptr) data_pointer;
    param.dstPitch = dest_stride;
    param.WidthInBytes = _get_plane_width (data->info, i);
    param.Height = _get_plane_height (data->info, i);

    cuda_ret = CuMemcpy2DAsync (&param, data->nvenc->cuda_stream);
    if (!gst_cuda_result (cuda_ret)) {
      GST_ERROR_OBJECT (data->nvenc, "failed to copy GL texture %u into cuda "
          "ret :%d", gl_mem->mem.tex_id, cuda_ret);
      g_assert_not_reached ();
    }

    gst_cuda_graphics_resource_unmap (resources[i], data->nvenc->cuda_stream);

    data_pointer = data_pointer +
        dest_stride * _get_plane_height (&data->nvenc->input_info, i);
  }
  gst_cuda_result (CuStreamSynchronize (data->nvenc->cuda_stream));
  gst_cuda_context_pop (NULL);

  data->ret = TRUE;
}
#endif

static GstFlowReturn
_acquire_input_buffer (GstNvBaseEnc * nvenc, gpointer * input)
{
  g_assert (input);

  GST_LOG_OBJECT (nvenc, "acquiring input buffer..");
  GST_VIDEO_ENCODER_STREAM_UNLOCK (nvenc);
  *input = g_async_queue_pop (nvenc->in_bufs_pool);
  GST_VIDEO_ENCODER_STREAM_LOCK (nvenc);

  if (*input == SHUTDOWN_COOKIE)
    return g_atomic_int_get (&nvenc->last_flow);

  return GST_FLOW_OK;
}

static GstFlowReturn
_submit_input_buffer (GstNvBaseEnc * nvenc, GstVideoCodecFrame * frame,
    GstVideoFrame * vframe, void *inputBuffer, void *inputBufferPtr,
    NV_ENC_BUFFER_FORMAT bufferFormat, void *outputBufferPtr)
{
  GstNvBaseEncClass *nvenc_class = GST_NV_BASE_ENC_GET_CLASS (nvenc);
  NV_ENC_PIC_PARAMS pic_params = { 0, };
  NVENCSTATUS nv_ret;

  GST_LOG_OBJECT (nvenc, "%u: input buffer %p, output buffer %p, "
      "pts %" GST_TIME_FORMAT, frame->system_frame_number, inputBuffer,
      outputBufferPtr, GST_TIME_ARGS (frame->pts));

  pic_params.version = gst_nvenc_get_pic_params_version ();
  pic_params.inputBuffer = inputBufferPtr;
  pic_params.bufferFmt = bufferFormat;

  pic_params.inputWidth = GST_VIDEO_FRAME_WIDTH (vframe);
  pic_params.inputHeight = GST_VIDEO_FRAME_HEIGHT (vframe);
  pic_params.outputBitstream = outputBufferPtr;
  pic_params.completionEvent = NULL;
  if (GST_VIDEO_FRAME_IS_INTERLACED (vframe)) {
    if (GST_VIDEO_FRAME_IS_TFF (vframe))
      pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FIELD_TOP_BOTTOM;
    else
      pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FIELD_BOTTOM_TOP;
  } else {
    pic_params.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
  }
  pic_params.inputTimeStamp = frame->pts;
  pic_params.inputDuration =
      GST_CLOCK_TIME_IS_VALID (frame->duration) ? frame->duration : 0;
  pic_params.frameIdx = frame->system_frame_number;

  if (GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME (frame))
    pic_params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
  else
    pic_params.encodePicFlags = 0;

  if (nvenc_class->set_pic_params
      && !nvenc_class->set_pic_params (nvenc, frame, &pic_params)) {
    GST_ERROR_OBJECT (nvenc, "Subclass failed to submit buffer");
    return GST_FLOW_ERROR;
  }

  nv_ret = NvEncEncodePicture (nvenc->encoder, &pic_params);
  if (nv_ret == NV_ENC_SUCCESS) {
    GST_LOG_OBJECT (nvenc, "Encoded picture");
  } else if (nv_ret == NV_ENC_ERR_NEED_MORE_INPUT) {
    /* FIXME: we should probably queue pending output buffers here and only
     * submit them to the async queue once we got sucess back */
    GST_DEBUG_OBJECT (nvenc, "Encoded picture (encoder needs more input)");
  } else {
    GST_ERROR_OBJECT (nvenc, "Failed to encode picture: %d", nv_ret);
    GST_DEBUG_OBJECT (nvenc, "re-enqueueing input buffer %p", inputBuffer);
    g_async_queue_push (nvenc->in_bufs_pool, inputBuffer);
    GST_DEBUG_OBJECT (nvenc, "re-enqueueing output buffer %p", outputBufferPtr);
    g_async_queue_push (nvenc->bitstream_pool, outputBufferPtr);

    return GST_FLOW_ERROR;
  }

  g_async_queue_push (nvenc->bitstream_queue, outputBufferPtr);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_nv_base_enc_handle_frame (GstVideoEncoder * enc, GstVideoCodecFrame * frame)
{
  gpointer input_buffer = NULL;
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);
  NV_ENC_OUTPUT_PTR out_buf;
  NVENCSTATUS nv_ret;
  GstVideoFrame vframe;
  GstVideoInfo *info = &nvenc->input_state->info;
  GstFlowReturn flow = GST_FLOW_OK;
  GstMapFlags in_map_flags = GST_MAP_READ;
  struct frame_state *state = NULL;
  guint frame_n = 0;

  g_assert (nvenc->encoder != NULL);

  if (g_atomic_int_compare_and_exchange (&nvenc->reconfig, TRUE, FALSE)) {
    if (!gst_nv_base_enc_set_format (enc, nvenc->input_state))
      return GST_FLOW_ERROR;

    /* reconfigured encode session should start from keyframe */
    GST_VIDEO_CODEC_FRAME_SET_FORCE_KEYFRAME (frame);
  }
#if HAVE_NVCODEC_GST_GL
  if (nvenc->gl_input)
    in_map_flags |= GST_MAP_GL;
#endif

  if (!gst_video_frame_map (&vframe, info, frame->input_buffer, in_map_flags))
    return GST_FLOW_ERROR;

  /* make sure our thread that waits for output to be ready is started */
  if (nvenc->bitstream_thread == NULL) {
    if (!gst_nv_base_enc_start_bitstream_thread (nvenc))
      goto error;
  }

  flow = _acquire_input_buffer (nvenc, &input_buffer);
  if (flow != GST_FLOW_OK)
    goto out;
  else if (input_buffer == SHUTDOWN_COOKIE)
    goto out;
  if (input_buffer == NULL)
    goto error;

  state = frame->user_data;
  if (!state)
    state = g_new0 (struct frame_state, 1);
  state->n_buffers = 1;

#if HAVE_NVCODEC_GST_GL
  if (nvenc->gl_input) {
    struct gl_input_resource *in_gl_resource = input_buffer;
    GstNvEncGLMapData data;

    GST_LOG_OBJECT (enc, "got input buffer %p", in_gl_resource);

    in_gl_resource->gl_mem[0] =
        (GstGLMemory *) gst_buffer_peek_memory (frame->input_buffer, 0);
    g_assert (gst_is_gl_memory ((GstMemory *) in_gl_resource->gl_mem[0]));

    data.nvenc = nvenc;
    data.buffer = frame->input_buffer;
    data.info = &vframe.info;
    data.in_gl_resource = in_gl_resource;

    gst_gl_context_thread_add (in_gl_resource->gl_mem[0]->mem.context,
        (GstGLContextThreadFunc) _map_gl_input_buffer, &data);

    if (!data.ret) {
      GST_ERROR_OBJECT (nvenc, "Could not map input buffer");
      goto error;
    }

    in_gl_resource->nv_mapped_resource.version =
        gst_nvenc_get_map_input_resource_version ();
    in_gl_resource->nv_mapped_resource.registeredResource =
        in_gl_resource->nv_resource.registeredResource;

    nv_ret =
        NvEncMapInputResource (nvenc->encoder,
        &in_gl_resource->nv_mapped_resource);
    if (nv_ret != NV_ENC_SUCCESS) {
      GST_ERROR_OBJECT (nvenc, "Failed to map input resource %p, ret %d",
          in_gl_resource, nv_ret);
      goto error;
    }

    in_gl_resource->mapped = TRUE;

    out_buf = g_async_queue_try_pop (nvenc->bitstream_pool);
    if (out_buf == NULL) {
      GST_DEBUG_OBJECT (nvenc, "wait for output buf to become available again");
      out_buf = g_async_queue_pop (nvenc->bitstream_pool);
    }

    state->in_bufs[frame_n] = in_gl_resource;
    state->out_bufs[frame_n++] = out_buf;

    frame->user_data = state;
    frame->user_data_destroy_notify = (GDestroyNotify) g_free;

    flow =
        _submit_input_buffer (nvenc, frame, &vframe, in_gl_resource,
        in_gl_resource->nv_mapped_resource.mappedResource,
        in_gl_resource->nv_mapped_resource.mappedBufferFmt, out_buf);

    /* encoder will keep frame in list internally, we'll look it up again later
     * in the thread where we get the output buffers and finish it there */
    gst_video_codec_frame_unref (frame);
    frame = NULL;
  }
#endif

  if (!nvenc->gl_input) {
    NV_ENC_LOCK_INPUT_BUFFER in_buf_lock = { 0, };
    NV_ENC_INPUT_PTR in_buf = input_buffer;
    guint8 *src, *dest;
    guint src_stride, dest_stride;
    guint height, width;
    guint y;

    GST_LOG_OBJECT (enc, "got input buffer %p", in_buf);

    in_buf_lock.version = gst_nvenc_get_lock_input_buffer_version ();
    in_buf_lock.inputBuffer = in_buf;

    nv_ret = NvEncLockInputBuffer (nvenc->encoder, &in_buf_lock);
    if (nv_ret != NV_ENC_SUCCESS) {
      GST_ERROR_OBJECT (nvenc, "Failed to lock input buffer: %d", nv_ret);
      /* FIXME: post proper error message */
      goto error;
    }
    GST_LOG_OBJECT (nvenc, "Locked input buffer %p", in_buf);

    width = GST_VIDEO_FRAME_COMP_WIDTH (&vframe, 0) *
        GST_VIDEO_FRAME_COMP_PSTRIDE (&vframe, 0);
    height = GST_VIDEO_FRAME_HEIGHT (&vframe);

    /* copy Y plane */
    src = GST_VIDEO_FRAME_PLANE_DATA (&vframe, 0);
    src_stride = GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 0);
    dest = in_buf_lock.bufferDataPtr;
    dest_stride = in_buf_lock.pitch;
    for (y = 0; y < height; ++y) {
      memcpy (dest, src, width);
      dest += dest_stride;
      src += src_stride;
    }

    if (GST_VIDEO_FRAME_FORMAT (&vframe) == GST_VIDEO_FORMAT_NV12 ||
        GST_VIDEO_FRAME_FORMAT (&vframe) == GST_VIDEO_FORMAT_P010_10LE ||
        GST_VIDEO_FRAME_FORMAT (&vframe) == GST_VIDEO_FORMAT_P010_10BE) {
      /* copy UV plane */
      src = GST_VIDEO_FRAME_PLANE_DATA (&vframe, 1);
      src_stride = GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 1);
      dest =
          (guint8 *) in_buf_lock.bufferDataPtr +
          GST_ROUND_UP_32 (height) * in_buf_lock.pitch;
      dest_stride = in_buf_lock.pitch;
      for (y = 0; y < GST_ROUND_UP_2 (height) / 2; ++y) {
        memcpy (dest, src, width);
        dest += dest_stride;
        src += src_stride;
      }
    } else if (GST_VIDEO_FRAME_FORMAT (&vframe) == GST_VIDEO_FORMAT_I420 ||
        GST_VIDEO_FRAME_FORMAT (&vframe) == GST_VIDEO_FORMAT_YV12) {
      guint8 *dest_u, *dest_v;

      dest_u = (guint8 *) in_buf_lock.bufferDataPtr +
          GST_ROUND_UP_32 (height) * in_buf_lock.pitch;
      dest_v = dest_u + ((GST_ROUND_UP_32 (height) / 2) *
          (in_buf_lock.pitch / 2));
      dest_stride = in_buf_lock.pitch / 2;

      /* copy U plane */
      src = GST_VIDEO_FRAME_PLANE_DATA (&vframe, 1);
      src_stride = GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 1);
      dest = dest_u;
      for (y = 0; y < GST_ROUND_UP_2 (height) / 2; ++y) {
        memcpy (dest, src, width / 2);
        dest += dest_stride;
        src += src_stride;
      }

      /* copy V plane */
      src = GST_VIDEO_FRAME_PLANE_DATA (&vframe, 2);
      src_stride = GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 2);
      dest = dest_v;
      for (y = 0; y < GST_ROUND_UP_2 (height) / 2; ++y) {
        memcpy (dest, src, width / 2);
        dest += dest_stride;
        src += src_stride;
      }
    } else if (GST_VIDEO_FRAME_FORMAT (&vframe) == GST_VIDEO_FORMAT_Y444 ||
        GST_VIDEO_FRAME_FORMAT (&vframe) == GST_VIDEO_FORMAT_Y444_16LE ||
        GST_VIDEO_FRAME_FORMAT (&vframe) == GST_VIDEO_FORMAT_Y444_16BE) {
      src = GST_VIDEO_FRAME_PLANE_DATA (&vframe, 1);
      src_stride = GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 1);
      dest = (guint8 *) in_buf_lock.bufferDataPtr +
          GST_ROUND_UP_32 (height) * in_buf_lock.pitch;
      dest_stride = in_buf_lock.pitch;

      for (y = 0; y < height; ++y) {
        memcpy (dest, src, width);
        dest += dest_stride;
        src += src_stride;
      }

      src = GST_VIDEO_FRAME_PLANE_DATA (&vframe, 2);
      src_stride = GST_VIDEO_FRAME_PLANE_STRIDE (&vframe, 2);
      dest = (guint8 *) in_buf_lock.bufferDataPtr +
          2 * GST_ROUND_UP_32 (height) * in_buf_lock.pitch;

      for (y = 0; y < height; ++y) {
        memcpy (dest, src, width);
        dest += dest_stride;
        src += src_stride;
      }
    } else if (GST_VIDEO_INFO_IS_RGB (info)) {
      /* nothing to do */
    } else {
      // FIXME: this only works for NV12 and I420
      g_assert_not_reached ();
    }

    nv_ret = NvEncUnlockInputBuffer (nvenc->encoder, in_buf);
    if (nv_ret != NV_ENC_SUCCESS) {
      GST_ERROR_OBJECT (nvenc, "Failed to unlock input buffer: %d", nv_ret);
      goto error;
    }

    out_buf = g_async_queue_try_pop (nvenc->bitstream_pool);
    if (out_buf == NULL) {
      GST_DEBUG_OBJECT (nvenc, "wait for output buf to become available again");
      out_buf = g_async_queue_pop (nvenc->bitstream_pool);
    }

    state->in_bufs[frame_n] = in_buf;
    state->out_bufs[frame_n++] = out_buf;
    frame->user_data = state;
    frame->user_data_destroy_notify = (GDestroyNotify) g_free;

    flow =
        _submit_input_buffer (nvenc, frame, &vframe, in_buf, in_buf,
        gst_nvenc_get_nv_buffer_format (GST_VIDEO_INFO_FORMAT (info)), out_buf);

    /* encoder will keep frame in list internally, we'll look it up again later
     * in the thread where we get the output buffers and finish it there */
    gst_video_codec_frame_unref (frame);
    frame = NULL;
  }

  if (flow != GST_FLOW_OK)
    goto out;

  flow = g_atomic_int_get (&nvenc->last_flow);

out:

  gst_video_frame_unmap (&vframe);

  return flow;

error:
  flow = GST_FLOW_ERROR;
  if (state)
    g_free (state);
  if (input_buffer)
    g_free (input_buffer);
  goto out;
}

static gboolean
gst_nv_base_enc_drain_encoder (GstNvBaseEnc * nvenc)
{
  NV_ENC_PIC_PARAMS pic_params = { 0, };
  NVENCSTATUS nv_ret;

  GST_INFO_OBJECT (nvenc, "draining encoder");

  if (nvenc->input_state == NULL) {
    GST_DEBUG_OBJECT (nvenc, "no input state, nothing to do");
    return TRUE;
  }

  pic_params.version = gst_nvenc_get_pic_params_version ();
  pic_params.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

  nv_ret = NvEncEncodePicture (nvenc->encoder, &pic_params);
  if (nv_ret != NV_ENC_SUCCESS) {
    GST_LOG_OBJECT (nvenc, "Failed to drain encoder, ret %d", nv_ret);
    return FALSE;
  }

  return TRUE;
}

static GstFlowReturn
gst_nv_base_enc_finish (GstVideoEncoder * enc)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);

  if (!gst_nv_base_enc_drain_encoder (nvenc))
    return GST_FLOW_ERROR;

  gst_nv_base_enc_stop_bitstream_thread (nvenc, FALSE);

  return GST_FLOW_OK;
}

#if 0
static gboolean
gst_nv_base_enc_flush (GstVideoEncoder * enc)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (enc);
  GST_INFO_OBJECT (nvenc, "done flushing encoder");
  return TRUE;
}
#endif

static void
gst_nv_base_enc_schedule_reconfig (GstNvBaseEnc * nvenc)
{
  g_atomic_int_set (&nvenc->reconfig, TRUE);
}

static void
gst_nv_base_enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (object);

  switch (prop_id) {
    case PROP_PRESET:
      nvenc->preset_enum = g_value_get_enum (value);
      nvenc->selected_preset = _nv_preset_to_guid (nvenc->preset_enum);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_RC_MODE:
      nvenc->rate_control_mode = g_value_get_enum (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_QP_MIN:
      nvenc->qp_min = g_value_get_int (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_QP_MAX:
      nvenc->qp_max = g_value_get_int (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_QP_CONST:
      nvenc->qp_const = g_value_get_int (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_BITRATE:
      nvenc->bitrate = g_value_get_uint (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    case PROP_GOP_SIZE:
      nvenc->gop_size = g_value_get_int (value);
      gst_nv_base_enc_schedule_reconfig (nvenc);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_nv_base_enc_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstNvBaseEnc *nvenc = GST_NV_BASE_ENC (object);
  GstNvBaseEncClass *nvenc_class = GST_NV_BASE_ENC_GET_CLASS (object);

  switch (prop_id) {
    case PROP_DEVICE_ID:
      g_value_set_uint (value, nvenc_class->cuda_device_id);
      break;
    case PROP_PRESET:
      g_value_set_enum (value, nvenc->preset_enum);
      break;
    case PROP_RC_MODE:
      g_value_set_enum (value, nvenc->rate_control_mode);
      break;
    case PROP_QP_MIN:
      g_value_set_int (value, nvenc->qp_min);
      break;
    case PROP_QP_MAX:
      g_value_set_int (value, nvenc->qp_max);
      break;
    case PROP_QP_CONST:
      g_value_set_int (value, nvenc->qp_const);
      break;
    case PROP_BITRATE:
      g_value_set_uint (value, nvenc->bitrate);
      break;
    case PROP_GOP_SIZE:
      g_value_set_int (value, nvenc->gop_size);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

typedef struct
{
  GstCaps *sink_caps;
  GstCaps *src_caps;
  guint cuda_device_id;
  gboolean is_default;
} GstNvEncClassData;

static void
gst_nv_base_enc_subclass_init (gpointer g_class, gpointer data)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);
  GstNvBaseEncClass *nvbaseenc_class = GST_NV_BASE_ENC_CLASS (g_class);
  GstNvEncClassData *cdata = data;

  if (!cdata->is_default) {
    const gchar *long_name;
    gchar *new_long_name;

    long_name = gst_element_class_get_metadata (element_class,
        GST_ELEMENT_METADATA_LONGNAME);

    new_long_name = g_strdup_printf ("%s with devide-id %d", long_name,
        cdata->cuda_device_id);

    gst_element_class_add_metadata (element_class,
        GST_ELEMENT_METADATA_LONGNAME, new_long_name);
    g_free (new_long_name);
  }


  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
          cdata->sink_caps));
  gst_element_class_add_pad_template (element_class,
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
          cdata->src_caps));

  nvbaseenc_class->cuda_device_id = cdata->cuda_device_id;

  gst_caps_unref (cdata->sink_caps);
  gst_caps_unref (cdata->src_caps);
  g_free (cdata);
}

void
gst_nv_base_enc_register (GstPlugin * plugin, GType type, const char *codec,
    guint device_id, guint rank, GstCaps * sink_caps, GstCaps * src_caps)
{
  GTypeQuery type_query;
  GTypeInfo type_info = { 0, };
  GType subtype;
  gchar *type_name;
  GstNvEncClassData *cdata;
  gboolean is_default = TRUE;

  cdata = g_new0 (GstNvEncClassData, 1);
  cdata->sink_caps = gst_caps_ref (sink_caps);
  cdata->src_caps = gst_caps_ref (src_caps);
  cdata->cuda_device_id = device_id;

  g_type_query (type, &type_query);
  memset (&type_info, 0, sizeof (type_info));
  type_info.class_size = type_query.class_size;
  type_info.instance_size = type_query.instance_size;
  type_info.class_init = gst_nv_base_enc_subclass_init;
  type_info.class_data = cdata;

  type_name = g_strdup_printf ("nv%senc", codec);

  if (g_type_from_name (type_name) != 0) {
    g_free (type_name);
    type_name = g_strdup_printf ("nv%sdevice%denc", codec, device_id);
    is_default = FALSE;
  }

  cdata->is_default = is_default;
  subtype = g_type_register_static (type, type_name, &type_info, 0);

  /* make lower rank than default device */
  if (rank > 0 && !is_default)
    rank--;

  if (!gst_element_register (plugin, type_name, rank, subtype))
    GST_WARNING ("Failed to register plugin '%s'", type_name);

  g_free (type_name);
}
