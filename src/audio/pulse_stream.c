/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s).
 *
 * A blocking PulseAudio stream that stays on its device.
 *
 * pa_simple moves a stream to the default device when its own goes away, and
 * can't be told not to.  This is pa_simple's design, chiefly changed so that a
 * stream opened on a named device is connected with PA_STREAM_DONT_MOVE: losing
 * the device fails the stream and its caller can reopen the device by name.  Each
 * stream has its own threaded mainloop and context, as a pa_simple does, so
 * streams share no lock or thread (the capture thread runs realtime).
 *
 * Adapted from PulseAudio's src/pulse/simple.c (libpulse):
 *   Copyright (C) 2004-2009 Lennart Poettering
 *   Copyright (C) 2006-2007 Pierre Ossman <ossman@cendio.se> for Cendio AB
 *   Licensed under the LGPL-2.1 or later; used here under the GPLv3 or later
 *   (LGPL-2.1 section 3).  See NOTICE.
 * Taken from it: the connect flags, the state callbacks that wake every wait,
 * the dead-stream check after each wait, the read that carries a fragment
 * across calls, the operation wait for drain and flush.  Changed: DONT_MOVE and
 * a check of the device actually connected for a named device, holes read as
 * silence, a suspended stream reported to the reader.
 */

#include "audio/pulse_stream.h"

#include <ctype.h>
#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/stream.h>
#include <pulse/thread-mainloop.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dawn_error.h"
#include "logging.h"

struct pulse_stream {
   pa_threaded_mainloop *mainloop;
   pa_context *context;
   pa_stream *stream;
   /* The fragment being read: NULL data with a length is a hole. */
   const uint8_t *read_data;
   size_t read_index;
   size_t read_length;
   int op_success;
};

/* A named device, as opposed to the default (which the stream may follow). */
static bool is_named(const char *device) {
   return device && device[0] != '\0' && device[0] != '@' && strcmp(device, "default") != 0;
}

static void context_state_cb(pa_context *c, void *userdata) {
   pulse_stream_t *s = userdata;
   switch (pa_context_get_state(c)) {
      case PA_CONTEXT_READY:
      case PA_CONTEXT_TERMINATED:
      case PA_CONTEXT_FAILED:
         pa_threaded_mainloop_signal(s->mainloop, 0);
         break;
      default:
         break;
   }
}

static void stream_state_cb(pa_stream *st, void *userdata) {
   pulse_stream_t *s = userdata;
   switch (pa_stream_get_state(st)) {
      case PA_STREAM_READY:
      case PA_STREAM_FAILED:
      case PA_STREAM_TERMINATED:
         pa_threaded_mainloop_signal(s->mainloop, 0);
         break;
      default:
         break;
   }
}

/* Data to read, room to write, or a suspend: wake the waiting caller. */
static void stream_request_cb(pa_stream *st, size_t length, void *userdata) {
   (void)st;
   (void)length;
   pa_threaded_mainloop_signal(((pulse_stream_t *)userdata)->mainloop, 0);
}

static void stream_wake_cb(pa_stream *st, void *userdata) {
   (void)st;
   pa_threaded_mainloop_signal(((pulse_stream_t *)userdata)->mainloop, 0);
}

static void stream_moved_cb(pa_stream *st, void *userdata) {
   (void)userdata;
   const char *to = pa_stream_get_device_name(st);
   OLOG_INFO("PulseAudio: default stream moved to %s", to ? to : "(unknown)");
}

static void success_cb(pa_stream *st, int success, void *userdata) {
   (void)st;
   pulse_stream_t *s = userdata;
   s->op_success = success;
   pa_threaded_mainloop_signal(s->mainloop, 0);
}

/* 0 while the context and stream can still be used, else the error. */
static int dead(const pulse_stream_t *s) {
   pa_context_state_t cs = pa_context_get_state(s->context);
   pa_stream_state_t ss = pa_stream_get_state(s->stream);
   if (PA_CONTEXT_IS_GOOD(cs) && PA_STREAM_IS_GOOD(ss)) {
      return 0;
   }
   if (cs == PA_CONTEXT_FAILED || ss == PA_STREAM_FAILED) {
      int err = pa_context_errno(s->context);
      return err ? err : PA_ERR_UNKNOWN;
   }
   return PA_ERR_BADSTATE;
}

