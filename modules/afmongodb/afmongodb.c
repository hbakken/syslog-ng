/*
 * Copyright (c) 2010-2015 Balabit
 * Copyright (c) 2010-2014 Gergely Nagy <algernon@balabit.hu>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */


#include "afmongodb.h"
#include "afmongodb-parser.h"
#include "messages.h"
#include "string-list.h"
#include "stats/stats-registry.h"
#include "logmsg/nvtable.h"
#include "logqueue.h"
#include "value-pairs/value-pairs.h"
#include "value-pairs/evttag.h"
#include "plugin.h"
#include "plugin-types.h"
#include "logthrdestdrv.h"

#include "mongoc.h"
#include <time.h>

typedef struct
{
  gchar *name;
  LogTemplate *value;
} MongoDBField;

typedef struct
{
  LogThrDestDriver super;

  /* Shared between main/writer; only read by the writer, never
     written */
  gchar *coll;
  gchar *uri;

  LogTemplateOptions template_options;

  time_t last_msg_stamp;

  ValuePairs *vp;

  /* Writer-only stuff */
  const gchar *db;
  mongoc_uri_t *uri_obj;
  mongoc_client_t *client;
  mongoc_collection_t *coll_obj;

  GString *current_value;
  bson_t *bson;
} MongoDBDestDriver;

/*
 * Configuration
 */

LogTemplateOptions *
afmongodb_dd_get_template_options(LogDriver *s)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *) s;

  return &self->template_options;
}

void
afmongodb_dd_set_uri(LogDriver *d, const gchar *uri)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *)d;

  g_free(self->uri);
  self->uri = g_strdup(uri);
}

void
afmongodb_dd_set_collection(LogDriver *d, const gchar *collection)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *)d;

  g_free(self->coll);
  self->coll = g_strdup(collection);
}

void
afmongodb_dd_set_value_pairs(LogDriver *d, ValuePairs *vp)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *)d;

  value_pairs_unref (self->vp);
  self->vp = vp;
}

/*
 * Utilities
 */

static gchar *
afmongodb_dd_format_stats_instance(LogThrDestDriver *d)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *)d;
  static gchar persist_name[1024];

  g_snprintf (persist_name, sizeof(persist_name), "mongodb,%s,%s", self->uri, self->coll);
  return persist_name;
}

static gchar *
afmongodb_dd_format_persist_name(LogThrDestDriver *d)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *)d;
  static gchar persist_name[1024];

  g_snprintf (persist_name, sizeof(persist_name), "afmongodb(%s,%s)", self->uri, self->coll);
  return persist_name;
}

static void
afmongodb_dd_disconnect(LogThrDestDriver *s)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *)s;

  mongoc_client_destroy(self->client);
  self->client = NULL;
}

static gboolean
afmongodb_dd_connect(MongoDBDestDriver *self, gboolean reconnect)
{
  if (reconnect && self->client)
    return TRUE;

  self->client = mongoc_client_new_from_uri (self->uri_obj);
  if (!self->client)
    {
      msg_error("Error connecting to MongoDB",
                evt_tag_str ("driver", self->super.super.super.id));
      return FALSE;
    }

  self->coll_obj = mongoc_client_get_collection (self->client, self->db,
                                                 self->coll);
  if (!self->coll_obj)
    {
      msg_error("Error getting specified MongoDB collection",
                evt_tag_str ("collection", self->coll),
                evt_tag_str ("driver", self->super.super.super.id));
      return FALSE;
    }

  return TRUE;
}

/*
 * Worker thread
 */
static gboolean
afmongodb_vp_obj_start(const gchar *name,
                       const gchar *prefix, gpointer *prefix_data,
                       const gchar *prev, gpointer *prev_data,
                       gpointer user_data)
{
  bson_t *o;

  if (prefix_data)
    {
      o = bson_new();
      *prefix_data = o;
    }
  return FALSE;
}

static gboolean
afmongodb_vp_obj_end(const gchar *name,
                     const gchar *prefix, gpointer *prefix_data,
                     const gchar *prev, gpointer *prev_data,
                     gpointer user_data)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *)user_data;
  bson_t *root;

  if (prev_data)
    root = (bson_t *)*prev_data;
  else
    root = self->bson;

  if (prefix_data)
    {
      bson_t *d = (bson_t *)*prefix_data;

      bson_append_document (root, name, -1, d);
      bson_free (d);
    }
  return FALSE;
}

