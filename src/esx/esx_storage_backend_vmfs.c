
/*
 * esx_storage_backend_vmfs.c: ESX storage driver backend for
 *                             managing VMFS datastores
 *
 * Copyright (C) 2010-2011 Red Hat, Inc.
 * Copyright (C) 2010-2012 Matthias Bolte <matthias.bolte@googlemail.com>
 * Copyright (C) 2012 Ata E Husain Bohra <ata.husain@hotmail.com>
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
 */

#include <config.h>

#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include "internal.h"
#include "md5.h"
#include "util.h"
#include "memory.h"
#include "logging.h"
#include "uuid.h"
#include "storage_conf.h"
#include "storage_file.h"
#include "esx_storage_backend_vmfs.h"
#include "esx_private.h"
#include "esx_vi.h"
#include "esx_vi_methods.h"
#include "esx_util.h"

#define VIR_FROM_THIS VIR_FROM_ESX

/*
 * The UUID of a storage pool is the MD5 sum of it's mount path. Therefore,
 * verify that UUID and MD5 sum match in size, because we rely on that.
 */
verify(MD5_DIGEST_SIZE == VIR_UUID_BUFLEN);



static int
esxLookupVMFSStoragePoolType(esxVI_Context *ctx, const char *poolName,
                             int *poolType)
{
    int result = -1;
    esxVI_String *propertyNameList = NULL;
    esxVI_ObjectContent *datastore = NULL;
    esxVI_DynamicProperty *dynamicProperty = NULL;
    esxVI_DatastoreInfo *datastoreInfo = NULL;

    if (esxVI_String_AppendValueToList(&propertyNameList, "info") < 0 ||
        esxVI_LookupDatastoreByName(ctx, poolName, propertyNameList, &datastore,
                                    esxVI_Occurrence_OptionalItem) < 0) {
        goto cleanup;
    }

    if (datastore == NULL) {
        /* Not found, let the base storage driver handle error reporting */
        goto cleanup;
    }

    for (dynamicProperty = datastore->propSet; dynamicProperty != NULL;
         dynamicProperty = dynamicProperty->_next) {
        if (STREQ(dynamicProperty->name, "info")) {
            if (esxVI_DatastoreInfo_CastFromAnyType(dynamicProperty->val,
                                                    &datastoreInfo) < 0) {
                goto cleanup;
            }

            break;
        }
    }

    if (esxVI_LocalDatastoreInfo_DynamicCast(datastoreInfo) != NULL) {
        *poolType = VIR_STORAGE_POOL_DIR;
    } else if (esxVI_NasDatastoreInfo_DynamicCast(datastoreInfo) != NULL) {
        *poolType = VIR_STORAGE_POOL_NETFS;
    } else if (esxVI_VmfsDatastoreInfo_DynamicCast(datastoreInfo) != NULL) {
        *poolType = VIR_STORAGE_POOL_FS;
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("DatastoreInfo has unexpected type"));
        goto cleanup;
    }

    result = 0;

  cleanup:
    esxVI_String_Free(&propertyNameList);
    esxVI_ObjectContent_Free(&datastore);
    esxVI_DatastoreInfo_Free(&datastoreInfo);

    return result;
}



static int
esxStorageBackendVMFSNumberOfPools(virConnectPtr conn)
{
    int count = 0;
    esxPrivate *priv = conn->storagePrivateData;
    esxVI_ObjectContent *datastoreList = NULL;
    esxVI_ObjectContent *datastore = NULL;

    if (esxVI_LookupDatastoreList(priv->primary, NULL, &datastoreList) < 0) {
        return -1;
    }

    for (datastore = datastoreList; datastore != NULL;
         datastore = datastore->_next) {
        ++count;
    }

    esxVI_ObjectContent_Free(&datastoreList);

    return count;
}



static int
esxStorageBackendVMFSListPools(virConnectPtr conn, char **const names,
                               const int maxnames)
{
    bool success = false;
    esxPrivate *priv = conn->storagePrivateData;
    esxVI_String *propertyNameList = NULL;
    esxVI_DynamicProperty *dynamicProperty = NULL;
    esxVI_ObjectContent *datastoreList = NULL;
    esxVI_ObjectContent *datastore = NULL;
    int count = 0;
    int i;

    if (maxnames == 0) {
        return 0;
    }

    if (esxVI_String_AppendValueToList(&propertyNameList,
                                       "summary.name") < 0 ||
        esxVI_LookupDatastoreList(priv->primary, propertyNameList,
                                  &datastoreList) < 0) {
        goto cleanup;
    }

    for (datastore = datastoreList; datastore != NULL;
         datastore = datastore->_next) {
        for (dynamicProperty = datastore->propSet; dynamicProperty != NULL;
             dynamicProperty = dynamicProperty->_next) {
            if (STREQ(dynamicProperty->name, "summary.name")) {
                if (esxVI_AnyType_ExpectType(dynamicProperty->val,
                                             esxVI_Type_String) < 0) {
                    goto cleanup;
                }

                names[count] = strdup(dynamicProperty->val->string);

                if (names[count] == NULL) {
                    virReportOOMError();
                    goto cleanup;
                }

                ++count;
                break;
            } else {
                VIR_WARN("Unexpected '%s' property", dynamicProperty->name);
            }
        }
    }

    success = true;

  cleanup:
    if (! success) {
        for (i = 0; i < count; ++i) {
            VIR_FREE(names[i]);
        }

        count = -1;
    }

    esxVI_String_Free(&propertyNameList);
    esxVI_ObjectContent_Free(&datastoreList);

    return count;
}



