/* GStreamer
 * Copyright (C) 2019 Seungha Yang <seungha.yang@navercorp.com>
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

#include <gst/check/gstcheck.h>
#include <gst/check/gstharness.h>
#include <string.h>

/* 64x64 h264 byte-stream format keyframe with sps/pps
 * encoded using x264enc with videotestsrc */
static guint8 h264_frame[] = {
  0x00, 0x00, 0x00, 0x01, 0x09, 0x10, 0x00, 0x00, 0x00, 0x01, 0x67, 0x64,
  0x00, 0x14, 0xac, 0xd9, 0x44, 0x26, 0xc0, 0x5a, 0x83, 0x00, 0x83, 0x52,
  0x80, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x1e, 0x47, 0x8a, 0x14,
  0xcb, 0x00, 0x00, 0x00, 0x01, 0x68, 0xeb, 0xec, 0xb2, 0x2c, 0x00, 0x00,
  0x00, 0x01, 0x06, 0x05, 0xff, 0xff, 0xf5, 0xdc, 0x45, 0xe9, 0xbd, 0xe6,
  0xd9, 0x48, 0xb7, 0x96, 0x2c, 0xd8, 0x20, 0xd9, 0x23, 0xee, 0xef, 0x78,
  0x32, 0x36, 0x34, 0x20, 0x2d, 0x20, 0x63, 0x6f, 0x72, 0x65, 0x20, 0x31,
  0x35, 0x37, 0x20, 0x72, 0x32, 0x39, 0x34, 0x35, 0x2b, 0x34, 0x35, 0x20,
  0x32, 0x37, 0x35, 0x36, 0x35, 0x64, 0x31, 0x20, 0x2d, 0x20, 0x48, 0x2e,
  0x32, 0x36, 0x34, 0x2f, 0x4d, 0x50, 0x45, 0x47, 0x2d, 0x34, 0x20, 0x41,
  0x56, 0x43, 0x20, 0x63, 0x6f, 0x64, 0x65, 0x63, 0x20, 0x2d, 0x20, 0x43,
  0x6f, 0x70, 0x79, 0x6c, 0x65, 0x66, 0x74, 0x20, 0x32, 0x30, 0x30, 0x33,
  0x2d, 0x32, 0x30, 0x31, 0x38, 0x20, 0x2d, 0x20, 0x68, 0x74, 0x74, 0x70,
  0x3a, 0x2f, 0x2f, 0x77, 0x77, 0x77, 0x2e, 0x76, 0x69, 0x64, 0x65, 0x6f,
  0x6c, 0x61, 0x6e, 0x2e, 0x6f, 0x72, 0x67, 0x2f, 0x78, 0x32, 0x36, 0x34,
  0x2e, 0x68, 0x74, 0x6d, 0x6c, 0x20, 0x2d, 0x20, 0x6f, 0x70, 0x74, 0x69,
  0x6f, 0x6e, 0x73, 0x3a, 0x20, 0x63, 0x61, 0x62, 0x61, 0x63, 0x3d, 0x31,
  0x20, 0x72, 0x65, 0x66, 0x3d, 0x33, 0x20, 0x64, 0x65, 0x62, 0x6c, 0x6f,
  0x63, 0x6b, 0x3d, 0x31, 0x3a, 0x30, 0x3a, 0x30, 0x20, 0x61, 0x6e, 0x61,
  0x6c, 0x79, 0x73, 0x65, 0x3d, 0x30, 0x78, 0x33, 0x3a, 0x30, 0x78, 0x31,
  0x31, 0x33, 0x20, 0x6d, 0x65, 0x3d, 0x68, 0x65, 0x78, 0x20, 0x73, 0x75,
  0x62, 0x6d, 0x65, 0x3d, 0x37, 0x20, 0x70, 0x73, 0x79, 0x3d, 0x31, 0x20,
  0x70, 0x73, 0x79, 0x5f, 0x72, 0x64, 0x3d, 0x31, 0x2e, 0x30, 0x30, 0x3a,
  0x30, 0x2e, 0x30, 0x30, 0x20, 0x6d, 0x69, 0x78, 0x65, 0x64, 0x5f, 0x72,
  0x65, 0x66, 0x3d, 0x31, 0x20, 0x6d, 0x65, 0x5f, 0x72, 0x61, 0x6e, 0x67,
  0x65, 0x3d, 0x31, 0x36, 0x20, 0x63, 0x68, 0x72, 0x6f, 0x6d, 0x61, 0x5f,
  0x6d, 0x65, 0x3d, 0x31, 0x20, 0x74, 0x72, 0x65, 0x6c, 0x6c, 0x69, 0x73,
  0x3d, 0x31, 0x20, 0x38, 0x78, 0x38, 0x64, 0x63, 0x74, 0x3d, 0x31, 0x20,
  0x63, 0x71, 0x6d, 0x3d, 0x30, 0x20, 0x64, 0x65, 0x61, 0x64, 0x7a, 0x6f,
  0x6e, 0x65, 0x3d, 0x32, 0x31, 0x2c, 0x31, 0x31, 0x20, 0x66, 0x61, 0x73,
  0x74, 0x5f, 0x70, 0x73, 0x6b, 0x69, 0x70, 0x3d, 0x31, 0x20, 0x63, 0x68,
  0x72, 0x6f, 0x6d, 0x61, 0x5f, 0x71, 0x70, 0x5f, 0x6f, 0x66, 0x66, 0x73,
  0x65, 0x74, 0x3d, 0x2d, 0x32, 0x20, 0x74, 0x68, 0x72, 0x65, 0x61, 0x64,
  0x73, 0x3d, 0x32, 0x20, 0x6c, 0x6f, 0x6f, 0x6b, 0x61, 0x68, 0x65, 0x61,
  0x64, 0x5f, 0x74, 0x68, 0x72, 0x65, 0x61, 0x64, 0x73, 0x3d, 0x31, 0x20,
  0x73, 0x6c, 0x69, 0x63, 0x65, 0x64, 0x5f, 0x74, 0x68, 0x72, 0x65, 0x61,
  0x64, 0x73, 0x3d, 0x30, 0x20, 0x6e, 0x72, 0x3d, 0x30, 0x20, 0x64, 0x65,
  0x63, 0x69, 0x6d, 0x61, 0x74, 0x65, 0x3d, 0x31, 0x20, 0x69, 0x6e, 0x74,
  0x65, 0x72, 0x6c, 0x61, 0x63, 0x65, 0x64, 0x3d, 0x30, 0x20, 0x62, 0x6c,
  0x75, 0x72, 0x61, 0x79, 0x5f, 0x63, 0x6f, 0x6d, 0x70, 0x61, 0x74, 0x3d,
  0x30, 0x20, 0x63, 0x6f, 0x6e, 0x73, 0x74, 0x72, 0x61, 0x69, 0x6e, 0x65,
  0x64, 0x5f, 0x69, 0x6e, 0x74, 0x72, 0x61, 0x3d, 0x30, 0x20, 0x62, 0x66,
  0x72, 0x61, 0x6d, 0x65, 0x73, 0x3d, 0x33, 0x20, 0x62, 0x5f, 0x70, 0x79,
  0x72, 0x61, 0x6d, 0x69, 0x64, 0x3d, 0x32, 0x20, 0x62, 0x5f, 0x61, 0x64,
  0x61, 0x70, 0x74, 0x3d, 0x31, 0x20, 0x62, 0x5f, 0x62, 0x69, 0x61, 0x73,
  0x3d, 0x30, 0x20, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x3d, 0x31, 0x20,
  0x77, 0x65, 0x69, 0x67, 0x68, 0x74, 0x62, 0x3d, 0x31, 0x20, 0x6f, 0x70,
  0x65, 0x6e, 0x5f, 0x67, 0x6f, 0x70, 0x3d, 0x30, 0x20, 0x77, 0x65, 0x69,
  0x67, 0x68, 0x74, 0x70, 0x3d, 0x32, 0x20, 0x6b, 0x65, 0x79, 0x69, 0x6e,
  0x74, 0x3d, 0x33, 0x30, 0x30, 0x20, 0x6b, 0x65, 0x79, 0x69, 0x6e, 0x74,
  0x5f, 0x6d, 0x69, 0x6e, 0x3d, 0x33, 0x30, 0x20, 0x73, 0x63, 0x65, 0x6e,
  0x65, 0x63, 0x75, 0x74, 0x3d, 0x34, 0x30, 0x20, 0x69, 0x6e, 0x74, 0x72,
  0x61, 0x5f, 0x72, 0x65, 0x66, 0x72, 0x65, 0x73, 0x68, 0x3d, 0x30, 0x20,
  0x72, 0x63, 0x5f, 0x6c, 0x6f, 0x6f, 0x6b, 0x61, 0x68, 0x65, 0x61, 0x64,
  0x3d, 0x34, 0x30, 0x20, 0x72, 0x63, 0x3d, 0x63, 0x62, 0x72, 0x20, 0x6d,
  0x62, 0x74, 0x72, 0x65, 0x65, 0x3d, 0x31, 0x20, 0x62, 0x69, 0x74, 0x72,
  0x61, 0x74, 0x65, 0x3d, 0x32, 0x30, 0x34, 0x38, 0x20, 0x72, 0x61, 0x74,
  0x65, 0x74, 0x6f, 0x6c, 0x3d, 0x31, 0x2e, 0x30, 0x20, 0x71, 0x63, 0x6f,
  0x6d, 0x70, 0x3d, 0x30, 0x2e, 0x36, 0x30, 0x20, 0x71, 0x70, 0x6d, 0x69,
  0x6e, 0x3d, 0x30, 0x20, 0x71, 0x70, 0x6d, 0x61, 0x78, 0x3d, 0x36, 0x39,
  0x20, 0x71, 0x70, 0x73, 0x74, 0x65, 0x70, 0x3d, 0x34, 0x20, 0x76, 0x62,
  0x76, 0x5f, 0x6d, 0x61, 0x78, 0x72, 0x61, 0x74, 0x65, 0x3d, 0x32, 0x30,
  0x34, 0x38, 0x20, 0x76, 0x62, 0x76, 0x5f, 0x62, 0x75, 0x66, 0x73, 0x69,
  0x7a, 0x65, 0x3d, 0x31, 0x32, 0x32, 0x38, 0x20, 0x6e, 0x61, 0x6c, 0x5f,
  0x68, 0x72, 0x64, 0x3d, 0x6e, 0x6f, 0x6e, 0x65, 0x20, 0x66, 0x69, 0x6c,
  0x6c, 0x65, 0x72, 0x3d, 0x30, 0x20, 0x69, 0x70, 0x5f, 0x72, 0x61, 0x74,
  0x69, 0x6f, 0x3d, 0x31, 0x2e, 0x34, 0x30, 0x20, 0x61, 0x71, 0x3d, 0x31,
  0x3a, 0x31, 0x2e, 0x30, 0x30, 0x00, 0x80, 0x00, 0x00, 0x00, 0x01, 0x65,
  0x88, 0x84, 0x00, 0x1a, 0xff, 0xfe, 0xf7, 0xd4, 0xb7, 0xcc, 0xb2, 0xee,
  0x07, 0x22, 0xb6, 0x0b, 0xa8, 0xf7, 0xa2, 0x9c, 0x8c, 0x81, 0xee, 0x0c,
  0x19, 0x51, 0xa2, 0x7c, 0x81, 0xbd, 0x7b, 0x46, 0xcd
};

