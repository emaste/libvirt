/*---------------------------------------------------------------------------*/
/* Copyright (C) 2012 Red Hat, Inc.
 * Copyright (c) 2011 SUSE LINUX Products GmbH, Nuernberg, Germany.
 * Copyright (C) 2011 Univention GmbH.
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
 * Authors:
 *     Jim Fehlig <jfehlig@novell.com>
 *     Markus Groß <gross@univention.de>
 */
/*---------------------------------------------------------------------------*/

#include <config.h>

#include <regex.h>
#include <libxl.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "internal.h"
#include "logging.h"
#include "virterror_internal.h"
#include "datatypes.h"
#include "virfile.h"
#include "virstring.h"
#include "memory.h"
#include "uuid.h"
#include "capabilities.h"
#include "libxl_driver.h"
#include "libxl_conf.h"
#include "libxl_utils.h"
#include "storage_file.h"


#define VIR_FROM_THIS VIR_FROM_LIBXL

/* see xen-unstable.hg/xen/include/asm-x86/cpufeature.h */
#define LIBXL_X86_FEATURE_PAE_MASK 0x40


struct guest_arch {
    virArch arch;
    int bits;
    int hvm;
    int pae;
    int nonpae;
    int ia64_be;
};

static const char *xen_cap_re = "(xen|hvm)-[[:digit:]]+\\.[[:digit:]]+-(x86_32|x86_64|ia64|powerpc64)(p|be)?";
static regex_t xen_cap_rec;


static int
libxlNextFreeVncPort(libxlDriverPrivatePtr driver, int startPort)
{
    int i;

    for (i = startPort ; i < LIBXL_VNC_PORT_MAX; i++) {
        int fd;
        int reuse = 1;
        struct sockaddr_in addr;
        bool used = false;

        if (virBitmapGetBit(driver->reservedVNCPorts,
                            i - LIBXL_VNC_PORT_MIN, &used) < 0)
            VIR_DEBUG("virBitmapGetBit failed on bit %d", i - LIBXL_VNC_PORT_MIN);

        if (used)
            continue;

        addr.sin_family = AF_INET;
        addr.sin_port = htons(i);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        fd = socket(PF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;

        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (void*)&reuse, sizeof(reuse)) < 0) {
            VIR_FORCE_CLOSE(fd);
            break;
        }

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            /* Not in use, lets grab it */
            VIR_FORCE_CLOSE(fd);
            /* Add port to bitmap of reserved ports */
            if (virBitmapSetBit(driver->reservedVNCPorts,
                                i - LIBXL_VNC_PORT_MIN) < 0) {
                VIR_DEBUG("virBitmapSetBit failed on bit %d",
                          i - LIBXL_VNC_PORT_MIN);
            }
            return i;
        }
        VIR_FORCE_CLOSE(fd);

        if (errno == EADDRINUSE) {
            /* In use, try next */
            continue;
        }
        /* Some other bad failure, get out.. */
        break;
    }
    return -1;
}


static int libxlDefaultConsoleType(const char *ostype,
                                   const char *arch ATTRIBUTE_UNUSED)
{
    if (STREQ(ostype, "hvm"))
        return VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_SERIAL;
    else
        return VIR_DOMAIN_CHR_CONSOLE_TARGET_TYPE_XEN;
}