void pulse_stream_free(pulse_stream_t *s) {
   if (!s) {
      return;
   }
   if (s->mainloop) {
      pa_threaded_mainloop_stop(s->mainloop);
   }
   if (s->stream) {
      pa_stream_unref(s->stream);
   }
   if (s->context) {
      pa_context_disconnect(s->context);
      pa_context_unref(s->context);
   }
   if (s->mainloop) {
      pa_threaded_mainloop_free(s->mainloop);
   }
   free(s);
}

pulse_stream_t *pulse_stream_new(pa_stream_direction_t dir,
                                 const char *device,
                                 const char *description,
                                 const pa_sample_spec *ss,
                                 const pa_buffer_attr *attr,
                                 int *error) {
   int err = PA_ERR_INTERNAL;
   const bool named = is_named(device);
   pulse_stream_t *s = calloc(1, sizeof(*s));
   if (!s) {
      goto fail;
   }
   s->mainloop = pa_threaded_mainloop_new();
   if (!s->mainloop) {
      goto fail;
   }
   s->context = pa_context_new(pa_threaded_mainloop_get_api(s->mainloop), "DAWN");
   if (!s->context) {
      goto fail;
   }
   pa_context_set_state_callback(s->context, context_state_cb, s);
   if (pa_context_connect(s->context, NULL, 0, NULL) < 0) {
      err = pa_context_errno(s->context);
      goto fail;
   }

   pa_threaded_mainloop_lock(s->mainloop);
   if (pa_threaded_mainloop_start(s->mainloop) < 0) {
      goto unlock_and_fail;
   }
   for (;;) {
      pa_context_state_t state = pa_context_get_state(s->context);
      if (state == PA_CONTEXT_READY) {
         break;
      }
      if (!PA_CONTEXT_IS_GOOD(state)) {
         err = pa_context_errno(s->context);
         goto unlock_and_fail;
      }
      pa_threaded_mainloop_wait(s->mainloop);
   }

   s->stream = pa_stream_new(s->context, description, ss, NULL);
   if (!s->stream) {
      err = pa_context_errno(s->context);
      goto unlock_and_fail;
   }
   pa_stream_set_state_callback(s->stream, stream_state_cb, s);
   pa_stream_set_read_callback(s->stream, stream_request_cb, s);
   pa_stream_set_write_callback(s->stream, stream_request_cb, s);
   pa_stream_set_suspended_callback(s->stream, stream_wake_cb, s);
   if (!named) {
      pa_stream_set_moved_callback(s->stream, stream_moved_cb, s);
   }

   /* pa_simple's flags (ADJUST_LATENCY is what makes the buffer attributes mean
    * what they say), and a named device is never left. */
   pa_stream_flags_t flags = PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_ADJUST_LATENCY |
                             PA_STREAM_AUTO_TIMING_UPDATE;
   if (named) {
      flags |= PA_STREAM_DONT_MOVE;
   }
   const char *dev = named ? device : NULL;
   int r = dir == PA_STREAM_PLAYBACK
               ? pa_stream_connect_playback(s->stream, dev, attr, flags, NULL, NULL)
               : pa_stream_connect_record(s->stream, dev, attr, flags);
   if (r < 0) {
      err = pa_context_errno(s->context);
      goto unlock_and_fail;
   }
   for (;;) {
      pa_stream_state_t state = pa_stream_get_state(s->stream);
      if (state == PA_STREAM_READY) {
         break;
      }
      if (!PA_STREAM_IS_GOOD(state)) {
         err = pa_context_errno(s->context);
         goto unlock_and_fail;
      }
      pa_threaded_mainloop_wait(s->mainloop);
   }

   /* A server may link a stream whose device is missing to another one
    * (pipewire-pulse can): that isn't the device asked for.  A device can be
    * named by its index, too. */
   if (named) {
      bool index = true;
      for (const char *p = device; *p; p++) {
         index = index && isdigit((unsigned char)*p);
      }
      const char *on = pa_stream_get_device_name(s->stream);
      const bool same = index ? pa_stream_get_device_index(s->stream) ==
                                    (uint32_t)strtoul(device, NULL, 10)
                              : on && strcmp(on, device) == 0;
      if (!same) {
         err = PA_ERR_NOENTITY;
         goto unlock_and_fail;
      }
   }
   pa_threaded_mainloop_unlock(s->mainloop);
   return s;

unlock_and_fail:
   pa_threaded_mainloop_unlock(s->mainloop);
fail:
   if (error) {
      *error = err;
   }
   pulse_stream_free(s);
   return NULL;
}

