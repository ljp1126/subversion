/*
 * replay.c :  routines for replaying revisions
 *
 * ====================================================================
 * Copyright (c) 2005 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

#include <apr_pools.h>
#include <apr_strings.h>
#include <apr_xml.h>
#include <apr_md5.h>

#include <http_request.h>
#include <http_log.h>
#include <mod_dav.h>

#include "svn_pools.h"
#include "svn_repos.h"
#include "svn_fs.h"
#include "svn_md5.h"
#include "svn_base64.h"
#include "svn_xml.h"
#include "svn_path.h"
#include "svn_dav.h"
#include "svn_props.h"

#include "../dav_svn.h"


typedef struct {
  apr_bucket_brigade *bb;
  ap_filter_t *output;
  svn_boolean_t started;
  svn_boolean_t sending_textdelta;
} edit_baton_t;



/*** Helper Functions ***/

static svn_error_t *
maybe_start_report(edit_baton_t *eb)
{
  if (! eb->started)
    {
      SVN_ERR(dav_svn__send_xml
                (eb->bb, eb->output,
                 DAV_XML_HEADER DEBUG_CR
                 "<S:editor-report xmlns:S=\"" SVN_XML_NAMESPACE "\">"
                 DEBUG_CR));

      eb->started = TRUE;
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
end_report(edit_baton_t *eb)
{
  SVN_ERR(dav_svn__send_xml(eb->bb, eb->output,
                            "</S:editor-report>" DEBUG_CR));

  return SVN_NO_ERROR;
}


static svn_error_t *
maybe_close_textdelta(edit_baton_t *eb)
{
  if (eb->sending_textdelta)
    { 
      SVN_ERR(dav_svn__send_xml(eb->bb, eb->output,
                                "</S:apply-textdelta>" DEBUG_CR));
      eb->sending_textdelta = FALSE;
    }

  return SVN_NO_ERROR;
}


static svn_error_t *
add_file_or_directory(const char *file_or_directory,
                      const char *path,
                      edit_baton_t *eb,
                      const char *copyfrom_path,
                      svn_revnum_t copyfrom_rev,
                      apr_pool_t *pool,
                      void **added_baton)
{
  const char *qname = apr_xml_quote_string(pool, path, 1);
  const char *qcopy = 
    copyfrom_path ? apr_xml_quote_string(pool, copyfrom_path, 1) : NULL;

  SVN_ERR(maybe_close_textdelta(eb));

  *added_baton = (void *)eb;

  if (! copyfrom_path)
    SVN_ERR(dav_svn__send_xml(eb->bb, eb->output,
                              "<S:add-%s name=\"%s\"/>" DEBUG_CR,
                              file_or_directory, qname));
  else
    SVN_ERR(dav_svn__send_xml(eb->bb, eb->output,
                              "<S:add-%s name=\"%s\" copyfrom-path=\"%s\" "
                                        "copyfrom-rev=\"%ld\"/>" DEBUG_CR,
                              file_or_directory, qname, qcopy, copyfrom_rev));

  return SVN_NO_ERROR;
}

static svn_error_t *
open_file_or_directory(const char *file_or_directory,
                       const char *path,
                       edit_baton_t *eb,
                       svn_revnum_t base_revision,
                       apr_pool_t *pool,
                       void **opened_baton)
{
  const char *qname = apr_xml_quote_string(pool, path, 1);
  SVN_ERR(maybe_close_textdelta(eb));
  *opened_baton = (void *)eb;
  return dav_svn__send_xml(eb->bb, eb->output,
                           "<S:open-%s name=\"%s\" rev=\"%ld\"/>" DEBUG_CR, 
                           file_or_directory, qname, base_revision);
}


static svn_error_t *
change_file_or_dir_prop(const char *file_or_dir,
                        edit_baton_t *eb,
                        const char *name,
                        const svn_string_t *value,
                        apr_pool_t *pool)
{
  const char *qname = apr_xml_quote_string(pool, name, 1);

  SVN_ERR(maybe_close_textdelta(eb));

  if (value)
    {
      const svn_string_t *enc_value = svn_base64_encode_string(value, pool);

      SVN_ERR(dav_svn__send_xml
                (eb->bb, eb->output,
                 "<S:change-%s-prop name=\"%s\">%s</S:change-%s-prop>" DEBUG_CR,
                 file_or_dir, qname, enc_value->data, file_or_dir));
    }
  else
    {
      SVN_ERR(dav_svn__send_xml
                (eb->bb, eb->output,
                 "<S:change-%s-prop name=\"%s\" del=\"true\"/>" DEBUG_CR,
                 file_or_dir, qname));
    }
  return SVN_NO_ERROR;
}



/*** Editor Implementation ***/


static svn_error_t *
set_target_revision(void *edit_baton,
                    svn_revnum_t target_revision,
                    apr_pool_t *pool)
{
  edit_baton_t *eb = edit_baton;
  SVN_ERR(maybe_start_report(eb));
  return dav_svn__send_xml(eb->bb, eb->output,
                           "<S:target-revision rev=\"%ld\"/>" DEBUG_CR,
                           target_revision);
}


static svn_error_t *
open_root(void *edit_baton,
          svn_revnum_t base_revision,
          apr_pool_t *pool,
          void **root_baton)
{
  edit_baton_t *eb = edit_baton;
  *root_baton = edit_baton;
  SVN_ERR(maybe_start_report(eb));
  return dav_svn__send_xml(eb->bb, eb->output,
                           "<S:open-root rev=\"%ld\"/>" DEBUG_CR,
                           base_revision);
}


static svn_error_t *
delete_entry(const char *path,
             svn_revnum_t revision,
             void *parent_baton,
             apr_pool_t *pool)
{
  edit_baton_t *eb = parent_baton;
  const char *qname = apr_xml_quote_string(pool, path, 1);
  SVN_ERR(maybe_close_textdelta(eb));
  return dav_svn__send_xml(eb->bb, eb->output,
                           "<S:delete-entry name=\"%s\" rev=\"%ld\"/>" DEBUG_CR,
                            qname, revision);
}


static svn_error_t *
add_directory(const char *path,
              void *parent_baton,
              const char *copyfrom_path,
              svn_revnum_t copyfrom_rev,
              apr_pool_t *pool,
              void **child_baton)
{
  return add_file_or_directory("directory", path, parent_baton, 
                               copyfrom_path, copyfrom_rev, pool, child_baton);
}


static svn_error_t *
open_directory(const char *path,
               void *parent_baton,
               svn_revnum_t base_revision,
               apr_pool_t *pool,
               void **child_baton)
{
  return open_file_or_directory("directory", path, parent_baton,
                                base_revision, pool, child_baton);
}


static svn_error_t *
change_dir_prop(void *baton,
                const char *name,
                const svn_string_t *value,
                apr_pool_t *pool)
{
  return change_file_or_dir_prop("dir", baton, name, value, pool);
}


static svn_error_t *
add_file(const char *path,
         void *parent_baton,
         const char *copyfrom_path,
         svn_revnum_t copyfrom_rev,
         apr_pool_t *pool,
         void **file_baton)
{
  return add_file_or_directory("file", path, parent_baton, 
                               copyfrom_path, copyfrom_rev, pool, file_baton);
}


static svn_error_t *
open_file(const char *path,
          void *parent_baton,
          svn_revnum_t base_revision,
          apr_pool_t *pool,
          void **file_baton)
{
  return open_file_or_directory("file", path, parent_baton,
                                base_revision, pool, file_baton);
}


static svn_error_t *
change_file_prop(void *baton,
                 const char *name,
                 const svn_string_t *value,
                 apr_pool_t *pool)
{
  return change_file_or_dir_prop("file", baton, name, value, pool);
}


static svn_error_t *
apply_textdelta(void *file_baton,
                const char *base_checksum,
                apr_pool_t *pool,
                svn_txdelta_window_handler_t *handler,
                void **handler_baton)
{
  edit_baton_t *eb = file_baton;

  SVN_ERR(dav_svn__send_xml(eb->bb, eb->output, "<S:apply-textdelta"));

  if (base_checksum)
    SVN_ERR(dav_svn__send_xml(eb->bb, eb->output, " checksum=\"%s\">",
                              base_checksum));
  else
    SVN_ERR(dav_svn__send_xml(eb->bb, eb->output, ">"));

  svn_txdelta_to_svndiff(dav_svn__make_base64_output_stream(eb->bb, eb->output,
                                                            pool), 
                         pool, handler, handler_baton);

  eb->sending_textdelta = TRUE;

  return SVN_NO_ERROR;
}


static svn_error_t *
close_file(void *file_baton, const char *text_checksum, apr_pool_t *pool)
{
  edit_baton_t *eb = file_baton;
  SVN_ERR(maybe_close_textdelta(eb));
  return dav_svn__send_xml(eb->bb, eb->output, "<S:close-file/>" DEBUG_CR);
}


static svn_error_t *
close_directory(void *dir_baton, apr_pool_t *pool)
{
  edit_baton_t *eb = dir_baton;
  return dav_svn__send_xml(eb->bb, eb->output, "<S:close-directory/>" DEBUG_CR);
}


static void
make_editor(const svn_delta_editor_t **editor,
            void **edit_baton,
            apr_bucket_brigade *bb,
            ap_filter_t *output,
            apr_pool_t *pool)
{
  edit_baton_t *eb = apr_pcalloc(pool, sizeof(*eb));
  svn_delta_editor_t *e = svn_delta_default_editor(pool);

  eb->bb = bb;
  eb->output = output;
  eb->started = FALSE;
  eb->sending_textdelta = FALSE;

  e->set_target_revision = set_target_revision;
  e->open_root = open_root;
  e->delete_entry = delete_entry;
  e->add_directory = add_directory;
  e->open_directory = open_directory;
  e->change_dir_prop = change_dir_prop;
  e->add_file = add_file;
  e->open_file = open_file;
  e->apply_textdelta = apply_textdelta;
  e->change_file_prop = change_file_prop;
  e->close_file = close_file;
  e->close_directory = close_directory;

  *editor = e;
  *edit_baton = eb;
}


static dav_error *
malformed_element_error(const char *tagname, apr_pool_t *pool)
{
  return dav_svn__new_error_tag(pool, HTTP_BAD_REQUEST, 0,
                                apr_pstrcat(pool,
                                            "The request's '", tagname,
                                            "' element is malformed; there "
                                            "is a problem with the client.",
                                            NULL),
                                SVN_DAV_ERROR_NAMESPACE, SVN_DAV_ERROR_TAG);
}


dav_error *
dav_svn__replay_report(const dav_resource *resource,
                       const apr_xml_doc *doc,
                       ap_filter_t *output)
{
  svn_revnum_t low_water_mark = SVN_INVALID_REVNUM;
  svn_revnum_t rev = SVN_INVALID_REVNUM;
  const svn_delta_editor_t *editor;
  svn_boolean_t send_deltas = TRUE;
  dav_svn__authz_read_baton arb;
  const char *base_dir = resource->info->repos_path;
  apr_bucket_brigade *bb;
  apr_xml_elem *child;
  svn_fs_root_t *root;
  svn_error_t *err;
  void *edit_baton;
  int ns;

  /* The request won't have a repos_path if it's for the root. */
  if (! base_dir)
    base_dir = "";

  arb.r = resource->info->r;
  arb.repos = resource->info->repos;

  ns = dav_svn__find_ns(doc->namespaces, SVN_XML_NAMESPACE);
  if (ns == -1)
    return dav_svn__new_error_tag(resource->pool, HTTP_BAD_REQUEST, 0,
                                  "The request does not contain the 'svn:' "
                                  "namespace, so it is not going to have an "
                                  "svn:revision element. That element is "
                                  "required.",
                                  SVN_DAV_ERROR_NAMESPACE,
                                  SVN_DAV_ERROR_TAG);

  for (child = doc->root->first_child; child != NULL; child = child->next)
    {
      if (child->ns == ns)
        {
          const char *cdata;

          if (strcmp(child->name, "revision") == 0)
            {
              cdata = dav_xml_get_cdata(child, resource->pool, 1);
              if (! cdata)
                return malformed_element_error("revision", resource->pool);
              rev = SVN_STR_TO_REV(cdata);
            }
          else if (strcmp(child->name, "low-water-mark") == 0)
            {
              cdata = dav_xml_get_cdata(child, resource->pool, 1);
              if (! cdata)
                return malformed_element_error("low-water-mark",
                                               resource->pool);
              low_water_mark = SVN_STR_TO_REV(cdata);
            }
          else if (strcmp(child->name, "send-deltas") == 0)
            {
              cdata = dav_xml_get_cdata(child, resource->pool, 1);
              if (! cdata)
                return malformed_element_error("send-deltas", resource->pool);
              send_deltas = atoi(cdata);
            }
        }
    }

  if (! SVN_IS_VALID_REVNUM(rev))
    return dav_svn__new_error_tag
             (resource->pool, HTTP_BAD_REQUEST, 0,
              "Request was missing the revision argument.",
              SVN_DAV_ERROR_NAMESPACE, SVN_DAV_ERROR_TAG);

  if (! SVN_IS_VALID_REVNUM(low_water_mark))
    return dav_svn__new_error_tag
             (resource->pool, HTTP_BAD_REQUEST, 0,
              "Request was missing the low-water-mark argument.",
              SVN_DAV_ERROR_NAMESPACE, SVN_DAV_ERROR_TAG);

  bb = apr_brigade_create(resource->pool, output->c->bucket_alloc);

  if ((err = svn_fs_revision_root(&root, resource->info->repos->fs, rev,
                                  resource->pool)))
    return dav_svn__convert_err(err, HTTP_INTERNAL_SERVER_ERROR,
                                "Couldn't retrieve revision root",
                                resource->pool);

  make_editor(&editor, &edit_baton, bb, output, resource->pool);;

  if ((err = svn_repos_replay2(root, base_dir, low_water_mark,
                               send_deltas, editor, edit_baton,
                               dav_svn__authz_read_func(&arb), &arb,
                               resource->pool)))
    return dav_svn__convert_err(err, HTTP_INTERNAL_SERVER_ERROR,
                                "Problem replaying revision",
                                resource->pool);

  if ((err = end_report(edit_baton)))
    return dav_svn__convert_err(err, HTTP_INTERNAL_SERVER_ERROR,
                                "Problem closing editor drive",
                                resource->pool);

  {
    const char *action;

    if (base_dir && base_dir[0] != '\0')
      action = apr_psprintf(resource->info->r->pool,
                            "replay %ld '%s'", rev,
                            svn_path_uri_encode(base_dir,
                                                resource->info->r->pool));
    else
      action = apr_psprintf(resource->info->r->pool, "replay %ld", rev);

    apr_table_set(resource->info->r->subprocess_env, "SVN-ACTION", action);
  }

  ap_fflush(output, bb);

  return NULL;
}