static gboolean
afmongodb_vp_process_value(const gchar *name, const gchar *prefix,
                           TypeHint type, const gchar *value, gsize value_len,
                           gpointer *prefix_data, gpointer user_data)
{
  bson_t *o;
  MongoDBDestDriver *self = (MongoDBDestDriver *)user_data;
  gboolean fallback = self->template_options.on_error & ON_ERROR_FALLBACK_TO_STRING;

  if (prefix_data)
    o = (bson_t *)*prefix_data;
  else
    o = self->bson;

  switch (type)
    {
    case TYPE_HINT_BOOLEAN:
      {
        gboolean b;

        if (type_cast_to_boolean (value, &b, NULL))
          bson_append_bool (o, name, -1, b);
        else
          {
            gboolean r = type_cast_drop_helper(self->template_options.on_error,
                                               value, "boolean");

            if (fallback)
              bson_append_utf8 (o, name, -1, value, value_len);
            else
              return r;
          }
        break;
      }
    case TYPE_HINT_INT32:
      {
        gint32 i;

        if (type_cast_to_int32 (value, &i, NULL))
          bson_append_int32 (o, name, -1, i);
        else
          {
            gboolean r = type_cast_drop_helper(self->template_options.on_error,
                                               value, "int32");

            if (fallback)
              bson_append_utf8 (o, name, -1, value, value_len);
            else
              return r;
          }
        break;
      }
    case TYPE_HINT_INT64:
      {
        gint64 i;

        if (type_cast_to_int64 (value, &i, NULL))
          bson_append_int64 (o, name, -1, i);
        else
          {
            gboolean r = type_cast_drop_helper(self->template_options.on_error,
                                               value, "int64");

            if (fallback)
              bson_append_utf8(o, name, -1, value, value_len);
            else
              return r;
          }

        break;
      }
    case TYPE_HINT_DOUBLE:
      {
        gdouble d;

        if (type_cast_to_double (value, &d, NULL))
          bson_append_double (o, name, -1, d);
        else
          {
            gboolean r = type_cast_drop_helper(self->template_options.on_error,
                                               value, "double");
            if (fallback)
              bson_append_utf8(o, name, -1, value, value_len);
            else
              return r;
          }

        break;
      }
    case TYPE_HINT_DATETIME:
      {
        guint64 i;

        if (type_cast_to_datetime_int (value, &i, NULL))
          bson_append_date_time (o, name, -1, (gint64)i);
        else
          {
            gboolean r = type_cast_drop_helper(self->template_options.on_error,
                                               value, "datetime");

            if (fallback)
              bson_append_utf8(o, name, -1, value, value_len);
            else
              return r;
          }

        break;
      }
    case TYPE_HINT_STRING:
    case TYPE_HINT_LITERAL:
      bson_append_utf8 (o, name, -1, value, value_len);
      break;
    default:
      return TRUE;
    }

  return FALSE;
}

static void
afmongodb_worker_retry_over_message(LogThrDestDriver *s, LogMessage *msg)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *)s;

  msg_error("Multiple failures while inserting this record into the database, "
            "message dropped",
            evt_tag_str("driver", self->super.super.super.id),
            evt_tag_int("number_of_retries", s->retries.max),
            evt_tag_value_pairs("message", self->vp, msg,
                                self->super.seq_num,
                                LTZ_SEND, &self->template_options));
}

static worker_insert_result_t
afmongodb_worker_insert (LogThrDestDriver *s, LogMessage *msg)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *)s;
  gboolean success;
  gboolean drop_silently = self->template_options.on_error & ON_ERROR_SILENT;

  if (!afmongodb_dd_connect(self, TRUE))
    return WORKER_INSERT_RESULT_NOT_CONNECTED;

  bson_reinit (self->bson);

  success = value_pairs_walk(self->vp,
                             afmongodb_vp_obj_start,
                             afmongodb_vp_process_value,
                             afmongodb_vp_obj_end,
                             msg, self->super.seq_num,
                             LTZ_SEND,
                             &self->template_options,
                             self);

  if (!success)
    {
      if (!drop_silently)
        {
          msg_error("Failed to format message for MongoDB, dropping message",
                    evt_tag_value_pairs("message", self->vp, msg,
                                        self->super.seq_num,
                                        LTZ_SEND, &self->template_options),
                    evt_tag_str("driver", self->super.super.super.id));
        }
      return WORKER_INSERT_RESULT_DROP;
    }
  else
    {
      bson_error_t error;
      msg_debug("Outgoing message to MongoDB destination",
                evt_tag_value_pairs("message", self->vp, msg,
                                    self->super.seq_num,
                                    LTZ_SEND, &self->template_options),
                evt_tag_str("driver", self->super.super.super.id));

      success = mongoc_collection_insert (self->coll_obj, MONGOC_INSERT_NONE,
                                          (const bson_t *) self->bson,
                                          NULL,
                                          &error);
      if (!success)
        {
          msg_error("Network error while inserting into MongoDB",
                    evt_tag_int("time_reopen", self->super.time_reopen),
                    evt_tag_str("reason", error.message),
                    evt_tag_str("driver", self->super.super.super.id));
        }
    }

  if (!success && (errno == ENOTCONN))
    return WORKER_INSERT_RESULT_NOT_CONNECTED;

  if (!success)
    return WORKER_INSERT_RESULT_ERROR;

  return WORKER_INSERT_RESULT_SUCCESS;
}