static virStoragePoolPtr
esxStorageBackendVMFSPoolLookupByName(virConnectPtr conn,
                                      const char *name)
{
    esxPrivate *priv = conn->storagePrivateData;
    esxVI_ObjectContent *datastore = NULL;
    esxVI_DatastoreHostMount *hostMount = NULL;
    /* MD5_DIGEST_SIZE = VIR_UUID_BUFLEN = 16 */
    unsigned char md5[MD5_DIGEST_SIZE];
    virStoragePoolPtr pool = NULL;

    if (esxVI_LookupDatastoreByName(priv->primary, name, NULL, &datastore,
                                    esxVI_Occurrence_OptionalItem) < 0) {
        goto cleanup;
    }

    if (datastore == NULL) {
        /* Not found, let the base storage driver handle error reporting */
        goto cleanup;
    }

    /*
     * Datastores don't have a UUID, but we can use the 'host.mountInfo.path'
     * property as source for a UUID. The mount path is unique per host and
     * cannot change during the lifetime of the datastore.
     *
     * The MD5 sum of the mount path can be used as UUID, assuming MD5 is
     * considered to be collision-free enough for this use case.
     */
    if (esxVI_LookupDatastoreHostMount(priv->primary, datastore->obj, &hostMount,
                                       esxVI_Occurrence_OptionalItem) < 0) {
        goto cleanup;
    }

    if (hostMount == NULL) {
        /* Not found, let the base storage driver handle error reporting */
        goto cleanup;
    }

    md5_buffer(hostMount->mountInfo->path,
               strlen(hostMount->mountInfo->path), md5);

    pool = virGetStoragePool(conn, name, md5, &esxStorageBackendVMFS, NULL);

  cleanup:
    esxVI_ObjectContent_Free(&datastore);
    esxVI_DatastoreHostMount_Free(&hostMount);

    return pool;
}



static virStoragePoolPtr
esxStorageBackendVMFSPoolLookupByUUID(virConnectPtr conn,
                                      const unsigned char *uuid)
{
    esxPrivate *priv = conn->storagePrivateData;
    esxVI_String *propertyNameList = NULL;
    esxVI_ObjectContent *datastoreList = NULL;
    esxVI_ObjectContent *datastore = NULL;
    esxVI_DatastoreHostMount *hostMount = NULL;
    /* MD5_DIGEST_SIZE = VIR_UUID_BUFLEN = 16 */
    unsigned char md5[MD5_DIGEST_SIZE];
    char *name = NULL;
    virStoragePoolPtr pool = NULL;


    if (esxVI_String_AppendValueToList(&propertyNameList, "summary.name") < 0 ||
        esxVI_LookupDatastoreList(priv->primary, propertyNameList,
                                  &datastoreList) < 0) {
        goto cleanup;
    }

    for (datastore = datastoreList; datastore != NULL;
         datastore = datastore->_next) {
        esxVI_DatastoreHostMount_Free(&hostMount);

        if (esxVI_LookupDatastoreHostMount(priv->primary, datastore->obj,
                                           &hostMount,
                                           esxVI_Occurrence_OptionalItem) < 0) {
            goto cleanup;
        }

        if (hostMount == NULL) {
            /*
             * Storage pool is not of VMFS type, leave error reporting to the
             * base storage driver.
             */
            goto cleanup;
        }

        md5_buffer(hostMount->mountInfo->path,
                   strlen(hostMount->mountInfo->path), md5);

        if (memcmp(uuid, md5, VIR_UUID_BUFLEN) == 0) {
            break;
        }
    }

    if (datastore == NULL) {
        /* Not found, let the base storage driver handle error reporting */
        goto cleanup;
    }

    if (esxVI_GetStringValue(datastore, "summary.name", &name,
                             esxVI_Occurrence_RequiredItem) < 0) {
        goto cleanup;
    }

    pool = virGetStoragePool(conn, name, uuid, &esxStorageBackendVMFS, NULL);

  cleanup:
    esxVI_String_Free(&propertyNameList);
    esxVI_ObjectContent_Free(&datastoreList);
    esxVI_DatastoreHostMount_Free(&hostMount);

    return pool;
}



static int
esxStorageBackendVMFSPoolRefresh(virStoragePoolPtr pool, unsigned int flags)
{
    int result = -1;
    esxPrivate *priv = pool->conn->storagePrivateData;
    esxVI_ObjectContent *datastore = NULL;

    virCheckFlags(0, -1);

    if (esxVI_LookupDatastoreByName(priv->primary, pool->name, NULL, &datastore,
                                    esxVI_Occurrence_RequiredItem) < 0 ||
        esxVI_RefreshDatastore(priv->primary, datastore->obj) < 0) {
        goto cleanup;
    }

    result = 0;

  cleanup:
    esxVI_ObjectContent_Free(&datastore);

    return result;
}



static int
esxStorageBackendVMFSPoolGetInfo(virStoragePoolPtr pool,
                                 virStoragePoolInfoPtr info)
{
    int result = -1;
    esxPrivate *priv = pool->conn->storagePrivateData;
    esxVI_String *propertyNameList = NULL;
    esxVI_ObjectContent *datastore = NULL;
    esxVI_DynamicProperty *dynamicProperty = NULL;
    esxVI_Boolean accessible = esxVI_Boolean_Undefined;

    if (esxVI_String_AppendValueListToList(&propertyNameList,
                                           "summary.accessible\0"
                                           "summary.capacity\0"
                                           "summary.freeSpace\0") < 0 ||
        esxVI_LookupDatastoreByName(priv->primary, pool->name,
                                    propertyNameList, &datastore,
                                    esxVI_Occurrence_RequiredItem) < 0 ||
        esxVI_GetBoolean(datastore, "summary.accessible",
                         &accessible, esxVI_Occurrence_RequiredItem) < 0) {
        goto cleanup;
    }

    if (accessible == esxVI_Boolean_True) {
        info->state = VIR_STORAGE_POOL_RUNNING;

        for (dynamicProperty = datastore->propSet; dynamicProperty != NULL;
             dynamicProperty = dynamicProperty->_next) {
            if (STREQ(dynamicProperty->name, "summary.capacity")) {
                if (esxVI_AnyType_ExpectType(dynamicProperty->val,
                                             esxVI_Type_Long) < 0) {
                    goto cleanup;
                }

                info->capacity = dynamicProperty->val->int64;
            } else if (STREQ(dynamicProperty->name, "summary.freeSpace")) {
                if (esxVI_AnyType_ExpectType(dynamicProperty->val,
                                             esxVI_Type_Long) < 0) {
                    goto cleanup;
                }

                info->available = dynamicProperty->val->int64;
            }
        }

        info->allocation = info->capacity - info->available;
    } else {
        info->state = VIR_STORAGE_POOL_INACCESSIBLE;
    }

    result = 0;

  cleanup:
    esxVI_String_Free(&propertyNameList);
    esxVI_ObjectContent_Free(&datastore);

    return result;
}



