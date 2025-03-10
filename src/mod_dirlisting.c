/* fstatat() fdopendir() */
#if !defined(_XOPEN_SOURCE) || _XOPEN_SOURCE-0 < 700
#undef  _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
/* NetBSD dirent.h improperly hides fdopendir() (POSIX.1-2008) declaration
 * which should be visible with _XOPEN_SOURCE 700 or _POSIX_C_SOURCE 200809L */
#ifdef __NetBSD__
#define _NETBSD_SOURCE
#endif
#endif

#include "first.h"

#include "sys-dirent.h"
#include "sys-stat.h"
#include "sys-time.h"
#include "sys-unistd.h" /* <unistd.h> */

#include "base.h"
#include "log.h"
#include "buffer.h"
#include "chunk.h"
#include "fdevent.h"
#include "http_chunk.h"
#include "http_etag.h"
#include "http_header.h"
#include "keyvalue.h"
#include "response.h"

#include "plugin.h"

#include "stat_cache.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#ifdef AT_FDCWD
#ifndef _ATFILE_SOURCE
#define _ATFILE_SOURCE
#endif
#endif

#ifdef _WIN32
#include <windows.h>
#include <stringapiset.h>
#include <stdio.h>      /* FILENAME_MAX */
#endif

/**
 * this is a dirlisting for a lighttpd plugin
 *
 * Notes:
 * - mod_dirlisting is a dir list implementation.  One size does not fit all.
 *   mod_dirlisting aims to be somewhat flexible, but if special behavior
 *   is needed, use custom CGI/FastCGI/SCGI to handle dir listing instead.
 *   - backend daemon could implement custom caching
 *   - backend daemon could monitor directory for changes (e.g. inotify)
 *   - backend daemon or scripts could trigger when directory is modified
 *     and could regenerate index.html (and mod_indexfile could be used
 *     instead of mod_dirlisting)
 * - basic alphabetical sorting (in C locale) is done on server side
 *   in case client does not execute javascript
 *   (otherwise, response could be streamed, which is not done)
 *   (disable server-side sorting with dir-listing.sort = "disable")
 * - reading entire directory into memory for sorting large directory
 *   can lead to large memory usage if many simultaneous requests occur
 *   (disable server-side sorting with dir-listing.sort = "disable")
 */

struct dirlist_cache {
	int32_t max_age;
	buffer *path;
};

typedef struct {
	char dir_listing;
	char json;
	char sort;
	char hide_dot_files;
	char hide_readme_file;
	char encode_readme;
	char hide_header_file;
	char encode_header;
	char auto_layout;

	pcre_keyvalue_buffer *excludes;

	const buffer *show_readme;
	const buffer *show_header;
	const buffer *external_css;
	const buffer *external_js;
	const buffer *encoding;
	const buffer *set_footer;
	const struct dirlist_cache *cache;
} plugin_config;

typedef struct {
	PLUGIN_DATA;
	plugin_config defaults;
	plugin_config conf;
	int processing;
} plugin_data;

typedef struct {
	uint32_t namelen;
	unix_time64_t mtime;
	off_t    size;
} dirls_entry_t;

typedef struct {
	dirls_entry_t **ent;
	uint32_t used;
} dirls_list_t;

#define DIRLIST_ENT_NAME(ent)  ((char*)(ent) + sizeof(dirls_entry_t))
/* DIRLIST_BLOB_SIZE must be power of 2 for current internal usage */
#define DIRLIST_BLOB_SIZE      16

typedef struct {
  #ifndef _WIN32
	DIR *dp;
  #endif
	dirls_list_t dirs;
	dirls_list_t files;
	char *path;
	char *path_file;
	int dfd; /*(dirfd() owned by (DIR *))*/
	uint32_t name_max;
	buffer *hb;
	buffer *jb;
	int jcomma;
	int jfd;
	char *jfn;
	uint32_t jfn_len;
	int use_xattr;
	const array *mimetypes;
	plugin_config conf;
  #ifdef _WIN32
	HANDLE hFind;
	WIN32_FIND_DATAW ffd;
	char fnUTF8[FILENAME_MAX*4+1];
  #endif
} handler_ctx;

#define DIRLIST_BATCH 32
static int dirlist_max_in_progress;


__attribute_returns_nonnull__
static handler_ctx * mod_dirlisting_handler_ctx_init (plugin_data * const p) {
    handler_ctx *hctx = ck_calloc(1, sizeof(*hctx));
  #ifdef _WIN32
    hctx->hFind = INVALID_HANDLE_VALUE;
  #endif
    memcpy(&hctx->conf, &p->conf, sizeof(plugin_config));
    return hctx;
}

static void mod_dirlisting_handler_ctx_free (handler_ctx *hctx) {
  #ifdef _WIN32
    if (INVALID_HANDLE_VALUE != hctx->hFind)
        FindClose(hctx->hFind);
  #else
    if (hctx->dp)
        closedir(hctx->dp);
  #endif
    if (hctx->files.ent) {
        dirls_entry_t ** const ent = hctx->files.ent;
        for (uint32_t i = 0, used = hctx->files.used; i < used; ++i)
            free(ent[i]);
        free(ent);
    }
    if (hctx->dirs.ent) {
        dirls_entry_t ** const ent = hctx->dirs.ent;
        for (uint32_t i = 0, used = hctx->dirs.used; i < used; ++i)
            free(ent[i]);
        free(ent);
    }
    if (hctx->jb || hctx->hb) {
        if (hctx->jb)
            chunk_buffer_release(hctx->jb);
        else /* (hctx->hb) */
            chunk_buffer_release(hctx->hb);
        if (-1 != hctx->jfd)
            close(hctx->jfd);
        if (hctx->jfn) {
            unlink(hctx->jfn);
            free(hctx->jfn);
        }
    }
    free(hctx->path);
    free(hctx);
}


static struct dirlist_cache * mod_dirlisting_parse_cache(server *srv, const array *a) {
    const data_unset *du;

    du = array_get_element_klen(a, CONST_STR_LEN("max-age"));
    const int32_t max_age = config_plugin_value_to_int32(du, 15);

    buffer *path = NULL;
    du = array_get_element_klen(a, CONST_STR_LEN("path"));
    if (NULL == du) {
        if (0 != max_age) {
            log_error(srv->errh, __FILE__, __LINE__,
              "dir-listing.cache must include \"path\"");
            return NULL;
        }
    }
    else {
        if (du->type != TYPE_STRING) {
            log_error(srv->errh, __FILE__, __LINE__,
              "dir-listing.cache \"path\" must have string value");
            return NULL;
        }
        path = &((data_string *)du)->value;
        if (!stat_cache_path_isdir(path)) {
            if (errno == ENOTDIR) {
                log_error(srv->errh, __FILE__, __LINE__,
                  "dir-listing.cache \"path\" => \"%s\" is not a dir",
                  path->ptr);
                return NULL;
            }
            if (errno == ENOENT) {
                log_error(srv->errh, __FILE__, __LINE__,
                  "dir-listing.cache \"path\" => \"%s\" does not exist",
                  path->ptr);
                /*(warning; not returning NULL)*/
            }
        }
    }

    struct dirlist_cache * const cache =
      ck_calloc(1, sizeof(struct dirlist_cache));
    cache->max_age = max_age;
    cache->path = path;
    return cache;
}


static pcre_keyvalue_buffer * mod_dirlisting_parse_excludes(server *srv, const array *a) {
    const int pcre_jit = config_feature_bool(srv, "server.pcre_jit", 1);
    pcre_keyvalue_buffer * const kvb = pcre_keyvalue_buffer_init();
    buffer empty = { NULL, 0, 0 };
    for (uint32_t j = 0; j < a->used; ++j) {
        const data_string *ds = (data_string *)a->data[j];
        if (!pcre_keyvalue_buffer_append(srv->errh, kvb, &ds->value, &empty,
                                         pcre_jit)) {
            log_error(srv->errh, __FILE__, __LINE__,
              "pcre_compile failed for %s", ds->key.ptr);
            pcre_keyvalue_buffer_free(kvb);
            return NULL;
        }
    }
    return kvb;
}

#ifdef __COVERITY__
#include "burl.h"
#endif

static int mod_dirlisting_exclude(pcre_keyvalue_buffer * const kvb, const char * const name, const uint32_t len) {
    /*(re-use keyvalue.[ch] for match-only;
     *  must have been configured with empty kvb 'value' during init)*/
    buffer input = { NULL, len+1, 0 };
    *(const char **)&input.ptr = name;
    pcre_keyvalue_ctx ctx = { NULL, NULL, -1, 0, NULL, NULL };
  #ifdef __COVERITY__
    /*(again, must have been configured w/ empty kvb 'value' during init)*/
    struct cond_match_t cache;
    memset(&cache, 0, sizeof(cache));
    struct burl_parts_t bp;
    memset(&bp, 0, sizeof(bp));
    ctx.cache = &cache;
    ctx.burl = &bp;
  #endif
    /*(fail closed (simulate match to exclude) if there is an error)*/
    return HANDLER_ERROR == pcre_keyvalue_buffer_process(kvb,&ctx,&input,NULL)
        || -1 != ctx.m;
}


INIT_FUNC(mod_dirlisting_init) {
    return ck_calloc(1, sizeof(plugin_data));
}

