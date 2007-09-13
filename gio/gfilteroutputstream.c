#include <config.h>
#include <glib/gi18n-lib.h>


#include "gfilteroutputstream.h"
#include "goutputstream.h"

/* TODO: Real P_() */
#define P_(_x) (_x)


enum {
  PROP_0,
  PROP_BASE_STREAM
};

static void     g_filter_output_stream_set_property (GObject      *object,
                                                     guint         prop_id,
                                                     const GValue *value,
                                                     GParamSpec   *pspec);

static void     g_filter_output_stream_get_property (GObject    *object,
                                                     guint       prop_id,
                                                     GValue     *value,
                                                     GParamSpec *pspec);
static void     g_filter_output_stream_finalize     (GObject *object);

G_DEFINE_TYPE (GFilterOutputStream, g_filter_output_stream, G_TYPE_OUTPUT_STREAM)



static void
g_filter_output_stream_class_init (GFilterOutputStreamClass *klass)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->get_property = g_filter_output_stream_get_property;
  object_class->set_property = g_filter_output_stream_set_property;
  object_class->finalize     = g_filter_output_stream_finalize;

  g_object_class_install_property (object_class,
                                   PROP_BASE_STREAM,
                                   g_param_spec_object ("base_class",
                                                         P_("The Filter Base Class"),
                                                         P_("The underlying base class the io ops will be done on"),
                                                         G_TYPE_OUTPUT_STREAM,
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | 
                                                         G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));

}

static void
g_filter_output_stream_set_property (GObject         *object,
                                     guint            prop_id,
                                     const GValue    *value,
                                     GParamSpec      *pspec)
{
  GFilterOutputStream *filter_stream;
  GObject *obj;

  filter_stream = G_FILTER_OUTPUT_STREAM (object);

  switch (prop_id) 
    {
    case PROP_BASE_STREAM:
      obj = g_value_dup_object (value);
      filter_stream->base_stream = G_OUTPUT_STREAM (obj);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }

}

static void
g_filter_output_stream_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  GFilterOutputStream *filter_stream;

  filter_stream = G_FILTER_OUTPUT_STREAM (object);

  switch (prop_id)
    {
    case PROP_BASE_STREAM:
      g_value_set_object (value, filter_stream->base_stream);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }

}

static void
g_filter_output_stream_finalize (GObject *object)
{
  GFilterOutputStream *stream;

  stream = G_FILTER_OUTPUT_STREAM (object);

  g_object_unref (stream->base_stream);

  if (G_OBJECT_CLASS (g_filter_output_stream_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_filter_output_stream_parent_class)->finalize) (object);
}

static void
g_filter_output_stream_init (GFilterOutputStream *stream)
{

}


GOutputStream *
g_filter_output_stream_get_base_stream (GFilterOutputStream *stream)
{
  return stream->base_stream;
}

// vim: ts=2 sw=2 et