static char *
esxStorageBackendVMFSPoolGetXMLDesc(virStoragePoolPtr pool, unsigned int flags)
{
    esxPrivate *priv = pool->conn->storagePrivateData;
    esxVI_String *propertyNameList = NULL;
    esxVI_ObjectContent *datastore = NULL;
    esxVI_DatastoreHostMount *hostMount = NULL;
    esxVI_DynamicProperty *dynamicProperty = NULL;
    esxVI_Boolean accessible = esxVI_Boolean_Undefined;
    virStoragePoolDef def;
    esxVI_DatastoreInfo *info = NULL;
    esxVI_NasDatastoreInfo *nasInfo = NULL;
    char *xml = NULL;

    virCheckFlags(0, NULL);

    memset(&def, 0, sizeof(def));

    if (esxVI_String_AppendValueListToList(&propertyNameList,
                                           "summary.accessible\0"
                                           "summary.capacity\0"
                                           "summary.freeSpace\0"
                                           "info\0") < 0 ||
        esxVI_LookupDatastoreByName(priv->primary, pool->name,
                                    propertyNameList, &datastore,
                                    esxVI_Occurrence_RequiredItem) < 0 ||
        esxVI_GetBoolean(datastore, "summary.accessible",
                         &accessible, esxVI_Occurrence_RequiredItem) < 0 ||
        esxVI_LookupDatastoreHostMount(priv->primary, datastore->obj, &hostMount,
                                       esxVI_Occurrence_RequiredItem) < 0) {
        goto cleanup;
    }

    def.name = pool->name;
    memcpy(def.uuid, pool->uuid, VIR_UUID_BUFLEN);

    def.target.path = hostMount->mountInfo->path;

    if (accessible == esxVI_Boolean_True) {
        for (dynamicProperty = datastore->propSet; dynamicProperty != NULL;
             dynamicProperty = dynamicProperty->_next) {
            if (STREQ(dynamicProperty->name, "summary.capacity")) {
                if (esxVI_AnyType_ExpectType(dynamicProperty->val,
                                             esxVI_Type_Long) < 0) {
                    goto cleanup;
                }

                def.capacity = dynamicProperty->val->int64;
            } else if (STREQ(dynamicProperty->name, "summary.freeSpace")) {
                if (esxVI_AnyType_ExpectType(dynamicProperty->val,
                                             esxVI_Type_Long) < 0) {
                    goto cleanup;
                }

                def.available = dynamicProperty->val->int64;
            }
        }

        def.allocation = def.capacity - def.available;
    }

    for (dynamicProperty = datastore->propSet; dynamicProperty != NULL;
         dynamicProperty = dynamicProperty->_next) {
        if (STREQ(dynamicProperty->name, "info")) {
            if (esxVI_DatastoreInfo_CastFromAnyType(dynamicProperty->val,
                                                    &info) < 0) {
                goto cleanup;
            }

            break;
        }
    }

    /* See vSphere API documentation about HostDatastoreSystem for details */
    if (esxVI_LocalDatastoreInfo_DynamicCast(info) != NULL) {
        def.type = VIR_STORAGE_POOL_DIR;
    } else if ((nasInfo = esxVI_NasDatastoreInfo_DynamicCast(info)) != NULL) {
        if (VIR_ALLOC_N(def.source.hosts, 1) < 0) {
            virReportOOMError();
            goto cleanup;
        }
        def.type = VIR_STORAGE_POOL_NETFS;
        def.source.hosts[0].name = nasInfo->nas->remoteHost;
        def.source.dir = nasInfo->nas->remotePath;

        if (STRCASEEQ(nasInfo->nas->type, "NFS")) {
            def.source.format = VIR_STORAGE_POOL_NETFS_NFS;
        } else  if (STRCASEEQ(nasInfo->nas->type, "CIFS")) {
            def.source.format = VIR_STORAGE_POOL_NETFS_CIFS;
        } else {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Datastore has unexpected type '%s'"),
                           nasInfo->nas->type);
            goto cleanup;
        }
    } else if (esxVI_VmfsDatastoreInfo_DynamicCast(info) != NULL) {
        def.type = VIR_STORAGE_POOL_FS;
        /*
         * FIXME: I'm not sure how to represent the source and target of a
         * VMFS based datastore in libvirt terms
         */
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("DatastoreInfo has unexpected type"));
        goto cleanup;
    }

    xml = virStoragePoolDefFormat(&def);

  cleanup:
    esxVI_String_Free(&propertyNameList);
    esxVI_ObjectContent_Free(&datastore);
    esxVI_DatastoreHostMount_Free(&hostMount);
    esxVI_DatastoreInfo_Free(&info);

    return xml;
}



static int
esxStorageBackendVMFSPoolNumberOfVolumes(virStoragePoolPtr pool)
{
    bool success = false;
    esxPrivate *priv = pool->conn->storagePrivateData;
    esxVI_HostDatastoreBrowserSearchResults *searchResultsList = NULL;
    esxVI_HostDatastoreBrowserSearchResults *searchResults = NULL;
    esxVI_FileInfo *fileInfo = NULL;
    int count = 0;

    if (esxVI_LookupDatastoreContentByDatastoreName(priv->primary, pool->name,
                                                    &searchResultsList) < 0) {
        goto cleanup;
    }

    /* Interpret search result */
    for (searchResults = searchResultsList; searchResults != NULL;
         searchResults = searchResults->_next) {
        for (fileInfo = searchResults->file; fileInfo != NULL;
             fileInfo = fileInfo->_next) {
            ++count;
        }
    }

    success = true;

  cleanup:
    esxVI_HostDatastoreBrowserSearchResults_Free(&searchResultsList);

    return success ? count : -1;
}



