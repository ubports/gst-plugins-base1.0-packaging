/* GStreamer
 * Copyright (C) 2003 Benjamin Otte <in7y118@public.uni-hamburg.de>
 * Copyright (C) 2005 Thomas Vander Stichele <thomas at apestaart dot org>
 * Copyright (C) 2011 Wim Taymans <wim.taymans at gmail dot com>
 *
 * gstaudioconvert.c: Convert audio to different audio formats automatically
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

/**
 * SECTION:element-audioconvert
 *
 * Audioconvert converts raw audio buffers between various possible formats.
 * It supports integer to float conversion, width/depth conversion,
 * signedness and endianness conversion and channel transformations
 * (ie. upmixing and downmixing), as well as dithering and noise-shaping.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 -v -m audiotestsrc ! audioconvert ! audio/x-raw,format=S8,channels=2 ! level ! fakesink silent=TRUE
 * ]| This pipeline converts audio to 8-bit.  The level element shows that
 * the output levels still match the one for a sine wave.
 * |[
 * gst-launch-1.0 -v -m uridecodebin uri=file:///path/to/audio.flac ! audioconvert ! vorbisenc ! oggmux ! filesink location=audio.ogg
 * ]| The vorbis encoder takes float audio data instead of the integer data
 * output by most other audio elements. This pipeline decodes a FLAC audio file
 * (or any other audio file for which decoders are installed) and re-encodes
 * it into an Ogg/Vorbis audio file.
 * </refsect2>
 */

/*
 * design decisions:
 * - audioconvert converts buffers in a set of supported caps. If it supports
 *   a caps, it supports conversion from these caps to any other caps it
 *   supports. (example: if it does A=>B and A=>C, it also does B=>C)
 * - audioconvert does not save state between buffers. Every incoming buffer is
 *   converted and the converted buffer is pushed out.
 * conclusion:
 * audioconvert is not supposed to be a one-element-does-anything solution for
 * audio conversions.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "gstaudioconvert.h"
#include "plugin.h"

GST_DEBUG_CATEGORY (audio_convert_debug);
GST_DEBUG_CATEGORY_STATIC (GST_CAT_PERFORMANCE);
#define GST_CAT_DEFAULT (audio_convert_debug)

/*** DEFINITIONS **************************************************************/

/* type functions */
static void gst_audio_convert_dispose (GObject * obj);

/* gstreamer functions */
static gboolean gst_audio_convert_get_unit_size (GstBaseTransform * base,
    GstCaps * caps, gsize * size);
static GstCaps *gst_audio_convert_transform_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static GstCaps *gst_audio_convert_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps);
static gboolean gst_audio_convert_set_caps (GstBaseTransform * base,
    GstCaps * incaps, GstCaps * outcaps);
static GstFlowReturn gst_audio_convert_transform (GstBaseTransform * base,
    GstBuffer * inbuf, GstBuffer * outbuf);
static gboolean gst_audio_convert_transform_meta (GstBaseTransform * trans,
    GstBuffer * outbuf, GstMeta * meta, GstBuffer * inbuf);
static GstFlowReturn gst_audio_convert_submit_input_buffer (GstBaseTransform *
    base, gboolean is_discont, GstBuffer * input);
static void gst_audio_convert_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_audio_convert_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

/* AudioConvert signals and args */
enum
{
  /* FILL ME */
  LAST_SIGNAL
};

enum
{
  PROP_0,
  PROP_DITHERING,
  PROP_NOISE_SHAPING,
};

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (audio_convert_debug, "audioconvert", 0, "audio conversion element"); \
  GST_DEBUG_CATEGORY_GET (GST_CAT_PERFORMANCE, "GST_PERFORMANCE");
#define gst_audio_convert_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstAudioConvert, gst_audio_convert,
    GST_TYPE_BASE_TRANSFORM, DEBUG_INIT);

/*** GSTREAMER PROTOTYPES *****************************************************/

