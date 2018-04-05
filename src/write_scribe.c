/**
 * Write scribe plugin 
 **/

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "configfile.h"

#include "utils_format_json.h"
#include "utils_tail.h"
#include "scribe_capi.h"

#define SCRIBE_BUF_SIZE 8192

static scribestruct *scribe = NULL;

struct instance_definition_s {
    char                 *instance;
    char                 *path;
    cu_tail_t            *tail;
    cdtime_t              interval;
    ssize_t               time_from;
    struct instance_definition_s *next;
};

typedef struct instance_definition_s instance_definition_t;

static int scribe_write_messages (const data_set_t *ds, const value_list_t *vl)
{
    char buffer[SCRIBE_BUF_SIZE];
    size_t   bfree = sizeof(buffer);
    size_t   bfill = 0;

    if (0 != strcmp (ds->type, vl->type))
    {
        ERROR ("scribe_write plugin: DS type does not match "
                "value list type");
        return -1;
    }

    memset (buffer, 0, sizeof (buffer));

    format_json_initialize(buffer, &bfill, &bfree);
    format_json_value_list(buffer, &bfill, &bfree, ds, vl, 1);
    format_json_finalize(buffer, &bfill, &bfree);

    char cat[128];
    ssnprintf(cat, sizeof(cat), "metrics-%d", rand() % 8);
    
    scribe_log(scribe, buffer, cat);

    return (0);
} /* int wl_write_messages */

static int scribe_write (const data_set_t *ds, const value_list_t *vl,
        user_data_t *user_data)
{
    int status = scribe_write_messages (ds, vl);
    return (status);
}

static int scribe_init(void)
{
    srand(time(NULL) ^ getpid());
    scribe = new_scribe();
    return (0);
}

static int scribe_shutdown(void)
{
    delete_scribe(scribe);
    scribe = NULL;

    return (0);
}

static void scribe_instance_definition_destroy(void *arg){
    instance_definition_t *id;

    id = arg;
    if (id == NULL)
        return;

    if (id->tail != NULL)
        cu_tail_destroy (id->tail);
    id->tail = NULL;

    sfree(id->instance);
    sfree(id->path);
    sfree(id);
}

/*
 * Uses replace_str2() implementation from
 * http://creativeandcritical.net/str-replace-c/
 * copyright (c) Laird Shaw, under public domain.
 */
char *replace_str(const char *str, const char *old, /* {{{ */
		const char *new)
{
	char *ret, *r;
	const char *p, *q;
	size_t oldlen = strlen(old);
	size_t count = strlen(new);
	size_t retlen;
	size_t newlen = count;
	int samesize = (oldlen == newlen);

	if (!samesize) {
		for (count = 0, p = str; (q = strstr(p, old)) != NULL; p = q + oldlen)
			count++;
		/* This is undefined if p - str > PTRDIFF_MAX */
		retlen = p - str + strlen(p) + count * (newlen - oldlen);
	} else
		retlen = strlen(str);

	ret = malloc(retlen + 1);
	if (ret == NULL)
		return NULL;
	// added to original: not optimized, but keeps valgrind happy.
	memset(ret, 0, retlen + 1);

	r = ret;
	p = str;
	while (1) {
		/* If the old and new strings are different lengths - in other
		 * words we have already iterated through with strstr above,
		 * and thus we know how many times we need to call it - then we
		 * can avoid the final (potentially lengthy) call to strstr,
		 * which we already know is going to return NULL, by
		 * decrementing and checking count.
		 */
		if (!samesize && !count--)
			break;
		/* Otherwise i.e. when the old and new strings are the same
		 * length, and we don't know how many times to call strstr,
		 * we must check for a NULL return here (we check it in any
		 * event, to avoid further conditions, and because there's
		 * no harm done with the check even when the old and new
		 * strings are different lengths).
		 */
		if ((q = strstr(p, old)) == NULL)
			break;
		/* This is undefined if q - p > PTRDIFF_MAX */
		ptrdiff_t l = q - p;
		memcpy(r, p, l);
		r += l;
		memcpy(r, new, newlen);
		r += newlen;
		p = q + oldlen;
	}
	strncpy(r, p, strlen(p));

	return ret;
} /* }}} char *replace_str */

static char *replace_json_reserved(const char *message) /* {{{ */
{
	char *msg = replace_str(message, "\\", "\\\\");
	if (msg == NULL) {
		ERROR("write_scribe plugin: Unable to alloc memory");
		return NULL;
	}
	char *tmp = replace_str(msg, "\"", "\\\"");
	free(msg);
	if (tmp == NULL) {
		ERROR("write_scribe plugin: Unable to alloc memory");
		return NULL;
	}
	msg = replace_str(tmp, "\n", "\\\n");
	free(tmp);
	if (msg == NULL) {
		ERROR("write_scribe plugin: Unable to alloc memory");
		return NULL;
	}
	return msg;
} /* }}} char *replace_json_reserved */


