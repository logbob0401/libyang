/**
 * @file tree_schema_helpers.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @brief Parsing and validation helper functions
 *
 * Copyright (c) 2015 - 2018 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */
#include "common.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>

#include "libyang.h"
#include "tree_schema_internal.h"

/**
 * @brief Parse an identifier.
 *
 * ;; An identifier MUST NOT start with (('X'|'x') ('M'|'m') ('L'|'l'))
 * identifier          = (ALPHA / "_")
 *                       *(ALPHA / DIGIT / "_" / "-" / ".")
 *
 * @param[in,out] id Identifier to parse. When returned, it points to the first character which is not part of the identifier.
 * @return LY_ERR value: LY_SUCCESS or LY_EINVAL in case of invalid starting character.
 */
static LY_ERR
lys_parse_id(const char **id)
{
    assert(id && *id);

    if (!is_yangidentstartchar(**id)) {
        return LY_EINVAL;
    }
    ++(*id);

    while (is_yangidentchar(**id)) {
        ++(*id);
    }
    return LY_SUCCESS;
}

LY_ERR
lys_parse_nodeid(const char **id, const char **prefix, size_t *prefix_len, const char **name, size_t *name_len)
{
    assert(id && *id);
    assert(prefix && prefix_len);
    assert(name && name_len);

    *prefix = *id;
    *prefix_len = 0;
    *name = NULL;
    *name_len = 0;

    LY_CHECK_RET(lys_parse_id(id));
    if (**id == ':') {
        /* there is prefix */
        *prefix_len = *id - *prefix;
        ++(*id);
        *name = *id;

        LY_CHECK_RET(lys_parse_id(id));
        *name_len = *id - *name;
    } else {
        /* there is no prefix, so what we have as prefix now is actually the name */
        *name = *prefix;
        *name_len = *id - *name;
        *prefix = NULL;
    }

    return LY_SUCCESS;
}

LY_ERR
lys_resolve_schema_nodeid(struct lysc_ctx *ctx, const char *nodeid, size_t nodeid_len, const struct lysc_node *context_node,
                          const struct lys_module *context_module, int nodetype, int implement,
                          const struct lysc_node **target, uint16_t *result_flag)
{
    LY_ERR ret = LY_EVALID;
    const char *name, *prefix, *id;
    size_t name_len, prefix_len;
    const struct lys_module *mod;
    const char *nodeid_type;
    int getnext_extra_flag = 0;
    int current_nodetype = 0;

    assert(nodeid);
    assert(target);
    assert(result_flag);
    *target = NULL;
    *result_flag = 0;

    id = nodeid;

    if (context_node) {
        /* descendant-schema-nodeid */
        nodeid_type = "descendant";

        if (*id == '/') {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                   "Invalid descendant-schema-nodeid value \"%.*s\" - absolute-schema-nodeid used.",
                   nodeid_len ? nodeid_len : strlen(nodeid), nodeid);
            return LY_EVALID;
        }
    } else {
        /* absolute-schema-nodeid */
        nodeid_type = "absolute";

        if (*id != '/') {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                   "Invalid absolute-schema-nodeid value \"%.*s\" - missing starting \"/\".",
                   nodeid_len ? nodeid_len : strlen(nodeid), nodeid);
            return LY_EVALID;
        }
        ++id;
    }

    while (*id && (ret = lys_parse_nodeid(&id, &prefix, &prefix_len, &name, &name_len)) == LY_SUCCESS) {
        if (prefix) {
            mod = lys_module_find_prefix(context_module, prefix, prefix_len);
            if (!mod) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                       "Invalid %s-schema-nodeid value \"%.*s\" - prefix \"%.*s\" not defined in module \"%s\".",
                       nodeid_type, id - nodeid, nodeid, prefix_len, prefix, context_module->name);
                return LY_ENOTFOUND;
            }
        } else {
            mod = context_module;
        }
        if (implement && !mod->implemented) {
            /* make the module implemented */
            ly_ctx_module_implement_internal(ctx->ctx, (struct lys_module*)mod, 2);
        }
        if (context_node && context_node->nodetype == LYS_ACTION) {
            /* move through input/output manually */
            if (!strncmp("input", name, name_len)) {
                (*result_flag) |= LYSC_OPT_RPC_INPUT;
            } else if (!strncmp("output", name, name_len)) {
                (*result_flag) |= LYSC_OPT_RPC_OUTPUT;
                getnext_extra_flag = LYS_GETNEXT_OUTPUT;
            } else {
                goto getnext;
            }
            current_nodetype = LYS_INOUT;
        } else {
getnext:
            context_node = lys_child(context_node, mod, name, name_len, 0,
                                     getnext_extra_flag | LYS_GETNEXT_NOSTATECHECK | LYS_GETNEXT_WITHCHOICE | LYS_GETNEXT_WITHCASE);
            if (!context_node) {
                LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                       "Invalid %s-schema-nodeid value \"%.*s\" - target node not found.", nodeid_type, id - nodeid, nodeid);
                return LY_ENOTFOUND;
            }
            getnext_extra_flag = 0;
            current_nodetype = context_node->nodetype;

            if (current_nodetype == LYS_NOTIF) {
                (*result_flag) |= LYSC_OPT_NOTIFICATION;
            }
        }
        if (!*id || (nodeid_len && ((size_t)(id - nodeid) >= nodeid_len))) {
            break;
        }
        if (*id != '/') {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                   "Invalid %s-schema-nodeid value \"%.*s\" - missing \"/\" as node-identifier separator.",
                   nodeid_type, id - nodeid + 1, nodeid);
            return LY_EVALID;
        }
        ++id;
    }

    if (ret == LY_SUCCESS) {
        *target = context_node;
        if (nodetype & LYS_INOUT) {
            /* instead of input/output nodes, the RPC/action node is actually returned */
        }
        if (nodetype && !(current_nodetype & nodetype)) {
            return LY_EDENIED;
        }
    } else {
        LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
               "Invalid %s-schema-nodeid value \"%.*s\" - unexpected end of expression.",
               nodeid_type, nodeid_len ? nodeid_len : strlen(nodeid), nodeid);
    }

    return ret;
}

