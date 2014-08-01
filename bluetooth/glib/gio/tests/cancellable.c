/* GIO - GLib Input, Output and Streaming Library
 *
 * Copyright (C) 2011 Collabora Ltd.
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
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Stef Walter <stefw@collabora.co.uk>
 */

#include <locale.h>

#include <gio/gio.h>

/* How long to wait in ms for each iteration */
#define WAIT_ITERATION (10)

static gint num_async_operations = 0;

typedef struct
{
  guint iterations_requested;
  guint iterations_done;
  GCancellable *cancellable;
} MockOperationData;

static void
mock_operation_free (gpointer user_data)
{
  MockOperationData *data = user_data;
  g_object_unref (data->cancellable);
  g_free (data);
}

static void
mock_operation_thread (GSimpleAsyncResult *simple,
                       GObject            *object,
                       GCancellable       *cancellable)
{
  MockOperationData *data;
  guint i;

  data = g_simple_async_result_get_op_res_gpointer (simple);
  g_assert (data->cancellable == cancellable);

  for (i = 0; i < data->iterations_requested; i++)
    {
      if (g_cancellable_is_cancelled (data->cancellable))
        break;
      if (g_test_verbose ())
        g_printerr ("THRD: %u iteration %u\n", data->iterations_requested, i);
      g_usleep (WAIT_ITERATION * 1000);
    }

  if (g_test_verbose ())
    g_printerr ("THRD: %u stopped at %u\n", data->iterations_requested, i);
  data->iterations_done = i;
}

static gboolean
mock_operation_timeout (gpointer user_data)
{
  GSimpleAsyncResult *simple;
  MockOperationData *data;
  GError *error = NULL;
  gboolean done = FALSE;

  simple = G_SIMPLE_ASYNC_RESULT (user_data);
  data = g_simple_async_result_get_op_res_gpointer (simple);

  if (data->iterations_done >= data->iterations_requested)
      done = TRUE;

  if (g_cancellable_set_error_if_cancelled (data->cancellable, &error)) {
      g_simple_async_result_take_error (simple, error);
      done = TRUE;
  }

  if (done) {
      if (g_test_verbose ())
        g_printerr ("LOOP: %u stopped at %u\n", data->iterations_requested,\
                    data->iterations_done);
      g_simple_async_result_complete (simple);
      return FALSE; /* don't call timeout again */

  } else {
      data->iterations_done++;
      if (g_test_verbose ())
        g_printerr ("LOOP: %u iteration %u\n", data->iterations_requested,
                    data->iterations_done);
      return TRUE; /* call timeout */
    }
}

static void
mock_operation_async (guint                wait_iterations,
                      gboolean             run_in_thread,
                      GCancellable        *cancellable,
                      GAsyncReadyCallback  callback,
                      gpointer             user_data)
{
  GSimpleAsyncResult *simple;
  MockOperationData *data;

  simple = g_simple_async_result_new (NULL, callback, user_data,
                                      mock_operation_async);
  data = g_new0 (MockOperationData, 1);
  data->iterations_requested = wait_iterations;
  data->cancellable = g_object_ref (cancellable);
  g_simple_async_result_set_op_res_gpointer (simple, data, mock_operation_free);

  if (run_in_thread) {
      g_simple_async_result_run_in_thread (simple, mock_operation_thread,
                                           G_PRIORITY_DEFAULT, cancellable);
      if (g_test_verbose ())
        g_printerr ("THRD: %d started\n", wait_iterations);
  } else {
      g_timeout_add_full (G_PRIORITY_DEFAULT, WAIT_ITERATION, mock_operation_timeout,
                          g_object_ref (simple), g_object_unref);
      if (g_test_verbose ())
        g_printerr ("LOOP: %d started\n", wait_iterations);
  }

  g_object_unref (simple);
}

static guint
mock_operation_finish (GAsyncResult  *result,
                       GError       **error)
{
  MockOperationData *data;

  g_assert (g_simple_async_result_is_valid (result, NULL, mock_operation_async));
  g_simple_async_result_propagate_error (G_SIMPLE_ASYNC_RESULT (result), error);

  data = g_simple_async_result_get_op_res_gpointer (G_SIMPLE_ASYNC_RESULT (result));
  return data->iterations_done;
}

static void
on_mock_operation_ready (GObject      *source,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  guint iterations_requested;
  guint iterations_done;
  GError *error = NULL;

  iterations_requested = GPOINTER_TO_UINT (user_data);
  iterations_done = mock_operation_finish (result, &error);

  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_error_free (error);

  g_assert_cmpint (iterations_requested, >, iterations_done);
  num_async_operations--;
}

static gboolean
on_main_loop_timeout_quit (gpointer user_data)
{
  GMainLoop *loop = user_data;
  g_main_loop_quit (loop);
  return FALSE;
}

static void
test_cancel_multiple_concurrent (void)
{
  GCancellable *cancellable;
  guint i, iterations;
  GMainLoop *loop;

  cancellable = g_cancellable_new ();
  loop = g_main_loop_new (NULL, FALSE);

  for (i = 0; i < 45; i++)
    {
      iterations = i + 10;
      mock_operation_async (iterations, g_random_boolean (), cancellable,
                            on_mock_operation_ready, GUINT_TO_POINTER (iterations));
      num_async_operations++;
    }

  /* Wait for two iterations, to give threads a chance to start up */
  g_timeout_add (WAIT_ITERATION * 2, on_main_loop_timeout_quit, loop);
  g_main_loop_run (loop);
  g_assert_cmpint (num_async_operations, ==, 45);
  if (g_test_verbose ())
    g_printerr ("CANCEL: %d operations\n", num_async_operations);
  g_cancellable_cancel (cancellable);
  g_assert (g_cancellable_is_cancelled (cancellable));

  /* Wait for two more iterations, and all threads should be cancelled */
  g_timeout_add (WAIT_ITERATION * 2, on_main_loop_timeout_quit, loop);
  g_main_loop_run (loop);
  g_assert_cmpint (num_async_operations, ==, 0);

  g_object_unref (cancellable);
  g_main_loop_unref (loop);
}

int
main (int argc, char *argv[])
{
  g_type_init ();
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/cancellable/multiple-concurrent", test_cancel_multiple_concurrent);

  return g_test_run ();
}