FREE_FUNC(mod_dirlisting_free) {
    plugin_data * const p = p_d;
    if (NULL == p->cvlist) return;
    /* (init i to 0 if global context; to 1 to skip empty global context) */
    for (int i = !p->cvlist[0].v.u2[1], used = p->nconfig; i < used; ++i) {
        config_plugin_value_t *cpv = p->cvlist + p->cvlist[i].v.u2[0];
        for (; -1 != cpv->k_id; ++cpv) {
            switch (cpv->k_id) {
              case 2: /* dir-listing.exclude */
                if (cpv->vtype != T_CONFIG_LOCAL) continue;
                pcre_keyvalue_buffer_free(cpv->v.v);
                break;
              case 15: /* dir-listing.cache */
                if (cpv->vtype != T_CONFIG_LOCAL) continue;
                free(cpv->v.v);
                break;
              default:
                break;
            }
        }
    }
}

static void mod_dirlisting_merge_config_cpv(plugin_config * const pconf, const config_plugin_value_t * const cpv) {
    switch (cpv->k_id) { /* index into static config_plugin_keys_t cpk[] */
      case 0: /* dir-listing.activate */
      case 1: /* server.dir-listing *//*(historical)*/
        pconf->dir_listing = (char)cpv->v.u;
        break;
      case 2: /* dir-listing.exclude */
        if (cpv->vtype == T_CONFIG_LOCAL)
            pconf->excludes = cpv->v.v;
        break;
      case 3: /* dir-listing.hide-dotfiles */
        pconf->hide_dot_files = (char)cpv->v.u;
        break;
      case 4: /* dir-listing.external-css */
        pconf->external_css = cpv->v.b;
        break;
      case 5: /* dir-listing.external-js */
        pconf->external_js = cpv->v.b;
        break;
      case 6: /* dir-listing.encoding */
        pconf->encoding = cpv->v.b;
        break;
      case 7: /* dir-listing.show-readme */
        pconf->show_readme = cpv->v.b;
        break;
      case 8: /* dir-listing.hide-readme-file */
        pconf->hide_readme_file = (char)cpv->v.u;
        break;
      case 9: /* dir-listing.show-header */
        pconf->show_header = cpv->v.b;
        break;
      case 10:/* dir-listing.hide-header-file */
        pconf->hide_header_file = (char)cpv->v.u;
        break;
      case 11:/* dir-listing.set-footer */
        pconf->set_footer = cpv->v.b;
        break;
      case 12:/* dir-listing.encode-readme */
        pconf->encode_readme = (char)cpv->v.u;
        break;
      case 13:/* dir-listing.encode-header */
        pconf->encode_header = (char)cpv->v.u;
        break;
      case 14:/* dir-listing.auto-layout */
        pconf->auto_layout = (char)cpv->v.u;
        break;
      case 15:/* dir-listing.cache */
        if (cpv->vtype == T_CONFIG_LOCAL)
            pconf->cache = cpv->v.v;
        break;
      case 16:/* dir-listing.sort */
        pconf->sort = (char)cpv->v.u;
        break;
      default:/* should not happen */
        return;
    }
}

static void mod_dirlisting_merge_config(plugin_config * const pconf, const config_plugin_value_t *cpv) {
    do {
        mod_dirlisting_merge_config_cpv(pconf, cpv);
    } while ((++cpv)->k_id != -1);
}

static void mod_dirlisting_patch_config(request_st * const r, plugin_data * const p) {
    memcpy(&p->conf, &p->defaults, sizeof(plugin_config));
    for (int i = 1, used = p->nconfig; i < used; ++i) {
        if (config_check_cond(r, (uint32_t)p->cvlist[i].k_id))
            mod_dirlisting_merge_config(&p->conf, p->cvlist + p->cvlist[i].v.u2[0]);
    }
}