static void
afmongodb_worker_thread_init(LogThrDestDriver *d)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *)d;

  afmongodb_dd_connect(self, FALSE);

  self->current_value = g_string_sized_new(256);

  self->bson = bson_sized_new(4096);
}

static void
afmongodb_worker_thread_deinit(LogThrDestDriver *d)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *)d;

  g_string_free (self->current_value, TRUE);

  bson_free (self->bson);
}

/*
 * Main thread
 */

static void
afmongodb_dd_init_value_pairs_dot_to_underscore_transformation(MongoDBDestDriver *self)
{
  ValuePairsTransformSet *vpts;

 /* Always replace a leading dot with an underscore. */
  vpts = value_pairs_transform_set_new(".*");
  value_pairs_transform_set_add_func(vpts, value_pairs_new_transform_replace_prefix(".", "_"));
  value_pairs_add_transforms(self->vp, vpts);
}

static gboolean
afmongodb_dd_init(LogPipe *s)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *)s;
  GlobalConfig *cfg = log_pipe_get_config(s);

  if (!log_dest_driver_init_method(s))
    return FALSE;

  log_template_options_init(&self->template_options, cfg);

  afmongodb_dd_init_value_pairs_dot_to_underscore_transformation(self);

  self->uri_obj = mongoc_uri_new (self->uri);
  if (!self->uri_obj)
    {
      msg_error("Error parsing MongoDB URI",
                evt_tag_str ("uri", self->uri),
                evt_tag_str ("driver", self->super.super.super.id));
      return FALSE;
    }

  self->db = mongoc_uri_get_database (self->uri_obj);
  if (!self->db || !strlen (self->db))
    {
      msg_error("Missing DB name from MongoDB URI",
                evt_tag_str ("uri", self->uri),
                evt_tag_str ("driver", self->super.super.super.id));
      return FALSE;
    }

  msg_verbose("Initializing MongoDB destination",
              evt_tag_str ("uri", self->uri),
              evt_tag_str ("db", self->db),
              evt_tag_str ("collection", self->coll),
              evt_tag_str ("driver", self->super.super.super.id));

  return log_threaded_dest_driver_start(s);
}

static void
afmongodb_dd_free(LogPipe *d)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *)d;

  log_template_options_destroy(&self->template_options);

  g_free(self->uri);
  g_free(self->coll);
  value_pairs_unref(self->vp);

  mongoc_uri_destroy(self->uri_obj);
  mongoc_collection_destroy (self->coll_obj);
  mongoc_cleanup ();
  log_threaded_dest_driver_free(d);
}

static void
afmongodb_dd_queue_method(LogThrDestDriver *d)
{
  MongoDBDestDriver *self = (MongoDBDestDriver *)d;

  self->last_msg_stamp = cached_g_current_time_sec ();
}

/*
 * Plugin glue.
 */

LogDriver *
afmongodb_dd_new(GlobalConfig *cfg)
{
  MongoDBDestDriver *self = g_new0(MongoDBDestDriver, 1);

  mongoc_init ();

  log_threaded_dest_driver_init_instance(&self->super, cfg);

  self->super.super.super.super.init = afmongodb_dd_init;
  self->super.super.super.super.free_fn = afmongodb_dd_free;
  self->super.queue_method = afmongodb_dd_queue_method;

  self->super.worker.thread_init = afmongodb_worker_thread_init;
  self->super.worker.thread_deinit = afmongodb_worker_thread_deinit;
  self->super.worker.disconnect = afmongodb_dd_disconnect;
  self->super.worker.insert = afmongodb_worker_insert;
  self->super.format.stats_instance = afmongodb_dd_format_stats_instance;
  self->super.format.persist_name = afmongodb_dd_format_persist_name;
  self->super.stats_source = SCS_MONGODB;
  self->super.messages.retry_over = afmongodb_worker_retry_over_message;

  afmongodb_dd_set_collection((LogDriver *)self, "messages");

  log_template_options_defaults(&self->template_options);
  afmongodb_dd_set_value_pairs(&self->super.super.super, value_pairs_new_default(cfg));

  return (LogDriver *)self;
}

extern CfgParser afmongodb_dd_parser;

static Plugin afmongodb_plugin =
{
  .type = LL_CONTEXT_DESTINATION,
  .name = "mongodb",
  .parser = &afmongodb_parser,
};

gboolean
afmongodb_module_init(GlobalConfig *cfg, CfgArgs *args)
{
  plugin_register(cfg, &afmongodb_plugin, 1);
  return TRUE;
}

const ModuleInfo module_info =
{
  .canonical_name = "afmongodb",
  .version = SYSLOG_NG_VERSION,
  .description = "The afmongodb module provides MongoDB destination support for syslog-ng.",
  .core_revision = SYSLOG_NG_SOURCE_REVISION,
  .plugins = &afmongodb_plugin,
  .plugins_len = 1,
};
