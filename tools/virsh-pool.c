/*
 * virsh-pool.c: Commands to manage storage pool
 *
 * Copyright (C) 2005, 2007-2012 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 *  Daniel Veillard <veillard@redhat.com>
 *  Karel Zak <kzak@redhat.com>
 *  Daniel P. Berrange <berrange@redhat.com>
 *
 */

#include <config.h>
#include "virsh-pool.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xmlsave.h>

#include "internal.h"
#include "buf.h"
#include "memory.h"
#include "util.h"
#include "xml.h"
#include "conf/storage_conf.h"

virStoragePoolPtr
vshCommandOptPoolBy(vshControl *ctl, const vshCmd *cmd, const char *optname,
                    const char **name, unsigned int flags)
{
    virStoragePoolPtr pool = NULL;
    const char *n = NULL;
    virCheckFlags(VSH_BYUUID | VSH_BYNAME, NULL);

    if (vshCommandOptString(cmd, optname, &n) <= 0)
        return NULL;

    vshDebug(ctl, VSH_ERR_INFO, "%s: found option <%s>: %s\n",
             cmd->def->name, optname, n);

    if (name)
        *name = n;

    /* try it by UUID */
    if ((flags & VSH_BYUUID) && strlen(n) == VIR_UUID_STRING_BUFLEN-1) {
        vshDebug(ctl, VSH_ERR_DEBUG, "%s: <%s> trying as pool UUID\n",
                 cmd->def->name, optname);
        pool = virStoragePoolLookupByUUIDString(ctl->conn, n);
    }
    /* try it by NAME */
    if (!pool && (flags & VSH_BYNAME)) {
        vshDebug(ctl, VSH_ERR_DEBUG, "%s: <%s> trying as pool NAME\n",
                 cmd->def->name, optname);
        pool = virStoragePoolLookupByName(ctl->conn, n);
    }

    if (!pool)
        vshError(ctl, _("failed to get pool '%s'"), n);

    return pool;
}

/*
 * "pool-autostart" command
 */