LY_ERR
lysp_check_prefix(struct ly_ctx *ctx, uint64_t *line, struct lysp_import *imports, const char *module_prefix, const char **value)
{
    struct lysp_import *i;

    if (module_prefix && &module_prefix != value && !strcmp(module_prefix, *value)) {
        LOGVAL(ctx, LY_VLOG_LINE, line, LYVE_REFERENCE,
               "Prefix \"%s\" already used as module prefix.", *value);
        return LY_EEXIST;
    }
    LY_ARRAY_FOR(imports, struct lysp_import, i) {
        if (i->prefix && &i->prefix != value && !strcmp(i->prefix, *value)) {
            LOGVAL(ctx, LY_VLOG_LINE, line, LYVE_REFERENCE, "Prefix \"%s\" already used to import \"%s\" module.",
                   *value, i->name);
            return LY_EEXIST;
        }
    }
    return LY_SUCCESS;
}

LY_ERR
lysc_check_status(struct lysc_ctx *ctx,
                  uint16_t flags1, void *mod1, const char *name1,
                  uint16_t flags2, void *mod2, const char *name2)
{
    uint16_t flg1, flg2;

    flg1 = (flags1 & LYS_STATUS_MASK) ? (flags1 & LYS_STATUS_MASK) : LYS_STATUS_CURR;
    flg2 = (flags2 & LYS_STATUS_MASK) ? (flags2 & LYS_STATUS_MASK) : LYS_STATUS_CURR;

    if ((flg1 < flg2) && (mod1 == mod2)) {
        if (ctx) {
            LOGVAL(ctx->ctx, LY_VLOG_STR, ctx->path, LYVE_REFERENCE,
                   "A %s definition \"%s\" is not allowed to reference %s definition \"%s\".",
                   flg1 == LYS_STATUS_CURR ? "current" : "deprecated", name1,
                   flg2 == LYS_STATUS_OBSLT ? "obsolete" : "deprecated", name2);
        }
        return LY_EVALID;
    }

    return LY_SUCCESS;
}

LY_ERR
lysp_check_date(struct ly_parser_ctx *ctx, const char *date, int date_len, const char *stmt)
{
    int i;
    struct tm tm, tm_;
    char *r;

    LY_CHECK_ARG_RET(ctx ? ctx->ctx : NULL, date, LY_EINVAL);
    LY_CHECK_ERR_RET(date_len != LY_REV_SIZE - 1, LOGARG(ctx ? ctx->ctx : NULL, date_len), LY_EINVAL);

    /* check format */
    for (i = 0; i < date_len; i++) {
        if (i == 4 || i == 7) {
            if (date[i] != '-') {
                goto error;
            }
        } else if (!isdigit(date[i])) {
            goto error;
        }
    }

    /* check content, e.g. 2018-02-31 */
    memset(&tm, 0, sizeof tm);
    r = strptime(date, "%Y-%m-%d", &tm);
    if (!r || r != &date[LY_REV_SIZE - 1]) {
        goto error;
    }
    memcpy(&tm_, &tm, sizeof tm);
    mktime(&tm_); /* mktime modifies tm_ if it refers invalid date */
    if (tm.tm_mday != tm_.tm_mday) { /* e.g 2018-02-29 -> 2018-03-01 */
        /* checking days is enough, since other errors
         * have been checked by strptime() */
        goto error;
    }

    return LY_SUCCESS;

error:
    if (stmt) {
        if (ctx) {
            LOGVAL(ctx->ctx, LY_VLOG_LINE, &ctx->line, LY_VCODE_INVAL, date_len, date, stmt);
        } else {
            LOGVAL(NULL, LY_VLOG_NONE, NULL, LY_VCODE_INVAL, date_len, date, stmt);
        }
    }
    return LY_EINVAL;
}

void
lysp_sort_revisions(struct lysp_revision *revs)
{
    uint8_t i, r;
    struct lysp_revision rev;

    for (i = 1, r = 0; revs && i < LY_ARRAY_SIZE(revs); i++) {
        if (strcmp(revs[i].date, revs[r].date) > 0) {
            r = i;
        }
    }

    if (r) {
        /* the newest revision is not on position 0, switch them */
        memcpy(&rev, &revs[0], sizeof rev);
        memcpy(&revs[0], &revs[r], sizeof rev);
        memcpy(&revs[r], &rev, sizeof rev);
    }
}

static const struct lysp_tpdf *
lysp_type_match(const char *name, struct lysp_node *node)
{
    const struct lysp_tpdf *typedefs;
    unsigned int u;

    typedefs = lysp_node_typedefs(node);
    LY_ARRAY_FOR(typedefs, u) {
        if (!strcmp(name, typedefs[u].name)) {
            /* match */
            return &typedefs[u];
        }
    }

    return NULL;
}

static LY_DATA_TYPE
lysp_type_str2builtin(const char *name, size_t len)
{
    if (len >= 4) { /* otherwise it does not match any built-in type */
        if (name[0] == 'b') {
            if (name[1] == 'i') {
                if (len == 6 && !strncmp(&name[2], "nary", 4)) {
                    return LY_TYPE_BINARY;
                } else if (len == 4 && !strncmp(&name[2], "ts", 2)) {
                    return LY_TYPE_BITS;
                }
            } else if (len == 7 && !strncmp(&name[1], "oolean", 6)) {
                return LY_TYPE_BOOL;
            }
        } else if (name[0] == 'd') {
            if (len == 9 && !strncmp(&name[1], "ecimal64", 8)) {
                return LY_TYPE_DEC64;
            }
        } else if (name[0] == 'e') {
            if (len == 5 && !strncmp(&name[1], "mpty", 4)) {
                return LY_TYPE_EMPTY;
            } else if (len == 11 && !strncmp(&name[1], "numeration", 10)) {
                return LY_TYPE_ENUM;
            }
        } else if (name[0] == 'i') {
            if (name[1] == 'n') {
                if (len == 4 && !strncmp(&name[2], "t8", 2)) {
                    return LY_TYPE_INT8;
                } else if (len == 5) {
                    if (!strncmp(&name[2], "t16", 3)) {
                        return LY_TYPE_INT16;
                    } else if (!strncmp(&name[2], "t32", 3)) {
                        return LY_TYPE_INT32;
                    } else if (!strncmp(&name[2], "t64", 3)) {
                        return LY_TYPE_INT64;
                    }
                } else if (len == 19 && !strncmp(&name[2], "stance-identifier", 17)) {
                    return LY_TYPE_INST;
                }
            } else if (len == 11 && !strncmp(&name[1], "dentityref", 10)) {
                return LY_TYPE_IDENT;
            }
        } else if (name[0] == 'l') {
            if (len == 7 && !strncmp(&name[1], "eafref", 6)) {
                return LY_TYPE_LEAFREF;
            }
        } else if (name[0] == 's') {
            if (len == 6 && !strncmp(&name[1], "tring", 5)) {
                return LY_TYPE_STRING;
            }
        } else if (name[0] == 'u') {
            if (name[1] == 'n') {
                if (len == 5 && !strncmp(&name[2], "ion", 3)) {
                    return LY_TYPE_UNION;
                }
            } else if (name[1] == 'i' && name[2] == 'n' && name[3] == 't') {
                if (len == 5 && name[4] == '8') {
                    return LY_TYPE_UINT8;
                } else if (len == 6) {
                    if (!strncmp(&name[4], "16", 2)) {
                        return LY_TYPE_UINT16;
                    } else if (!strncmp(&name[4], "32", 2)) {
                        return LY_TYPE_UINT32;
                    } else if (!strncmp(&name[4], "64", 2)) {
                        return LY_TYPE_UINT64;
                    }
                }
            }
        }
    }

    return LY_TYPE_UNKNOWN;
}