static virCapsPtr
libxlBuildCapabilities(const char *hostmachine,
                       int host_pae,
                       struct guest_arch *guest_archs,
                       int nr_guest_archs)
{
    virCapsPtr caps;
    int i;

    if ((caps = virCapabilitiesNew(hostmachine, 1, 1)) == NULL)
        goto no_memory;

    virCapabilitiesSetMacPrefix(caps, (unsigned char[]){ 0x00, 0x16, 0x3e });

    if (host_pae &&
        virCapabilitiesAddHostFeature(caps, "pae") < 0)
        goto no_memory;

    for (i = 0; i < nr_guest_archs; ++i) {
        virCapsGuestPtr guest;
        char const *const xen_machines[] = {guest_archs[i].hvm ? "xenfv" : "xenpv"};
        virCapsGuestMachinePtr *machines;

        if ((machines = virCapabilitiesAllocMachines(xen_machines, 1)) == NULL)
            goto no_memory;

        if ((guest = virCapabilitiesAddGuest(caps,
                                             guest_archs[i].hvm ? "hvm" : "xen",
                                             guest_archs[i].arch,
                                             ((hostarch == VIR_ARCH_X86_64) ?
                                              "/usr/lib64/xen/bin/qemu-dm" :
                                              "/usr/lib/xen/bin/qemu-dm"),
                                             (guest_archs[i].hvm ?
                                              "/usr/lib/xen/boot/hvmloader" :
                                              NULL),
                                             1,
                                             machines)) == NULL) {
            virCapabilitiesFreeMachines(machines, 1);
            goto no_memory;
        }
        machines = NULL;

        if (virCapabilitiesAddGuestDomain(guest,
                                          "xen",
                                          NULL,
                                          NULL,
                                          0,
                                          NULL) == NULL)
            goto no_memory;

        if (guest_archs[i].pae &&
            virCapabilitiesAddGuestFeature(guest,
                                           "pae",
                                           1,
                                           0) == NULL)
            goto no_memory;

        if (guest_archs[i].nonpae &&
            virCapabilitiesAddGuestFeature(guest,
                                           "nonpae",
                                           1,
                                           0) == NULL)
            goto no_memory;

        if (guest_archs[i].ia64_be &&
            virCapabilitiesAddGuestFeature(guest,
                                           "ia64_be",
                                           1,
                                           0) == NULL)
            goto no_memory;

        if (guest_archs[i].hvm) {
            if (virCapabilitiesAddGuestFeature(guest,
                                               "acpi",
                                               1,
                                               1) == NULL)
                goto no_memory;

            if (virCapabilitiesAddGuestFeature(guest, "apic",
                                               1,
                                               0) == NULL)
                goto no_memory;

            if (virCapabilitiesAddGuestFeature(guest,
                                               "hap",
                                               0,
                                               1) == NULL)
                goto no_memory;
        }
    }

    caps->defaultConsoleTargetType = libxlDefaultConsoleType;

    return caps;

 no_memory:
    virCapabilitiesFree(caps);
    return NULL;
}