static int
esxStorageBackendVMFSPoolListVolumes(virStoragePoolPtr pool, char **const names,
                                     int maxnames)
{
    bool success = false;
    esxPrivate *priv = pool->conn->storagePrivateData;
    esxVI_HostDatastoreBrowserSearchResults *searchResultsList = NULL;
    esxVI_HostDatastoreBrowserSearchResults *searchResults = NULL;
    esxVI_FileInfo *fileInfo = NULL;
    char *directoryAndFileName = NULL;
    size_t length;
    int count = 0;
    int i;

    if (names == NULL || maxnames < 0) {
        virReportError(VIR_ERR_INVALID_ARG, "%s", _("Invalid argument"));
        return -1;
    }

    if (maxnames == 0) {
        return 0;
    }

    if (esxVI_LookupDatastoreContentByDatastoreName(priv->primary, pool->name,
                                                    &searchResultsList) < 0) {
        goto cleanup;
    }

    /* Interpret search result */
    for (searchResults = searchResultsList; searchResults != NULL;
         searchResults = searchResults->_next) {
        VIR_FREE(directoryAndFileName);

        if (esxUtil_ParseDatastorePath(searchResults->folderPath, NULL, NULL,
                                       &directoryAndFileName) < 0) {
            goto cleanup;
        }

        /* Strip trailing separators */
        length = strlen(directoryAndFileName);

        while (length > 0 && directoryAndFileName[length - 1] == '/') {
            directoryAndFileName[length - 1] = '\0';
            --length;
        }

        /* Build volume names */
        for (fileInfo = searchResults->file; fileInfo != NULL;
             fileInfo = fileInfo->_next) {
            if (length < 1) {
                names[count] = strdup(fileInfo->path);

                if (names[count] == NULL) {
                    virReportOOMError();
                    goto cleanup;
                }
            } else if (virAsprintf(&names[count], "%s/%s", directoryAndFileName,
                                   fileInfo->path) < 0) {
                virReportOOMError();
                goto cleanup;
            }

            ++count;
        }
    }

    success = true;

  cleanup:
    if (! success) {
        for (i = 0; i < count; ++i) {
            VIR_FREE(names[i]);
        }

        count = -1;
    }

    esxVI_HostDatastoreBrowserSearchResults_Free(&searchResultsList);
    VIR_FREE(directoryAndFileName);

    return count;
}



static virStorageVolPtr
esxStorageBackendVMFSVolumeLookupByName(virStoragePoolPtr pool,
                                        const char *name)
{
    virStorageVolPtr volume = NULL;
    esxPrivate *priv = pool->conn->storagePrivateData;
    char *datastorePath = NULL;
    char *key = NULL;

    if (virAsprintf(&datastorePath, "[%s] %s", pool->name, name) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if (esxVI_LookupStorageVolumeKeyByDatastorePath(priv->primary,
                                                    datastorePath, &key) < 0) {
        goto cleanup;
    }

    volume = virGetStorageVol(pool->conn, pool->name, name, key,
                              &esxStorageBackendVMFS, NULL);

  cleanup:
    VIR_FREE(datastorePath);
    VIR_FREE(key);

    return volume;
}



static virStorageVolPtr
esxStorageBackendVMFSVolumeLookupByPath(virConnectPtr conn, const char *path)
{
    virStorageVolPtr volume = NULL;
    esxPrivate *priv = conn->storagePrivateData;
    char *datastoreName = NULL;
    char *directoryAndFileName = NULL;
    char *key = NULL;

    if (esxUtil_ParseDatastorePath(path, &datastoreName, NULL,
                                   &directoryAndFileName) < 0) {
        goto cleanup;
    }

    if (esxVI_LookupStorageVolumeKeyByDatastorePath(priv->primary, path,
                                                    &key) < 0) {
        goto cleanup;
    }

    volume = virGetStorageVol(conn, datastoreName, directoryAndFileName, key,
                              &esxStorageBackendVMFS, NULL);

  cleanup:
    VIR_FREE(datastoreName);
    VIR_FREE(directoryAndFileName);
    VIR_FREE(key);

    return volume;
}



static virStorageVolPtr
esxStorageBackendVMFSVolumeLookupByKey(virConnectPtr conn, const char *key)
{
    virStorageVolPtr volume = NULL;
    esxPrivate *priv = conn->storagePrivateData;
    esxVI_String *propertyNameList = NULL;
    esxVI_ObjectContent *datastoreList = NULL;
    esxVI_ObjectContent *datastore = NULL;
    char *datastoreName = NULL;
    esxVI_HostDatastoreBrowserSearchResults *searchResultsList = NULL;
    esxVI_HostDatastoreBrowserSearchResults *searchResults = NULL;
    char *directoryAndFileName = NULL;
    size_t length;
    char *datastorePath = NULL;
    char *volumeName = NULL;
    esxVI_FileInfo *fileInfo = NULL;
    char *uuid_string = NULL;
    char key_candidate[VIR_UUID_STRING_BUFLEN] = "";

    if (STRPREFIX(key, "[")) {
        /* Key is probably a datastore path */
        return esxStorageBackendVMFSVolumeLookupByPath(conn, key);
    }

    if (!priv->primary->hasQueryVirtualDiskUuid) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("QueryVirtualDiskUuid not available, "
                         "cannot lookup storage volume by UUID"));
        return NULL;
    }

    /* Lookup all datastores */
    if (esxVI_String_AppendValueToList(&propertyNameList, "summary.name") < 0 ||
        esxVI_LookupDatastoreList(priv->primary, propertyNameList,
                                  &datastoreList) < 0) {
        goto cleanup;
    }

    for (datastore = datastoreList; datastore != NULL;
         datastore = datastore->_next) {
        datastoreName = NULL;

        if (esxVI_GetStringValue(datastore, "summary.name", &datastoreName,
                                 esxVI_Occurrence_RequiredItem) < 0) {
            goto cleanup;
        }

        /* Lookup datastore content */
        esxVI_HostDatastoreBrowserSearchResults_Free(&searchResultsList);

        if (esxVI_LookupDatastoreContentByDatastoreName
              (priv->primary, datastoreName, &searchResultsList) < 0) {
            goto cleanup;
        }

        /* Interpret search result */
        for (searchResults = searchResultsList; searchResults != NULL;
             searchResults = searchResults->_next) {
            VIR_FREE(directoryAndFileName);

            if (esxUtil_ParseDatastorePath(searchResults->folderPath, NULL,
                                           NULL, &directoryAndFileName) < 0) {
                goto cleanup;
            }

            /* Strip trailing separators */
            length = strlen(directoryAndFileName);

            while (length > 0 && directoryAndFileName[length - 1] == '/') {
                directoryAndFileName[length - 1] = '\0';
                --length;
            }

            /* Build datastore path and query the UUID */
            for (fileInfo = searchResults->file; fileInfo != NULL;
                 fileInfo = fileInfo->_next) {
                VIR_FREE(datastorePath);

                if (length < 1) {
                    if (virAsprintf(&volumeName, "%s",
                                    fileInfo->path) < 0) {
                        virReportOOMError();
                        goto cleanup;
                    }
                } else if (virAsprintf(&volumeName, "%s/%s",
                                       directoryAndFileName,
                                       fileInfo->path) < 0) {
                    virReportOOMError();
                    goto cleanup;
                }

                if (virAsprintf(&datastorePath, "[%s] %s", datastoreName,
                                volumeName) < 0) {
                    virReportOOMError();
                    goto cleanup;
                }

                if (esxVI_VmDiskFileInfo_DynamicCast(fileInfo) == NULL) {
                    /* Only a VirtualDisk has a UUID */
                    continue;
                }

                VIR_FREE(uuid_string);

                if (esxVI_QueryVirtualDiskUuid
                      (priv->primary, datastorePath,
                       priv->primary->datacenter->_reference,
                       &uuid_string) < 0) {
                    goto cleanup;
                }

                if (esxUtil_ReformatUuid(uuid_string, key_candidate) < 0) {
                    goto cleanup;
                }

                if (STREQ(key, key_candidate)) {
                    /* Found matching UUID */
                    volume = virGetStorageVol(conn, datastoreName,
                                              volumeName, key,
                                              &esxStorageBackendVMFS, NULL);
                    goto cleanup;
                }
            }
        }
    }

  cleanup:
    esxVI_String_Free(&propertyNameList);
    esxVI_ObjectContent_Free(&datastoreList);
    esxVI_HostDatastoreBrowserSearchResults_Free(&searchResultsList);
    VIR_FREE(directoryAndFileName);
    VIR_FREE(datastorePath);
    VIR_FREE(volumeName);
    VIR_FREE(uuid_string);

    return volume;
}