SETDEFAULTS_FUNC(mod_dirlisting_set_defaults) {
    static const config_plugin_keys_t cpk[] = {
      { CONST_STR_LEN("dir-listing.activate"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("server.dir-listing"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.exclude"),
        T_CONFIG_ARRAY_VLIST,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.hide-dotfiles"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.external-css"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.external-js"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.encoding"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.show-readme"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.hide-readme-file"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.show-header"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.hide-header-file"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.set-footer"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.encode-readme"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.encode-header"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.auto-layout"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.cache"),
        T_CONFIG_ARRAY_KVANY,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.sort"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ NULL, 0,
        T_CONFIG_UNSET,
        T_CONFIG_SCOPE_UNSET }
    };

    plugin_data * const p = p_d;
    if (!config_plugin_values_init(srv, p, cpk, "mod_dirlisting"))
        return HANDLER_ERROR;

    /* process and validate config directives
     * (init i to 0 if global context; to 1 to skip empty global context) */
    for (int i = !p->cvlist[0].v.u2[1]; i < p->nconfig; ++i) {
        config_plugin_value_t *cpv = p->cvlist + p->cvlist[i].v.u2[0];
        for (; -1 != cpv->k_id; ++cpv) {
            switch (cpv->k_id) {
              case 0: /* dir-listing.activate */
              case 1: /* server.dir-listing *//*(historical)*/
                break;
              case 2: /* dir-listing.exclude */
                cpv->v.v = mod_dirlisting_parse_excludes(srv, cpv->v.a);
                if (NULL == cpv->v.v) return HANDLER_ERROR;
                cpv->vtype = T_CONFIG_LOCAL;
                break;
              case 3: /* dir-listing.hide-dotfiles */
                break;
              case 4: /* dir-listing.external-css */
              case 5: /* dir-listing.external-js */
              case 6: /* dir-listing.encoding */
                if (buffer_is_blank(cpv->v.b))
                    cpv->v.b = NULL;
                break;
              case 7: /* dir-listing.show-readme */
                if (!buffer_is_blank(cpv->v.b)) {
                    buffer *b;
                    *(const buffer **)&b = cpv->v.b;
                    if (buffer_is_equal_string(b, CONST_STR_LEN("enable")))
                        buffer_copy_string_len(b, CONST_STR_LEN("README.txt"));
                    else if (buffer_is_equal_string(b,CONST_STR_LEN("disable")))
                        buffer_clear(b);
                }
                else
                    cpv->v.b = NULL;
                break;
              case 8: /* dir-listing.hide-readme-file */
                break;
              case 9: /* dir-listing.show-header */
                if (!buffer_is_blank(cpv->v.b)) {
                    buffer *b;
                    *(const buffer **)&b = cpv->v.b;
                    if (buffer_is_equal_string(b, CONST_STR_LEN("enable")))
                        buffer_copy_string_len(b, CONST_STR_LEN("HEADER.txt"));
                    else if (buffer_is_equal_string(b,CONST_STR_LEN("disable")))
                        buffer_clear(b);
                }
                else
                    cpv->v.b = NULL;
                break;
              case 10:/* dir-listing.hide-header-file */
                break;
              case 11:/* dir-listing.set-footer */
                if (buffer_is_blank(cpv->v.b))
                    cpv->v.b = NULL;
                break;
              case 12:/* dir-listing.encode-readme */
              case 13:/* dir-listing.encode-header */
              case 14:/* dir-listing.auto-layout */
                break;
              case 15:/* dir-listing.cache */
                cpv->v.v = mod_dirlisting_parse_cache(srv, cpv->v.a);
                if (NULL == cpv->v.v) return HANDLER_ERROR;
                if (0 == ((struct dirlist_cache *)cpv->v.v)->max_age) {
                    free(cpv->v.v);
                    cpv->v.v = NULL; /*(to disable after having been enabled)*/
                }
                cpv->vtype = T_CONFIG_LOCAL;
                break;
              case 16:/* dir-listing.sort */
                break;
              default:/* should not happen */
                break;
            }
        }
    }

    dirlist_max_in_progress = srv->srvconf.max_conns >> 4;
    if (0 == dirlist_max_in_progress) dirlist_max_in_progress = 1;

    p->defaults.dir_listing = 0;
    p->defaults.json = 0;
    p->defaults.hide_dot_files = 1;
    p->defaults.hide_readme_file = 0;
    p->defaults.hide_header_file = 0;
    p->defaults.encode_readme = 1;
    p->defaults.encode_header = 1;
    p->defaults.auto_layout = 1;
    p->defaults.sort = 1;

    /* initialize p->defaults from global config context */
    if (p->nconfig > 0 && p->cvlist->v.u2[1]) {
        const config_plugin_value_t *cpv = p->cvlist + p->cvlist->v.u2[0];
        if (-1 != cpv->k_id)
            mod_dirlisting_merge_config(&p->defaults, cpv);
    }

    return HANDLER_GO_ON;
}

/* simple combsort algorithm */
static void http_dirls_sort(dirls_entry_t **ent, int num) {
	int gap = num;
	int i, j;
	int swapped;
	dirls_entry_t *tmp;

	do {
		gap = (gap * 10) / 13;
		if (gap == 9 || gap == 10)
			gap = 11;
		if (gap < 1)
			gap = 1;
		swapped = 0;

		for (i = 0; i < num - gap; i++) {
			j = i + gap;
			if (strcmp(DIRLIST_ENT_NAME(ent[i]), DIRLIST_ENT_NAME(ent[j])) > 0) {
				tmp = ent[i];
				ent[i] = ent[j];
				ent[j] = tmp;
				swapped = 1;
			}
		}

	} while (gap > 1 || swapped);
}

/* buffer must be able to hold "999.9K"
 * conversion is simple but not perfect
 */
static size_t http_list_directory_sizefmt(char *buf, size_t bufsz, off_t size) {
	int remain;
	int u = -1;  /* u will always increment at least once */
	size_t buflen;

	if (0 < size && size < 100)
		size += 99;

	do {
		remain = (int)(size & 1023);
		size >>= 10;
		++u;
	} while (size & ~1023);

	remain /= 100;
	if (remain > 9)
		remain = 9;
	if (size > 999) {
		size   = 0;
		remain = 9;
		u++;
	}

	buflen = li_itostrn(buf, bufsz, size);
	if (buflen + 3 >= bufsz) return buflen;
	buf[buflen+0] = '.';
	buf[buflen+1] = remain + '0';
	buf[buflen+2] = "KMGTPE"[u];  /* Kilo, Mega, Giga, Tera, Peta, Exa */
	buf[buflen+3] = '\0';

	return buflen + 3;
}

static void http_list_directory_include_file(request_st * const r, const handler_ctx * const p, int is_header) {
    const buffer *path;
    int encode = 0;
    if (is_header) {
        path = p->conf.show_header;
        encode = p->conf.encode_header;
    }
    else {
        path = p->conf.show_readme;
        encode = p->conf.encode_readme;
    }
    if (NULL == path) return;

    uint32_t len = 0;
    if (path->ptr[0] != '/') { /* temporarily extend r->physical.path */
        len = buffer_clen(&r->physical.path);
        buffer_append_path_len(&r->physical.path, BUF_PTR_LEN(path));
        path = &r->physical.path;
    }
    stat_cache_entry * const sce =
      stat_cache_get_entry_open(path, r->conf.follow_symlink);
    if (len)
        buffer_truncate(&r->physical.path, len);
    if (NULL == sce || sce->fd < 0 || 0 == sce->st.st_size)
        return;

    chunkqueue * const cq = &r->write_queue;
    if (encode) {
        if (is_header)
            chunkqueue_append_mem(cq, CONST_STR_LEN("<pre class=\"header\">"));
        else
            chunkqueue_append_mem(cq, CONST_STR_LEN("<pre class=\"readme\">"));

        /* Note: encoding a very large file may cause lighttpd to pause handling
         * other requests while lighttpd encodes the file, especially if file is
         * on a remote filesystem */

        /* encoding can consume 6x file size in worst case scenario,
         * so send encoded contents of files > 32k to tempfiles) */
        buffer * const tb = r->tmp_buf;
        buffer * const out = sce->st.st_size <= 32768
          ? chunkqueue_append_buffer_open(cq)
          : tb;
        buffer_clear(out);
        const int fd = sce->fd;
        ssize_t rd;
        off_t off = 0;
        char buf[8192];
        while ((rd = chunk_file_pread(fd, buf, sizeof(buf), off)) > 0) {
            off += rd;
            buffer_append_string_encoded(out, buf, (size_t)rd, ENCODING_MINIMAL_XML);
            if (out == tb) {
                if (0 != chunkqueue_append_mem_to_tempfile(cq,
                                                           BUF_PTR_LEN(out),
                                                           r->conf.errh))
                    break;
                buffer_clear(out);
            }
        }
        if (out != tb)
            chunkqueue_append_buffer_commit(cq);

        chunkqueue_append_mem(cq, CONST_STR_LEN("</pre>"));
    }
    else {
        (void)http_chunk_append_file_ref(r, sce);
    }
}

static void http_dirlist_link (request_st * const r, const buffer *b, const char *params, size_t plen) {
    /*(params must be at least ">" to close Link url-reference)*/
    buffer * const tb = r->tmp_buf;
    buffer_clear(tb);
    buffer_append_str3(tb, CONST_STR_LEN("<"), BUF_PTR_LEN(b), params, plen);
    http_header_response_insert(r, HTTP_HEADER_LINK,
                                CONST_STR_LEN("Link"),
                                BUF_PTR_LEN(tb));
}

static void http_dirlist_auto_layout_early_hints (request_st * const r, const plugin_data * const p) {
    if (p->conf.external_css)
        http_dirlist_link(r, p->conf.external_css,
          CONST_STR_LEN(">; rel=\"preload\"; as=\"style\""));
    if (p->conf.external_js)
        http_dirlist_link(r, p->conf.external_js,
          CONST_STR_LEN(">; rel=\"preload\"; as=\"script\""));
}

/* portions copied from mod_status
 * modified and specialized for stable dirlist sorting by name */
static const char js_simple_table_resort[] = \
"var click_column;\n" \
"var name_column = 0;\n" \
"var date_column = 1;\n" \
"var size_column = 2;\n" \
"var type_column = 3;\n" \
"var prev_span = null;\n" \
"\n" \
"if (typeof(String.prototype.localeCompare) === 'undefined') {\n" \
" String.prototype.localeCompare = function(str, locale, options) {\n" \
"   return ((this == str) ? 0 : ((this > str) ? 1 : -1));\n" \
" };\n" \
"}\n" \
"\n" \
"if (typeof(String.prototype.toLocaleUpperCase) === 'undefined') {\n" \
" String.prototype.toLocaleUpperCase = function() {\n" \
"  return this.toUpperCase();\n" \
" };\n" \
"}\n" \
"\n" \
"function get_inner_text(el) {\n" \
" if((typeof el == 'string')||(typeof el == 'undefined'))\n" \
"  return el;\n" \
" if(el.dataset\n" \
"    && (typeof el.dataset.value === 'string'\n" \
"        || typeof el.dataset.value === 'number'))\n" \
"  return el.dataset.value;\n" \
" if(el.innerText)\n" \
"  return el.innerText;\n" \
" else {\n" \
"  var str = \"\";\n" \
"  var cs = el.childNodes;\n" \
"  var l = cs.length;\n" \
"  for (var i=0;i<l;i++) {\n" \
"   if (cs[i].nodeType==1) str += get_inner_text(cs[i]);\n" \
"   else if (cs[i].nodeType==3) str += cs[i].nodeValue;\n" \
"  }\n" \
" }\n" \
" return str;\n" \
"}\n" \
"\n" \
"var li_date_regex=/(\\d{4})-(\\w{3})-(\\d{2}) (\\d{2}):(\\d{2}):(\\d{2})/;\n" \
"\n" \
"var li_mon = ['Jan','Feb','Mar','Apr','May','Jun',\n" \
"              'Jul','Aug','Sep','Oct','Nov','Dec'];\n" \
"\n" \
"function li_mon_num(mon) {\n" \
" var i; for (i = 0; i < 12 && mon != li_mon[i]; ++i); return i;\n" \
"}\n" \
"\n" \
"function li_dates_to_dv(table) {\n" \
" for (var j=1;j<table.rows.length;j++) {\n" \
"  var el = table.rows[j].cells[date_column];\n" \
"  if(el.dataset && typeof el.dataset.value != 'undefined') {\n" \
"   if (typeof el.dataset.value == 'number')\n" \
"    break;\n" \
"   if (el.dataset.value === \"-1\")\n" \
"    el.dataset.value = -1;\n" \
"  } else {\n" \
"   var d = li_date_regex.exec(get_inner_text(el));\n" \
"   el.dataset.value = (parseInt(d[1])*(1<<26))\n" \
"     + ( (li_mon_num(d[2])<<22)\n" \
"        |(parseInt(d[3])  <<17)\n" \
"        |(parseInt(d[4])  <<12)\n" \
"        |(parseInt(d[5])  << 6)\n" \
"        |(parseInt(d[6])) );\n" \
"  }\n" \
" }\n" \
"}\n" \
"\n" \
"function sortfn_then_by_name(a,b,sort_column) {\n" \
" if (sort_column == name_column || sort_column == type_column) {\n" \
"  var ad = (a.cells[type_column].innerHTML === 'Directory');\n" \
"  var bd = (b.cells[type_column].innerHTML === 'Directory');\n" \
"  if (ad != bd) return (ad ? -1 : 1);\n" \
" }\n" \
" var at = get_inner_text(a.cells[sort_column]);\n" \
" var bt = get_inner_text(b.cells[sort_column]);\n" \
" var cmp;\n" \
" if (sort_column == size_column) {\n" \
"  cmp = parseInt(at)-parseInt(bt);\n" \
" } else if (sort_column == date_column) {\n" \
"  cmp = at-bt;\n" \
" } else {\n" \
"  if (sort_column == name_column) {\n" \
"   if (at == '../') return -1;\n" \
"   if (bt == '../') return  1;\n" \
"  }\n" \
"  cmp = at.toLocaleUpperCase().localeCompare(bt.toLocaleUpperCase());\n" \
"  if (0 != cmp) return cmp;\n" \
"  cmp = at.localeCompare(bt);\n" \
" }\n" \
" if (0 != cmp || sort_column == name_column) return cmp;\n" \
" return sortfn_then_by_name(a,b,name_column);\n" \
"}\n" \
"\n" \
"function sortfn(a,b) {\n" \
" return sortfn_then_by_name(a,b,click_column);\n" \
"}\n" \
"\n" \
"function resort(lnk) {\n" \
" var span = lnk.childNodes[1];\n" \
" var table = lnk.parentNode.parentNode.parentNode.parentNode;\n" \
" click_column = lnk.parentNode.cellIndex;\n" \
" if (click_column == date_column) li_dates_to_dv(table);\n" \
" var rows = new Array();\n" \
" for (var j=1;j<table.rows.length;j++)\n" \
"  rows[j-1] = table.rows[j];\n" \
" rows.sort(sortfn);\n" \
"\n" \
" if (prev_span != null) prev_span.innerHTML = '';\n" \
" if (span.getAttribute('sortdir')=='down') {\n" \
"  span.innerHTML = '&uarr;';\n" \
"  span.setAttribute('sortdir','up');\n" \
"  rows.reverse();\n" \
" } else {\n" \
"  span.innerHTML = '&darr;';\n" \
"  span.setAttribute('sortdir','down');\n" \
" }\n" \
" for (var i=0;i<rows.length;i++)\n" \
"  table.tBodies[0].appendChild(rows[i]);\n" \
" prev_span = span;\n" \
"}\n";

/* portions copied from mod_dirlist (lighttpd2) */
static const char js_simple_table_init_sort[] = \
"\n" \
"function init_sort(init_sort_column, descending) {\n" \
" var tables = document.getElementsByTagName(\"table\");\n" \
" for (var i = 0; i < tables.length; i++) {\n" \
"  var table = tables[i];\n" \
"  //var c = table.getAttribute(\"class\")\n" \
"  //if (-1 != c.split(\" \").indexOf(\"sort\")) {\n" \
"   var row = table.rows[0].cells;\n" \
"   for (var j = 0; j < row.length; j++) {\n" \
"    var n = row[j];\n" \
"    if (n.childNodes.length == 1 && n.childNodes[0].nodeType == 3) {\n" \
"     var link = document.createElement(\"a\");\n" \
"     var title = n.childNodes[0].nodeValue.replace(/:$/, \"\");\n" \
"     link.appendChild(document.createTextNode(title));\n" \
"     link.setAttribute(\"href\", \"#\");\n" \
"     link.setAttribute(\"class\", \"sortheader\");\n" \
"     link.setAttribute(\"onclick\", \"resort(this);return false;\");\n" \
"     var arrow = document.createElement(\"span\");\n" \
"     arrow.setAttribute(\"class\", \"sortarrow\");\n" \
"     arrow.appendChild(document.createTextNode(\":\"));\n" \
"     link.appendChild(arrow)\n" \
"     n.replaceChild(link, n.firstChild);\n" \
"    }\n" \
"   }\n" \
"   var lnk = row[init_sort_column].firstChild;\n" \
"   if (descending) {\n" \
"    var span = lnk.childNodes[1];\n" \
"    span.setAttribute('sortdir','down');\n" \
"   }\n" \
"   resort(lnk);\n" \
"  //}\n" \
" }\n" \
"}\n" \
"\n" \
"function init_sort_from_query() {\n" \
"  var urlParams = new URLSearchParams(location.search);\n" \
"  var c = 0;\n" \
"  var o = 0;\n" \
"  switch (urlParams.get('C')) {\n" \
"    case \"N\": c=0; break;\n" \
"    case \"M\": c=1; break;\n" \
"    case \"S\": c=2; break;\n" \
"    case \"T\":\n" \
"    case \"D\": c=3; break;\n" \
"  }\n" \
"  switch (urlParams.get('O')) {\n" \
"    case \"A\": o=0; break;\n" \
"    case \"D\": o=1; break;\n" \
"  }\n" \
"  init_sort(c,o);\n" \
"}\n" \
"init_sort_from_query();\n";


static void http_dirlist_append_js_table_resort (buffer * const b) {
	struct const_iovec iov[] = {
	  { CONST_STR_LEN("\n<script type=\"text/javascript\">\n// <!--\n\n") }
	 ,{ CONST_STR_LEN(js_simple_table_resort) }
	 ,{ CONST_STR_LEN(js_simple_table_init_sort) }
	 ,{ CONST_STR_LEN("\n// -->\n</script>\n\n") }
	};
	buffer_append_iovec(b, iov, sizeof(iov)/sizeof(*iov));
}

static void http_list_directory_header(request_st * const r, const handler_ctx * const p) {

	chunkqueue * const cq = &r->write_queue;
	if (p->conf.auto_layout) {
		buffer * const out = chunkqueue_append_buffer_open(cq);
		buffer_append_string_len(out, CONST_STR_LEN(
			"<!DOCTYPE html>\n"
			"<html>\n"
			"<head>\n"
		));
		if (p->conf.encoding) {
			buffer_append_str3(out,
			  CONST_STR_LEN("<meta charset=\""),
			  BUF_PTR_LEN(p->conf.encoding),
			  CONST_STR_LEN("\">\n"));
		}
		buffer_append_string_len(out, CONST_STR_LEN("<title>Index of "));
		buffer_append_string_encoded(out, BUF_PTR_LEN(&r->uri.path), ENCODING_MINIMAL_XML);
		buffer_append_string_len(out, CONST_STR_LEN("</title>\n"));

		if (p->conf.external_css) {
			buffer_append_str3(out,
			  CONST_STR_LEN("<meta name=\"viewport\" content=\"initial-scale=1\">"
			                "<link rel=\"stylesheet\" type=\"text/css\" href=\""),
			  BUF_PTR_LEN(p->conf.external_css),
			  CONST_STR_LEN("\">\n"));
		} else {
			buffer_append_string_len(out, CONST_STR_LEN(
				"<style type=\"text/css\">\n"
				"a, a:active {text-decoration: none; color: blue;}\n"
				"a:visited {color: #48468F;}\n"
				"a:hover, a:focus {text-decoration: underline; color: red;}\n"
				"body {background-color: #F5F5F5;}\n"
				"h2 {margin-bottom: 12px;}\n"
				"table {margin-left: 12px;}\n"
				"th, td {"
				" font: 90% monospace;"
				" text-align: left;"
				"}\n"
				"th {"
				" font-weight: bold;"
				" padding-right: 14px;"
				" padding-bottom: 3px;"
				"}\n"
				"td {padding-right: 14px;}\n"
				"td.s, th.s {text-align: right;}\n"
				"div.list {"
				" background-color: white;"
				" border-top: 1px solid #646464;"
				" border-bottom: 1px solid #646464;"
				" padding-top: 10px;"
				" padding-bottom: 14px;"
				"}\n"
				"div.foot {"
				" font: 90% monospace;"
				" color: #787878;"
				" padding-top: 4px;"
				"}\n"
				"@media (prefers-color-scheme: dark) {\n"
				" a, a:active {color: #9E9EFF;}\n"
				" a:visited {color: #D0ADF0;}\n"
				" body, div.list {background-color: transparent;}\n"
				" div.foot {color: #878787;}\n"
				"}\n"
				"</style>\n"
				"<meta name=\"color-scheme\" content=\"light dark\">\n"
			));
		}

		buffer_append_string_len(out, CONST_STR_LEN("</head>\n<body>\n"));
		chunkqueue_append_buffer_commit(cq);
	}

	if (p->conf.show_header) {
		http_list_directory_include_file(r, p, 1);/*0 for readme; 1 for header*/
	}

	buffer * const out = chunkqueue_append_buffer_open(cq);
	buffer_append_string_len(out, CONST_STR_LEN("<h2>Index of "));
	buffer_append_string_encoded(out, BUF_PTR_LEN(&r->uri.path), ENCODING_MINIMAL_XML);
	buffer_append_string_len(out, CONST_STR_LEN(
		"</h2>\n"
		"<div class=\"list\">\n"
		"<table summary=\"Directory Listing\" cellpadding=\"0\" cellspacing=\"0\">\n"
		"<thead>"
		"<tr>"
			"<th class=\"n\">Name</th>"
			"<th class=\"m\">Last Modified</th>"
			"<th class=\"s\">Size</th>"
			"<th class=\"t\">Type</th>"
		"</tr>"
		"</thead>\n"
		"<tbody>\n"
	));
	if (!buffer_is_equal_string(&r->uri.path, CONST_STR_LEN("/"))) {
		buffer_append_string_len(out, CONST_STR_LEN(
		"<tr class=\"d\">"
			"<td class=\"n\"><a href=\"../\">..</a>/</td>"
			"<td class=\"m\" data-value=\"-1\">&nbsp;</td>"
			"<td class=\"s\" data-value=\"-1\">- &nbsp;</td>"
			"<td class=\"t\">Directory</td>"
		"</tr>\n"
		));
	}
	chunkqueue_append_buffer_commit(cq);
}

static void http_list_directory_mtime(buffer * const out, const dirls_entry_t * const ent) {
	struct tm tm;
  #ifdef __MINGW32__
	buffer_append_strftime(out, "%Y-%b-%d %H:%M:%S", localtime64_r(&ent->mtime, &tm));
  #else
	buffer_append_strftime(out, "%Y-%b-%d %T", localtime64_r(&ent->mtime, &tm));
  #endif
}

static void http_list_directory_ent(buffer * const out, const dirls_entry_t * const ent, const char * const name) {
	buffer_append_string_encoded(out, name, ent->namelen, ENCODING_REL_URI_PART);
	buffer_append_string_len(out, CONST_STR_LEN("/\">"));
	buffer_append_string_encoded(out, name, ent->namelen, ENCODING_MINIMAL_XML);
	buffer_append_string_len(out, CONST_STR_LEN("</a>/</td><td class=\"m\">"));

	http_list_directory_mtime(out, ent);
}

__attribute_noinline__
static void http_list_directory_dirname(buffer * const out, const dirls_entry_t * const ent, const char * const name) {
	buffer_append_string_len(out, CONST_STR_LEN("<tr class=\"d\"><td class=\"n\"><a href=\""));

	http_list_directory_ent(out, ent, name);

	buffer_append_string_len(out, CONST_STR_LEN("</td><td class=\"s\" data-value=\"-1\">- &nbsp;</td><td class=\"t\">Directory</td></tr>\n"));
}

static void http_list_file_ent(buffer * const out, const dirls_entry_t * const ent, const char * const name) {
	buffer_append_string_encoded(out, name, ent->namelen, ENCODING_REL_URI_PART);
	buffer_append_string_len(out, CONST_STR_LEN("\">"));
	buffer_append_string_encoded(out, name, ent->namelen, ENCODING_MINIMAL_XML);
	buffer_append_string_len(out, CONST_STR_LEN("</a></td><td class=\"m\">"));

	http_list_directory_mtime(out, ent);
}

static void http_list_directory_filename(buffer * const out, const dirls_entry_t * const ent, const char * const name, handler_ctx * const hctx) {
	buffer_append_string_len(out, CONST_STR_LEN("<tr><td class=\"n\"><a href=\""));

	http_list_file_ent(out, ent, name);

	const buffer *content_type;
  #if defined(HAVE_XATTR) || defined(HAVE_EXTATTR) /*(pass full path)*/
	content_type = NULL;
	if (hctx->use_xattr) {
		memcpy(hctx->path_file, name, ent->namelen + 1);
		content_type = stat_cache_mimetype_by_xattr(hctx->path);
	}
	if (NULL == content_type)
  #endif
		content_type = stat_cache_mimetype_by_ext(hctx->mimetypes, name, ent->namelen);
	if (NULL == content_type) {
		static const buffer octet_stream =
		  { "application/octet-stream",
		    sizeof("application/octet-stream"), 0 };
		content_type = &octet_stream;
	}

	char sizebuf[sizeof("999.9K")];
	char dvbuf[LI_ITOSTRING_LENGTH];
	size_t buflen =
	  http_list_directory_sizefmt(sizebuf, sizeof(sizebuf), ent->size);
	struct const_iovec iov[] = {
	  { CONST_STR_LEN("</td><td class=\"s\" data-value=\"") }
	 ,{ dvbuf, li_itostrn(dvbuf, sizeof(dvbuf), ent->size) }
	 ,{ CONST_STR_LEN("\">") }
	 ,{ sizebuf, buflen }
	 ,{ CONST_STR_LEN("</td><td class=\"t\">") }
	 ,{ BUF_PTR_LEN(content_type) }
	 ,{ CONST_STR_LEN("</td></tr>\n") }
	};
	buffer_append_iovec(out, iov, sizeof(iov)/sizeof(*iov));
}

static void http_list_directory_dir(buffer * const out, const dirls_entry_t * const ent) {
	http_list_directory_dirname(out, ent, DIRLIST_ENT_NAME(ent));
}

static void http_list_directory_file(buffer * const out, const dirls_entry_t * const ent, handler_ctx * const hctx) {
	http_list_directory_filename(out, ent, DIRLIST_ENT_NAME(ent), hctx);
}

static void http_list_directory_footer(request_st * const r, const handler_ctx * const p) {

	chunkqueue * const cq = &r->write_queue;
	chunkqueue_append_mem(cq, CONST_STR_LEN(
		"</tbody>\n"
		"</table>\n"
		"</div>\n"
	));

	if (p->conf.show_readme) {
		http_list_directory_include_file(r, p, 0);/*0 for readme; 1 for header*/
	}

	if (p->conf.auto_layout) {
		buffer * const out = chunkqueue_append_buffer_open(cq);
		const buffer * const footer =
		  p->conf.set_footer
		    ? p->conf.set_footer
		    : r->conf.server_tag
		        ? r->conf.server_tag
		        : NULL;
		if (footer)
			buffer_append_str3(out,
			  CONST_STR_LEN("<div class=\"foot\">"),
			  BUF_PTR_LEN(footer),
			  CONST_STR_LEN("</div>\n"));

		if (p->conf.external_js)
			buffer_append_str3(out,
			  CONST_STR_LEN("<script type=\"text/javascript\" src=\""),
			  BUF_PTR_LEN(p->conf.external_js),
			  CONST_STR_LEN("\"></script>\n"));
		else
			http_dirlist_append_js_table_resort(out);

		buffer_append_string_len(out, CONST_STR_LEN(
			"</body>\n"
			"</html>\n"
		));
		chunkqueue_append_buffer_commit(cq);
	}
}

__attribute_noinline__
static void http_list_directory_jsonname(buffer * const out, const dirls_entry_t * const ent, const char * const name, handler_ctx * const hctx, const int isdir) {
	if (__builtin_expect( (hctx->jcomma), 1))/*(to avoid excess comma)*/
		buffer_append_string_len(out, CONST_STR_LEN(",{\"name\":\""));
	else {
		hctx->jcomma = 1;
		buffer_append_string_len(out, CONST_STR_LEN( "{\"name\":\""));
	}
	buffer_append_bs_escaped_json(out, name, ent->namelen);

	const char *t;
	size_t tlen;
	if (!isdir) {
		t =           "\",\"type\":\"file\",\"size\":";
		tlen = sizeof("\",\"type\":\"file\",\"size\":")-1;
	}
	else {
		t =           "\",\"type\":\"dir\",\"size\":";
		tlen = sizeof("\",\"type\":\"dir\",\"size\":")-1;
	}
	char sstr[LI_ITOSTRING_LENGTH];
	char mstr[LI_ITOSTRING_LENGTH];
	struct const_iovec iov[] = {
	  { t, tlen }
	 ,{ sstr, li_itostrn(sstr, sizeof(sstr), ent->size) }
	 ,{ CONST_STR_LEN(",\"mtime\":") }
	 ,{ mstr, li_itostrn(mstr, sizeof(mstr), ent->mtime) }
	 ,{ CONST_STR_LEN("}") }
	};
	buffer_append_iovec(out, iov, sizeof(iov)/sizeof(*iov));
}

static int http_open_directory(request_st * const r, handler_ctx * const hctx) {
    const uint32_t dlen = buffer_clen(&r->physical.path);
#ifdef _WIN32
    hctx->name_max = FILENAME_MAX*4; /*(260 chars * 4 for (max) UTF-8 bytes)*/
#else
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
    /* allocate based on PATH_MAX rather than pathconf() to get _PC_NAME_MAX */
    hctx->name_max = PATH_MAX - dlen - 1;
#endif
    hctx->path = ck_malloc(dlen + hctx->name_max + 1);
    memcpy(hctx->path, r->physical.path.ptr, dlen+1);
  #if defined(HAVE_XATTR) || defined(HAVE_EXTATTR) \
   || (!defined(_ATFILE_SOURCE) && !defined(_WIN32))
    hctx->path_file = hctx->path + dlen;
  #endif

  #ifdef _WIN32
    hctx->path[dlen] = '*';
    hctx->path[dlen+1] = '\0';
    WCHAR wbuf[4096];
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   hctx->path, dlen+1,
                                   wbuf, (sizeof(wbuf)/sizeof(*wbuf))-2);
    if (0 == wlen) return -1;
    hctx->hFind = FindFirstFileExW(wbuf, FindExInfoBasic, &hctx->ffd,
                                   FindExSearchNameMatch, NULL,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (INVALID_HANDLE_VALUE == hctx->hFind) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            log_error(r->conf.errh, __FILE__, __LINE__,
              "FindFirstFileEx failed: %s", r->physical.path.ptr);
            return -1;
        }
    }
  #else
  #ifndef _ATFILE_SOURCE /*(not using fdopendir unless _ATFILE_SOURCE)*/
    hctx->dfd = -1;
    hctx->dp = opendir(hctx->path);
  #else
    hctx->dfd = fdevent_open_dirname(hctx->path, r->conf.follow_symlink);
    hctx->dp = (hctx->dfd >= 0) ? fdopendir(hctx->dfd) : NULL;
  #endif
    if (NULL == hctx->dp) {
        log_perror(r->conf.errh, __FILE__, __LINE__, "opendir %s", hctx->path);
        if (hctx->dfd >= 0) {
            close(hctx->dfd);
            hctx->dfd = -1;
        }
        return -1;
    }
  #endif

    if (hctx->conf.json) return 0;

    dirls_list_t * const dirs = &hctx->dirs;
    dirls_list_t * const files = &hctx->files;
    dirs->ent   = NULL;
    dirs->used  = 0;
    files->ent  = NULL;
    files->used = 0;

    return 0;
}