LY_ERR
lysp_type_find(const char *id, struct lysp_node *start_node, struct lysp_module *start_module,
               LY_DATA_TYPE *type, const struct lysp_tpdf **tpdf, struct lysp_node **node, struct lysp_module **module)
{
    const char *str, *name;
    struct lysp_tpdf *typedefs;
    unsigned int u, v;

    assert(id);
    assert(start_module);
    assert(tpdf);
    assert(node);
    assert(module);

    *node = NULL;
    str = strchr(id, ':');
    if (str) {
        *module = lysp_module_find_prefix(start_module, id, str - id);
        name = str + 1;
        *type = LY_TYPE_UNKNOWN;
    } else {
        *module = start_module;
        name = id;

        /* check for built-in types */
        *type = lysp_type_str2builtin(name, strlen(name));
        if (*type) {
            *tpdf = NULL;
            return LY_SUCCESS;
        }
    }
    LY_CHECK_RET(!(*module), LY_ENOTFOUND);

    if (start_node && *module == start_module) {
        /* search typedefs in parent's nodes */
        *node = start_node;
        while (*node) {
            *tpdf = lysp_type_match(name, *node);
            if (*tpdf) {
                /* match */
                return LY_SUCCESS;
            }
            *node = (*node)->parent;
        }
    }

    /* search in top-level typedefs */
    if ((*module)->typedefs) {
        LY_ARRAY_FOR((*module)->typedefs, u) {
            if (!strcmp(name, (*module)->typedefs[u].name)) {
                /* match */
                *tpdf = &(*module)->typedefs[u];
                return LY_SUCCESS;
            }
        }
    }

    /* search in submodules' typedefs */
    LY_ARRAY_FOR((*module)->includes, u) {
        typedefs = (*module)->includes[u].submodule->typedefs;
        LY_ARRAY_FOR(typedefs, v) {
            if (!strcmp(name, typedefs[v].name)) {
                /* match */
                *tpdf = &typedefs[v];
                return LY_SUCCESS;
            }
        }
    }

    return LY_ENOTFOUND;
}

/*
 * @brief Check name of a new type to avoid name collisions.
 *
 * @param[in] ctx Parser context, module where the type is being defined is taken from here.
 * @param[in] node Schema node where the type is being defined, NULL in case of a top-level typedef.
 * @param[in] tpdf Typedef definition to check.
 * @param[in,out] tpdfs_global Initialized hash table to store temporary data between calls. When the module's
 *            typedefs are checked, caller is supposed to free the table.
 * @param[in,out] tpdfs_global Initialized hash table to store temporary data between calls. When the module's
 *            typedefs are checked, caller is supposed to free the table.
 * @return LY_EEXIST in case of collision, LY_SUCCESS otherwise.
 */
static LY_ERR
lysp_check_typedef(struct ly_parser_ctx *ctx, struct lysp_node *node, const struct lysp_tpdf *tpdf,
                   struct hash_table *tpdfs_global, struct hash_table *tpdfs_scoped)
{
    struct lysp_node *parent;
    uint32_t hash;
    size_t name_len;
    const char *name;
    unsigned int u;
    const struct lysp_tpdf *typedefs;

    assert(ctx);
    assert(tpdf);

    name = tpdf->name;
    name_len = strlen(name);

    if (lysp_type_str2builtin(name, name_len)) {
        LOGVAL(ctx->ctx, LY_VLOG_LINE, &ctx->line, LYVE_SYNTAX_YANG,
               "Invalid name \"%s\" of typedef - name collision with a built-in type.", name);
        return LY_EEXIST;
    }

    /* check locally scoped typedefs (avoid name shadowing) */
    if (node) {
        typedefs = lysp_node_typedefs(node);
        LY_ARRAY_FOR(typedefs, u) {
            if (&typedefs[u] == tpdf) {
                break;
            }
            if (!strcmp(name, typedefs[u].name)) {
                LOGVAL(ctx->ctx, LY_VLOG_LINE, &ctx->line, LYVE_SYNTAX_YANG,
                       "Invalid name \"%s\" of typedef - name collision with sibling type.", name);
                return LY_EEXIST;
            }
        }
        /* search typedefs in parent's nodes */
        for (parent = node->parent; parent; parent = node->parent) {
            if (lysp_type_match(name, parent)) {
                LOGVAL(ctx->ctx, LY_VLOG_LINE, &ctx->line, LYVE_SYNTAX_YANG,
                       "Invalid name \"%s\" of typedef - name collision with another scoped type.", name);
                return LY_EEXIST;
            }
        }
    }