int pulse_stream_read(pulse_stream_t *s, void *data, size_t bytes, int *error) {
   uint8_t *out = data;
   pa_threaded_mainloop_lock(s->mainloop);
   int err = dead(s);
   while (!err && bytes > 0) {
      if (s->read_length == 0) {
         const void *frag = NULL;
         size_t n = 0;
         if (pa_stream_peek(s->stream, &frag, &n) < 0) {
            err = pa_context_errno(s->context);
            break;
         }
         if (n == 0) {
            if (pa_stream_is_suspended(s->stream) == 1) {
               err = PULSE_STREAM_ERR_SUSPENDED;
               break;
            }
            pa_threaded_mainloop_wait(s->mainloop);
            err = dead(s);
            continue;
         }
         s->read_data = frag;
         s->read_length = n;
         s->read_index = 0;
      }

      size_t l = s->read_length < bytes ? s->read_length : bytes;
      if (s->read_data) {
         memcpy(out, s->read_data + s->read_index, l);
      } else {
         memset(out, 0, l); /* a hole: what the server didn't capture */
      }
      out += l;
      bytes -= l;
      s->read_index += l;
      s->read_length -= l;
      if (s->read_length == 0) {
         s->read_data = NULL;
         s->read_index = 0;
         if (pa_stream_drop(s->stream) < 0) {
            err = pa_context_errno(s->context);
         }
      }
   }
   pa_threaded_mainloop_unlock(s->mainloop);
   if (err && error) {
      *error = err;
   }
   return err ? FAILURE : SUCCESS;
}

int pulse_stream_write(pulse_stream_t *s, const void *data, size_t bytes, int *error) {
   const uint8_t *in = data;
   pa_threaded_mainloop_lock(s->mainloop);
   int err = dead(s);
   while (!err && bytes > 0) {
      size_t l = pa_stream_writable_size(s->stream);
      if (l == (size_t)-1) {
         err = pa_context_errno(s->context);
         break;
      }
      if (l == 0) {
         pa_threaded_mainloop_wait(s->mainloop);
         err = dead(s);
         continue;
      }
      if (l > bytes) {
         l = bytes;
      }
      if (pa_stream_write(s->stream, in, l, NULL, 0, PA_SEEK_RELATIVE) < 0) {
         err = pa_context_errno(s->context);
         break;
      }
      in += l;
      bytes -= l;
   }
   pa_threaded_mainloop_unlock(s->mainloop);
   if (err && error) {
      *error = err;
   }
   return err ? FAILURE : SUCCESS;
}

/* Runs a drain or flush and waits for it, unless the stream dies first. */
static int run_operation(pulse_stream_t *s,
                         pa_operation *(*op)(pa_stream *, pa_stream_success_cb_t, void *),
                         int *error) {
   pa_threaded_mainloop_lock(s->mainloop);
   int err = dead(s);
   if (!err) {
      s->op_success = 0;
      pa_operation *o = op(s->stream, success_cb, s);
      if (!o) {
         err = pa_context_errno(s->context);
      } else {
         while (pa_operation_get_state(o) == PA_OPERATION_RUNNING) {
            pa_threaded_mainloop_wait(s->mainloop);
            err = dead(s);
            if (err) {
               pa_operation_cancel(o);
               break;
            }
         }
         pa_operation_unref(o);
         if (!err && !s->op_success) {
            err = pa_context_errno(s->context);
            if (!err) {
               err = PA_ERR_UNKNOWN;
            }
         }
      }
   }
   pa_threaded_mainloop_unlock(s->mainloop);
   if (err && error) {
      *error = err;
   }
   return err ? FAILURE : SUCCESS;
}

int pulse_stream_drain(pulse_stream_t *s, int *error) {
   return run_operation(s, pa_stream_drain, error);
}

int pulse_stream_flush(pulse_stream_t *s, int *error) {
   return run_operation(s, pa_stream_flush, error);
}