static int http_read_directory(handler_ctx * const p) {
  #ifdef _WIN32
	int count = 0;
	if (INVALID_HANDLE_VALUE == p->hFind) {
		/* GetLastError() == ERROR_FILE_NOT_FOUND
		 * (other errors handled in http_open_directory()) */
	}
	else do
  #else
	int count = -1;
	struct dirent *dent;
	struct stat st;
	while (++count < DIRLIST_BATCH && (dent = readdir(p->dp)) != NULL)
  #endif
	{
	  #ifdef _WIN32
		/* WC_ERR_INVALID_CHARS not used in string conversion since
		 * expecting valid unicode from reading directory */
		const char * const d_name = p->fnUTF8;
		uint32_t dsz = (uint32_t)
		  WideCharToMultiByte(CP_UTF8, 0, p->ffd.cFileName, -1,
		                      p->fnUTF8, sizeof(p->fnUTF8), NULL, NULL);
		if (0 == dsz) continue;
		--dsz;
	  #else
		const char * const d_name = dent->d_name;
		const uint32_t dsz = (uint32_t) _D_EXACT_NAMLEN(dent);
	  #endif
		if (d_name[0] == '.') {
			if (p->conf.hide_dot_files)
				continue;
			if (d_name[1] == '\0')
				continue;
			if (d_name[1] == '.' && d_name[2] == '\0')
				continue;
		}

		if (p->conf.hide_readme_file
		    && p->conf.show_readme
		    && buffer_eq_slen(p->conf.show_readme, d_name, dsz))
			continue;
		if (p->conf.hide_header_file
		    && p->conf.show_header
		    && buffer_eq_slen(p->conf.show_header, d_name, dsz))
			continue;

		/* compare d_name against excludes array
		 * elements, skipping any that match.
		 */
		if (p->conf.excludes
		    && mod_dirlisting_exclude(p->conf.excludes, d_name, dsz))
			continue;

		/* NOTE: the manual says, d_name is never more than NAME_MAX
		 *       so this should actually not be a buffer-overflow-risk
		 */
		if (dsz > p->name_max) continue;
	  #ifdef __COVERITY__
		/* For some reason, Coverity overlooks the strlen() performed
		 * a few lines above and thinks memcpy() below might access
		 * bytes beyond end of d_name[] with dsz+1 */
		force_assert(dsz < sizeof(dent->d_name));
	  #endif

	  #ifndef _WIN32
	  #ifndef _ATFILE_SOURCE
		memcpy(p->path_file, d_name, dsz + 1);
		if (stat(p->path, &st) != 0)
			continue;
	  #else
		/*(XXX: follow symlinks, like stat(); not using AT_SYMLINK_NOFOLLOW) */
		if (0 != fstatat(p->dfd, d_name, &st, 0))
			continue; /* file *just* disappeared? */
	  #endif
	  #endif

		/* const dirls_entry_t ent = { namelen, mtime, size } */
	  #ifdef _WIN32 /*(convert 100ns ticks since 1 Jan 1601 to unix time_t)*/
		/*(future: preserve FILETIME here and use Windows fn to format time
		 * below instead of localtime_r() and buffer_append_strftime())*/
		const dirls_entry_t ent = {
		  dsz,
		  (unix_time64_t)
		    ((((int64_t)p->ffd.ftLastWriteTime.dwHighDateTime << 32)
		              | p->ffd.ftLastWriteTime.dwLowDateTime)
		    / 10000000 - 11644473600LL),
		  ((int64_t)p->ffd.nFileSizeHigh << 32) | p->ffd.nFileSizeLow
		};
		const int isdir = (p->ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
	  #else
		const dirls_entry_t ent = { dsz, TIME64_CAST(st.st_mtime), st.st_size };
		const int isdir = S_ISDIR(st.st_mode);
	  #endif

		if (p->jb) { /* json output */
			http_list_directory_jsonname(p->jb, &ent, d_name, p, isdir);
			continue;
		}

		if (p->hb) { /* html output **unsorted** */
			if (isdir)
				http_list_directory_dirname(p->hb, &ent, d_name);
			else
				http_list_directory_filename(p->hb, &ent, d_name, p);
			continue;
		}

		dirls_list_t * const list = !isdir ? &p->files : &p->dirs;
		if (!(list->used & (DIRLIST_BLOB_SIZE-1)))
			ck_realloc_u32((void **)&list->ent, list->used,
			               DIRLIST_BLOB_SIZE, sizeof(*list->ent));
		dirls_entry_t * const tmp = list->ent[list->used++] =
		  (dirls_entry_t*) ck_malloc(sizeof(dirls_entry_t) + 1 + ent.namelen);
		*tmp = ent; /* copy struct */
		memcpy(DIRLIST_ENT_NAME(tmp), d_name, ent.namelen + 1);
	}
  #ifdef _WIN32
	  while (++count < DIRLIST_BATCH && FindNextFileW(p->hFind, &p->ffd) != 0);
	if (count == DIRLIST_BATCH)
		return HANDLER_WAIT_FOR_EVENT;
	if (GetLastError() != ERROR_NO_MORE_FILES) {
		/*(some other GetLastError() value; ignore; truncate listing)*/
	}
	FindClose(p->hFind);
	p->hFind = INVALID_HANDLE_VALUE;
  #else
	if (count == DIRLIST_BATCH)
		return HANDLER_WAIT_FOR_EVENT;
	closedir(p->dp);
	p->dp = NULL;
  #endif

	return HANDLER_FINISHED;
}

static void http_list_directory(request_st * const r, handler_ctx * const hctx) {
	dirls_list_t * const dirs = &hctx->dirs;
	dirls_list_t * const files = &hctx->files;
	/*(note: sorting can be time consuming on large dirs (O(n log n))*/
	if (dirs->used) http_dirls_sort(dirs->ent, dirs->used);
	if (files->used) http_dirls_sort(files->ent, files->used);

	/* generate large directory listings into tempfiles
	 * (estimate approx 200-256 bytes of HTML per item; could be up to ~512) */
	chunkqueue * const cq = &r->write_queue;
	buffer * const tb = r->tmp_buf;
	buffer_clear(tb);
	buffer * const out = (dirs->used + files->used <= 256)
	  ? chunkqueue_append_buffer_open(cq)
	  : tb;
	buffer_clear(out);

	/* directories */
	dirls_entry_t ** const dirs_ent = dirs->ent;
	for (uint32_t i = 0, used = dirs->used; i < used; ++i) {
		http_list_directory_dir(out, dirs_ent[i]);
		if (buffer_string_space(out) < 256) {
			if (out == tb) {
				if (0 != chunkqueue_append_mem_to_tempfile(cq,
				                                           BUF_PTR_LEN(out),
				                                           r->conf.errh))
					break;
				buffer_clear(out);
			}
		}
	}

	/* files */
	dirls_entry_t ** const files_ent = files->ent;
	for (uint32_t i = 0, used = files->used; i < used; ++i) {
		http_list_directory_file(out, files_ent[i], hctx);
		if (buffer_string_space(out) < 256) {
			if (out == tb) {
				if (0 != chunkqueue_append_mem_to_tempfile(cq,
				                                           BUF_PTR_LEN(out),
				                                           r->conf.errh))
					break;
				buffer_clear(out);
			}
		}
	}

	if (out == tb) {
		if (!buffer_is_blank(out)
		    && 0 != chunkqueue_append_mem_to_tempfile(cq,
		                                              BUF_PTR_LEN(out),
		                                              r->conf.errh)) {
			/* ignore */
		}
	}
	else {
		chunkqueue_append_buffer_commit(cq);
	}
}


static void mod_dirlisting_content_type (request_st * const r, const buffer * const encoding) {
    buffer * const vb =
      http_header_response_set_ptr(r, HTTP_HEADER_CONTENT_TYPE,
                                   CONST_STR_LEN("Content-Type"));
    if (NULL == encoding)
        buffer_copy_string_len(vb, CONST_STR_LEN("text/html"));
    else
        buffer_append_str2(vb, CONST_STR_LEN("text/html;charset="),
                               BUF_PTR_LEN(encoding));
}


static void mod_dirlisting_response (request_st * const r, handler_ctx * const hctx) {
    http_list_directory_header(r, hctx);
    http_list_directory(r, hctx);
    http_list_directory_footer(r, hctx);
    mod_dirlisting_content_type(r, hctx->conf.encoding);
}


static void mod_dirlisting_stream_append (request_st * const r, handler_ctx * const hctx, const int fin) {
    buffer * const jb = hctx->jb ? hctx->jb : hctx->hb;
    if (!fin && buffer_clen(jb) < 16384-1024)
        return; /* aggregate bunches of entries, even if streaming response */

    if (hctx->jfn) {
        if (__builtin_expect( (write_all(hctx->jfd, BUF_PTR_LEN(jb)) < 0), 0)) {
            /*(cleanup, cease caching if error occurs writing to cache file)*/
            close(hctx->jfd);
            hctx->jfd = -1;
            unlink(hctx->jfn);
            free(hctx->jfn);
            hctx->jfn = NULL;
        }
        /* Note: writing cache file is separate from the response so that if an
         * error occurs with cache, the response still proceeds.  While this is
         * duplicative if the response is large enough to spill to temporary
         * files, it is expected that only very large directories will spill to
         * temporary files, and even then most responses will be less than 1 MB.
         * The cache path can be different from server.upload-dirs.
         */
    }

    http_chunk_append_buffer(r, jb); /* clears jb */
}


SUBREQUEST_FUNC(mod_dirlisting_subrequest);
REQUEST_FUNC(mod_dirlisting_reset);
static handler_t mod_dirlisting_cache_check (request_st * const r, plugin_data * const p);
__attribute_noinline__
static void mod_dirlisting_cache_add (request_st * const r, handler_ctx * const hctx);
__attribute_noinline__
static void mod_dirlisting_cache_stream_add_footer (request_st * const r, handler_ctx * const hctx);
__attribute_noinline__
static void mod_dirlisting_cache_stream_init (request_st * const r, handler_ctx * const hctx);
__attribute_noinline__
static void mod_dirlisting_cache_stream (request_st * const r, handler_ctx * const hctx);


URIHANDLER_FUNC(mod_dirlisting_subrequest_start) {
	plugin_data *p = p_d;

	if (NULL != r->handler_module) return HANDLER_GO_ON;
	if (!buffer_has_slash_suffix(&r->uri.path)) return HANDLER_GO_ON;
	if (!http_method_get_or_head(r->http_method)) return HANDLER_GO_ON;
	/* r->physical.path is non-empty for handle_subrequest_start */
	/*if (buffer_is_blank(&r->physical.path)) return HANDLER_GO_ON;*/

	mod_dirlisting_patch_config(r, p);

	if (!p->conf.dir_listing) return HANDLER_GO_ON;

	if (r->conf.log_request_handling) {
		log_debug(r->conf.errh, __FILE__, __LINE__,
		  "-- handling the request as Dir-Listing");
		log_debug(r->conf.errh, __FILE__, __LINE__,
		  "URI          : %s", r->uri.path.ptr);
	}

  #if 0 /* redundant check; not necessary */
	/* r->physical.path is a dir since it ends in slash, or else
	 * http_response_physical_path_check() would have redirected
	 * before calling handle_subrequest_start */
	if (!stat_cache_path_isdir(&r->physical.path)) {
		if (errno == ENOTDIR)
			return HANDLER_GO_ON;
		log_perror(r->conf.errh,__FILE__,__LINE__,"%s",r->physical.path.ptr);
		r->http_status = 500;
		return HANDLER_FINISHED;
	}
  #endif

  #if 0 /* XXX: ??? might this be enabled accidentally by clients ??? */
	/* XXX: would have to add "Vary: Accept" response header, too */
	const buffer * const vb =
	  http_header_request_get(r, HTTP_HEADER_ACCEPT, CONST_STR_LEN("Accept"));
	p->conf.json = (vb && strstr(vb->ptr, "application/json")); /*(coarse)*/
	if (p->conf.json) p->conf.auto_layout = 0;
  #else
	/* check URL for /<path>/?json to enable json output */
	if (buffer_clen(&r->uri.query) == sizeof("json")-1
	    && 0 == memcmp(r->uri.query.ptr, CONST_STR_LEN("json")-1)) {
	  #if 0
		/* streaming response not set here for mod_deflate (which
		 * currently does not compress incomplete streaming responses),
		 * since json response is generally highly compressible.
		 * Admin should enable streaming response in lighttpd.conf,
		 * if desired. */
		if (!(r->conf.stream_response_body
		      & (FDEVENT_STREAM_RESPONSE|FDEVENT_STREAM_RESPONSE_BUFMIN)))
			r->conf.stream_response_body |= FDEVENT_STREAM_RESPONSE;
	  #endif
		p->conf.json = 1;
		p->conf.auto_layout = 0;
	}
  #endif

	if (p->conf.cache) {
		handler_t rc = mod_dirlisting_cache_check(r, p);
		if (rc != HANDLER_GO_ON)
			return rc;
	}

	/* upper limit for dirlisting requests in progress (per lighttpd worker)
	 * (attempt to avoid "livelock" scenarios or starvation of other requests)
	 * (100 is still a high arbitrary limit;
	 *  and limit applies only to directories larger than DIRLIST_BATCH-2) */
	if (p->processing == dirlist_max_in_progress) {
		r->http_status = 503;
		http_header_response_set(r, HTTP_HEADER_OTHER,
		                         CONST_STR_LEN("Retry-After"),
		                         CONST_STR_LEN("2"));
		return HANDLER_FINISHED;
	}

	handler_ctx * const hctx = mod_dirlisting_handler_ctx_init(p);
	hctx->use_xattr = r->conf.use_xattr;
	hctx->mimetypes = r->conf.mimetypes;

	/* future: might implement a queue to limit max number of dirlisting
	 * requests being serviced in parallel (increasing disk I/O), and if
	 * caching is enabled, to avoid repeating the work on the same directory
	 * in parallel.  Could continue serving (expired) cached entry while
	 * updating, but burst of requests on first access to dir would still
	 * need to be handled.
	 *
	 * If queueing (not implemented), defer opening dir until pulled off
	 * queue.  Since joblist is per-connection, would need to handle single
	 * request from queue even if multiple streams are queued on same
	 * HTTP/2 connection.  If queueing, must check for and remove from
	 * queue in mod_dirlisting_reset() if request is still queued. */

	if (0 != http_open_directory(r, hctx)) {
		/* dirlisting failed */
		r->http_status = 403;
	        mod_dirlisting_handler_ctx_free(hctx);
		return HANDLER_FINISHED;
	}
	++p->processing;

	if (p->conf.json) {
		hctx->jb = chunk_buffer_acquire();
		buffer_append_char(hctx->jb, '[');
		http_header_response_set(r, HTTP_HEADER_CONTENT_TYPE,
		                         CONST_STR_LEN("Content-Type"),
		                         CONST_STR_LEN("application/json"));
	}
	else {
		if (p->conf.auto_layout)
			http_dirlist_auto_layout_early_hints(r, p);
		if (!p->conf.sort) {
			mod_dirlisting_content_type(r, hctx->conf.encoding);
			http_list_directory_header(r, hctx);
			hctx->hb = chunk_buffer_acquire();
		}
	}

	if (hctx->jb || hctx->hb) {
		hctx->jfd = -1;
		if (p->conf.cache)
			mod_dirlisting_cache_stream_init(r, hctx);
		r->http_status = 200;
		r->resp_body_started = 1;
	}

	r->plugin_ctx[p->id] = hctx;
	r->handler_module = p->self;
	handler_t rc = mod_dirlisting_subrequest(r, p);

	if (rc == HANDLER_WAIT_FOR_EVENT && p->conf.auto_layout
	    && (p->conf.external_js || p->conf.external_css)
              /*(skip if might stream unsorted since r->http_status and
               * Content-Type would have to be saved/restored for response,
               * as well as any partial response body of html dir header)*/
	    && 0 == r->resp_body_started
	      /* (paranoia: do not send 103 for HTTP/1.x; only for HTTP/2 +)
	       *  https://www.rfc-editor.org/rfc/rfc8297.html#section-3 */
	    && r->http_version >= HTTP_VERSION_2) {
		/* send 103 Early Hints intermediate response (send once only)*/
		r->http_status = 103; /* 103 Early Hints */
		if (!http_response_send_1xx(r))
			rc = HANDLER_ERROR;
	}

	return rc;
}


SUBREQUEST_FUNC(mod_dirlisting_subrequest) {
    if ((r->conf.stream_response_body & FDEVENT_STREAM_RESPONSE_BUFMIN)
        && chunkqueue_length(&r->write_queue) > 65536 - 4096
        && !r->con->is_writable)
        /* defer reading more from directory while data is sent to client
         * (must check !r->con->is_writable or else r may not be rescheduled to
         *  run and produce more output since r->write_queue sent out later) */
        return HANDLER_WAIT_FOR_EVENT;

    plugin_data * const p = p_d;
    handler_ctx * const hctx = r->plugin_ctx[p->id];
    if (NULL == hctx) return HANDLER_GO_ON; /*(should not happen)*/

    handler_t rc = http_read_directory(hctx);
    switch (rc) {
      case HANDLER_FINISHED:
        if (hctx->jb || hctx->hb) { /* (hctx->conf.json || !hctx->conf.sort) */
            if (hctx->jb)
                buffer_append_char(hctx->jb, ']');
            mod_dirlisting_stream_append(r, hctx, 1);
            if (hctx->hb)
                mod_dirlisting_cache_stream_add_footer(r, hctx);
            if (hctx->jfn) /* (also (hctx->conf.cache) */
                mod_dirlisting_cache_stream(r, hctx);
        }
        else {
            mod_dirlisting_response(r, hctx);
            if (hctx->conf.cache)
                mod_dirlisting_cache_add(r, hctx);
        }
        r->resp_body_finished = 1;
        mod_dirlisting_reset(r, p); /*(release resources, including hctx)*/
        break;
      case HANDLER_WAIT_FOR_EVENT: /*(used here to mean 'yield')*/
        if (hctx->jb || hctx->hb)  /* (hctx->conf.json || !hctx->conf.sort) */
            mod_dirlisting_stream_append(r, hctx, 0);
        joblist_append(r->con);
        break;
      default:
        break;
    }

    return rc;
}


REQUEST_FUNC(mod_dirlisting_reset) {
    void ** const restrict dptr = &r->plugin_ctx[((plugin_data *)p_d)->id];
    if (*dptr) {
        --((plugin_data *)p_d)->processing;
        mod_dirlisting_handler_ctx_free(*dptr);
        *dptr = NULL;
    }
    return HANDLER_GO_ON;
}


static void mod_dirlisting_cache_control (request_st * const r, unix_time64_t max_age) {
    if (!light_btst(r->resp_htags, HTTP_HEADER_CACHE_CONTROL)) {
        buffer * const vb =
          http_header_response_set_ptr(r, HTTP_HEADER_CACHE_CONTROL,
                                       CONST_STR_LEN("Cache-Control"));
        buffer_append_string_len(vb, CONST_STR_LEN("max-age="));
        buffer_append_int(vb, max_age);
    }
}


static void mod_dirlisting_cache_etag (request_st * const r, int fd)
{
    if (0 != r->conf.etag_flags) {
        struct stat st;
        if (0 == fstat(fd, &st)) {
            buffer * const vb =
              http_header_response_set_ptr(r, HTTP_HEADER_ETAG,
                                           CONST_STR_LEN("ETag"));
            http_etag_create(vb, &st, r->conf.etag_flags);
        }
    }
}


static handler_t mod_dirlisting_cache_check (request_st * const r, plugin_data * const p) {
    /* optional: an external process can trigger a refresh by deleting the cache
     * entry when the external process detects (or initiates) changes to dir */
    buffer * const tb = r->tmp_buf;
    buffer_copy_path_len2(tb, BUF_PTR_LEN(p->conf.cache->path),
                              BUF_PTR_LEN(&r->physical.path));
    buffer_append_string_len(tb, p->conf.json ? "dirlist.json" : "dirlist.html",
                             sizeof("dirlist.html")-1);
    stat_cache_entry * const sce = stat_cache_get_entry_open(tb, 1);
    if (NULL == sce || sce->fd == -1)
        return HANDLER_GO_ON;
    if (TIME64_CAST(sce->st.st_mtime) + p->conf.cache->max_age < log_epoch_secs)
        return HANDLER_GO_ON;
    const unix_time64_t max_age =
      TIME64_CAST(sce->st.st_mtime) + p->conf.cache->max_age - log_epoch_secs;

    !p->conf.json
      ? mod_dirlisting_content_type(r, p->conf.encoding)
      : http_header_response_set(r, HTTP_HEADER_CONTENT_TYPE,
                                 CONST_STR_LEN("Content-Type"),
                                 CONST_STR_LEN("application/json"));

  #if 0
    /*(XXX: ETag needs to be set for mod_deflate to potentially handle)*/
    /*(XXX: should ETag be created from cache file mtime or directory mtime?)*/
    const int follow_symlink = r->conf.follow_symlink;
    r->conf.follow_symlink = 1; /*(skip symlink checks into cache)*/
    http_response_send_file(r, sce->name, sce);
    r->conf.follow_symlink = follow_symlink;
    if (r->http_status < 400)
        return HANDLER_FINISHED;
    r->http_status = 0;
  #endif

    /* Note: dirlist < 350 or so entries will generally trigger file
     * read into memory for dirlist < 32k, which will not be able to use
     * mod_deflate cache.  Still, this is much more efficient than lots of
     * stat() calls to generate the dirlisting for each and every request */
    if (0 != http_chunk_append_file_ref(r, sce)) {
        http_header_response_unset(r, HTTP_HEADER_CONTENT_TYPE,
                                   CONST_STR_LEN("Content-Type"));
        http_response_body_clear(r, 0);
        return HANDLER_GO_ON;
    }

    /* Cache-Control and ETag (also done in mod_dirlisting_cache_add())*/
    mod_dirlisting_cache_control(r, max_age);
    if (0 != r->conf.etag_flags) {
        const buffer *etag = stat_cache_etag_get(sce, r->conf.etag_flags);
        if (etag && !buffer_is_blank(etag))
            http_header_response_set(r, HTTP_HEADER_ETAG,
                                     CONST_STR_LEN("ETag"),
                                     BUF_PTR_LEN(etag));
    }
    if (p->conf.auto_layout)
        http_dirlist_auto_layout_early_hints(r, p);

    r->resp_body_finished = 1;
    return HANDLER_FINISHED;
}


static int mod_dirlisting_write_cq (const int fd, chunkqueue * const cq, log_error_st * const errh)
{
    chunkqueue in;
    memset(&in, 0, sizeof(in));
    chunkqueue_append_chunkqueue(&in, cq);
    cq->bytes_in  -= in.bytes_in;
    cq->bytes_out -= in.bytes_in;

    /*(similar to mod_webdav.c:mod_webdav_write_cq(), but operates on two cqs)*/
    while (!chunkqueue_is_empty(&in)) {
        ssize_t wr = chunkqueue_write_chunk(fd, &in, errh);
        if (__builtin_expect( (wr > 0), 1))
            chunkqueue_steal(cq, &in, wr);
        else if (wr < 0) {
            /*(writing to tempfile failed; transfer remaining data back to cq)*/
            chunkqueue_append_chunkqueue(cq, &in);
            return 0;
        }
        else /*(wr == 0)*/
            chunkqueue_remove_finished_chunks(&in);
    }
    return 1;
}


/*(similar to mod_deflate.c:mkdir_recursive(), but starts mid-path)*/
static int mkdir_recursive (char *dir, size_t off) {
    char *p = dir+off;
    if (*p != '/') {
        if (off && p[-1] == '/')
            --p;
        else {
            errno = ENOTDIR;
            return -1;
        }
    }
    do {
        *p = '\0';
        int rc = mkdir(dir, 0700);
        *p = '/';
        if (0 != rc && errno != EEXIST) return -1;
    } while ((p = strchr(p+1, '/')) != NULL);
    return 0;
}


__attribute_noinline__
static void mod_dirlisting_cache_add (request_st * const r, handler_ctx * const hctx) {
  #ifndef PATH_MAX
  #define PATH_MAX 4096
  #endif
    char oldpath[PATH_MAX];
    char newpath[PATH_MAX];
    buffer * const tb = r->tmp_buf;
    buffer_copy_path_len2(tb, BUF_PTR_LEN(hctx->conf.cache->path),
                              BUF_PTR_LEN(&r->physical.path));
    if (!stat_cache_path_isdir(tb)
        && 0 != mkdir_recursive(tb->ptr, buffer_clen(hctx->conf.cache->path)))
        return;
    buffer_append_string_len(tb, CONST_STR_LEN("dirlist.html"));
    const size_t len = buffer_clen(tb);
    if (len + 7 >= PATH_MAX) return;
    memcpy(newpath, tb->ptr, len+1);   /*(include '\0')*/
    buffer_append_string_len(tb, CONST_STR_LEN(".XXXXXX"));
    memcpy(oldpath, tb->ptr, len+7+1); /*(include '\0')*/
    const int fd = fdevent_mkostemp(oldpath, 0);
    if (fd < 0) return;
    int rc = mod_dirlisting_write_cq(fd, &r->write_queue, r->conf.errh);
    if (rc)
        mod_dirlisting_cache_etag(r, fd);
    close(fd); /*(_WIN32 rename fails if file open; MS filesystem limitation)*/
    if (rc && 0 == fdevent_rename(oldpath, newpath)) {
        stat_cache_invalidate_entry(newpath, len);
        mod_dirlisting_cache_control(r, hctx->conf.cache->max_age);
    }
    else
        unlink(oldpath);
}


__attribute_noinline__
static void mod_dirlisting_cache_stream_append_cq (request_st * const r, handler_ctx * const hctx) {
    if (chunkqueue_is_empty(&r->write_queue) || hctx->jfd == -1) return;

    /* append HTML in r->write_queue to cache file
     * (but must abort cache file if streaming response and sending HTTP/1.1
     *  chunked response due to http_list_directory_footer() writing directly
     *  to r->write_queue, which will be written in HTTP chunked format) */
    if (r->resp_send_chunked
        || !mod_dirlisting_write_cq(hctx->jfd, &r->write_queue, r->conf.errh)) {
        close(hctx->jfd);
        hctx->jfd = -1;
        unlink(hctx->jfn);
        free(hctx->jfn);
        hctx->jfn = NULL;
    }
}


static void mod_dirlisting_cache_stream_add_header (request_st * const r, handler_ctx * const hctx) {
    mod_dirlisting_cache_stream_append_cq(r, hctx);
}


__attribute_noinline__
static void mod_dirlisting_cache_stream_add_footer (request_st * const r, handler_ctx * const hctx) {
    /* save and restore r->write_queue around adding footer and updating cache
     * (due to http_list_directory_footer() appending to r->write_queue)*/
    chunkqueue * const cq = &r->write_queue;
    chunkqueue in;
    memset(&in, 0, sizeof(in));
    chunkqueue_append_chunkqueue(&in, cq);
    cq->bytes_in  -= in.bytes_in;
    cq->bytes_out -= in.bytes_in;

    http_list_directory_footer(r, hctx);
    mod_dirlisting_cache_stream_append_cq(r, hctx);

    off_t len = chunkqueue_length(cq);
    chunkqueue_append_chunkqueue(&in, cq);
    cq->bytes_in  -= len;
    cq->bytes_out -= len;
    chunkqueue_append_chunkqueue(cq, &in);
}


__attribute_noinline__
static void mod_dirlisting_cache_stream_init (request_st * const r, handler_ctx * const hctx) {
  #ifndef PATH_MAX
  #define PATH_MAX 4096
  #endif
    buffer * const tb = r->tmp_buf;
    buffer_copy_path_len2(tb, BUF_PTR_LEN(hctx->conf.cache->path),
                              BUF_PTR_LEN(&r->physical.path));
    if (!stat_cache_path_isdir(tb)
        && 0 != mkdir_recursive(tb->ptr, buffer_clen(hctx->conf.cache->path)))
        return;
    buffer_append_string_len(tb, hctx->jb
                                  ? "dirlist.json.XXXXXX"
                                  : "dirlist.html.XXXXXX",
                             sizeof("dirlist.json.XXXXXX")-1);
    const int fd = fdevent_mkostemp(tb->ptr, 0);
    if (fd < 0) return;
    hctx->jfn_len = buffer_clen(tb);
    hctx->jfd = fd;
    hctx->jfn = ck_malloc(hctx->jfn_len+1);
    memcpy(hctx->jfn, tb->ptr, hctx->jfn_len+1); /*(include '\0')*/
    if (hctx->hb)
        mod_dirlisting_cache_stream_add_header(r, hctx);
}


__attribute_noinline__
static void mod_dirlisting_cache_stream (request_st * const r, handler_ctx * const hctx) {
  #ifndef PATH_MAX
  #define PATH_MAX 4096
  #endif
    UNUSED(r);
    char newpath[PATH_MAX];
    const size_t len = hctx->jfn_len - 7; /*(-7 for .XXXXXX)*/
    force_assert(len < PATH_MAX);
    memcpy(newpath, hctx->jfn, len);
    newpath[len] = '\0';
    if (0 == r->resp_header_len) /*(response headers not yet sent)*/
        mod_dirlisting_cache_etag(r, hctx->jfd);
    close(hctx->jfd); /*(_WIN32 rename fails if file open; MS fs limitation)*/
    hctx->jfd = -1;
    if (0 == fdevent_rename(hctx->jfn, newpath)) {
        stat_cache_invalidate_entry(newpath, len);
        if (0 == r->resp_header_len) /*(response headers not yet sent)*/
            mod_dirlisting_cache_control(r, hctx->conf.cache->max_age);
    }
    else
        unlink(hctx->jfn);
    free(hctx->jfn);
    hctx->jfn = NULL;
}


__attribute_cold__
__declspec_dllexport__
int mod_dirlisting_plugin_init(plugin *p);
int mod_dirlisting_plugin_init(plugin *p) {
	p->version     = LIGHTTPD_VERSION_ID;
	p->name        = "dirlisting";

	p->init        = mod_dirlisting_init;
	p->handle_subrequest_start = mod_dirlisting_subrequest_start;
	p->handle_subrequest       = mod_dirlisting_subrequest;
	p->handle_request_reset    = mod_dirlisting_reset;
	p->set_defaults  = mod_dirlisting_set_defaults;
	p->cleanup     = mod_dirlisting_free;

	return 0;
}