static virStorageVolPtr
esxStorageBackendVMFSVolumeCreateXML(virStoragePoolPtr pool,
                                     const char *xmldesc,
                                     unsigned int flags)
{
    virStorageVolPtr volume = NULL;
    esxPrivate *priv = pool->conn->storagePrivateData;
    virStoragePoolDef poolDef;
    virStorageVolDefPtr def = NULL;
    char *tmp;
    char *unescapedDatastorePath = NULL;
    char *unescapedDirectoryName = NULL;
    char *unescapedDirectoryAndFileName = NULL;
    char *directoryName = NULL;
    char *fileName = NULL;
    char *datastorePathWithoutFileName = NULL;
    char *datastorePath = NULL;
    esxVI_FileInfo *fileInfo = NULL;
    esxVI_FileBackedVirtualDiskSpec *virtualDiskSpec = NULL;
    esxVI_ManagedObjectReference *task = NULL;
    esxVI_TaskInfoState taskInfoState;
    char *taskInfoErrorMessage = NULL;
    char *uuid_string = NULL;
    char *key = NULL;

    virCheckFlags(0, NULL);

    memset(&poolDef, 0, sizeof(poolDef));

    if (esxLookupVMFSStoragePoolType(priv->primary, pool->name,
                                     &poolDef.type) < 0) {
        goto cleanup;
    }

    /* Parse config */
    def = virStorageVolDefParseString(&poolDef, xmldesc);

    if (def == NULL) {
        goto cleanup;
    }

    if (def->type != VIR_STORAGE_VOL_FILE) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Creating non-file volumes is not supported"));
        goto cleanup;
    }

    /* Validate config */
    tmp = strrchr(def->name, '/');

    if (tmp == NULL || *def->name == '/' || tmp[1] == '\0') {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Volume name '%s' doesn't have expected format "
                         "'<directory>/<file>'"), def->name);
        goto cleanup;
    }

    if (! virFileHasSuffix(def->name, ".vmdk")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Volume name '%s' has unsupported suffix, "
                         "expecting '.vmdk'"), def->name);
        goto cleanup;
    }

    if (virAsprintf(&unescapedDatastorePath, "[%s] %s", pool->name,
                    def->name) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if (def->target.format == VIR_STORAGE_FILE_VMDK) {
        /* Parse and escape datastore path */
        if (esxUtil_ParseDatastorePath(unescapedDatastorePath, NULL,
                                       &unescapedDirectoryName,
                                       &unescapedDirectoryAndFileName) < 0) {
            goto cleanup;
        }

        directoryName = esxUtil_EscapeDatastoreItem(unescapedDirectoryName);

        if (directoryName == NULL) {
            goto cleanup;
        }

        fileName = esxUtil_EscapeDatastoreItem(unescapedDirectoryAndFileName +
                                               strlen(unescapedDirectoryName) + 1);

        if (fileName == NULL) {
            goto cleanup;
        }

        if (virAsprintf(&datastorePathWithoutFileName, "[%s] %s", pool->name,
                        directoryName) < 0) {
            virReportOOMError();
            goto cleanup;
        }

        if (virAsprintf(&datastorePath, "[%s] %s/%s", pool->name, directoryName,
                        fileName) < 0) {
            virReportOOMError();
            goto cleanup;
        }

        /* Create directory, if it doesn't exist yet */
        if (esxVI_LookupFileInfoByDatastorePath
              (priv->primary, datastorePathWithoutFileName, true, &fileInfo,
               esxVI_Occurrence_OptionalItem) < 0) {
            goto cleanup;
        }

        if (fileInfo == NULL) {
            if (esxVI_MakeDirectory(priv->primary, datastorePathWithoutFileName,
                                    priv->primary->datacenter->_reference,
                                    esxVI_Boolean_True) < 0) {
                goto cleanup;
            }
        }

        /* Create VirtualDisk */
        if (esxVI_FileBackedVirtualDiskSpec_Alloc(&virtualDiskSpec) < 0 ||
            esxVI_Long_Alloc(&virtualDiskSpec->capacityKb) < 0) {
            goto cleanup;
        }

        /* From the vSphere API documentation about VirtualDiskType ... */
        if (def->allocation == def->capacity) {
            /*
             * "A preallocated disk has all space allocated at creation time
             *  and the space is zeroed on demand as the space is used."
             */
            virtualDiskSpec->diskType = (char *)"preallocated";
        } else if (def->allocation == 0) {
            /*
             * "Space required for thin-provisioned virtual disk is allocated
             *  and zeroed on demand as the space is used."
             */
            virtualDiskSpec->diskType = (char *)"thin";
        } else {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Unsupported capacity-to-allocation relation"));
            goto cleanup;
        }

        /*
         * FIXME: The adapter type is a required parameter, but there is no
         * way to let the user specify it in the volume XML config. Therefore,
         * default to 'busLogic' here.
         */
        virtualDiskSpec->adapterType = (char *)"busLogic";

        virtualDiskSpec->capacityKb->value =
          VIR_DIV_UP(def->capacity, 1024); /* Scale from byte to kilobyte */

        if (esxVI_CreateVirtualDisk_Task
              (priv->primary, datastorePath,
               priv->primary->datacenter->_reference,
               esxVI_VirtualDiskSpec_DynamicCast(virtualDiskSpec), &task) < 0 ||
            esxVI_WaitForTaskCompletion(priv->primary, task, NULL,
                                        esxVI_Occurrence_None,
                                        priv->parsedUri->autoAnswer,
                                        &taskInfoState,
                                        &taskInfoErrorMessage) < 0) {
            goto cleanup;
        }

        if (taskInfoState != esxVI_TaskInfoState_Success) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not create volume: %s"),
                           taskInfoErrorMessage);
            goto cleanup;
        }

        if (priv->primary->hasQueryVirtualDiskUuid) {
            if (VIR_ALLOC_N(key, VIR_UUID_STRING_BUFLEN) < 0) {
                virReportOOMError();
                goto cleanup;
            }

            if (esxVI_QueryVirtualDiskUuid(priv->primary, datastorePath,
                                           priv->primary->datacenter->_reference,
                                           &uuid_string) < 0) {
                goto cleanup;
            }

            if (esxUtil_ReformatUuid(uuid_string, key) < 0) {
                goto cleanup;
            }
        } else {
            /* Fall back to the path as key */
            if (esxVI_String_DeepCopyValue(&key, datastorePath) < 0) {
                goto cleanup;
            }
        }
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Creation of %s volumes is not supported"),
                       virStorageFileFormatTypeToString(def->target.format));
        goto cleanup;
    }

    volume = virGetStorageVol(pool->conn, pool->name, def->name, key,
                              &esxStorageBackendVMFS, NULL);

  cleanup:
    if (virtualDiskSpec != NULL) {
        virtualDiskSpec->diskType = NULL;
        virtualDiskSpec->adapterType = NULL;
    }

    virStorageVolDefFree(def);
    VIR_FREE(unescapedDatastorePath);
    VIR_FREE(unescapedDirectoryName);
    VIR_FREE(unescapedDirectoryAndFileName);
    VIR_FREE(directoryName);
    VIR_FREE(fileName);
    VIR_FREE(datastorePathWithoutFileName);
    VIR_FREE(datastorePath);
    esxVI_FileInfo_Free(&fileInfo);
    esxVI_FileBackedVirtualDiskSpec_Free(&virtualDiskSpec);
    esxVI_ManagedObjectReference_Free(&task);
    VIR_FREE(taskInfoErrorMessage);
    VIR_FREE(uuid_string);
    VIR_FREE(key);

    return volume;
}