static virCapsPtr
libxlMakeCapabilitiesInternal(virArch hostarch,
                              libxl_physinfo *phy_info,
                              char *capabilities)
{
    char *str, *token;
    regmatch_t subs[4];
    char *saveptr = NULL;
    int i;

    int host_pae = 0;
    struct guest_arch guest_archs[32];
    int nr_guest_archs = 0;
    virCapsPtr caps = NULL;

    memset(guest_archs, 0, sizeof(guest_archs));

    /* hw_caps is an array of 32-bit words whose meaning is listed in
     * xen-unstable.hg/xen/include/asm-x86/cpufeature.h.  Each feature
     * is defined in the form X*32+Y, corresponding to the Y'th bit in
     * the X'th 32-bit word of hw_cap.
     */
    host_pae = phy_info->hw_cap[0] & LIBXL_X86_FEATURE_PAE_MASK;

    /* Format of capabilities string is documented in the code in
     * xen-unstable.hg/xen/arch/.../setup.c.
     *
     * It is a space-separated list of supported guest architectures.
     *
     * For x86:
     *    TYP-VER-ARCH[p]
     *    ^   ^   ^    ^
     *    |   |   |    +-- PAE supported
     *    |   |   +------- x86_32 or x86_64
     *    |   +----------- the version of Xen, eg. "3.0"
     *    +--------------- "xen" or "hvm" for para or full virt respectively
     *
     * For IA64:
     *    TYP-VER-ARCH[be]
     *    ^   ^   ^    ^
     *    |   |   |    +-- Big-endian supported
     *    |   |   +------- always "ia64"
     *    |   +----------- the version of Xen, eg. "3.0"
     *    +--------------- "xen" or "hvm" for para or full virt respectively
     */

    /* Split capabilities string into tokens. strtok_r is OK here because
     * we "own" the buffer.  Parse out the features from each token.
     */
    for (str = capabilities, nr_guest_archs = 0;
         nr_guest_archs < sizeof(guest_archs) / sizeof(guest_archs[0])
                 && (token = strtok_r(str, " ", &saveptr)) != NULL;
         str = NULL) {
        if (regexec(&xen_cap_rec, token, sizeof(subs) / sizeof(subs[0]),
                    subs, 0) == 0) {
            int hvm = STRPREFIX(&token[subs[1].rm_so], "hvm");
            virArch arch;
            int pae = 0, nonpae = 0, ia64_be = 0;

            if (STRPREFIX(&token[subs[2].rm_so], "x86_32")) {
                arch = VIR_ARCH_I686;
                if (subs[3].rm_so != -1 &&
                    STRPREFIX(&token[subs[3].rm_so], "p"))
                    pae = 1;
                else
                    nonpae = 1;
            }
            else if (STRPREFIX(&token[subs[2].rm_so], "x86_64")) {
                arch = VIR_ARCH_X86_64;
            }
            else if (STRPREFIX(&token[subs[2].rm_so], "ia64")) {
                arch = VIR_ARCH_ITANIUM;
                if (subs[3].rm_so != -1 &&
                    STRPREFIX(&token[subs[3].rm_so], "be"))
                    ia64_be = 1;
            }
            else if (STRPREFIX(&token[subs[2].rm_so], "powerpc64")) {
                arch = VIR_ARCH_PPC64;
            } else {
                /* XXX arm ? */
                continue;
            }

            /* Search for existing matching (model,hvm) tuple */
            for (i = 0 ; i < nr_guest_archs ; i++) {
                if ((guest_archs[i].arch == arch) &&
                    guest_archs[i].hvm == hvm) {
                    break;
                }
            }

            /* Too many arch flavours - highly unlikely ! */
            if (i >= ARRAY_CARDINALITY(guest_archs))
                continue;
            /* Didn't find a match, so create a new one */
            if (i == nr_guest_archs)
                nr_guest_archs++;

            guest_archs[i].arch = arch;
            guest_archs[i].bits = bits;
            guest_archs[i].hvm = hvm;

            /* Careful not to overwrite a previous positive
               setting with a negative one here - some archs
               can do both pae & non-pae, but Xen reports
               separately capabilities so we're merging archs */
            if (pae)
                guest_archs[i].pae = pae;
            if (nonpae)
                guest_archs[i].nonpae = nonpae;
            if (ia64_be)
                guest_archs[i].ia64_be = ia64_be;
        }
    }

    if ((caps = libxlBuildCapabilities(hostarch,
                                       host_pae,
                                       guest_archs,
                                       nr_guest_archs)) == NULL)
        goto no_memory;

    return caps;

 no_memory:
    virReportOOMError();
    virCapabilitiesFree(caps);
    return NULL;
}

static int
libxlMakeDomCreateInfo(libxlDriverPrivatePtr driver,
                       virDomainDefPtr def,
                       libxl_domain_create_info *c_info)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    libxl_domain_create_info_init(c_info);

    if (STREQ(def->os.type, "hvm"))
        c_info->type = LIBXL_DOMAIN_TYPE_HVM;
    else
        c_info->type = LIBXL_DOMAIN_TYPE_PV;

    if ((c_info->name = strdup(def->name)) == NULL) {
        virReportOOMError();
        goto error;
    }

    if (def->nseclabels &&
        def->seclabels[0]->type == VIR_DOMAIN_SECLABEL_STATIC) {
        if (libxl_flask_context_to_sid(driver->ctx,
                                       def->seclabels[0]->label,
                                       strlen(def->seclabels[0]->label),
                                       &c_info->ssidref)) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("libxenlight failed to resolve security label '%s'"),
                           def->seclabels[0]->label);
        }
    }

    virUUIDFormat(def->uuid, uuidstr);
    if (libxl_uuid_from_string(&c_info->uuid, uuidstr)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("libxenlight failed to parse UUID '%s'"), uuidstr);
        goto error;
    }

    return 0;