#define STATIC_CAPS \
GST_STATIC_CAPS (GST_AUDIO_CAPS_MAKE (GST_AUDIO_FORMATS_ALL) \
    ", layout = (string) interleaved")

static GstStaticPadTemplate gst_audio_convert_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    STATIC_CAPS);

static GstStaticPadTemplate gst_audio_convert_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    STATIC_CAPS);


/*** TYPE FUNCTIONS ***********************************************************/
static void
gst_audio_convert_class_init (GstAudioConvertClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *basetransform_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->dispose = gst_audio_convert_dispose;
  gobject_class->set_property = gst_audio_convert_set_property;
  gobject_class->get_property = gst_audio_convert_get_property;

  g_object_class_install_property (gobject_class, PROP_DITHERING,
      g_param_spec_enum ("dithering", "Dithering",
          "Selects between different dithering methods.",
          GST_TYPE_AUDIO_DITHER_METHOD, GST_AUDIO_DITHER_TPDF,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_NOISE_SHAPING,
      g_param_spec_enum ("noise-shaping", "Noise shaping",
          "Selects between different noise shaping methods.",
          GST_TYPE_AUDIO_NOISE_SHAPING_METHOD, GST_AUDIO_NOISE_SHAPING_NONE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_audio_convert_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_audio_convert_sink_template));
  gst_element_class_set_static_metadata (element_class,
      "Audio converter", "Filter/Converter/Audio",
      "Convert audio to different formats", "Benjamin Otte <otte@gnome.org>");

  basetransform_class->get_unit_size =
      GST_DEBUG_FUNCPTR (gst_audio_convert_get_unit_size);
  basetransform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_audio_convert_transform_caps);
  basetransform_class->fixate_caps =
      GST_DEBUG_FUNCPTR (gst_audio_convert_fixate_caps);
  basetransform_class->set_caps =
      GST_DEBUG_FUNCPTR (gst_audio_convert_set_caps);
  basetransform_class->transform =
      GST_DEBUG_FUNCPTR (gst_audio_convert_transform);
  basetransform_class->transform_meta =
      GST_DEBUG_FUNCPTR (gst_audio_convert_transform_meta);
  basetransform_class->submit_input_buffer =
      GST_DEBUG_FUNCPTR (gst_audio_convert_submit_input_buffer);

  basetransform_class->passthrough_on_same_caps = TRUE;
}

static void
gst_audio_convert_init (GstAudioConvert * this)
{
  this->dither = GST_AUDIO_DITHER_TPDF;
  this->ns = GST_AUDIO_NOISE_SHAPING_NONE;

  gst_base_transform_set_gap_aware (GST_BASE_TRANSFORM (this), TRUE);
}

static void
gst_audio_convert_dispose (GObject * obj)
{
  GstAudioConvert *this = GST_AUDIO_CONVERT (obj);

  if (this->convert) {
    gst_audio_converter_free (this->convert);
    this->convert = NULL;
  }

  G_OBJECT_CLASS (parent_class)->dispose (obj);
}

/*** GSTREAMER FUNCTIONS ******************************************************/

/* BaseTransform vmethods */
static gboolean
gst_audio_convert_get_unit_size (GstBaseTransform * base, GstCaps * caps,
    gsize * size)
{
  GstAudioInfo info;

  g_assert (size);

  if (!gst_audio_info_from_caps (&info, caps))
    goto parse_error;

  *size = info.bpf;
  GST_INFO_OBJECT (base, "unit_size = %" G_GSIZE_FORMAT, *size);

  return TRUE;

parse_error:
  {
    GST_INFO_OBJECT (base, "failed to parse caps to get unit_size");
    return FALSE;
  }
}