static virStorageVolPtr
esxStorageBackendVMFSVolumeCreateXMLFrom(virStoragePoolPtr pool,
                                         const char *xmldesc,
                                         virStorageVolPtr sourceVolume,
                                         unsigned int flags)
{
    virStorageVolPtr volume = NULL;
    esxPrivate *priv = pool->conn->storagePrivateData;
    virStoragePoolDef poolDef;
    char *sourceDatastorePath = NULL;
    virStorageVolDefPtr def = NULL;
    char *tmp;
    char *unescapedDatastorePath = NULL;
    char *unescapedDirectoryName = NULL;
    char *unescapedDirectoryAndFileName = NULL;
    char *directoryName = NULL;
    char *fileName = NULL;
    char *datastorePathWithoutFileName = NULL;
    char *datastorePath = NULL;
    esxVI_FileInfo *fileInfo = NULL;
    esxVI_ManagedObjectReference *task = NULL;
    esxVI_TaskInfoState taskInfoState;
    char *taskInfoErrorMessage = NULL;
    char *uuid_string = NULL;
    char *key = NULL;

    virCheckFlags(0, NULL);

    memset(&poolDef, 0, sizeof(poolDef));

    if (esxLookupVMFSStoragePoolType(priv->primary, pool->name,
                                     &poolDef.type) < 0) {
        goto cleanup;
    }

    if (virAsprintf(&sourceDatastorePath, "[%s] %s", sourceVolume->pool,
                    sourceVolume->name) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    /* Parse config */
    def = virStorageVolDefParseString(&poolDef, xmldesc);

    if (def == NULL) {
        goto cleanup;
    }

    if (def->type != VIR_STORAGE_VOL_FILE) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Creating non-file volumes is not supported"));
        goto cleanup;
    }

    /* Validate config */
    tmp = strrchr(def->name, '/');

    if (tmp == NULL || *def->name == '/' || tmp[1] == '\0') {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Volume name '%s' doesn't have expected format "
                         "'<directory>/<file>'"), def->name);
        goto cleanup;
    }

    if (! virFileHasSuffix(def->name, ".vmdk")) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Volume name '%s' has unsupported suffix, "
                         "expecting '.vmdk'"), def->name);
        goto cleanup;
    }

    if (virAsprintf(&unescapedDatastorePath, "[%s] %s", pool->name,
                    def->name) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if (def->target.format == VIR_STORAGE_FILE_VMDK) {
        /* Parse and escape datastore path */
        if (esxUtil_ParseDatastorePath(unescapedDatastorePath, NULL,
                                       &unescapedDirectoryName,
                                       &unescapedDirectoryAndFileName) < 0) {
            goto cleanup;
        }

        directoryName = esxUtil_EscapeDatastoreItem(unescapedDirectoryName);

        if (directoryName == NULL) {
            goto cleanup;
        }

        fileName = esxUtil_EscapeDatastoreItem(unescapedDirectoryAndFileName +
                                               strlen(unescapedDirectoryName) + 1);

        if (fileName == NULL) {
            goto cleanup;
        }

        if (virAsprintf(&datastorePathWithoutFileName, "[%s] %s", pool->name,
                        directoryName) < 0) {
            virReportOOMError();
            goto cleanup;
        }

        if (virAsprintf(&datastorePath, "[%s] %s/%s", pool->name, directoryName,
                        fileName) < 0) {
            virReportOOMError();
            goto cleanup;
        }

        /* Create directory, if it doesn't exist yet */
        if (esxVI_LookupFileInfoByDatastorePath
              (priv->primary, datastorePathWithoutFileName, true, &fileInfo,
               esxVI_Occurrence_OptionalItem) < 0) {
            goto cleanup;
        }

        if (fileInfo == NULL) {
            if (esxVI_MakeDirectory(priv->primary, datastorePathWithoutFileName,
                                    priv->primary->datacenter->_reference,
                                    esxVI_Boolean_True) < 0) {
                goto cleanup;
            }
        }

        /* Copy VirtualDisk */
        if (esxVI_CopyVirtualDisk_Task(priv->primary, sourceDatastorePath,
                                       priv->primary->datacenter->_reference,
                                       datastorePath,
                                       priv->primary->datacenter->_reference,
                                       NULL, esxVI_Boolean_False, &task) < 0 ||
            esxVI_WaitForTaskCompletion(priv->primary, task, NULL,
                                        esxVI_Occurrence_None,
                                        priv->parsedUri->autoAnswer,
                                        &taskInfoState,
                                        &taskInfoErrorMessage) < 0) {
            goto cleanup;
        }

        if (taskInfoState != esxVI_TaskInfoState_Success) {
            virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not copy volume: %s"),
                           taskInfoErrorMessage);
            goto cleanup;
        }

        if (priv->primary->hasQueryVirtualDiskUuid) {
            if (VIR_ALLOC_N(key, VIR_UUID_STRING_BUFLEN) < 0) {
                virReportOOMError();
                goto cleanup;
            }

            if (esxVI_QueryVirtualDiskUuid(priv->primary, datastorePath,
                                           priv->primary->datacenter->_reference,
                                           &uuid_string) < 0) {
                goto cleanup;
            }

            if (esxUtil_ReformatUuid(uuid_string, key) < 0) {
                goto cleanup;
            }
        } else {
            /* Fall back to the path as key */
            if (esxVI_String_DeepCopyValue(&key, datastorePath) < 0) {
                goto cleanup;
            }
        }
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Creation of %s volumes is not supported"),
                       virStorageFileFormatTypeToString(def->target.format));
        goto cleanup;
    }

    volume = virGetStorageVol(pool->conn, pool->name, def->name, key,
                              &esxStorageBackendVMFS, NULL);

  cleanup:
    VIR_FREE(sourceDatastorePath);
    virStorageVolDefFree(def);
    VIR_FREE(unescapedDatastorePath);
    VIR_FREE(unescapedDirectoryName);
    VIR_FREE(unescapedDirectoryAndFileName);
    VIR_FREE(directoryName);
    VIR_FREE(fileName);
    VIR_FREE(datastorePathWithoutFileName);
    VIR_FREE(datastorePath);
    esxVI_FileInfo_Free(&fileInfo);
    esxVI_ManagedObjectReference_Free(&task);
    VIR_FREE(taskInfoErrorMessage);
    VIR_FREE(uuid_string);
    VIR_FREE(key);

    return volume;
}