error:
    libxl_domain_create_info_dispose(c_info);
    return -1;
}

static int
libxlMakeDomBuildInfo(virDomainDefPtr def, libxl_domain_config *d_config)
{
    libxl_domain_build_info *b_info = &d_config->b_info;
    int hvm = STREQ(def->os.type, "hvm");
    int i;

    libxl_domain_build_info_init(b_info);

    if (hvm)
        libxl_domain_build_info_init_type(b_info, LIBXL_DOMAIN_TYPE_HVM);
    else
        libxl_domain_build_info_init_type(b_info, LIBXL_DOMAIN_TYPE_PV);
    b_info->max_vcpus = def->maxvcpus;
    libxl_bitmap_set((&b_info->avail_vcpus), def->vcpus);
    if (def->clock.ntimers > 0 &&
        def->clock.timers[0]->name == VIR_DOMAIN_TIMER_NAME_TSC) {
        switch (def->clock.timers[0]->mode) {
            case VIR_DOMAIN_TIMER_MODE_NATIVE:
                b_info->tsc_mode = 2;
                break;
            case VIR_DOMAIN_TIMER_MODE_PARAVIRT:
                b_info->tsc_mode = 3;
                break;
            default:
                b_info->tsc_mode = 1;
        }
    }
    b_info->sched_params.weight = 1000;
    b_info->max_memkb = def->mem.max_balloon;
    b_info->target_memkb = def->mem.cur_balloon;
    if (hvm) {
        char bootorder[VIR_DOMAIN_BOOT_LAST + 1];

        libxl_defbool_set(&b_info->u.hvm.pae,
                          def->features & (1 << VIR_DOMAIN_FEATURE_PAE));
        libxl_defbool_set(&b_info->u.hvm.apic,
                          def->features & (1 << VIR_DOMAIN_FEATURE_APIC));
        libxl_defbool_set(&b_info->u.hvm.acpi,
                          def->features & (1 << VIR_DOMAIN_FEATURE_ACPI));
        for (i = 0; i < def->clock.ntimers; i++) {
            if (def->clock.timers[i]->name == VIR_DOMAIN_TIMER_NAME_HPET &&
                def->clock.timers[i]->present == 1) {
                libxl_defbool_set(&b_info->u.hvm.hpet, 1);
            }
        }
        for (i = 0 ; i < def->os.nBootDevs ; i++) {
            switch (def->os.bootDevs[i]) {
                case VIR_DOMAIN_BOOT_FLOPPY:
                    bootorder[i] = 'a';
                    break;
                default:
                case VIR_DOMAIN_BOOT_DISK:
                    bootorder[i] = 'c';
                    break;
                case VIR_DOMAIN_BOOT_CDROM:
                    bootorder[i] = 'd';
                    break;
                case VIR_DOMAIN_BOOT_NET:
                    bootorder[i] = 'n';
                    break;
            }
        }
        if (def->os.nBootDevs == 0) {
            bootorder[0] = 'c';
            bootorder[1] = '\0';
        }
        else {
            bootorder[def->os.nBootDevs] = '\0';
        }
        if ((b_info->u.hvm.boot = strdup(bootorder)) == NULL) {
            virReportOOMError();
            goto error;
        }

        /*
         * The following comment and calculation were taken directly from
         * libxenlight's internal function libxl_get_required_shadow_memory():
         *
         * 256 pages (1MB) per vcpu, plus 1 page per MiB of RAM for the P2M map,
         * plus 1 page per MiB of RAM to shadow the resident processes.
         */
        b_info->shadow_memkb = 4 * (256 * libxl_bitmap_count_set(&b_info->avail_vcpus) +
                                    2 * (b_info->max_memkb / 1024));
    } else {
        if (def->os.bootloader) {
            if ((b_info->u.pv.bootloader = strdup(def->os.bootloader)) == NULL) {
                virReportOOMError();
                goto error;
            }
        }
        if (def->os.bootloaderArgs) {
            if (!(b_info->u.pv.bootloader_args =
                  virStringSplit(def->os.bootloaderArgs, " \t\n", 0)))
                goto error;
        }
        if (def->os.cmdline) {
            if ((b_info->u.pv.cmdline = strdup(def->os.cmdline)) == NULL) {
                virReportOOMError();
                goto error;
            }
        }
        if (def->os.kernel) {
            /* libxl_init_build_info() sets kernel.path = strdup("hvmloader") */
            VIR_FREE(b_info->u.pv.kernel);
            if ((b_info->u.pv.kernel = strdup(def->os.kernel)) == NULL) {
                virReportOOMError();
                goto error;
            }
        }
        if (def->os.initrd) {
            if ((b_info->u.pv.ramdisk = strdup(def->os.initrd)) == NULL) {
                virReportOOMError();
                goto error;
            }
        }
    }

    return 0;

error:
    libxl_domain_build_info_dispose(b_info);
    return -1;
}