    /* check collision with the top-level typedefs */
    hash = dict_hash(name, name_len);
    if (node) {
        lyht_insert(tpdfs_scoped, &name, hash, NULL);
        if (!lyht_find(tpdfs_global, &name, hash, NULL)) {
            LOGVAL(ctx->ctx, LY_VLOG_LINE, &ctx->line, LYVE_SYNTAX_YANG,
                   "Invalid name \"%s\" of typedef - scoped type collide with a top-level type.", name);
            return LY_EEXIST;
        }
    } else {
        if (lyht_insert(tpdfs_global, &name, hash, NULL)) {
            LOGVAL(ctx->ctx, LY_VLOG_LINE, &ctx->line, LYVE_SYNTAX_YANG,
                   "Invalid name \"%s\" of typedef - name collision with another top-level type.", name);
            return LY_EEXIST;
        }
        /* it is not necessary to test collision with the scoped types - in lysp_check_typedefs, all the
         * top-level typedefs are inserted into the tables before the scoped typedefs, so the collision
         * is detected in the first branch few lines above */
    }

    return LY_SUCCESS;
}

static int
lysp_id_cmp(void *val1, void *val2, int UNUSED(mod), void *UNUSED(cb_data))
{
    return !strcmp(val1, val2);
}

LY_ERR
lysp_check_typedefs(struct ly_parser_ctx *ctx, struct lysp_module *mod)
{
    struct hash_table *ids_global;
    struct hash_table *ids_scoped;
    const struct lysp_tpdf *typedefs;
    unsigned int i, u;
    LY_ERR ret = LY_EVALID;

    /* check name collisions - typedefs and groupings */
    ids_global = lyht_new(8, sizeof(char*), lysp_id_cmp, NULL, 1);
    ids_scoped = lyht_new(8, sizeof(char*), lysp_id_cmp, NULL, 1);
    LY_ARRAY_FOR(mod->typedefs, i) {
        if (lysp_check_typedef(ctx, NULL, &mod->typedefs[i], ids_global, ids_scoped)) {
            goto cleanup;
        }
    }
    LY_ARRAY_FOR(mod->includes, i) {
        LY_ARRAY_FOR(mod->includes[i].submodule->typedefs, u) {
            if (lysp_check_typedef(ctx, NULL, &mod->includes[i].submodule->typedefs[u], ids_global, ids_scoped)) {
                goto cleanup;
            }
        }
    }
    for (u = 0; u < ctx->tpdfs_nodes.count; ++u) {
        typedefs = lysp_node_typedefs((struct lysp_node *)ctx->tpdfs_nodes.objs[u]);
        LY_ARRAY_FOR(typedefs, i) {
            if (lysp_check_typedef(ctx, (struct lysp_node *)ctx->tpdfs_nodes.objs[u], &typedefs[i], ids_global, ids_scoped)) {
                goto cleanup;
            }
        }
    }
    ret = LY_SUCCESS;
cleanup:
    lyht_free(ids_global);
    lyht_free(ids_scoped);
    ly_set_erase(&ctx->tpdfs_nodes, NULL);

    return ret;
}

struct lysp_load_module_check_data {
    const char *name;
    const char *revision;
    const char *path;
    const char* submoduleof;
};

static LY_ERR
lysp_load_module_check(struct ly_ctx *ctx, struct lysp_module *mod, struct lysp_submodule *submod, void *data)
{
    struct lysp_load_module_check_data *info = data;
    const char *filename, *dot, *rev, *name;
    size_t len;
    struct lysp_revision *revs;

    name = mod ? mod->mod->name : submod->name;
    revs = mod ? mod->revs : submod->revs;

    if (info->name) {
        /* check name of the parsed model */
        if (strcmp(info->name, name)) {
            LOGERR(ctx, LY_EINVAL, "Unexpected module \"%s\" parsed instead of \"%s\").", name, info->name);
            return LY_EINVAL;
        }
    }
    if (info->revision) {
        /* check revision of the parsed model */
        if (!revs || strcmp(info->revision, revs[0].date)) {
            LOGERR(ctx, LY_EINVAL, "Module \"%s\" parsed with the wrong revision (\"%s\" instead \"%s\").", name,
                   revs[0].date, info->revision);
            return LY_EINVAL;
        }
    }
    if (submod) {
        assert(info->submoduleof);

        /* check that the submodule belongs-to our module */
        if (strcmp(info->submoduleof, submod->belongsto)) {
            LOGVAL(ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE, "Included \"%s\" submodule from \"%s\" belongs-to a different module \"%s\".",
                   submod->name, info->submoduleof, submod->belongsto);
            return LY_EVALID;
        }
        /* check circular dependency */
        if (submod->parsing) {
            LOGVAL(ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE, "A circular dependency (include) for module \"%s\".", submod->name);
            return LY_EVALID;
        }
    }
    if (info->path) {
        /* check that name and revision match filename */
        filename = strrchr(info->path, '/');
        if (!filename) {
            filename = info->path;
        } else {
            filename++;
        }
        /* name */
        len = strlen(name);
        rev = strchr(filename, '@');
        dot = strrchr(info->path, '.');
        if (strncmp(filename, name, len) ||
                ((rev && rev != &filename[len]) || (!rev && dot != &filename[len]))) {
            LOGWRN(ctx, "File name \"%s\" does not match module name \"%s\".", filename, name);
        }
        /* revision */
        if (rev) {
            len = dot - ++rev;
            if (!revs || len != 10 || strncmp(revs[0].date, rev, len)) {
                LOGWRN(ctx, "File name \"%s\" does not match module revision \"%s\".", filename,
                       revs ? revs[0].date : "none");
            }
        }
    }
    return LY_SUCCESS;
}