static int
esxStorageBackendVMFSVolumeDelete(virStorageVolPtr volume, unsigned int flags)
{
    int result = -1;
    esxPrivate *priv = volume->conn->storagePrivateData;
    char *datastorePath = NULL;
    esxVI_ManagedObjectReference *task = NULL;
    esxVI_TaskInfoState taskInfoState;
    char *taskInfoErrorMessage = NULL;

    virCheckFlags(0, -1);

    if (virAsprintf(&datastorePath, "[%s] %s", volume->pool, volume->name) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if (esxVI_DeleteVirtualDisk_Task(priv->primary, datastorePath,
                                     priv->primary->datacenter->_reference,
                                     &task) < 0 ||
        esxVI_WaitForTaskCompletion(priv->primary, task, NULL,
                                    esxVI_Occurrence_None,
                                    priv->parsedUri->autoAnswer,
                                    &taskInfoState, &taskInfoErrorMessage) < 0) {
        goto cleanup;
    }

    if (taskInfoState != esxVI_TaskInfoState_Success) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not delete volume: %s"),
                       taskInfoErrorMessage);
        goto cleanup;
    }

    result = 0;

  cleanup:
    VIR_FREE(datastorePath);
    esxVI_ManagedObjectReference_Free(&task);
    VIR_FREE(taskInfoErrorMessage);

    return result;
}



static int
esxStorageBackendVMFSVolumeWipe(virStorageVolPtr volume, unsigned int flags)
{
    int result = -1;
    esxPrivate *priv = volume->conn->storagePrivateData;
    char *datastorePath = NULL;
    esxVI_ManagedObjectReference *task = NULL;
    esxVI_TaskInfoState taskInfoState;
    char *taskInfoErrorMessage = NULL;

    virCheckFlags(0, -1);

    if (virAsprintf(&datastorePath, "[%s] %s", volume->pool, volume->name) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if (esxVI_ZeroFillVirtualDisk_Task(priv->primary, datastorePath,
                                       priv->primary->datacenter->_reference,
                                       &task) < 0 ||
        esxVI_WaitForTaskCompletion(priv->primary, task, NULL,
                                    esxVI_Occurrence_None,
                                    priv->parsedUri->autoAnswer,
                                    &taskInfoState, &taskInfoErrorMessage) < 0) {
        goto cleanup;
    }

    if (taskInfoState != esxVI_TaskInfoState_Success) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not wipe volume: %s"),
                       taskInfoErrorMessage);
        goto cleanup;
    }

    result = 0;

  cleanup:
    VIR_FREE(datastorePath);
    esxVI_ManagedObjectReference_Free(&task);
    VIR_FREE(taskInfoErrorMessage);

    return result;
}