int
libxlMakeDisk(virDomainDiskDefPtr l_disk, libxl_device_disk *x_disk)
{
    if (l_disk->src && (x_disk->pdev_path = strdup(l_disk->src)) == NULL) {
        virReportOOMError();
        return -1;
    }

    if (l_disk->dst && (x_disk->vdev = strdup(l_disk->dst)) == NULL) {
        virReportOOMError();
        return -1;
    }

    if (l_disk->driverName) {
        if (STREQ(l_disk->driverName, "tap") ||
            STREQ(l_disk->driverName, "tap2")) {
            switch (l_disk->format) {
            case VIR_STORAGE_FILE_QCOW:
                x_disk->format = LIBXL_DISK_FORMAT_QCOW;
                x_disk->backend = LIBXL_DISK_BACKEND_QDISK;
                break;
            case VIR_STORAGE_FILE_QCOW2:
                x_disk->format = LIBXL_DISK_FORMAT_QCOW2;
                x_disk->backend = LIBXL_DISK_BACKEND_QDISK;
                break;
            case VIR_STORAGE_FILE_VHD:
                x_disk->format = LIBXL_DISK_FORMAT_VHD;
                x_disk->backend = LIBXL_DISK_BACKEND_TAP;
                break;
            case VIR_STORAGE_FILE_NONE:
                /* No subtype specified, default to raw/tap */
            case VIR_STORAGE_FILE_RAW:
                x_disk->format = LIBXL_DISK_FORMAT_RAW;
                x_disk->backend = LIBXL_DISK_BACKEND_TAP;
                break;
            default:
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("libxenlight does not support disk driver %s"),
                               virStorageFileFormatTypeToString(l_disk->format));
                return -1;
            }
        } else if (STREQ(l_disk->driverName, "file")) {
            x_disk->format = LIBXL_DISK_FORMAT_RAW;
            x_disk->backend = LIBXL_DISK_BACKEND_TAP;
        } else if (STREQ(l_disk->driverName, "phy")) {
            x_disk->format = LIBXL_DISK_FORMAT_RAW;
            x_disk->backend = LIBXL_DISK_BACKEND_PHY;
        } else {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("libxenlight does not support disk driver %s"),
                           l_disk->driverName);
            return -1;
        }
    } else {
        /* No driverName - default to raw/tap?? */
        x_disk->format = LIBXL_DISK_FORMAT_RAW;
        x_disk->backend = LIBXL_DISK_BACKEND_TAP;
    }

    /* XXX is this right? */
    x_disk->removable = 1;
    x_disk->readwrite = !l_disk->readonly;
    x_disk->is_cdrom = l_disk->device == VIR_DOMAIN_DISK_DEVICE_CDROM ? 1 : 0;
    if (l_disk->transient) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("libxenlight does not support transient disks"));
        return -1;
    }

    return 0;
}