LY_ERR
lys_module_localfile(struct ly_ctx *ctx, const char *name, const char *revision, int implement, struct ly_parser_ctx *main_ctx,
                     void **result)
{
    int fd;
    char *filepath = NULL;
    const char **fp;
    LYS_INFORMAT format;
    void *mod = NULL;
    LY_ERR ret = LY_SUCCESS;
    struct lysp_load_module_check_data check_data = {0};
    char rpath[PATH_MAX];

    LY_CHECK_RET(lys_search_localfile(ly_ctx_get_searchdirs(ctx), !(ctx->flags & LY_CTX_DISABLE_SEARCHDIR_CWD), name, revision,
                                      &filepath, &format));
    LY_CHECK_ERR_RET(!filepath, LOGERR(ctx, LY_ENOTFOUND, "Data model \"%s%s%s\" not found in local searchdirs.",
                                       name, revision ? "@" : "", revision ? revision : ""), LY_ENOTFOUND);


    LOGVRB("Loading schema from \"%s\" file.", filepath);

    /* open the file */
    fd = open(filepath, O_RDONLY);
    LY_CHECK_ERR_GOTO(fd < 0, LOGERR(ctx, LY_ESYS, "Unable to open data model file \"%s\" (%s).",
                                     filepath, strerror(errno)); ret = LY_ESYS, cleanup);

    check_data.name = name;
    check_data.revision = revision;
    check_data.path = filepath;
    mod = lys_parse_fd_(ctx, fd, format, implement, main_ctx,
                        lysp_load_module_check, &check_data);
    close(fd);
    LY_CHECK_ERR_GOTO(!mod, ly_errcode(ctx), cleanup);

    if (main_ctx) {
        fp = &((struct lysp_submodule*)mod)->filepath;
    } else {
        fp = &((struct lys_module*)mod)->filepath;
    }
    if (!(*fp)) {
        if (realpath(filepath, rpath) != NULL) {
            (*fp) = lydict_insert(ctx, rpath, 0);
        } else {
            (*fp) = lydict_insert(ctx, filepath, 0);
        }
    }

    *result = mod;

    /* success */
cleanup:
    free(filepath);
    return ret;
}

LY_ERR
lysp_load_module(struct ly_ctx *ctx, const char *name, const char *revision, int implement, int require_parsed, struct lys_module **mod)
{
    const char *module_data = NULL;
    LYS_INFORMAT format = LYS_IN_UNKNOWN;
    void (*module_data_free)(void *module_data, void *user_data) = NULL;
    struct lysp_load_module_check_data check_data = {0};
    struct lys_module *m;

    assert(mod);

    if (!*mod) {
        /* try to get the module from the context */
        if (revision) {
            *mod = (struct lys_module*)ly_ctx_get_module(ctx, name, revision);
        } else {
            *mod = (struct lys_module*)ly_ctx_get_module_latest(ctx, name);
        }
    }

    if (!(*mod) || (require_parsed && !(*mod)->parsed)) {
        (*mod) = NULL;

        /* check collision with other implemented revision */
        if (implement && ly_ctx_get_module_implemented(ctx, name)) {
            LOGVAL(ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE,
                   "Module \"%s\" is already present in other implemented revision.", name);
            return LY_EDENIED;
        }

        /* module not present in the context, get the input data and parse it */
        if (!(ctx->flags & LY_CTX_PREFER_SEARCHDIRS)) {
search_clb:
            if (ctx->imp_clb) {
                if (ctx->imp_clb(name, revision, NULL, NULL, ctx->imp_clb_data,
                                      &format, &module_data, &module_data_free) == LY_SUCCESS) {
                    check_data.name = name;
                    check_data.revision = revision;
                    *mod = lys_parse_mem_module(ctx, module_data, format, implement,
                                                lysp_load_module_check, &check_data);
                    if (module_data_free) {
                        module_data_free((void*)module_data, ctx->imp_clb_data);
                    }
                    if (*mod && implement && lys_compile(*mod, 0)) {
                        ly_set_rm(&ctx->list, *mod, NULL);
                        lys_module_free(*mod, NULL);
                        *mod = NULL;
                    }
                }
            }
            if (!(*mod) && !(ctx->flags & LY_CTX_PREFER_SEARCHDIRS)) {
                goto search_file;
            }
        } else {
search_file:
            if (!(ctx->flags & LY_CTX_DISABLE_SEARCHDIRS)) {
                /* module was not received from the callback or there is no callback set */
                lys_module_localfile(ctx, name, revision, implement, NULL, (void **)mod);
            }
            if (!(*mod) && (ctx->flags & LY_CTX_PREFER_SEARCHDIRS)) {
                goto search_clb;
            }
        }

        if ((*mod) && !revision && ((*mod)->latest_revision == 1)) {
            /* update the latest_revision flag - here we have selected the latest available schema,
             * consider that even the callback provides correct latest revision */
            (*mod)->latest_revision = 2;
        }
    } else {
        /* we have module from the current context */
        if (implement) {
            m = ly_ctx_get_module_implemented(ctx, name);
            if (m && m != *mod) {
                /* check collision with other implemented revision */
                LOGVAL(ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE,
                       "Module \"%s\" is already present in other implemented revision.", name);
                *mod = NULL;
                return LY_EDENIED;
            }
        }

        /* circular check */
        if ((*mod)->parsed && (*mod)->parsed->parsing) {
            LOGVAL(ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE, "A circular dependency (import) for module \"%s\".", name);
            *mod = NULL;
            return LY_EVALID;
        }
    }
    if (!(*mod)) {
        LOGVAL(ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE, "%s \"%s\" module failed.", implement ? "Loading" : "Importing", name);
        return LY_EVALID;
    }

    if (implement) {
        /* mark the module implemented, check for collision was already done */
        (*mod)->implemented = 1;
    }

    return LY_SUCCESS;
}

LY_ERR
lysp_load_submodule(struct ly_parser_ctx *ctx, struct lysp_module *mod, struct lysp_include *inc)
{
    struct lysp_submodule *submod;
    const char *submodule_data = NULL;
    LYS_INFORMAT format = LYS_IN_UNKNOWN;
    void (*submodule_data_free)(void *module_data, void *user_data) = NULL;
    struct lysp_load_module_check_data check_data = {0};

    /* submodule not present in the context, get the input data and parse it */
    if (!(ctx->ctx->flags & LY_CTX_PREFER_SEARCHDIRS)) {
search_clb:
        if (ctx->ctx->imp_clb) {
            if (ctx->ctx->imp_clb(mod->mod->name, NULL, inc->name, inc->rev[0] ? inc->rev : NULL, ctx->ctx->imp_clb_data,
                                  &format, &submodule_data, &submodule_data_free) == LY_SUCCESS) {
                check_data.name = inc->name;
                check_data.revision = inc->rev[0] ? inc->rev : NULL;
                check_data.submoduleof = mod->mod->name;
                submod = lys_parse_mem_submodule(ctx->ctx, submodule_data, format, ctx,
                                                 lysp_load_module_check, &check_data);
                if (submodule_data_free) {
                    submodule_data_free((void*)submodule_data, ctx->ctx->imp_clb_data);
                }
            }
        }
        if (!submod && !(ctx->ctx->flags & LY_CTX_PREFER_SEARCHDIRS)) {
            goto search_file;
        }
    } else {
search_file:
        if (!(ctx->ctx->flags & LY_CTX_DISABLE_SEARCHDIRS)) {
            /* submodule was not received from the callback or there is no callback set */
            lys_module_localfile(ctx->ctx, inc->name, inc->rev[0] ? inc->rev : NULL, 0, ctx, (void**)&submod);
        }
        if (!submod && (ctx->ctx->flags & LY_CTX_PREFER_SEARCHDIRS)) {
            goto search_clb;
        }
    }
    if (submod) {
        if (!inc->rev[0] && (submod->latest_revision == 1)) {
            /* update the latest_revision flag - here we have selected the latest available schema,
             * consider that even the callback provides correct latest revision */
            submod->latest_revision = 2;
        }

        inc->submodule = submod;
    }
    if (!inc->submodule) {
        LOGVAL(ctx->ctx, LY_VLOG_NONE, NULL, LYVE_REFERENCE, "Including \"%s\" submodule into \"%s\" failed.",
               inc->name, mod->mod->name);
        return LY_EVALID;
    }

    return LY_SUCCESS;
}