static guint h264_frame_len = 849;

#define MAX_PUSH_BUFFER 64

GST_START_TEST (test_downstream_reconfigure)
{
  GstHarness *h;
  GstElement *capsfilter;
  GstCaps *caps;
  GstBuffer *in_buf, *out_buf = NULL;
  GstMapInfo map;
  GstFlowReturn ret;
  gint i = 0;

  h = gst_harness_new_parse ("h264parse ! nvh264dec ! capsfilter");
  fail_unless (h != NULL);

  capsfilter = gst_harness_find_element (h, "capsfilter");

  gst_harness_play (h);

  gst_harness_set_src_caps_str (h, "video/x-h264,width=64,height=64,"
      "stream-format=byte-stream,alignment=au");

  /* enforce system memory */
  caps = gst_caps_from_string ("video/x-raw");
  g_object_set (capsfilter, "caps", caps, NULL);
  gst_caps_unref (caps);

  in_buf = gst_buffer_new_and_alloc (h264_frame_len);
  gst_buffer_map (in_buf, &map, GST_MAP_WRITE);
  memcpy (map.data, h264_frame, h264_frame_len);
  gst_buffer_unmap (in_buf, &map);

  GST_BUFFER_DURATION (in_buf) = GST_SECOND;

  /* Push bufffers until get decoder output */
  do {
    fail_if (i > MAX_PUSH_BUFFER);

    GST_BUFFER_PTS (in_buf) = GST_BUFFER_DTS (in_buf) = i * GST_SECOND;

    ret = gst_harness_push (h, gst_buffer_ref (in_buf));
    fail_unless (ret == GST_FLOW_OK, "GstFlowReturn was %s",
        gst_flow_get_name (ret));

    out_buf = gst_harness_try_pull (h);
    i++;
  } while (out_buf == NULL);
  gst_buffer_unref (out_buf);

  /* Use opengl memory */
  caps = gst_caps_from_string ("video/x-raw(memory:GLMemory)");
  g_object_set (capsfilter, "caps", caps, NULL);
  gst_caps_unref (caps);

  do {
    fail_if (i > 2 * MAX_PUSH_BUFFER);

    GST_BUFFER_PTS (in_buf) = GST_BUFFER_DTS (in_buf) = i * GST_SECOND;

    ret = gst_harness_push (h, gst_buffer_ref (in_buf));
    fail_unless (ret == GST_FLOW_OK, "GstFlowReturn was %s",
        gst_flow_get_name (ret));

    out_buf = gst_harness_try_pull (h);
    i++;
  } while (out_buf == NULL);
  gst_buffer_unref (in_buf);
  gst_buffer_unref (out_buf);

  gst_object_unref (capsfilter);
  gst_harness_teardown (h);
}