static const vshCmdInfo info_pool_autostart[] = {
    {"help", N_("autostart a pool")},
    {"desc",
     N_("Configure a pool to be automatically started at boot.")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_autostart[] = {
    {"pool",  VSH_OT_DATA, VSH_OFLAG_REQ, N_("pool name or uuid")},
    {"disable", VSH_OT_BOOL, 0, N_("disable autostarting")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolAutostart(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolPtr pool;
    const char *name;
    int autostart;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", &name)))
        return false;

    autostart = !vshCommandOptBool(cmd, "disable");

    if (virStoragePoolSetAutostart(pool, autostart) < 0) {
        if (autostart)
            vshError(ctl, _("failed to mark pool %s as autostarted"), name);
        else
            vshError(ctl, _("failed to unmark pool %s as autostarted"), name);
        virStoragePoolFree(pool);
        return false;
    }

    if (autostart)
        vshPrint(ctl, _("Pool %s marked as autostarted\n"), name);
    else
        vshPrint(ctl, _("Pool %s unmarked as autostarted\n"), name);

    virStoragePoolFree(pool);
    return true;
}

/*
 * "pool-create" command
 */
static const vshCmdInfo info_pool_create[] = {
    {"help", N_("create a pool from an XML file")},
    {"desc", N_("Create a pool.")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_create[] = {
    {"file", VSH_OT_DATA, VSH_OFLAG_REQ,
     N_("file containing an XML pool description")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolCreate(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolPtr pool;
    const char *from = NULL;
    bool ret = true;
    char *buffer;

    if (vshCommandOptString(cmd, "file", &from) <= 0)
        return false;

    if (virFileReadAll(from, VSH_MAX_XML_FILE, &buffer) < 0)
        return false;

    pool = virStoragePoolCreateXML(ctl->conn, buffer, 0);
    VIR_FREE(buffer);

    if (pool != NULL) {
        vshPrint(ctl, _("Pool %s created from %s\n"),
                 virStoragePoolGetName(pool), from);
        virStoragePoolFree(pool);
    } else {
        vshError(ctl, _("Failed to create pool from %s"), from);
        ret = false;
    }
    return ret;
}

/*
 * XML Building helper for pool-define-as and pool-create-as
 */
static const vshCmdOptDef opts_pool_X_as[] = {
    {"name", VSH_OT_DATA, VSH_OFLAG_REQ, N_("name of the pool")},
    {"print-xml", VSH_OT_BOOL, 0, N_("print XML document, but don't define/create")},
    {"type", VSH_OT_DATA, VSH_OFLAG_REQ, N_("type of the pool")},
    {"source-host", VSH_OT_DATA, 0, N_("source-host for underlying storage")},
    {"source-path", VSH_OT_DATA, 0, N_("source path for underlying storage")},
    {"source-dev", VSH_OT_DATA, 0, N_("source device for underlying storage")},
    {"source-name", VSH_OT_DATA, 0, N_("source name for underlying storage")},
    {"target", VSH_OT_DATA, 0, N_("target for underlying storage")},
    {"source-format", VSH_OT_STRING, 0, N_("format for underlying storage")},
    {NULL, 0, 0, NULL}
};

static int buildPoolXML(const vshCmd *cmd, const char **retname, char **xml) {

    const char *name = NULL, *type = NULL, *srcHost = NULL, *srcPath = NULL,
               *srcDev = NULL, *srcName = NULL, *srcFormat = NULL, *target = NULL;
    virBuffer buf = VIR_BUFFER_INITIALIZER;

    if (vshCommandOptString(cmd, "name", &name) <= 0)
        goto cleanup;
    if (vshCommandOptString(cmd, "type", &type) <= 0)
        goto cleanup;

    if (vshCommandOptString(cmd, "source-host", &srcHost) < 0 ||
        vshCommandOptString(cmd, "source-path", &srcPath) < 0 ||
        vshCommandOptString(cmd, "source-dev", &srcDev) < 0 ||
        vshCommandOptString(cmd, "source-name", &srcName) < 0 ||
        vshCommandOptString(cmd, "source-format", &srcFormat) < 0 ||
        vshCommandOptString(cmd, "target", &target) < 0) {
        vshError(NULL, "%s", _("missing argument"));
        goto cleanup;
    }

    virBufferAsprintf(&buf, "<pool type='%s'>\n", type);
    virBufferAsprintf(&buf, "  <name>%s</name>\n", name);
    if (srcHost || srcPath || srcDev || srcFormat || srcName) {
        virBufferAddLit(&buf, "  <source>\n");

        if (srcHost)
            virBufferAsprintf(&buf, "    <host name='%s'/>\n", srcHost);
        if (srcPath)
            virBufferAsprintf(&buf, "    <dir path='%s'/>\n", srcPath);
        if (srcDev)
            virBufferAsprintf(&buf, "    <device path='%s'/>\n", srcDev);
        if (srcFormat)
            virBufferAsprintf(&buf, "    <format type='%s'/>\n", srcFormat);
        if (srcName)
            virBufferAsprintf(&buf, "    <name>%s</name>\n", srcName);

        virBufferAddLit(&buf, "  </source>\n");
    }
    if (target) {
        virBufferAddLit(&buf, "  <target>\n");
        virBufferAsprintf(&buf, "    <path>%s</path>\n", target);
        virBufferAddLit(&buf, "  </target>\n");
    }
    virBufferAddLit(&buf, "</pool>\n");

    if (virBufferError(&buf)) {
        vshPrint(ctl, "%s", _("Failed to allocate XML buffer"));
        return false;
    }

    *xml = virBufferContentAndReset(&buf);
    *retname = name;
    return true;

cleanup:
    virBufferFreeAndReset(&buf);
    return false;
}

/*
 * "pool-create-as" command
 */
static const vshCmdInfo info_pool_create_as[] = {
    {"help", N_("create a pool from a set of args")},
    {"desc", N_("Create a pool.")},
    {NULL, NULL}
};

static bool
cmdPoolCreateAs(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolPtr pool;
    const char *name;
    char *xml;
    bool printXML = vshCommandOptBool(cmd, "print-xml");

    if (!buildPoolXML(cmd, &name, &xml))
        return false;

    if (printXML) {
        vshPrint(ctl, "%s", xml);
        VIR_FREE(xml);
    } else {
        pool = virStoragePoolCreateXML(ctl->conn, xml, 0);
        VIR_FREE(xml);

        if (pool != NULL) {
            vshPrint(ctl, _("Pool %s created\n"), name);
            virStoragePoolFree(pool);
        } else {
            vshError(ctl, _("Failed to create pool %s"), name);
            return false;
        }
    }
    return true;
}

/*
 * "pool-define" command
 */
static const vshCmdInfo info_pool_define[] = {
    {"help", N_("define (but don't start) a pool from an XML file")},
    {"desc", N_("Define a pool.")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_define[] = {
    {"file", VSH_OT_DATA, VSH_OFLAG_REQ, N_("file containing an XML pool description")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolDefine(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolPtr pool;
    const char *from = NULL;
    bool ret = true;
    char *buffer;

    if (vshCommandOptString(cmd, "file", &from) <= 0)
        return false;

    if (virFileReadAll(from, VSH_MAX_XML_FILE, &buffer) < 0)
        return false;

    pool = virStoragePoolDefineXML(ctl->conn, buffer, 0);
    VIR_FREE(buffer);

    if (pool != NULL) {
        vshPrint(ctl, _("Pool %s defined from %s\n"),
                 virStoragePoolGetName(pool), from);
        virStoragePoolFree(pool);
    } else {
        vshError(ctl, _("Failed to define pool from %s"), from);
        ret = false;
    }
    return ret;
}

/*
 * "pool-define-as" command
 */
static const vshCmdInfo info_pool_define_as[] = {
    {"help", N_("define a pool from a set of args")},
    {"desc", N_("Define a pool.")},
    {NULL, NULL}
};

static bool
cmdPoolDefineAs(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolPtr pool;
    const char *name;
    char *xml;
    bool printXML = vshCommandOptBool(cmd, "print-xml");

    if (!buildPoolXML(cmd, &name, &xml))
        return false;

    if (printXML) {
        vshPrint(ctl, "%s", xml);
        VIR_FREE(xml);
    } else {
        pool = virStoragePoolDefineXML(ctl->conn, xml, 0);
        VIR_FREE(xml);

        if (pool != NULL) {
            vshPrint(ctl, _("Pool %s defined\n"), name);
            virStoragePoolFree(pool);
        } else {
            vshError(ctl, _("Failed to define pool %s"), name);
            return false;
        }
    }
    return true;
}

/*
 * "pool-build" command
 */
static const vshCmdInfo info_pool_build[] = {
    {"help", N_("build a pool")},
    {"desc", N_("Build a given pool.")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_build[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, N_("pool name or uuid")},
    {"no-overwrite", VSH_OT_BOOL, 0, N_("do not overwrite an existing pool of this type")},
    {"overwrite", VSH_OT_BOOL, 0, N_("overwrite any existing data")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolBuild(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolPtr pool;
    bool ret = true;
    const char *name;
    unsigned int flags = 0;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", &name)))
        return false;

    if (vshCommandOptBool(cmd, "no-overwrite")) {
        flags |= VIR_STORAGE_POOL_BUILD_NO_OVERWRITE;
    }

    if (vshCommandOptBool(cmd, "overwrite")) {
        flags |= VIR_STORAGE_POOL_BUILD_OVERWRITE;
    }

    if (virStoragePoolBuild(pool, flags) == 0) {
        vshPrint(ctl, _("Pool %s built\n"), name);
    } else {
        vshError(ctl, _("Failed to build pool %s"), name);
        ret = false;
    }

    virStoragePoolFree(pool);

    return ret;
}

/*
 * "pool-destroy" command
 */
static const vshCmdInfo info_pool_destroy[] = {
    {"help", N_("destroy (stop) a pool")},
    {"desc",
     N_("Forcefully stop a given pool. Raw data in the pool is untouched")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_destroy[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, N_("pool name or uuid")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolDestroy(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolPtr pool;
    bool ret = true;
    const char *name;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", &name)))
        return false;

    if (virStoragePoolDestroy(pool) == 0) {
        vshPrint(ctl, _("Pool %s destroyed\n"), name);
    } else {
        vshError(ctl, _("Failed to destroy pool %s"), name);
        ret = false;
    }

    virStoragePoolFree(pool);
    return ret;
}

/*
 * "pool-delete" command
 */
static const vshCmdInfo info_pool_delete[] = {
    {"help", N_("delete a pool")},
    {"desc", N_("Delete a given pool.")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_delete[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, N_("pool name or uuid")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolDelete(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolPtr pool;
    bool ret = true;
    const char *name;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", &name)))
        return false;

    if (virStoragePoolDelete(pool, 0) == 0) {
        vshPrint(ctl, _("Pool %s deleted\n"), name);
    } else {
        vshError(ctl, _("Failed to delete pool %s"), name);
        ret = false;
    }

    virStoragePoolFree(pool);
    return ret;
}

/*
 * "pool-refresh" command
 */
static const vshCmdInfo info_pool_refresh[] = {
    {"help", N_("refresh a pool")},
    {"desc", N_("Refresh a given pool.")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_refresh[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, N_("pool name or uuid")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolRefresh(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolPtr pool;
    bool ret = true;
    const char *name;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", &name)))
        return false;

    if (virStoragePoolRefresh(pool, 0) == 0) {
        vshPrint(ctl, _("Pool %s refreshed\n"), name);
    } else {
        vshError(ctl, _("Failed to refresh pool %s"), name);
        ret = false;
    }
    virStoragePoolFree(pool);

    return ret;
}

/*
 * "pool-dumpxml" command
 */
static const vshCmdInfo info_pool_dumpxml[] = {
    {"help", N_("pool information in XML")},
    {"desc", N_("Output the pool information as an XML dump to stdout.")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_dumpxml[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, N_("pool name or uuid")},
    {"inactive", VSH_OT_BOOL, 0, N_("show inactive defined XML")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolDumpXML(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolPtr pool;
    bool ret = true;
    bool inactive = vshCommandOptBool(cmd, "inactive");
    unsigned int flags = 0;
    char *dump;

    if (inactive)
        flags |= VIR_STORAGE_XML_INACTIVE;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", NULL)))
        return false;

    dump = virStoragePoolGetXMLDesc(pool, flags);
    if (dump != NULL) {
        vshPrint(ctl, "%s", dump);
        VIR_FREE(dump);
    } else {
        ret = false;
    }

    virStoragePoolFree(pool);
    return ret;
}

static int
vshStoragePoolSorter(const void *a, const void *b)
{
    virStoragePoolPtr *pa = (virStoragePoolPtr *) a;
    virStoragePoolPtr *pb = (virStoragePoolPtr *) b;

    if (*pa && !*pb)
        return -1;

    if (!*pa)
        return *pb != NULL;

    return vshStrcasecmp(virStoragePoolGetName(*pa),
                         virStoragePoolGetName(*pb));
}

struct vshStoragePoolList {
    virStoragePoolPtr *pools;
    size_t npools;
};
typedef struct vshStoragePoolList *vshStoragePoolListPtr;

static void
vshStoragePoolListFree(vshStoragePoolListPtr list)
{
    int i;

    if (list && list->pools) {
        for (i = 0; i < list->npools; i++) {
            if (list->pools[i])
                virStoragePoolFree(list->pools[i]);
        }
        VIR_FREE(list->pools);
    }
    VIR_FREE(list);
}

static vshStoragePoolListPtr
vshStoragePoolListCollect(vshControl *ctl,
                          unsigned int flags)
{
    vshStoragePoolListPtr list = vshMalloc(ctl, sizeof(*list));
    int i;
    int ret;
    char **names = NULL;
    virStoragePoolPtr pool;
    bool success = false;
    size_t deleted = 0;
    int persistent;
    int autostart;
    int nActivePools = 0;
    int nInactivePools = 0;
    int nAllPools = 0;

    /* try the list with flags support (0.10.2 and later) */
    if ((ret = virConnectListAllStoragePools(ctl->conn,
                                             &list->pools,
                                             flags)) >= 0) {
        list->npools = ret;
        goto finished;
    }

    /* check if the command is actually supported */
    if (last_error && last_error->code == VIR_ERR_NO_SUPPORT)
        goto fallback;

    if (last_error && last_error->code ==  VIR_ERR_INVALID_ARG) {
        /* try the new API again but mask non-guaranteed flags */
        unsigned int newflags = flags & (VIR_CONNECT_LIST_STORAGE_POOLS_ACTIVE |
                                         VIR_CONNECT_LIST_STORAGE_POOLS_INACTIVE);
        vshResetLibvirtError();
        if ((ret = virConnectListAllStoragePools(ctl->conn, &list->pools,
                                                 newflags)) >= 0) {
            list->npools = ret;
            goto filter;
        }
    }

    /* there was an error during the first or second call */
    vshError(ctl, "%s", _("Failed to list pools"));
    goto cleanup;


fallback:
    /* fall back to old method (0.10.1 and older) */
    vshResetLibvirtError();

    /* There is no way to get the pool type */
    if (VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_FILTERS_POOL_TYPE)) {
        vshError(ctl, "%s", _("Filtering using --type is not supported "
                              "by this libvirt"));
        goto cleanup;
    }

    /* Get the number of active pools */
    if (!VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_FILTERS_ACTIVE) ||
        VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_ACTIVE)) {
        if ((nActivePools = virConnectNumOfStoragePools(ctl->conn)) < 0) {
            vshError(ctl, "%s", _("Failed to get the number of active pools "));
            goto cleanup;
        }
    }

    /* Get the number of inactive pools */
    if (!VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_FILTERS_ACTIVE) ||
        VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_INACTIVE)) {
        if ((nInactivePools = virConnectNumOfDefinedStoragePools(ctl->conn)) < 0) {
            vshError(ctl, "%s", _("Failed to get the number of inactive pools"));
            goto cleanup;
        }
    }

    nAllPools = nActivePools + nInactivePools;

    if (nAllPools == 0)
        return list;

    names = vshMalloc(ctl, sizeof(char *) * nAllPools);

    /* Retrieve a list of active storage pool names */
    if (!VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_FILTERS_ACTIVE) ||
        VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_ACTIVE)) {
        if (virConnectListStoragePools(ctl->conn,
                                       names, nActivePools) < 0) {
            vshError(ctl, "%s", _("Failed to list active pools"));
            goto cleanup;
        }
    }

    /* Add the inactive storage pools to the end of the name list */
    if (!VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_FILTERS_ACTIVE) ||
        VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_ACTIVE)) {
        if (virConnectListDefinedStoragePools(ctl->conn,
                                              &names[nActivePools],
                                              nInactivePools) < 0) {
            vshError(ctl, "%s", _("Failed to list inactive pools"));
            goto cleanup;
        }
    }

    list->pools = vshMalloc(ctl, sizeof(virStoragePoolPtr) * (nAllPools));
    list->npools = 0;

    /* get active pools */
    for (i = 0; i < nActivePools; i++) {
        if (!(pool = virStoragePoolLookupByName(ctl->conn, names[i])))
            continue;
        list->pools[list->npools++] = pool;
    }

    /* get inactive pools */
    for (i = 0; i < nInactivePools; i++) {
        if (!(pool = virStoragePoolLookupByName(ctl->conn, names[i])))
            continue;
        list->pools[list->npools++] = pool;
    }

    /* truncate pools that weren't found */
    deleted = nAllPools - list->npools;

filter:
    /* filter list the list if the list was acquired by fallback means */
    for (i = 0; i < list->npools; i++) {
        pool = list->pools[i];

        /* persistence filter */
        if (VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_FILTERS_PERSISTENT)) {
            if ((persistent = virStoragePoolIsPersistent(pool)) < 0) {
                vshError(ctl, "%s", _("Failed to get pool persistence info"));
                goto cleanup;
            }

            if (!((VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_PERSISTENT) && persistent) ||
                  (VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_TRANSIENT) && !persistent)))
                goto remove_entry;
        }

        /* autostart filter */
        if (VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_FILTERS_AUTOSTART)) {
            if (virStoragePoolGetAutostart(pool, &autostart) < 0) {
                vshError(ctl, "%s", _("Failed to get pool autostart state"));
                goto cleanup;
            }

            if (!((VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_AUTOSTART) && autostart) ||
                  (VSH_MATCH(VIR_CONNECT_LIST_STORAGE_POOLS_NO_AUTOSTART) && !autostart)))
                goto remove_entry;
        }

        /* the pool matched all filters, it may stay */
        continue;

remove_entry:
        /* the pool has to be removed as it failed one of the filters */
        virStoragePoolFree(list->pools[i]);
        list->pools[i] = NULL;
        deleted++;
    }

finished:
    /* sort the list */
    if (list->pools && list->npools)
        qsort(list->pools, list->npools,
              sizeof(*list->pools), vshStoragePoolSorter);

    /* truncate the list if filter simulation deleted entries */
    if (deleted)
        VIR_SHRINK_N(list->pools, list->npools, deleted);

    success = true;

cleanup:
    for (i = 0; i < nAllPools; i++)
        VIR_FREE(names[i]);

    if (!success) {
        vshStoragePoolListFree(list);
        list = NULL;
    }

    VIR_FREE(names);
    return list;
}

/*
 * "pool-list" command
 */
static const vshCmdInfo info_pool_list[] = {
    {"help", N_("list pools")},
    {"desc", N_("Returns list of pools.")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_list[] = {
    {"inactive", VSH_OT_BOOL, 0, N_("list inactive pools")},
    {"all", VSH_OT_BOOL, 0, N_("list inactive & active pools")},
    {"transient", VSH_OT_BOOL, 0, N_("list transient pools")},
    {"persistent", VSH_OT_BOOL, 0, N_("list persistent pools")},
    {"autostart", VSH_OT_BOOL, 0, N_("list pools with autostart enabled")},
    {"no-autostart", VSH_OT_BOOL, 0, N_("list pools with autostart disabled")},
    {"type", VSH_OT_STRING, 0, N_("only list pool of specified type(s) (if supported)")},
    {"details", VSH_OT_BOOL, 0, N_("display extended details for pools")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolList(vshControl *ctl, const vshCmd *cmd ATTRIBUTE_UNUSED)
{
    virStoragePoolInfo info;
    int i, ret;
    bool functionReturn = false;
    size_t stringLength = 0, nameStrLength = 0;
    size_t autostartStrLength = 0, persistStrLength = 0;
    size_t stateStrLength = 0, capStrLength = 0;
    size_t allocStrLength = 0, availStrLength = 0;
    struct poolInfoText {
        char *state;
        char *autostart;
        char *persistent;
        char *capacity;
        char *allocation;
        char *available;
    };
    struct poolInfoText *poolInfoTexts = NULL;
    unsigned int flags = VIR_CONNECT_LIST_STORAGE_POOLS_ACTIVE;
    vshStoragePoolListPtr list = NULL;
    const char *type = NULL;
    bool details = vshCommandOptBool(cmd, "details");
    bool inactive, all;

    inactive = vshCommandOptBool(cmd, "inactive");
    all = vshCommandOptBool(cmd, "all");

    if (inactive)
        flags = VIR_CONNECT_LIST_STORAGE_POOLS_INACTIVE;

    if (all)
        flags = VIR_CONNECT_LIST_STORAGE_POOLS_ACTIVE |
                VIR_CONNECT_LIST_STORAGE_POOLS_INACTIVE;

    if (vshCommandOptBool(cmd, "autostart"))
        flags |= VIR_CONNECT_LIST_STORAGE_POOLS_AUTOSTART;

    if (vshCommandOptBool(cmd, "no-autostart"))
        flags |= VIR_CONNECT_LIST_STORAGE_POOLS_NO_AUTOSTART;

    if (vshCommandOptBool(cmd, "persistent"))
        flags |= VIR_CONNECT_LIST_STORAGE_POOLS_PERSISTENT;

    if (vshCommandOptBool(cmd, "transient"))
        flags |= VIR_CONNECT_LIST_STORAGE_POOLS_TRANSIENT;

    if (vshCommandOptString(cmd, "type", &type) < 0) {
        vshError(ctl, "%s", _("Invalid argument for 'type'"));
        return false;
    }

    if (type) {
        int poolType = -1;
        char **poolTypes = NULL;
        int npoolTypes = 0;

        npoolTypes = vshStringToArray(type, &poolTypes);

        for (i = 0; i < npoolTypes; i++) {
            if ((poolType = virStoragePoolTypeFromString(poolTypes[i])) < 0) {
                vshError(ctl, "%s", _("Invalid pool type"));
                VIR_FREE(poolTypes);
                return false;
            }

            switch (poolType) {
            case VIR_STORAGE_POOL_DIR:
                flags |= VIR_CONNECT_LIST_STORAGE_POOLS_DIR;
                break;
            case VIR_STORAGE_POOL_FS:
                flags |= VIR_CONNECT_LIST_STORAGE_POOLS_FS;
                break;
            case VIR_STORAGE_POOL_NETFS:
                flags |= VIR_CONNECT_LIST_STORAGE_POOLS_NETFS;
                break;
            case VIR_STORAGE_POOL_LOGICAL:
                flags |= VIR_CONNECT_LIST_STORAGE_POOLS_LOGICAL;
                break;
            case VIR_STORAGE_POOL_DISK:
                flags |= VIR_CONNECT_LIST_STORAGE_POOLS_DISK;
                break;
            case VIR_STORAGE_POOL_ISCSI:
                flags |= VIR_CONNECT_LIST_STORAGE_POOLS_ISCSI;
                break;
            case VIR_STORAGE_POOL_SCSI:
                flags |= VIR_CONNECT_LIST_STORAGE_POOLS_SCSI;
                break;
            case VIR_STORAGE_POOL_MPATH:
                flags |= VIR_CONNECT_LIST_STORAGE_POOLS_MPATH;
                break;
            case VIR_STORAGE_POOL_RBD:
                flags |= VIR_CONNECT_LIST_STORAGE_POOLS_RBD;
                break;
            default:
                break;
            }
        }
        if (poolTypes) {
            VIR_FREE(*poolTypes);
            VIR_FREE(poolTypes);
        }
    }

    if (!(list = vshStoragePoolListCollect(ctl, flags)))
        goto cleanup;

    poolInfoTexts = vshCalloc(ctl, list->npools, sizeof(*poolInfoTexts));

    /* Collect the storage pool information for display */
    for (i = 0; i < list->npools; i++) {
        int autostart = 0, persistent = 0;

        /* Retrieve the autostart status of the pool */
        if (virStoragePoolGetAutostart(list->pools[i], &autostart) < 0)
            poolInfoTexts[i].autostart = vshStrdup(ctl, _("no autostart"));
        else
            poolInfoTexts[i].autostart = vshStrdup(ctl, autostart ?
                                                    _("yes") : _("no"));

        /* Retrieve the persistence status of the pool */
        if (details) {
            persistent = virStoragePoolIsPersistent(list->pools[i]);
            vshDebug(ctl, VSH_ERR_DEBUG, "Persistent flag value: %d\n",
                     persistent);
            if (persistent < 0)
                poolInfoTexts[i].persistent = vshStrdup(ctl, _("unknown"));
            else
                poolInfoTexts[i].persistent = vshStrdup(ctl, persistent ?
                                                         _("yes") : _("no"));

            /* Keep the length of persistent string if longest so far */
            stringLength = strlen(poolInfoTexts[i].persistent);
            if (stringLength > persistStrLength)
                persistStrLength = stringLength;
        }

        /* Collect further extended information about the pool */
        if (virStoragePoolGetInfo(list->pools[i], &info) != 0) {
            /* Something went wrong retrieving pool info, cope with it */
            vshError(ctl, "%s", _("Could not retrieve pool information"));
            poolInfoTexts[i].state = vshStrdup(ctl, _("unknown"));
            if (details) {
                poolInfoTexts[i].capacity = vshStrdup(ctl, _("unknown"));
                poolInfoTexts[i].allocation = vshStrdup(ctl, _("unknown"));
                poolInfoTexts[i].available = vshStrdup(ctl, _("unknown"));
            }
        } else {
            /* Decide which state string to display */
            if (details) {
                /* --details option was specified, we're using detailed state
                 * strings */
                switch (info.state) {
                case VIR_STORAGE_POOL_INACTIVE:
                    poolInfoTexts[i].state = vshStrdup(ctl, _("inactive"));
                    break;
                case VIR_STORAGE_POOL_BUILDING:
                    poolInfoTexts[i].state = vshStrdup(ctl, _("building"));
                    break;
                case VIR_STORAGE_POOL_RUNNING:
                    poolInfoTexts[i].state = vshStrdup(ctl, _("running"));
                    break;
                case VIR_STORAGE_POOL_DEGRADED:
                    poolInfoTexts[i].state = vshStrdup(ctl, _("degraded"));
                    break;
                case VIR_STORAGE_POOL_INACCESSIBLE:
                    poolInfoTexts[i].state = vshStrdup(ctl, _("inaccessible"));
                    break;
                }

                /* Create the pool size related strings */
                if (info.state == VIR_STORAGE_POOL_RUNNING ||
                    info.state == VIR_STORAGE_POOL_DEGRADED) {
                    double val;
                    const char *unit;

                    /* Create the capacity output string */
                    val = vshPrettyCapacity(info.capacity, &unit);
                    ret = virAsprintf(&poolInfoTexts[i].capacity,
                                      "%.2lf %s", val, unit);
                    if (ret < 0)
                        /* An error occurred creating the string, return */
                        goto asprintf_failure;

                    /* Create the allocation output string */
                    val = vshPrettyCapacity(info.allocation, &unit);
                    ret = virAsprintf(&poolInfoTexts[i].allocation,
                                      "%.2lf %s", val, unit);
                    if (ret < 0)
                        /* An error occurred creating the string, return */
                        goto asprintf_failure;

                    /* Create the available space output string */
                    val = vshPrettyCapacity(info.available, &unit);
                    ret = virAsprintf(&poolInfoTexts[i].available,
                                      "%.2lf %s", val, unit);
                    if (ret < 0)
                        /* An error occurred creating the string, return */
                        goto asprintf_failure;
                } else {
                    /* Capacity related information isn't available */
                    poolInfoTexts[i].capacity = vshStrdup(ctl, _("-"));
                    poolInfoTexts[i].allocation = vshStrdup(ctl, _("-"));
                    poolInfoTexts[i].available = vshStrdup(ctl, _("-"));
                }

                /* Keep the length of capacity string if longest so far */
                stringLength = strlen(poolInfoTexts[i].capacity);
                if (stringLength > capStrLength)
                    capStrLength = stringLength;

                /* Keep the length of allocation string if longest so far */
                stringLength = strlen(poolInfoTexts[i].allocation);
                if (stringLength > allocStrLength)
                    allocStrLength = stringLength;

                /* Keep the length of available string if longest so far */
                stringLength = strlen(poolInfoTexts[i].available);
                if (stringLength > availStrLength)
                    availStrLength = stringLength;
            } else {
                /* --details option was not specified, only active/inactive
                 * state strings are used */
                if (virStoragePoolIsActive(list->pools[i]))
                    poolInfoTexts[i].state = vshStrdup(ctl, _("active"));
                else
                    poolInfoTexts[i].state = vshStrdup(ctl, _("inactive"));
           }
        }

        /* Keep the length of name string if longest so far */
        stringLength = strlen(virStoragePoolGetName(list->pools[i]));
        if (stringLength > nameStrLength)
            nameStrLength = stringLength;

        /* Keep the length of state string if longest so far */
        stringLength = strlen(poolInfoTexts[i].state);
        if (stringLength > stateStrLength)
            stateStrLength = stringLength;

        /* Keep the length of autostart string if longest so far */
        stringLength = strlen(poolInfoTexts[i].autostart);
        if (stringLength > autostartStrLength)
            autostartStrLength = stringLength;
    }

    /* If the --details option wasn't selected, we output the pool
     * info using the fixed string format from previous versions to
     * maintain backward compatibility.
     */

    /* Output basic info then return if --details option not selected */
    if (!details) {
        /* Output old style header */
        vshPrintExtra(ctl, "%-20s %-10s %-10s\n", _("Name"), _("State"),
                      _("Autostart"));
        vshPrintExtra(ctl, "-----------------------------------------\n");

        /* Output old style pool info */
        for (i = 0; i < list->npools; i++) {
            const char *name = virStoragePoolGetName(list->pools[i]);
            vshPrint(ctl, "%-20s %-10s %-10s\n",
                 name,
                 poolInfoTexts[i].state,
                 poolInfoTexts[i].autostart);
        }

        /* Cleanup and return */
        functionReturn = true;
        goto cleanup;
    }

    /* We only get here if the --details option was selected. */

    /* Use the length of name header string if it's longest */
    stringLength = strlen(_("Name"));
    if (stringLength > nameStrLength)
        nameStrLength = stringLength;

    /* Use the length of state header string if it's longest */
    stringLength = strlen(_("State"));
    if (stringLength > stateStrLength)
        stateStrLength = stringLength;

    /* Use the length of autostart header string if it's longest */
    stringLength = strlen(_("Autostart"));
    if (stringLength > autostartStrLength)
        autostartStrLength = stringLength;

    /* Use the length of persistent header string if it's longest */
    stringLength = strlen(_("Persistent"));
    if (stringLength > persistStrLength)
        persistStrLength = stringLength;

    /* Use the length of capacity header string if it's longest */
    stringLength = strlen(_("Capacity"));
    if (stringLength > capStrLength)
        capStrLength = stringLength;

    /* Use the length of allocation header string if it's longest */
    stringLength = strlen(_("Allocation"));
    if (stringLength > allocStrLength)
        allocStrLength = stringLength;

    /* Use the length of available header string if it's longest */
    stringLength = strlen(_("Available"));
    if (stringLength > availStrLength)
        availStrLength = stringLength;

    /* Display the string lengths for debugging. */
    vshDebug(ctl, VSH_ERR_DEBUG, "Longest name string = %lu chars\n",
             (unsigned long) nameStrLength);
    vshDebug(ctl, VSH_ERR_DEBUG, "Longest state string = %lu chars\n",
             (unsigned long) stateStrLength);
    vshDebug(ctl, VSH_ERR_DEBUG, "Longest autostart string = %lu chars\n",
             (unsigned long) autostartStrLength);
    vshDebug(ctl, VSH_ERR_DEBUG, "Longest persistent string = %lu chars\n",
             (unsigned long) persistStrLength);
    vshDebug(ctl, VSH_ERR_DEBUG, "Longest capacity string = %lu chars\n",
             (unsigned long) capStrLength);
    vshDebug(ctl, VSH_ERR_DEBUG, "Longest allocation string = %lu chars\n",
             (unsigned long) allocStrLength);
    vshDebug(ctl, VSH_ERR_DEBUG, "Longest available string = %lu chars\n",
             (unsigned long) availStrLength);

    /* Create the output template.  Each column is sized according to
     * the longest string.
     */
    char *outputStr;
    ret = virAsprintf(&outputStr,
              "%%-%lus  %%-%lus  %%-%lus  %%-%lus  %%%lus  %%%lus  %%%lus\n",
              (unsigned long) nameStrLength,
              (unsigned long) stateStrLength,
              (unsigned long) autostartStrLength,
              (unsigned long) persistStrLength,
              (unsigned long) capStrLength,
              (unsigned long) allocStrLength,
              (unsigned long) availStrLength);
    if (ret < 0) {
        /* An error occurred creating the string, return */
        goto asprintf_failure;
    }

    /* Display the header */
    vshPrint(ctl, outputStr, _("Name"), _("State"), _("Autostart"),
             _("Persistent"), _("Capacity"), _("Allocation"), _("Available"));
    for (i = nameStrLength + stateStrLength + autostartStrLength
                           + persistStrLength + capStrLength
                           + allocStrLength + availStrLength
                           + 12; i > 0; i--)
        vshPrintExtra(ctl, "-");
    vshPrintExtra(ctl, "\n");

    /* Display the pool info rows */
    for (i = 0; i < list->npools; i++) {
        vshPrint(ctl, outputStr,
                 virStoragePoolGetName(list->pools[i]),
                 poolInfoTexts[i].state,
                 poolInfoTexts[i].autostart,
                 poolInfoTexts[i].persistent,
                 poolInfoTexts[i].capacity,
                 poolInfoTexts[i].allocation,
                 poolInfoTexts[i].available);
    }

    /* Cleanup and return */
    functionReturn = true;
    goto cleanup;

asprintf_failure:
    /* Display an appropriate error message then cleanup and return */
    switch (errno) {
    case ENOMEM:
        /* Couldn't allocate memory */
        vshError(ctl, "%s", _("Out of memory"));
        break;
    default:
        /* Some other error */
        vshError(ctl, _("virAsprintf failed (errno %d)"), errno);
    }
    functionReturn = false;

cleanup:
    if (list && list->npools) {
        for (i = 0; i < list->npools; i++) {
            VIR_FREE(poolInfoTexts[i].state);
            VIR_FREE(poolInfoTexts[i].autostart);
            VIR_FREE(poolInfoTexts[i].persistent);
            VIR_FREE(poolInfoTexts[i].capacity);
            VIR_FREE(poolInfoTexts[i].allocation);
            VIR_FREE(poolInfoTexts[i].available);
        }
    }
    VIR_FREE(poolInfoTexts);

    vshStoragePoolListFree(list);
    return functionReturn;
}

/*
 * "find-storage-pool-sources-as" command
 */
static const vshCmdInfo info_find_storage_pool_sources_as[] = {
    {"help", N_("find potential storage pool sources")},
    {"desc", N_("Returns XML <sources> document.")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_find_storage_pool_sources_as[] = {
    {"type", VSH_OT_DATA, VSH_OFLAG_REQ,
     N_("type of storage pool sources to find")},
    {"host", VSH_OT_DATA, VSH_OFLAG_NONE, N_("optional host to query")},
    {"port", VSH_OT_DATA, VSH_OFLAG_NONE, N_("optional port to query")},
    {"initiator", VSH_OT_DATA, VSH_OFLAG_NONE, N_("optional initiator IQN to use for query")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolDiscoverSourcesAs(vshControl * ctl, const vshCmd * cmd ATTRIBUTE_UNUSED)
{
    const char *type = NULL, *host = NULL;
    char *srcSpec = NULL;
    char *srcList;
    const char *initiator = NULL;

    if (vshCommandOptString(cmd, "type", &type) <= 0 ||
        vshCommandOptString(cmd, "host", &host) < 0 ||
        vshCommandOptString(cmd, "initiator", &initiator) < 0) {
        vshError(ctl,"%s", _("missing argument"));
        return false;
    }

    if (host) {
        const char *port = NULL;
        virBuffer buf = VIR_BUFFER_INITIALIZER;

        if (vshCommandOptString(cmd, "port", &port) < 0) {
            vshError(ctl, "%s", _("missing argument"));
            virBufferFreeAndReset(&buf);
            return false;
        }
        virBufferAddLit(&buf, "<source>\n");
        virBufferAsprintf(&buf, "  <host name='%s'", host);
        if (port)
            virBufferAsprintf(&buf, " port='%s'", port);
        virBufferAddLit(&buf, "/>\n");
        if (initiator) {
            virBufferAddLit(&buf, "  <initiator>\n");
            virBufferAsprintf(&buf, "    <iqn name='%s'/>\n", initiator);
            virBufferAddLit(&buf, "  </initiator>\n");
        }
        virBufferAddLit(&buf, "</source>\n");
        if (virBufferError(&buf)) {
            vshError(ctl, "%s", _("Out of memory"));
            return false;
        }
        srcSpec = virBufferContentAndReset(&buf);
    }

    srcList = virConnectFindStoragePoolSources(ctl->conn, type, srcSpec, 0);
    VIR_FREE(srcSpec);
    if (srcList == NULL) {
        vshError(ctl, _("Failed to find any %s pool sources"), type);
        return false;
    }
    vshPrint(ctl, "%s", srcList);
    VIR_FREE(srcList);

    return true;
}

/*
 * "find-storage-pool-sources" command
 */
static const vshCmdInfo info_find_storage_pool_sources[] = {
    {"help", N_("discover potential storage pool sources")},
    {"desc", N_("Returns XML <sources> document.")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_find_storage_pool_sources[] = {
    {"type", VSH_OT_DATA, VSH_OFLAG_REQ,
     N_("type of storage pool sources to discover")},
    {"srcSpec", VSH_OT_DATA, VSH_OFLAG_NONE,
     N_("optional file of source xml to query for pools")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolDiscoverSources(vshControl * ctl, const vshCmd * cmd ATTRIBUTE_UNUSED)
{
    const char *type = NULL, *srcSpecFile = NULL;
    char *srcSpec = NULL, *srcList;

    if (vshCommandOptString(cmd, "type", &type) <= 0)
        return false;

    if (vshCommandOptString(cmd, "srcSpec", &srcSpecFile) < 0) {
        vshError(ctl, "%s", _("missing option"));
        return false;
    }

    if (srcSpecFile && virFileReadAll(srcSpecFile, VSH_MAX_XML_FILE,
                                      &srcSpec) < 0)
        return false;

    srcList = virConnectFindStoragePoolSources(ctl->conn, type, srcSpec, 0);
    VIR_FREE(srcSpec);
    if (srcList == NULL) {
        vshError(ctl, _("Failed to find any %s pool sources"), type);
        return false;
    }
    vshPrint(ctl, "%s", srcList);
    VIR_FREE(srcList);

    return true;
}

/*
 * "pool-info" command
 */
static const vshCmdInfo info_pool_info[] = {
    {"help", N_("storage pool information")},
    {"desc", N_("Returns basic information about the storage pool.")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_info[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, N_("pool name or uuid")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolInfo(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolInfo info;
    virStoragePoolPtr pool;
    int autostart = 0;
    int persistent = 0;
    bool ret = true;
    char uuid[VIR_UUID_STRING_BUFLEN];

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", NULL)))
        return false;

    vshPrint(ctl, "%-15s %s\n", _("Name:"), virStoragePoolGetName(pool));

    if (virStoragePoolGetUUIDString(pool, &uuid[0])==0)
        vshPrint(ctl, "%-15s %s\n", _("UUID:"), uuid);

    if (virStoragePoolGetInfo(pool, &info) == 0) {
        double val;
        const char *unit;
        switch (info.state) {
        case VIR_STORAGE_POOL_INACTIVE:
            vshPrint(ctl, "%-15s %s\n", _("State:"),
                     _("inactive"));
            break;
        case VIR_STORAGE_POOL_BUILDING:
            vshPrint(ctl, "%-15s %s\n", _("State:"),
                     _("building"));
            break;
        case VIR_STORAGE_POOL_RUNNING:
            vshPrint(ctl, "%-15s %s\n", _("State:"),
                     _("running"));
            break;
        case VIR_STORAGE_POOL_DEGRADED:
            vshPrint(ctl, "%-15s %s\n", _("State:"),
                     _("degraded"));
            break;
        case VIR_STORAGE_POOL_INACCESSIBLE:
            vshPrint(ctl, "%-15s %s\n", _("State:"),
                     _("inaccessible"));
            break;
        }

        /* Check and display whether the pool is persistent or not */
        persistent = virStoragePoolIsPersistent(pool);
        vshDebug(ctl, VSH_ERR_DEBUG, "Pool persistent flag value: %d\n",
                 persistent);
        if (persistent < 0)
            vshPrint(ctl, "%-15s %s\n", _("Persistent:"),  _("unknown"));
        else
            vshPrint(ctl, "%-15s %s\n", _("Persistent:"), persistent ? _("yes") : _("no"));

        /* Check and display whether the pool is autostarted or not */
        if (virStoragePoolGetAutostart(pool, &autostart) < 0)
            vshPrint(ctl, "%-15s %s\n", _("Autostart:"), _("no autostart"));
        else
            vshPrint(ctl, "%-15s %s\n", _("Autostart:"), autostart ? _("yes") : _("no"));

        if (info.state == VIR_STORAGE_POOL_RUNNING ||
            info.state == VIR_STORAGE_POOL_DEGRADED) {
            val = vshPrettyCapacity(info.capacity, &unit);
            vshPrint(ctl, "%-15s %2.2lf %s\n", _("Capacity:"), val, unit);

            val = vshPrettyCapacity(info.allocation, &unit);
            vshPrint(ctl, "%-15s %2.2lf %s\n", _("Allocation:"), val, unit);

            val = vshPrettyCapacity(info.available, &unit);
            vshPrint(ctl, "%-15s %2.2lf %s\n", _("Available:"), val, unit);
        }
    } else {
        ret = false;
    }

    virStoragePoolFree(pool);
    return ret;
}

/*
 * "pool-name" command
 */
static const vshCmdInfo info_pool_name[] = {
    {"help", N_("convert a pool UUID to pool name")},
    {"desc", ""},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_name[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, N_("pool uuid")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolName(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolPtr pool;

    if (!(pool = vshCommandOptPoolBy(ctl, cmd, "pool", NULL,
                                           VSH_BYUUID)))
        return false;

    vshPrint(ctl, "%s\n", virStoragePoolGetName(pool));
    virStoragePoolFree(pool);
    return true;
}

/*
 * "pool-start" command
 */
static const vshCmdInfo info_pool_start[] = {
    {"help", N_("start a (previously defined) inactive pool")},
    {"desc", N_("Start a pool.")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_start[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, N_("name or uuid of the inactive pool")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolStart(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolPtr pool;
    bool ret = true;
    const char *name = NULL;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", &name)))
         return false;

    if (virStoragePoolCreate(pool, 0) == 0) {
        vshPrint(ctl, _("Pool %s started\n"), name);
    } else {
        vshError(ctl, _("Failed to start pool %s"), name);
        ret = false;
    }

    virStoragePoolFree(pool);
    return ret;
}

/*
 * "pool-undefine" command
 */
static const vshCmdInfo info_pool_undefine[] = {
    {"help", N_("undefine an inactive pool")},
    {"desc", N_("Undefine the configuration for an inactive pool.")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_undefine[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, N_("pool name or uuid")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolUndefine(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolPtr pool;
    bool ret = true;
    const char *name;

    if (!(pool = vshCommandOptPool(ctl, cmd, "pool", &name)))
        return false;

    if (virStoragePoolUndefine(pool) == 0) {
        vshPrint(ctl, _("Pool %s has been undefined\n"), name);
    } else {
        vshError(ctl, _("Failed to undefine pool %s"), name);
        ret = false;
    }

    virStoragePoolFree(pool);
    return ret;
}

/*
 * "pool-uuid" command
 */
static const vshCmdInfo info_pool_uuid[] = {
    {"help", N_("convert a pool name to pool UUID")},
    {"desc", ""},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_uuid[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, N_("pool name")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolUuid(vshControl *ctl, const vshCmd *cmd)
{
    virStoragePoolPtr pool;
    char uuid[VIR_UUID_STRING_BUFLEN];

    if (!(pool = vshCommandOptPoolBy(ctl, cmd, "pool", NULL,
                                           VSH_BYNAME)))
        return false;

    if (virStoragePoolGetUUIDString(pool, uuid) != -1)
        vshPrint(ctl, "%s\n", uuid);
    else
        vshError(ctl, "%s", _("failed to get pool UUID"));

    virStoragePoolFree(pool);
    return true;
}

/*
 * "pool-edit" command
 */
static const vshCmdInfo info_pool_edit[] = {
    {"help", N_("edit XML configuration for a storage pool")},
    {"desc", N_("Edit the XML configuration for a storage pool.")},
    {NULL, NULL}
};

static const vshCmdOptDef opts_pool_edit[] = {
    {"pool", VSH_OT_DATA, VSH_OFLAG_REQ, N_("pool name or uuid")},
    {NULL, 0, 0, NULL}
};

static bool
cmdPoolEdit(vshControl *ctl, const vshCmd *cmd)
{
    bool ret = false;
    virStoragePoolPtr pool = NULL;
    virStoragePoolPtr pool_edited = NULL;
    unsigned int flags = VIR_STORAGE_XML_INACTIVE;
    char *tmp_desc = NULL;

    pool = vshCommandOptPool(ctl, cmd, "pool", NULL);
    if (pool == NULL)
        goto cleanup;

    /* Some old daemons don't support _INACTIVE flag */
    if (!(tmp_desc = virStoragePoolGetXMLDesc(pool, flags))) {
        if (last_error->code == VIR_ERR_INVALID_ARG) {
            flags &= ~VIR_STORAGE_XML_INACTIVE;
            vshResetLibvirtError();
        } else {
            goto cleanup;
        }
    } else {
        VIR_FREE(tmp_desc);
    }

#define EDIT_GET_XML virStoragePoolGetXMLDesc(pool, flags)
#define EDIT_NOT_CHANGED \
    vshPrint(ctl, _("Pool %s XML configuration not changed.\n"),    \
             virStoragePoolGetName(pool));                          \
    ret = true; goto edit_cleanup;
#define EDIT_DEFINE \
    (pool_edited = virStoragePoolDefineXML(ctl->conn, doc_edited, 0))
#define EDIT_FREE \
    if (pool_edited)    \
        virStoragePoolFree(pool_edited);
#include "virsh-edit.c"

    vshPrint(ctl, _("Pool %s XML configuration edited.\n"),
             virStoragePoolGetName(pool_edited));

    ret = true;

 cleanup:
    if (pool)
        virStoragePoolFree(pool);
    if (pool_edited)
        virStoragePoolFree(pool_edited);

    return ret;
}

const vshCmdDef storagePoolCmds[] = {
    {"find-storage-pool-sources-as", cmdPoolDiscoverSourcesAs,
     opts_find_storage_pool_sources_as, info_find_storage_pool_sources_as, 0},
    {"find-storage-pool-sources", cmdPoolDiscoverSources,
     opts_find_storage_pool_sources, info_find_storage_pool_sources, 0},
    {"pool-autostart", cmdPoolAutostart, opts_pool_autostart,
     info_pool_autostart, 0},
    {"pool-build", cmdPoolBuild, opts_pool_build, info_pool_build, 0},
    {"pool-create-as", cmdPoolCreateAs, opts_pool_X_as, info_pool_create_as, 0},
    {"pool-create", cmdPoolCreate, opts_pool_create, info_pool_create, 0},
    {"pool-define-as", cmdPoolDefineAs, opts_pool_X_as, info_pool_define_as, 0},
    {"pool-define", cmdPoolDefine, opts_pool_define, info_pool_define, 0},
    {"pool-delete", cmdPoolDelete, opts_pool_delete, info_pool_delete, 0},
    {"pool-destroy", cmdPoolDestroy, opts_pool_destroy, info_pool_destroy, 0},
    {"pool-dumpxml", cmdPoolDumpXML, opts_pool_dumpxml, info_pool_dumpxml, 0},
    {"pool-edit", cmdPoolEdit, opts_pool_edit, info_pool_edit, 0},
    {"pool-info", cmdPoolInfo, opts_pool_info, info_pool_info, 0},
    {"pool-list", cmdPoolList, opts_pool_list, info_pool_list, 0},
    {"pool-name", cmdPoolName, opts_pool_name, info_pool_name, 0},
    {"pool-refresh", cmdPoolRefresh, opts_pool_refresh, info_pool_refresh, 0},
    {"pool-start", cmdPoolStart, opts_pool_start, info_pool_start, 0},
    {"pool-undefine", cmdPoolUndefine, opts_pool_undefine,
     info_pool_undefine, 0},
    {"pool-uuid", cmdPoolUuid, opts_pool_uuid, info_pool_uuid, 0},
    {NULL, NULL, NULL, NULL, 0}
};