static int scribe_tail_read (user_data_t *ud) {
    instance_definition_t *id;
    id = ud->data;

    if (id->tail == NULL)
    {
        id->tail = cu_tail_create (id->path);
        if (id->tail == NULL)
        {
            ERROR ("write_scribe plugin: cu_tail_create (\"%s\") failed.",
                    id->path);
            return (-1);
        }
    }

    while (1)
    {
        char buffer[8192];
        size_t buffer_len;
        int status;

        status = cu_tail_readline (id->tail, buffer, (int) sizeof (buffer));
        if (status != 0)
        {
            ERROR ("scribe_write plugin: File \"%s\": cu_tail_readline failed "
                    "with status %i.", id->path, status);
            return (-1);
        }

        buffer_len = strlen (buffer);
        if (buffer_len == 0)
            break;

        /* Remove newlines at the end of line. */
        while (buffer_len > 0) {
            if ((buffer[buffer_len - 1] == '\n') || (buffer[buffer_len - 1] == '\r')) {
                buffer[buffer_len - 1] = 0;
                buffer_len--;
            } else {
                break;
            }
        }

        /* Ignore empty lines. */
        if (buffer_len == 0)
            return (0);

        /** Convert line to json and output to scribe */
        char tmp[8192+1024];
        ssnprintf (tmp, sizeof (tmp), "{\"file\": \"%s\", \"timestamp\": %.3f, \"host\":\"%s\", \"message\": \"%s\"}", \
                  id->instance, CDTIME_T_TO_DOUBLE (cdtime()), hostname_g, replace_json_reserved(buffer));

	char cat[128];
	ssnprintf(cat, sizeof(cat), "logs-%d", rand() % 8);
        scribe_log(scribe, tmp, cat);
    }

    return (0);
}

static int scribe_config_add_file_tail(oconfig_item_t *ci)
{
  instance_definition_t* id;
  int status = 0;
  int i;

  /* Registration variables */
  char cb_name[DATA_MAX_NAME_LEN];
  user_data_t cb_data;

  id = malloc(sizeof(*id));
  if (id == NULL)
      return (-1);
  memset(id, 0, sizeof(*id));
  id->instance = NULL;
  id->path = NULL;
  id->time_from = -1;
  id->next = NULL;

  status = cf_util_get_string (ci, &id->path);
  if (status != 0) {
      sfree (id);
      return (status);
  }

  /* Use default interval. */
  id->interval = plugin_get_interval();

  for (i = 0; i < ci->children_num; ++i){
      oconfig_item_t *option = ci->children + i;
      status = 0;

      if (strcasecmp("Instance", option->key) == 0)
          status = cf_util_get_string(option, &id->instance);
      else if (strcasecmp("Interval", option->key) == 0)
          cf_util_get_cdtime(option, &id->interval);
      else {
          WARNING("scribe_write plugin: Option `%s' not allowed here.", option->key);
          status = -1;
      }

      if (status != 0)
          break;
  }

  if (status != 0) {
      scribe_instance_definition_destroy(id);
      return (-1);
  }

  /* Verify all necessary options have been set. */
  if (id->path == NULL){
      WARNING("scribe_write plugin: Option `Path' must be set.");
      status = -1;
  }

  if (status != 0){
      scribe_instance_definition_destroy(id);
      return (-1);
  }

  ssnprintf (cb_name, sizeof (cb_name), "write_scribe/%s", id->path);
  memset(&cb_data, 0, sizeof(cb_data));
  cb_data.data = id;
  cb_data.free_func = scribe_instance_definition_destroy;
  status = plugin_register_complex_read(NULL, cb_name, scribe_tail_read, id->interval, &cb_data);

  if (status != 0){
      ERROR("scribe_write plugin: Registering complex read function failed.");
      scribe_instance_definition_destroy(id);
      return (-1);
  }

  return (0);
}

static int scribe_config(oconfig_item_t *ci)
{
    int i;
    for (i = 0; i < ci->children_num; ++i){
        oconfig_item_t *child = ci->children + i;
        if (strcasecmp("File", child->key) == 0)
            scribe_config_add_file_tail(child);
        else
            WARNING("write_scribe plugin: Ignoring unknown config option `%s'.", child->key);
    }

    return (0);
}

static void scribe_plugin_log (int severity, const char *msg,
                user_data_t __attribute__((unused)) *user_data)
{
        if (scribe == NULL)
            return;

        /** Convert line to json and output to scribe */
        char tmp[8192+1024];
        ssnprintf (tmp, sizeof (tmp), "{\"severity\":%u, \"timestamp\":%.3f, \"host\":\"%s\", \"message\":\"%s\"}", \
                   severity, CDTIME_T_TO_DOUBLE (cdtime()), hostname_g, replace_json_reserved(msg));

	char cat[128];
	ssnprintf(cat, sizeof(cat), "clogs-%d", rand() % 8);
	
        scribe_log(scribe, tmp, cat);
} /* void logfile_log (int, const char *) */


static int scribe_notification (const notification_t *n,
                user_data_t __attribute__((unused)) *user_data)
{
        char  buf[1024] = "";
        int   buf_len = sizeof (buf);

        ssnprintf (buf, buf_len, "{\"severity\":%u, \"host\":\"%s\", \"timestamp\":%.3f, \"plugin\":\"%s\", \"plugin_instance\":\"%s\", \"type\":\"%s\", \"type_instance\":\"%s\", \"message\":\"%s\"}", \
                   n->severity, replace_json_reserved(n->host), CDTIME_T_TO_DOUBLE ((n->time != 0) ? n->time : cdtime ()), replace_json_reserved(n->plugin), replace_json_reserved(n->plugin_instance), replace_json_reserved(n->type), \
                   replace_json_reserved(n->type_instance), replace_json_reserved(n->message));


	char cat[128];
	ssnprintf(cat, sizeof(cat), "alerts-%d", rand() % 8);
        scribe_log(scribe, buf, cat);
        return (0);
} /* int logfile_notification */



void module_register (void)
{
    plugin_register_init("write_scribe", scribe_init);
    plugin_register_complex_config("write_scribe", scribe_config);
    plugin_register_shutdown("write_scribe", scribe_shutdown);
    plugin_register_write ("write_scribe", scribe_write, NULL);
    plugin_register_log("write_scribe", scribe_plugin_log, NULL);
    plugin_register_notification("write_scribe", scribe_notification, NULL);
}

/* vim: set sw=4 ts=4 sts=4 tw=78 et : */