static int
libxlMakeDiskList(virDomainDefPtr def, libxl_domain_config *d_config)
{
    virDomainDiskDefPtr *l_disks = def->disks;
    int ndisks = def->ndisks;
    libxl_device_disk *x_disks;
    int i;

    if (VIR_ALLOC_N(x_disks, ndisks) < 0) {
        virReportOOMError();
        return -1;
    }

    for (i = 0; i < ndisks; i++) {
        if (libxlMakeDisk(l_disks[i], &x_disks[i]) < 0)
            goto error;
    }

    d_config->disks = x_disks;
    d_config->num_disks = ndisks;

    return 0;

error:
    for (i = 0; i < ndisks; i++)
        libxl_device_disk_dispose(&x_disks[i]);
    VIR_FREE(x_disks);
    return -1;
}

int
libxlMakeNic(virDomainNetDefPtr l_nic, libxl_device_nic *x_nic)
{
    /* TODO: Where is mtu stored?
     *
     * x_nics[i].mtu = 1492;
     */

    virMacAddrGetRaw(&l_nic->mac, x_nic->mac);

    if (l_nic->model && !STREQ(l_nic->model, "netfront")) {
        if ((x_nic->model = strdup(l_nic->model)) == NULL) {
            virReportOOMError();
            return -1;
        }
        x_nic->nictype = LIBXL_NIC_TYPE_VIF_IOEMU;
    } else {
        x_nic->nictype = LIBXL_NIC_TYPE_VIF;
    }

    if (l_nic->ifname && (x_nic->ifname = strdup(l_nic->ifname)) == NULL) {
        virReportOOMError();
        return -1;
    }

    if (l_nic->type == VIR_DOMAIN_NET_TYPE_BRIDGE) {
        if (l_nic->data.bridge.brname &&
            (x_nic->bridge = strdup(l_nic->data.bridge.brname)) == NULL) {
            virReportOOMError();
            return -1;
        }
        if (l_nic->script &&
            (x_nic->script = strdup(l_nic->script)) == NULL) {
            virReportOOMError();
            return -1;
        }
    } else {
        if (l_nic->script) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                           _("scripts are not supported on interfaces of type %s"),
                           virDomainNetTypeToString(l_nic->type));
            return -1;
        }
    }

    return 0;
}

static int
libxlMakeNicList(virDomainDefPtr def,  libxl_domain_config *d_config)
{
    virDomainNetDefPtr *l_nics = def->nets;
    int nnics = def->nnets;
    libxl_device_nic *x_nics;
    int i;

    if (VIR_ALLOC_N(x_nics, nnics) < 0) {
        virReportOOMError();
        return -1;
    }

    for (i = 0; i < nnics; i++) {
        x_nics[i].devid = i;

        if (libxlMakeNic(l_nics[i], &x_nics[i]))
            goto error;
    }

    d_config->nics = x_nics;
    d_config->num_nics = nnics;

    return 0;

error:
    for (i = 0; i < nnics; i++)
        libxl_device_nic_dispose(&x_nics[i]);
    VIR_FREE(x_nics);
    return -1;
}

int
libxlMakeVfb(libxlDriverPrivatePtr driver,
             virDomainGraphicsDefPtr l_vfb,
             libxl_device_vfb *x_vfb)
{
    int port;
    const char *listenAddr;

    switch (l_vfb->type) {
        case VIR_DOMAIN_GRAPHICS_TYPE_SDL:
            libxl_defbool_set(&x_vfb->sdl.enable, 1);
            if (l_vfb->data.sdl.display &&
                (x_vfb->sdl.display = strdup(l_vfb->data.sdl.display)) == NULL) {
                virReportOOMError();
                return -1;
            }
            if (l_vfb->data.sdl.xauth &&
                (x_vfb->sdl.xauthority =
                    strdup(l_vfb->data.sdl.xauth)) == NULL) {
                virReportOOMError();
                return -1;
            }
            break;
        case  VIR_DOMAIN_GRAPHICS_TYPE_VNC:
            libxl_defbool_set(&x_vfb->vnc.enable, 1);
            /* driver handles selection of free port */
            libxl_defbool_set(&x_vfb->vnc.findunused, 0);
            if (l_vfb->data.vnc.autoport) {
                port = libxlNextFreeVncPort(driver, LIBXL_VNC_PORT_MIN);
                if (port < 0) {
                    virReportError(VIR_ERR_INTERNAL_ERROR,
                                   "%s", _("Unable to find an unused VNC port"));
                    return -1;
                }
                l_vfb->data.vnc.port = port;
            }
            x_vfb->vnc.display = l_vfb->data.vnc.port - LIBXL_VNC_PORT_MIN;

            listenAddr = virDomainGraphicsListenGetAddress(l_vfb, 0);
            if (listenAddr) {
                /* libxl_device_vfb_init() does strdup("127.0.0.1") */
                VIR_FREE(x_vfb->vnc.listen);
                if ((x_vfb->vnc.listen = strdup(listenAddr)) == NULL) {
                    virReportOOMError();
                    return -1;
                }
            }
            if (l_vfb->data.vnc.keymap &&
                (x_vfb->keymap =
                    strdup(l_vfb->data.vnc.keymap)) == NULL) {
                virReportOOMError();
                return -1;
            }
            break;
    }

    return 0;
}