#define FIND_MODULE(TYPE, MOD) \
    TYPE *imp; \
    if (!strncmp((MOD)->mod->prefix, prefix, len) && (MOD)->mod->prefix[len] == '\0') { \
        /* it is the prefix of the module itself */ \
        m = ly_ctx_get_module((MOD)->mod->ctx, (MOD)->mod->name, (MOD)->mod->revision); \
    } \
    /* search in imports */ \
    if (!m) { \
        LY_ARRAY_FOR((MOD)->imports, TYPE, imp) { \
            if (!strncmp(imp->prefix, prefix, len) && imp->prefix[len] == '\0') { \
                m = imp->module; \
                break; \
            } \
        } \
    }

struct lysc_module *
lysc_module_find_prefix(const struct lysc_module *mod, const char *prefix, size_t len)
{
    const struct lys_module *m = NULL;

    FIND_MODULE(struct lysc_import, mod);
    return m ? m->compiled : NULL;
}

struct lysp_module *
lysp_module_find_prefix(const struct lysp_module *mod, const char *prefix, size_t len)
{
    const struct lys_module *m = NULL;

    FIND_MODULE(struct lysp_import, mod);
    return m ? m->parsed : NULL;
}

struct lys_module *
lys_module_find_prefix(const struct lys_module *mod, const char *prefix, size_t len)
{
    const struct lys_module *m = NULL;

    if (mod->compiled) {
        FIND_MODULE(struct lysc_import, mod->compiled);
    } else {
        FIND_MODULE(struct lysp_import, mod->parsed);
    }
    return (struct lys_module*)m;
}

const char *
lys_nodetype2str(uint16_t nodetype)
{
    switch(nodetype) {
    case LYS_CONTAINER:
        return "container";
    case LYS_CHOICE:
        return "choice";
    case LYS_LEAF:
        return "leaf";
    case LYS_LEAFLIST:
        return "leaf-list";
    case LYS_LIST:
        return "list";
    case LYS_ANYXML:
        return "anyxml";
    case LYS_ANYDATA:
        return "anydata";
    case LYS_CASE:
        return "case";
    case LYS_ACTION:
        return "RPC/action";
    case LYS_NOTIF:
        return "Notification";
    default:
        return "unknown";
    }
}

API const struct lysp_tpdf *
lysp_node_typedefs(const struct lysp_node *node)
{
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return ((struct lysp_node_container*)node)->typedefs;
    case LYS_LIST:
        return ((struct lysp_node_list*)node)->typedefs;
    case LYS_GROUPING:
        return ((struct lysp_grp*)node)->typedefs;
    case LYS_ACTION:
        return ((struct lysp_action*)node)->typedefs;
    case LYS_INOUT:
        return ((struct lysp_action_inout*)node)->typedefs;
    case LYS_NOTIF:
        return ((struct lysp_notif*)node)->typedefs;
    default:
        return NULL;
    }
}

API const struct lysp_grp *
lysp_node_groupings(const struct lysp_node *node)
{
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return ((struct lysp_node_container*)node)->groupings;
    case LYS_LIST:
        return ((struct lysp_node_list*)node)->groupings;
    case LYS_GROUPING:
        return ((struct lysp_grp*)node)->groupings;
    case LYS_ACTION:
        return ((struct lysp_action*)node)->groupings;
    case LYS_INOUT:
        return ((struct lysp_action_inout*)node)->groupings;
    case LYS_NOTIF:
        return ((struct lysp_notif*)node)->groupings;
    default:
        return NULL;
    }
}

struct lysp_action **
lysp_node_actions_p(struct lysp_node *node)
{
    assert(node);
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return &((struct lysp_node_container*)node)->actions;
    case LYS_LIST:
        return &((struct lysp_node_list*)node)->actions;
    case LYS_GROUPING:
        return &((struct lysp_grp*)node)->actions;
    case LYS_AUGMENT:
        return &((struct lysp_augment*)node)->actions;
    default:
        return NULL;
    }
}

API const struct lysp_action *
lysp_node_actions(const struct lysp_node *node)
{
    struct lysp_action **actions;
    actions = lysp_node_actions_p((struct lysp_node*)node);
    if (actions) {
        return *actions;
    } else {
        return NULL;
    }
}

struct lysp_notif **
lysp_node_notifs_p(struct lysp_node *node)
{
    assert(node);
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return &((struct lysp_node_container*)node)->notifs;
    case LYS_LIST:
        return &((struct lysp_node_list*)node)->notifs;
    case LYS_GROUPING:
        return &((struct lysp_grp*)node)->notifs;
    case LYS_AUGMENT:
        return &((struct lysp_augment*)node)->notifs;
    default:
        return NULL;
    }
}

API const struct lysp_notif *
lysp_node_notifs(const struct lysp_node *node)
{
    struct lysp_notif **notifs;
    notifs = lysp_node_notifs_p((struct lysp_node*)node);
    if (notifs) {
        return *notifs;
    } else {
        return NULL;
    }
}