static int
esxStorageBackendVMFSVolumeGetInfo(virStorageVolPtr volume,
                                   virStorageVolInfoPtr info)
{
    int result = -1;
    esxPrivate *priv = volume->conn->storagePrivateData;
    char *datastorePath = NULL;
    esxVI_FileInfo *fileInfo = NULL;
    esxVI_VmDiskFileInfo *vmDiskFileInfo = NULL;

    memset(info, 0, sizeof(*info));

    if (virAsprintf(&datastorePath, "[%s] %s", volume->pool, volume->name) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if (esxVI_LookupFileInfoByDatastorePath(priv->primary, datastorePath,
                                            false, &fileInfo,
                                            esxVI_Occurrence_RequiredItem) < 0) {
        goto cleanup;
    }

    vmDiskFileInfo = esxVI_VmDiskFileInfo_DynamicCast(fileInfo);

    info->type = VIR_STORAGE_VOL_FILE;

    if (vmDiskFileInfo != NULL) {
        /* Scale from kilobyte to byte */
        info->capacity = vmDiskFileInfo->capacityKb->value * 1024;
        info->allocation = vmDiskFileInfo->fileSize->value;
    } else {
        info->capacity = fileInfo->fileSize->value;
        info->allocation = fileInfo->fileSize->value;
    }

    result = 0;

  cleanup:
    VIR_FREE(datastorePath);
    esxVI_FileInfo_Free(&fileInfo);

    return result;
}



static char *
esxStorageBackendVMFSVolumeGetXMLDesc(virStorageVolPtr volume,
                                      unsigned int flags)
{
    esxPrivate *priv = volume->conn->storagePrivateData;
    virStoragePoolDef pool;
    char *datastorePath = NULL;
    esxVI_FileInfo *fileInfo = NULL;
    esxVI_VmDiskFileInfo *vmDiskFileInfo = NULL;
    esxVI_IsoImageFileInfo *isoImageFileInfo = NULL;
    esxVI_FloppyImageFileInfo *floppyImageFileInfo = NULL;
    virStorageVolDef def;
    char *xml = NULL;

    virCheckFlags(0, NULL);

    memset(&pool, 0, sizeof(pool));
    memset(&def, 0, sizeof(def));

    if (esxLookupVMFSStoragePoolType(priv->primary, volume->pool,
                                     &pool.type) < 0) {
        return NULL;
    }

    /* Lookup file info */
    if (virAsprintf(&datastorePath, "[%s] %s", volume->pool, volume->name) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if (esxVI_LookupFileInfoByDatastorePath(priv->primary, datastorePath,
                                            false, &fileInfo,
                                            esxVI_Occurrence_RequiredItem) < 0) {
        goto cleanup;
    }

    vmDiskFileInfo = esxVI_VmDiskFileInfo_DynamicCast(fileInfo);
    isoImageFileInfo = esxVI_IsoImageFileInfo_DynamicCast(fileInfo);
    floppyImageFileInfo = esxVI_FloppyImageFileInfo_DynamicCast(fileInfo);

    def.name = volume->name;

    if (esxVI_LookupStorageVolumeKeyByDatastorePath(priv->primary, datastorePath,
                                                    &def.key) < 0) {
        goto cleanup;
    }

    def.type = VIR_STORAGE_VOL_FILE;
    def.target.path = datastorePath;

    if (vmDiskFileInfo != NULL) {
        /* Scale from kilobyte to byte */
        def.capacity = vmDiskFileInfo->capacityKb->value * 1024;
        def.allocation = vmDiskFileInfo->fileSize->value;

        def.target.format = VIR_STORAGE_FILE_VMDK;
    } else if (isoImageFileInfo != NULL) {
        def.capacity = fileInfo->fileSize->value;
        def.allocation = fileInfo->fileSize->value;

        def.target.format = VIR_STORAGE_FILE_ISO;
    } else if (floppyImageFileInfo != NULL) {
        def.capacity = fileInfo->fileSize->value;
        def.allocation = fileInfo->fileSize->value;

        def.target.format = VIR_STORAGE_FILE_RAW;
    } else {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("File '%s' has unknown type"), datastorePath);
        goto cleanup;
    }

    xml = virStorageVolDefFormat(&pool, &def);

  cleanup:
    VIR_FREE(datastorePath);
    esxVI_FileInfo_Free(&fileInfo);
    VIR_FREE(def.key);

    return xml;
}



static char *
esxStorageBackendVMFSVolumeGetPath(virStorageVolPtr volume)
{
    char *path;

    if (virAsprintf(&path, "[%s] %s", volume->pool, volume->name) < 0) {
        virReportOOMError();
        return NULL;
    }

    return path;
}



virStorageDriver esxStorageBackendVMFS = {
    .numOfPools = esxStorageBackendVMFSNumberOfPools, /* 0.8.2 */
    .listPools = esxStorageBackendVMFSListPools, /* 0.8.2 */
    .poolLookupByName = esxStorageBackendVMFSPoolLookupByName, /* 0.8.2 */
    .poolLookupByUUID = esxStorageBackendVMFSPoolLookupByUUID, /* 0.8.2 */
    .poolRefresh = esxStorageBackendVMFSPoolRefresh, /* 0.8.2 */
    .poolGetInfo = esxStorageBackendVMFSPoolGetInfo, /* 0.8.2 */
    .poolGetXMLDesc = esxStorageBackendVMFSPoolGetXMLDesc, /* 0.8.2 */
    .poolNumOfVolumes = esxStorageBackendVMFSPoolNumberOfVolumes, /* 0.8.4 */
    .poolListVolumes = esxStorageBackendVMFSPoolListVolumes, /* 0.8.4 */
    .volLookupByName = esxStorageBackendVMFSVolumeLookupByName, /* 0.8.4 */
    .volLookupByPath = esxStorageBackendVMFSVolumeLookupByPath, /* 0.8.4 */
    .volLookupByKey = esxStorageBackendVMFSVolumeLookupByKey, /* 0.8.4 */
    .volCreateXML = esxStorageBackendVMFSVolumeCreateXML, /* 0.8.4 */
    .volCreateXMLFrom = esxStorageBackendVMFSVolumeCreateXMLFrom, /* 0.8.7 */
    .volDelete = esxStorageBackendVMFSVolumeDelete, /* 0.8.7 */
    .volWipe = esxStorageBackendVMFSVolumeWipe, /* 0.8.7 */
    .volGetInfo = esxStorageBackendVMFSVolumeGetInfo, /* 0.8.4 */
    .volGetXMLDesc = esxStorageBackendVMFSVolumeGetXMLDesc, /* 0.8.4 */
    .volGetPath = esxStorageBackendVMFSVolumeGetPath, /* 0.8.4 */
};
