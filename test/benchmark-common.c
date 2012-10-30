/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */


/* This file should be included directly in each benchmark program */

#define __USE_GNU 1

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <glib/gprintf.h>

static GMainLoop *main_loop;

typedef struct
{
  gdouble x;
  gdouble y;
}
BenchmarkDataPoint;

typedef struct
{
  /* Array of BenchmarkDataPoints */
  GArray *points;
}
BenchmarkDataSet;

typedef struct
{
  gchar  *name;
  gchar  *x_unit;
  gchar  *y_unit;

  GList  *data_sets;
}
BenchmarkDataPlot;

static gint benchmark_run (gint argc, gchar *argv []);

static GList    *benchmark_data_plots = NULL;
static gboolean  benchmark_is_running = FALSE;

#if 0
static void
benchmark_begin_data_plot (const gchar *name, const gchar *x_unit, const gchar *y_unit)
{
  BenchmarkDataPlot *data_plot;

  data_plot = g_new0 (BenchmarkDataPlot, 1);

  data_plot->name   = g_strdup (name);
  data_plot->x_unit = g_strdup (x_unit);
  data_plot->y_unit = g_strdup (y_unit);

  data_plot->data_sets = NULL;

  benchmark_data_plots = g_list_prepend (benchmark_data_plots, data_plot);
}

static void
benchmark_begin_data_set (void)
{
  BenchmarkDataPlot *data_plot;
  BenchmarkDataSet  *data_set;

  if (!benchmark_data_plots)
    g_error ("Must begin a data plot before adding data sets!");

  data_plot = benchmark_data_plots->data;

  data_set = g_new0 (BenchmarkDataSet, 1);
  data_set->points = g_array_new (FALSE, FALSE, sizeof (BenchmarkDataPoint));

  data_plot->data_sets = g_list_prepend (data_plot->data_sets, data_set);
}

static void
benchmark_add_data_point (gdouble x, gdouble y)
{
  BenchmarkDataPlot  *data_plot;
  BenchmarkDataSet   *data_set;
  BenchmarkDataPoint  data_point;

  if (!benchmark_data_plots)
    g_error ("Must begin a data plot before adding data sets!");

  data_plot = benchmark_data_plots->data;

  if (!data_plot->data_sets)
    g_error ("Must begin a data set before adding data points!");

  data_set = data_plot->data_sets->data;

  data_point.x = x;
  data_point.y = y;

  g_array_append_val (data_set->points, data_point);
}

#endif

static void
benchmark_end (void)
{
  BenchmarkDataPlot *plot;
  GList             *l;

  /* Dump plots */

  if (!benchmark_data_plots)
    exit (1);

  plot = benchmark_data_plots->data;
  if (!plot)
    exit (1);

  for (l = plot->data_sets; l; l = g_list_next (l))
  {
    BenchmarkDataSet *set = l->data;
    guint             i;

    for (i = 0; i < set->points->len; i++)
    {
      BenchmarkDataPoint *point = &g_array_index (set->points, BenchmarkDataPoint, i);

      g_print ("%20lf %20lf\n", point->x, point->y);
    }
  }

  exit (0);
}

static void
benchmark_begin (const gchar *name)
{
  g_log_set_always_fatal (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);
  main_loop = g_main_loop_new (NULL, FALSE);
}

G_GNUC_UNUSED static void
benchmark_run_main_loop (void)
{
  g_main_loop_run (main_loop);
}

G_GNUC_UNUSED static void
benchmark_quit_main_loop (void)
{
  g_main_loop_quit (main_loop);
}

static void
benchmark_timeout (int signal)
{
  benchmark_is_running = FALSE;
}

G_GNUC_UNUSED static void
benchmark_start_wallclock_timer (gint n_seconds)
{
  benchmark_is_running = TRUE;
  signal (SIGALRM, benchmark_timeout);
  alarm (n_seconds);
}

G_GNUC_UNUSED static void
benchmark_start_cpu_timer (gint n_seconds)
{
  struct itimerval itv;

  benchmark_is_running = TRUE;

  memset (&itv, 0, sizeof (itv));
  itv.it_value.tv_sec = n_seconds;

  signal (SIGPROF, benchmark_timeout);
  setitimer (ITIMER_PROF, &itv, NULL);
}

gint
main (gint argc, gchar *argv [])
{
  gint result;

  benchmark_begin (BENCHMARK_UNIT_NAME);
  result = benchmark_run (argc, argv);
  benchmark_end ();

  return result;
}