struct lysp_node **
lysp_node_children_p(struct lysp_node *node)
{
    assert(node);
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return &((struct lysp_node_container*)node)->child;
    case LYS_CHOICE:
        return &((struct lysp_node_choice*)node)->child;
    case LYS_LIST:
        return &((struct lysp_node_list*)node)->child;
    case LYS_CASE:
        return &((struct lysp_node_case*)node)->child;
    case LYS_GROUPING:
        return &((struct lysp_grp*)node)->data;
    case LYS_AUGMENT:
        return &((struct lysp_augment*)node)->child;
    case LYS_INOUT:
        return &((struct lysp_action_inout*)node)->data;
    case LYS_NOTIF:
        return &((struct lysp_notif*)node)->data;
    default:
        return NULL;
    }
}

API const struct lysp_node *
lysp_node_children(const struct lysp_node *node)
{
    struct lysp_node **children;
    children = lysp_node_children_p((struct lysp_node*)node);
    if (children) {
        return *children;
    } else {
        return NULL;
    }
}

struct lysc_action **
lysc_node_actions_p(struct lysc_node *node)
{
    assert(node);
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return &((struct lysc_node_container*)node)->actions;
    case LYS_LIST:
        return &((struct lysc_node_list*)node)->actions;
    default:
        return NULL;
    }
}

API const struct lysc_action *
lysc_node_actions(const struct lysc_node *node)
{
    struct lysc_action **actions;
    actions = lysc_node_actions_p((struct lysc_node*)node);
    if (actions) {
        return *actions;
    } else {
        return NULL;
    }
}

struct lysc_notif **
lysc_node_notifs_p(struct lysc_node *node)
{
    assert(node);
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return &((struct lysc_node_container*)node)->notifs;
    case LYS_LIST:
        return &((struct lysc_node_list*)node)->notifs;
    default:
        return NULL;
    }
}

API const struct lysc_notif *
lysc_node_notifs(const struct lysc_node *node)
{
    struct lysc_notif **notifs;
    notifs = lysc_node_notifs_p((struct lysc_node*)node);
    if (notifs) {
        return *notifs;
    } else {
        return NULL;
    }
}

struct lysc_node **
lysc_node_children_p(const struct lysc_node *node, uint16_t flags)
{
    assert(node);
    switch (node->nodetype) {
    case LYS_CONTAINER:
        return &((struct lysc_node_container*)node)->child;
    case LYS_CHOICE:
        if (((struct lysc_node_choice*)node)->cases) {
            return &((struct lysc_node_choice*)node)->cases->child;
        } else {
            return NULL;
        }
    case LYS_CASE:
        return &((struct lysc_node_case*)node)->child;
    case LYS_LIST:
        return &((struct lysc_node_list*)node)->child;
    case LYS_ACTION:
        if (flags & LYS_CONFIG_R) {
            return &((struct lysc_action*)node)->output.data;
        } else {
            /* LYS_CONFIG_W, but also the default case */
            return &((struct lysc_action*)node)->input.data;
        }
    /* TODO Notification */
    default:
        return NULL;
    }
}

API const struct lysc_node *
lysc_node_children(const struct lysc_node *node, uint16_t flags)
{
    struct lysc_node **children;
    children = lysc_node_children_p((struct lysc_node*)node, flags);
    if (children) {
        return *children;
    } else {
        return NULL;
    }
}

struct lys_module *
lysp_find_module(struct ly_ctx *ctx, const struct lysp_module *mod)
{
    unsigned int u;

    for (u = 0; u < ctx->list.count; ++u) {
        if (((struct lys_module*)ctx->list.objs[u])->parsed == mod) {
            return ((struct lys_module*)ctx->list.objs[u]);
        }
    }
    return NULL;
}