/* copies the given caps */
static GstCaps *
gst_audio_convert_caps_remove_format_info (GstCaps * caps, gboolean channels)
{
  GstStructure *st;
  gint i, n;
  GstCaps *res;
  guint64 channel_mask;

  res = gst_caps_new_empty ();

  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    gboolean remove_channels = FALSE;

    st = gst_caps_get_structure (caps, i);

    /* If this is already expressed by the existing caps
     * skip this structure */
    if (i > 0 && gst_caps_is_subset_structure (res, st))
      continue;

    st = gst_structure_copy (st);
    gst_structure_remove_field (st, "format");

    /* Only remove the channels and channel-mask for non-NONE layouts */
    if (gst_structure_get (st, "channel-mask", GST_TYPE_BITMASK, &channel_mask,
            NULL)) {
      if (channel_mask != 0)
        remove_channels = TRUE;
    } else {
      remove_channels = TRUE;
    }

    if (remove_channels && channels)
      gst_structure_remove_fields (st, "channel-mask", "channels", NULL);

    gst_caps_append_structure (res, st);
  }

  return res;
}

/* The caps can be transformed into any other caps with format info removed.
 * However, we should prefer passthrough, so if passthrough is possible,
 * put it first in the list. */
static GstCaps *
gst_audio_convert_transform_caps (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *tmp, *tmp2;
  GstCaps *result;

  /* Get all possible caps that we can transform to */
  tmp = gst_audio_convert_caps_remove_format_info (caps, TRUE);

  if (filter) {
    tmp2 = gst_caps_intersect_full (filter, tmp, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (tmp);
    tmp = tmp2;
  }

  result = tmp;

  GST_DEBUG_OBJECT (btrans, "transformed %" GST_PTR_FORMAT " into %"
      GST_PTR_FORMAT, caps, result);

  return result;
}

static gint
n_bits_set (guint64 x)
{
  gint i;
  gint c = 0;
  guint64 y = 1;

  for (i = 0; i < 64; i++) {
    if (x & y)
      c++;
    y <<= 1;
  }

  return c;
}

static guint64
find_suitable_mask (guint64 mask, gint n_chans)
{
  guint64 intersection;
  gint i;

  i = 0;

  g_assert (n_bits_set (mask) >= n_chans);

  intersection = mask;
  do {
    intersection = intersection & ((~G_GUINT64_CONSTANT (0)) >> i);
    i++;
  } while (n_bits_set (intersection) > n_chans && i < 64);

  if (i < 64)
    return intersection;
  return 0;
}

static void
gst_audio_convert_fixate_format (GstBaseTransform * base, GstStructure * ins,
    GstStructure * outs)
{
  const gchar *in_format;
  const GValue *format;
  const GstAudioFormatInfo *in_info, *out_info = NULL;
  GstAudioFormatFlags in_flags, out_flags = 0;
  gint in_depth, out_depth = -1;
  gint i, len;

  in_format = gst_structure_get_string (ins, "format");
  if (!in_format)
    return;

  format = gst_structure_get_value (outs, "format");
  /* should not happen */
  if (format == NULL)
    return;

  /* nothing to fixate? */
  if (!GST_VALUE_HOLDS_LIST (format))
    return;

  in_info =
      gst_audio_format_get_info (gst_audio_format_from_string (in_format));
  if (!in_info)
    return;

  in_flags = GST_AUDIO_FORMAT_INFO_FLAGS (in_info);
  in_flags &= ~(GST_AUDIO_FORMAT_FLAG_UNPACK);
  in_flags &= ~(GST_AUDIO_FORMAT_FLAG_SIGNED);

  in_depth = GST_AUDIO_FORMAT_INFO_DEPTH (in_info);

  len = gst_value_list_get_size (format);
  for (i = 0; i < len; i++) {
    const GstAudioFormatInfo *t_info;
    GstAudioFormatFlags t_flags;
    gboolean t_flags_better;
    const GValue *val;
    const gchar *fname;
    gint t_depth;

    val = gst_value_list_get_value (format, i);
    if (!G_VALUE_HOLDS_STRING (val))
      continue;

    fname = g_value_get_string (val);
    t_info = gst_audio_format_get_info (gst_audio_format_from_string (fname));
    if (!t_info)
      continue;

    /* accept input format immediately */
    if (strcmp (fname, in_format) == 0) {
      out_info = t_info;
      break;
    }

    t_flags = GST_AUDIO_FORMAT_INFO_FLAGS (t_info);
    t_flags &= ~(GST_AUDIO_FORMAT_FLAG_UNPACK);
    t_flags &= ~(GST_AUDIO_FORMAT_FLAG_SIGNED);

    t_depth = GST_AUDIO_FORMAT_INFO_DEPTH (t_info);

    /* Any output format is better than no output format at all */
    if (!out_info) {
      out_info = t_info;
      out_depth = t_depth;
      out_flags = t_flags;
      continue;
    }

    t_flags_better = (t_flags == in_flags && out_flags != in_flags);

    if (t_depth == in_depth && (out_depth != in_depth || t_flags_better)) {
      /* Prefer to use the first format that has the same depth with the same
       * flags, and if none with the same flags exist use the first other one
       * that has the same depth */
      out_info = t_info;
      out_depth = t_depth;
      out_flags = t_flags;
    } else if (t_depth >= in_depth && (in_depth > out_depth
            || (out_depth >= in_depth && t_flags_better))) {
      /* Otherwise use the first format that has a higher depth with the same flags,
       * if none with the same flags exist use the first other one that has a higher
       * depth */
      out_info = t_info;
      out_depth = t_depth;
      out_flags = t_flags;
    } else if ((t_depth > out_depth && out_depth < in_depth)
        || (t_flags_better && out_depth == t_depth)) {
      /* Else get at least the one with the highest depth, ideally with the same flags */
      out_info = t_info;
      out_depth = t_depth;
      out_flags = t_flags;
    }

  }

  if (out_info)
    gst_structure_set (outs, "format", G_TYPE_STRING,
        GST_AUDIO_FORMAT_INFO_NAME (out_info), NULL);
}

static void
gst_audio_convert_fixate_channels (GstBaseTransform * base, GstStructure * ins,
    GstStructure * outs)
{
  gint in_chans, out_chans;
  guint64 in_mask = 0, out_mask = 0;
  gboolean has_in_mask = FALSE, has_out_mask = FALSE;

  if (!gst_structure_get_int (ins, "channels", &in_chans))
    return;                     /* this shouldn't really happen, should it? */

  if (!gst_structure_has_field (outs, "channels")) {
    /* we could try to get the implied number of channels from the layout,
     * but that seems overdoing it for a somewhat exotic corner case */
    gst_structure_remove_field (outs, "channel-mask");
    return;
  }

  /* ok, let's fixate the channels if they are not fixated yet */
  gst_structure_fixate_field_nearest_int (outs, "channels", in_chans);

  if (!gst_structure_get_int (outs, "channels", &out_chans)) {
    /* shouldn't really happen ... */
    gst_structure_remove_field (outs, "channel-mask");
    return;
  }

  /* get the channel layout of the output if any */
  has_out_mask = gst_structure_has_field (outs, "channel-mask");
  if (has_out_mask) {
    gst_structure_get (outs, "channel-mask", GST_TYPE_BITMASK, &out_mask, NULL);
  } else {
    /* channels == 1 => MONO */
    if (out_chans == 2) {
      out_mask =
          GST_AUDIO_CHANNEL_POSITION_MASK (FRONT_LEFT) |
          GST_AUDIO_CHANNEL_POSITION_MASK (FRONT_RIGHT);
      has_out_mask = TRUE;
      gst_structure_set (outs, "channel-mask", GST_TYPE_BITMASK, out_mask,
          NULL);
    }
  }

  /* get the channel layout of the input if any */
  has_in_mask = gst_structure_has_field (ins, "channel-mask");
  if (has_in_mask) {
    gst_structure_get (ins, "channel-mask", GST_TYPE_BITMASK, &in_mask, NULL);
  } else {
    /* channels == 1 => MONO */
    if (in_chans == 2) {
      in_mask =
          GST_AUDIO_CHANNEL_POSITION_MASK (FRONT_LEFT) |
          GST_AUDIO_CHANNEL_POSITION_MASK (FRONT_RIGHT);
      has_in_mask = TRUE;
    } else if (in_chans > 2)
      g_warning ("%s: Upstream caps contain no channel mask",
          GST_ELEMENT_NAME (base));
  }

  if (!has_out_mask && out_chans == 1 && (in_chans != out_chans
          || !has_in_mask))
    return;                     /* nothing to do, default layout will be assumed */

  if (in_chans == out_chans && (has_in_mask || in_chans == 1)) {
    /* same number of channels and no output layout: just use input layout */
    if (!has_out_mask) {
      /* in_chans == 1 handled above already */
      gst_structure_set (outs, "channel-mask", GST_TYPE_BITMASK, in_mask, NULL);
      return;
    }

    /* If both masks are the same we're done, this includes the NONE layout case */
    if (in_mask == out_mask)
      return;

    /* if output layout is fixed already and looks sane, we're done */
    if (n_bits_set (out_mask) == out_chans)
      return;

    if (n_bits_set (out_mask) < in_chans) {
      /* Not much we can do here, this shouldn't just happen */
      g_warning ("%s: Invalid downstream channel-mask with too few bits set",
          GST_ELEMENT_NAME (base));
    } else {
      guint64 intersection;

      /* if the output layout is not fixed, check if the output layout contains
       * the input layout */
      intersection = in_mask & out_mask;
      if (n_bits_set (intersection) >= in_chans) {
        gst_structure_set (outs, "channel-mask", GST_TYPE_BITMASK, in_mask,
            NULL);
        return;
      }

      /* output layout is not fixed and does not contain the input layout, so
       * just pick the first possibility */
      intersection = find_suitable_mask (out_mask, out_chans);
      if (intersection) {
        gst_structure_set (outs, "channel-mask", GST_TYPE_BITMASK, intersection,
            NULL);
        return;
      }
    }

    /* ... else fall back to default layout (NB: out_layout is NULL here) */
    GST_WARNING_OBJECT (base, "unexpected output channel layout");
  } else {
    guint64 intersection;

    /* number of input channels != number of output channels:
     * if this value contains a list of channel layouts (or even worse: a list
     * with another list), just pick the first value and repeat until we find a
     * channel position array or something else that's not a list; we assume
     * the input if half-way sane and don't try to fall back on other list items
     * if the first one is something unexpected or non-channel-pos-array-y */
    if (n_bits_set (out_mask) >= out_chans) {
      intersection = find_suitable_mask (out_mask, out_chans);
      gst_structure_set (outs, "channel-mask", GST_TYPE_BITMASK, intersection,
          NULL);
      return;
    }

    /* what now?! Just ignore what we're given and use default positions */
    GST_WARNING_OBJECT (base, "invalid or unexpected channel-positions");
  }

  /* missing or invalid output layout and we can't use the input layout for
   * one reason or another, so just pick a default layout (we could be smarter
   * and try to add/remove channels from the input layout, or pick a default
   * layout based on LFE-presence in input layout, but let's save that for
   * another day). For mono, no mask is required and the fallback mask is 0 */
  if (out_chans > 1
      && (out_mask = gst_audio_channel_get_fallback_mask (out_chans))) {
    GST_DEBUG_OBJECT (base, "using default channel layout as fallback");
    gst_structure_set (outs, "channel-mask", GST_TYPE_BITMASK, out_mask, NULL);
  } else if (out_chans > 1) {
    GST_ERROR_OBJECT (base, "Have no default layout for %d channels",
        out_chans);
  }
}

/* try to keep as many of the structure members the same by fixating the
 * possible ranges; this way we convert the least amount of things as possible
 */
static GstCaps *
gst_audio_convert_fixate_caps (GstBaseTransform * base,
    GstPadDirection direction, GstCaps * caps, GstCaps * othercaps)
{
  GstStructure *ins, *outs;
  GstCaps *result;

  GST_DEBUG_OBJECT (base, "trying to fixate othercaps %" GST_PTR_FORMAT
      " based on caps %" GST_PTR_FORMAT, othercaps, caps);

  result = gst_caps_intersect (othercaps, caps);
  if (gst_caps_is_empty (result)) {
    GstCaps *removed;

    if (result)
      gst_caps_unref (result);
    /* try to preserve channels */
    removed = gst_audio_convert_caps_remove_format_info (caps, FALSE);
    result = gst_caps_intersect (othercaps, removed);
    gst_caps_unref (removed);
    if (gst_caps_is_empty (result)) {
      if (result)
        gst_caps_unref (result);
      result = othercaps;
    } else {
      gst_caps_unref (othercaps);
    }
  } else {
    gst_caps_unref (othercaps);
  }

  GST_DEBUG_OBJECT (base, "now fixating %" GST_PTR_FORMAT, result);

  /* fixate remaining fields */
  result = gst_caps_make_writable (result);

  ins = gst_caps_get_structure (caps, 0);
  outs = gst_caps_get_structure (result, 0);

  gst_audio_convert_fixate_channels (base, ins, outs);
  gst_audio_convert_fixate_format (base, ins, outs);

  /* fixate remaining */
  result = gst_caps_fixate (result);

  GST_DEBUG_OBJECT (base, "fixated othercaps to %" GST_PTR_FORMAT, result);

  return result;
}

static gboolean
gst_audio_convert_set_caps (GstBaseTransform * base, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstAudioConvert *this = GST_AUDIO_CONVERT (base);
  GstAudioInfo in_info;
  GstAudioInfo out_info;

  GST_DEBUG_OBJECT (base, "incaps %" GST_PTR_FORMAT ", outcaps %"
      GST_PTR_FORMAT, incaps, outcaps);

  if (this->convert) {
    gst_audio_converter_free (this->convert);
    this->convert = NULL;
  }

  if (!gst_audio_info_from_caps (&in_info, incaps))
    goto invalid_in;
  if (!gst_audio_info_from_caps (&out_info, outcaps))
    goto invalid_out;

  this->convert = gst_audio_converter_new (0, &in_info, &out_info,
      gst_structure_new ("GstAudioConverterConfig",
          GST_AUDIO_CONVERTER_OPT_DITHER_METHOD, GST_TYPE_AUDIO_DITHER_METHOD,
          this->dither,
          GST_AUDIO_CONVERTER_OPT_NOISE_SHAPING_METHOD,
          GST_TYPE_AUDIO_NOISE_SHAPING_METHOD, this->ns, NULL));

  if (this->convert == NULL)
    goto no_converter;

  this->in_info = in_info;
  this->out_info = out_info;

  return TRUE;

  /* ERRORS */
invalid_in:
  {
    GST_ERROR_OBJECT (base, "invalid input caps");
    return FALSE;
  }
invalid_out:
  {
    GST_ERROR_OBJECT (base, "invalid output caps");
    return FALSE;
  }
no_converter:
  {
    GST_ERROR_OBJECT (base, "could not make converter");
    return FALSE;
  }
}

static GstFlowReturn
gst_audio_convert_transform (GstBaseTransform * base, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstFlowReturn ret;
  GstAudioConvert *this = GST_AUDIO_CONVERT (base);
  GstMapInfo srcmap, dstmap;
  gint insize, outsize;
  gboolean inbuf_writable;
  GstAudioConverterFlags flags;
  gsize samples;

  /* get amount of samples to convert. */
  samples = gst_buffer_get_size (inbuf) / this->in_info.bpf;

  /* get in/output sizes, to see if the buffers we got are of correct
   * sizes */
  insize = samples * this->in_info.bpf;
  outsize = samples * this->out_info.bpf;

  if (insize == 0 || outsize == 0)
    return GST_FLOW_OK;

  inbuf_writable = gst_buffer_is_writable (inbuf)
      && gst_buffer_n_memory (inbuf) == 1
      && gst_memory_is_writable (gst_buffer_peek_memory (inbuf, 0));

  /* get src and dst data */
  gst_buffer_map (inbuf, &srcmap,
      inbuf_writable ? GST_MAP_READWRITE : GST_MAP_READ);
  gst_buffer_map (outbuf, &dstmap, GST_MAP_WRITE);

  /* check in and outsize */
  if (srcmap.size < insize)
    goto wrong_size;
  if (dstmap.size < outsize)
    goto wrong_size;

  /* and convert the samples */
  flags = 0;
  if (inbuf_writable)
    flags |= GST_AUDIO_CONVERTER_FLAG_IN_WRITABLE;

  if (!GST_BUFFER_FLAG_IS_SET (inbuf, GST_BUFFER_FLAG_GAP)) {
    gpointer in[1] = { srcmap.data };
    gpointer out[1] = { dstmap.data };

    if (!gst_audio_converter_samples (this->convert, flags,
            in, samples, out, samples))
      goto convert_error;
  } else {
    /* Create silence buffer */
    gst_audio_format_fill_silence (this->out_info.finfo, dstmap.data, outsize);
  }
  ret = GST_FLOW_OK;

done:
  gst_buffer_unmap (outbuf, &dstmap);
  gst_buffer_unmap (inbuf, &srcmap);

  return ret;

  /* ERRORS */
wrong_size:
  {
    GST_ELEMENT_ERROR (this, STREAM, FORMAT,
        (NULL),
        ("input/output buffers are of wrong size in: %" G_GSIZE_FORMAT " < %d"
            " or out: %" G_GSIZE_FORMAT " < %d",
            srcmap.size, insize, dstmap.size, outsize));
    ret = GST_FLOW_ERROR;
    goto done;
  }
convert_error:
  {
    GST_ELEMENT_ERROR (this, STREAM, FORMAT,
        (NULL), ("error while converting"));
    ret = GST_FLOW_ERROR;
    goto done;
  }
}

static gboolean
gst_audio_convert_transform_meta (GstBaseTransform * trans, GstBuffer * outbuf,
    GstMeta * meta, GstBuffer * inbuf)
{
  const GstMetaInfo *info = meta->info;
  const gchar *const *tags;

  tags = gst_meta_api_type_get_tags (info->api);

  if (!tags || (g_strv_length ((gchar **) tags) == 1
          && gst_meta_api_type_has_tag (info->api,
              g_quark_from_string (GST_META_TAG_AUDIO_STR))))
    return TRUE;

  return FALSE;
}

static GstFlowReturn
gst_audio_convert_submit_input_buffer (GstBaseTransform * base,
    gboolean is_discont, GstBuffer * input)
{
  GstAudioConvert *this = GST_AUDIO_CONVERT (base);

  if (base->segment.format == GST_FORMAT_TIME) {
    input =
        gst_audio_buffer_clip (input, &base->segment, this->in_info.rate,
        this->in_info.bpf);

    if (!input)
      return GST_FLOW_OK;
  }

  return GST_BASE_TRANSFORM_CLASS (parent_class)->submit_input_buffer (base,
      is_discont, input);
}

static void
gst_audio_convert_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstAudioConvert *this = GST_AUDIO_CONVERT (object);

  switch (prop_id) {
    case PROP_DITHERING:
      this->dither = g_value_get_enum (value);
      break;
    case PROP_NOISE_SHAPING:
      this->ns = g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_audio_convert_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstAudioConvert *this = GST_AUDIO_CONVERT (object);

  switch (prop_id) {
    case PROP_DITHERING:
      g_value_set_enum (value, this->dither);
      break;
    case PROP_NOISE_SHAPING:
      g_value_set_enum (value, this->ns);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}