GST_END_TEST;

static gboolean
check_nvcodec_available (void)
{
  gboolean ret = TRUE;
  GstElement *nvdec;

  nvdec = gst_element_factory_make ("nvh264dec", NULL);
  if (!nvdec) {
    GST_WARNING ("nvh264dec is not available, possibly driver load failure");
    return FALSE;
  }

  /* GST_STATE_READY is meaning that driver could be loaded */
  if (gst_element_set_state (nvdec,
          GST_STATE_PAUSED) != GST_STATE_CHANGE_SUCCESS) {
    GST_WARNING ("cannot open device");
    ret = FALSE;
  }

  gst_element_set_state (nvdec, GST_STATE_NULL);
  gst_object_unref (nvdec);

  return ret;
}

static Suite *
nvdec_suite (void)
{
  Suite *s;
  TCase *tc_chain;

  /* HACK: cuda device init/deinit with fork seems to problematic */
  g_setenv ("CK_FORK", "no", TRUE);

  s = suite_create ("nvdec");
  tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);

  if (!check_nvcodec_available ()) {
    GST_DEBUG ("Skip nvdec test since cannot open device");
    goto end;
  }

  tcase_add_test (tc_chain, test_downstream_reconfigure);

end:
  return s;
}

GST_CHECK_MAIN (nvdec);