enum yang_keyword
match_keyword(const char *data, size_t len, size_t prefix_len)
{
    if (prefix_len != 0) {
        return YANG_CUSTOM;
    }

#define MOVE_IN(DATA, COUNT) (data)+=COUNT;
#define IF_KEYWORD(STR, LEN, STMT) if (!strncmp((data), STR, LEN)) {MOVE_IN(data, LEN);kw=STMT;}
#define IF_KEYWORD_PREFIX(STR, LEN) if (!strncmp((data), STR, LEN)) {MOVE_IN(data, LEN);
#define IF_KEYWORD_PREFIX_END }

    const char *start = data;
    enum yang_keyword kw = YANG_NONE;
    /* try to match the keyword itself */
    switch (*data) {
    case 'a':
        MOVE_IN(data, 1);
        IF_KEYWORD("rgument", 7, YANG_ARGUMENT)
        else IF_KEYWORD("ugment", 6, YANG_AUGMENT)
        else IF_KEYWORD("ction", 5, YANG_ACTION)
        else IF_KEYWORD_PREFIX("ny", 2)
            IF_KEYWORD("data", 4, YANG_ANYDATA)
            else IF_KEYWORD("xml", 3, YANG_ANYXML)
        IF_KEYWORD_PREFIX_END
        break;
    case 'b':
        MOVE_IN(data, 1);
        IF_KEYWORD("ase", 3, YANG_BASE)
        else IF_KEYWORD("elongs-to", 9, YANG_BELONGS_TO)
        else IF_KEYWORD("it", 2, YANG_BIT)
        break;
    case 'c':
        MOVE_IN(data, 1);
        IF_KEYWORD("ase", 3, YANG_CASE)
        else IF_KEYWORD("hoice", 5, YANG_CHOICE)
        else IF_KEYWORD_PREFIX("on", 2)
            IF_KEYWORD("fig", 3, YANG_CONFIG)
            else IF_KEYWORD_PREFIX("ta", 2)
                IF_KEYWORD("ct", 2, YANG_CONTACT)
                else IF_KEYWORD("iner", 4, YANG_CONTAINER)
            IF_KEYWORD_PREFIX_END
        IF_KEYWORD_PREFIX_END
        break;
    case 'd':
        MOVE_IN(data, 1);
        IF_KEYWORD_PREFIX("e", 1)
            IF_KEYWORD("fault", 5, YANG_DEFAULT)
            else IF_KEYWORD("scription", 9, YANG_DESCRIPTION)
            else IF_KEYWORD_PREFIX("viat", 4)
                IF_KEYWORD("e", 1, YANG_DEVIATE)
                else IF_KEYWORD("ion", 3, YANG_DEVIATION)
            IF_KEYWORD_PREFIX_END
        IF_KEYWORD_PREFIX_END
        break;
    case 'e':
        MOVE_IN(data, 1);
        IF_KEYWORD("num", 3, YANG_ENUM)
        else IF_KEYWORD_PREFIX("rror-", 5)
            IF_KEYWORD("app-tag", 7, YANG_ERROR_APP_TAG)
            else IF_KEYWORD("message", 7, YANG_ERROR_MESSAGE)
        IF_KEYWORD_PREFIX_END
        else IF_KEYWORD("xtension", 8, YANG_EXTENSION)
        break;
    case 'f':
        MOVE_IN(data, 1);
        IF_KEYWORD("eature", 6, YANG_FEATURE)
        else IF_KEYWORD("raction-digits", 14, YANG_FRACTION_DIGITS)
        break;
    case 'g':
        MOVE_IN(data, 1);
        IF_KEYWORD("rouping", 7, YANG_GROUPING)
        break;
    case 'i':
        MOVE_IN(data, 1);
        IF_KEYWORD("dentity", 7, YANG_IDENTITY)
        else IF_KEYWORD("f-feature", 9, YANG_IF_FEATURE)
        else IF_KEYWORD("mport", 5, YANG_IMPORT)
        else IF_KEYWORD_PREFIX("n", 1)
            IF_KEYWORD("clude", 5, YANG_INCLUDE)
            else IF_KEYWORD("put", 3, YANG_INPUT)
        IF_KEYWORD_PREFIX_END
        break;
    case 'k':
        MOVE_IN(data, 1);
        IF_KEYWORD("ey", 2, YANG_KEY)
        break;
    case 'l':
        MOVE_IN(data, 1);
        IF_KEYWORD_PREFIX("e", 1)
            IF_KEYWORD("af-list", 7, YANG_LEAF_LIST)
            else IF_KEYWORD("af", 2, YANG_LEAF)
            else IF_KEYWORD("ngth", 4, YANG_LENGTH)
        IF_KEYWORD_PREFIX_END
        else IF_KEYWORD("ist", 3, YANG_LIST)
        break;
    case 'm':
        MOVE_IN(data, 1);
        IF_KEYWORD_PREFIX("a", 1)
            IF_KEYWORD("ndatory", 7, YANG_MANDATORY)
            else IF_KEYWORD("x-elements", 10, YANG_MAX_ELEMENTS)
        IF_KEYWORD_PREFIX_END
        else IF_KEYWORD("in-elements", 11, YANG_MIN_ELEMENTS)
        else IF_KEYWORD("ust", 3, YANG_MUST)
        else IF_KEYWORD_PREFIX("od", 2)
            IF_KEYWORD("ule", 3, YANG_MODULE)
            else IF_KEYWORD("ifier", 5, YANG_MODIFIER)
        IF_KEYWORD_PREFIX_END
        break;
    case 'n':
        MOVE_IN(data, 1);
        IF_KEYWORD("amespace", 8, YANG_NAMESPACE)
        else IF_KEYWORD("otification", 11, YANG_NOTIFICATION)
        break;
    case 'o':
        MOVE_IN(data, 1);
        IF_KEYWORD_PREFIX("r", 1)
            IF_KEYWORD("dered-by", 8, YANG_ORDERED_BY)
            else IF_KEYWORD("ganization", 10, YANG_ORGANIZATION)
        IF_KEYWORD_PREFIX_END
        else IF_KEYWORD("utput", 5, YANG_OUTPUT)
        break;
    case 'p':
        MOVE_IN(data, 1);
        IF_KEYWORD("ath", 3, YANG_PATH)
        else IF_KEYWORD("attern", 6, YANG_PATTERN)
        else IF_KEYWORD("osition", 7, YANG_POSITION)
        else IF_KEYWORD_PREFIX("re", 2)
            IF_KEYWORD("fix", 3, YANG_PREFIX)
            else IF_KEYWORD("sence", 5, YANG_PRESENCE)
        IF_KEYWORD_PREFIX_END
        break;
    case 'r':
        MOVE_IN(data, 1);
        IF_KEYWORD("ange", 4, YANG_RANGE)
        else IF_KEYWORD_PREFIX("e", 1)
            IF_KEYWORD_PREFIX("f", 1)
                IF_KEYWORD("erence", 6, YANG_REFERENCE)
                else IF_KEYWORD("ine", 3, YANG_REFINE)
            IF_KEYWORD_PREFIX_END
            else IF_KEYWORD("quire-instance", 14, YANG_REQUIRE_INSTANCE)
            else IF_KEYWORD("vision-date", 11, YANG_REVISION_DATE)
            else IF_KEYWORD("vision", 6, YANG_REVISION)
        IF_KEYWORD_PREFIX_END
        else IF_KEYWORD("pc", 2, YANG_RPC)
        break;
    case 's':
        MOVE_IN(data, 1);
        IF_KEYWORD("tatus", 5, YANG_STATUS)
        else IF_KEYWORD("ubmodule", 8, YANG_SUBMODULE)
        break;
    case 't':
        MOVE_IN(data, 1);
        IF_KEYWORD("ypedef", 6, YANG_TYPEDEF)
        else IF_KEYWORD("ype", 3, YANG_TYPE)
        break;
    case 'u':
        MOVE_IN(data, 1);
        IF_KEYWORD_PREFIX("ni", 2)
            IF_KEYWORD("que", 3, YANG_UNIQUE)
            else IF_KEYWORD("ts", 2, YANG_UNITS)
        IF_KEYWORD_PREFIX_END
        else IF_KEYWORD("ses", 3, YANG_USES)
        break;
    case 'v':
        MOVE_IN(data, 1);
        IF_KEYWORD("alue", 4, YANG_VALUE)
        break;
    case 'w':
        MOVE_IN(data, 1);
        IF_KEYWORD("hen", 3, YANG_WHEN)
        break;
    case 'y':
        MOVE_IN(data, 1);
        IF_KEYWORD("ang-version", 11, YANG_YANG_VERSION)
        else IF_KEYWORD("in-element", 10, YANG_YIN_ELEMENT)
        break;
    default:
        break;
    }

#undef MOVE_IN
#undef IF_KEYWORD
#undef IF_KEYWORD_PREFIX
#undef IF_KEYWORD_PREFIX_END

    if (data - start == (long int)len) {
        return kw;
    } else {
        return YANG_NONE;
    }

}