static int
libxlMakeVfbList(libxlDriverPrivatePtr driver,
                 virDomainDefPtr def,
                 libxl_domain_config *d_config)
{
    virDomainGraphicsDefPtr *l_vfbs = def->graphics;
    int nvfbs = def->ngraphics;
    libxl_device_vfb *x_vfbs;
    libxl_device_vkb *x_vkbs;
    int i;

    if (nvfbs == 0)
        return 0;

    if (VIR_ALLOC_N(x_vfbs, nvfbs) < 0) {
        virReportOOMError();
        return -1;
    }
    if (VIR_ALLOC_N(x_vkbs, nvfbs) < 0) {
        virReportOOMError();
        VIR_FREE(x_vfbs);
        return -1;
    }

    for (i = 0; i < nvfbs; i++) {
        libxl_device_vfb_init(&x_vfbs[i]);
        libxl_device_vkb_init(&x_vkbs[i]);

        if (libxlMakeVfb(driver, l_vfbs[i], &x_vfbs[i]) < 0)
            goto error;
    }

    d_config->vfbs = x_vfbs;
    d_config->vkbs = x_vkbs;
    d_config->num_vfbs = d_config->num_vkbs = nvfbs;

    return 0;

error:
    for (i = 0; i < nvfbs; i++) {
        libxl_device_vfb_dispose(&x_vfbs[i]);
        libxl_device_vkb_dispose(&x_vkbs[i]);
    }
    VIR_FREE(x_vfbs);
    VIR_FREE(x_vkbs);
    return -1;
}

virCapsPtr
libxlMakeCapabilities(libxl_ctx *ctx)
{
    libxl_physinfo phy_info;
    const libxl_version_info *ver_info;

    regcomp(&xen_cap_rec, xen_cap_re, REG_EXTENDED);

    if (libxl_get_physinfo(ctx, &phy_info) != 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to get node physical info from libxenlight"));
        return NULL;
    }

    if ((ver_info = libxl_get_version_info(ctx)) == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Failed to get version info from libxenlight"));
        return NULL;
    }

    return libxlMakeCapabilitiesInternal(virArchFromHost(),
                                         &phy_info,
                                         ver_info->capabilities);
}

int
libxlBuildDomainConfig(libxlDriverPrivatePtr driver,
                       virDomainDefPtr def, libxl_domain_config *d_config)
{

    if (libxlMakeDomCreateInfo(driver, def, &d_config->c_info) < 0)
        return -1;

    if (libxlMakeDomBuildInfo(def, d_config) < 0) {
        goto error;
    }

    if (libxlMakeDiskList(def, d_config) < 0) {
        goto error;
    }

    if (libxlMakeNicList(def, d_config) < 0) {
        goto error;
    }

    if (libxlMakeVfbList(driver, def, d_config) < 0) {
        goto error;
    }

    d_config->on_reboot = def->onReboot;
    d_config->on_poweroff = def->onPoweroff;
    d_config->on_crash = def->onCrash;

    return 0;

error:
    libxl_domain_config_dispose(d_config);
    return -1;
}
