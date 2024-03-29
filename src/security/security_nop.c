/*
 * Copyright (C) 2010-2011 Red Hat, Inc.
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

#include "security_nop.h"

#include "virterror_internal.h"

#define VIR_FROM_THIS VIR_FROM_SECURITY

static virSecurityDriverStatus virSecurityDriverProbeNop(const char *virtDriver ATTRIBUTE_UNUSED)
{
    return SECURITY_DRIVER_ENABLE;
}

static int virSecurityDriverOpenNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDriverCloseNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED)
{
    return 0;
}

static const char * virSecurityDriverGetModelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED)
{
    return "none";
}

static const char * virSecurityDriverGetDOINop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED)
{
    return "0";
}

static int virSecurityDomainRestoreImageLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                                 virDomainDefPtr vm ATTRIBUTE_UNUSED,
                                                 virDomainDiskDefPtr disk ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainSetDaemonSocketLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                                    virDomainDefPtr vm ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainSetSocketLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                              virDomainDefPtr vm ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainClearSocketLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                                virDomainDefPtr vm ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainSetImageLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                             virDomainDefPtr vm ATTRIBUTE_UNUSED,
                                             virDomainDiskDefPtr disk ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainRestoreHostdevLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                                   virDomainDefPtr vm ATTRIBUTE_UNUSED,
                                                   virDomainHostdevDefPtr dev ATTRIBUTE_UNUSED,
                                                   const char *vroot ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainSetHostdevLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                               virDomainDefPtr vm ATTRIBUTE_UNUSED,
                                               virDomainHostdevDefPtr dev ATTRIBUTE_UNUSED,
                                               const char *vroot ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainSetSavedStateLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                                  virDomainDefPtr vm ATTRIBUTE_UNUSED,
                                                  const char *savefile ATTRIBUTE_UNUSED)
{
    return 0;
}
static int virSecurityDomainRestoreSavedStateLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                                      virDomainDefPtr vm ATTRIBUTE_UNUSED,
                                                      const char *savefile ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainGenLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                        virDomainDefPtr sec ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainReserveLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                            virDomainDefPtr sec ATTRIBUTE_UNUSED,
                                            pid_t pid ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainReleaseLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                            virDomainDefPtr sec ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainSetAllLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                           virDomainDefPtr sec ATTRIBUTE_UNUSED,
                                           const char *stdin_path ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainRestoreAllLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                               virDomainDefPtr vm ATTRIBUTE_UNUSED,
                                               int migrated ATTRIBUTE_UNUSED)
{
    return 0;
}
static int virSecurityDomainGetProcessLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                               virDomainDefPtr vm ATTRIBUTE_UNUSED,
                                               pid_t pid ATTRIBUTE_UNUSED,
                                               virSecurityLabelPtr sec ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainSetProcessLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                               virDomainDefPtr vm ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainVerifyNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                      virDomainDefPtr def ATTRIBUTE_UNUSED)
{
    return 0;
}

static int virSecurityDomainSetFDLabelNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                          virDomainDefPtr sec ATTRIBUTE_UNUSED,
                                          int fd ATTRIBUTE_UNUSED)
{
    return 0;
}

static char *virSecurityDomainGetMountOptionsNop(virSecurityManagerPtr mgr ATTRIBUTE_UNUSED,
                                                 virDomainDefPtr vm ATTRIBUTE_UNUSED)
{
    char *opts;

    if (!(opts = strdup(""))) {
        virReportOOMError();
        return NULL;
    }
    return opts;
}

virSecurityDriver virSecurityDriverNop = {
    .privateDataLen                     = 0,
    .name                               = "none",
    .probe                              = virSecurityDriverProbeNop,
    .open                               = virSecurityDriverOpenNop,
    .close                              = virSecurityDriverCloseNop,

    .getModel                           = virSecurityDriverGetModelNop,
    .getDOI                             = virSecurityDriverGetDOINop,

    .domainSecurityVerify               = virSecurityDomainVerifyNop,

    .domainSetSecurityImageLabel        = virSecurityDomainSetImageLabelNop,
    .domainRestoreSecurityImageLabel    = virSecurityDomainRestoreImageLabelNop,

    .domainSetSecurityDaemonSocketLabel = virSecurityDomainSetDaemonSocketLabelNop,
    .domainSetSecuritySocketLabel       = virSecurityDomainSetSocketLabelNop,
    .domainClearSecuritySocketLabel     = virSecurityDomainClearSocketLabelNop,

    .domainGenSecurityLabel             = virSecurityDomainGenLabelNop,
    .domainReserveSecurityLabel         = virSecurityDomainReserveLabelNop,
    .domainReleaseSecurityLabel         = virSecurityDomainReleaseLabelNop,

    .domainGetSecurityProcessLabel      = virSecurityDomainGetProcessLabelNop,
    .domainSetSecurityProcessLabel      = virSecurityDomainSetProcessLabelNop,

    .domainSetSecurityAllLabel          = virSecurityDomainSetAllLabelNop,
    .domainRestoreSecurityAllLabel      = virSecurityDomainRestoreAllLabelNop,

    .domainSetSecurityHostdevLabel      = virSecurityDomainSetHostdevLabelNop,
    .domainRestoreSecurityHostdevLabel  = virSecurityDomainRestoreHostdevLabelNop,

    .domainSetSavedStateLabel           = virSecurityDomainSetSavedStateLabelNop,
    .domainRestoreSavedStateLabel       = virSecurityDomainRestoreSavedStateLabelNop,

    .domainSetSecurityImageFDLabel      = virSecurityDomainSetFDLabelNop,
    .domainSetSecurityTapFDLabel        = virSecurityDomainSetFDLabelNop,

    .domainGetSecurityMountOptions      = virSecurityDomainGetMountOptionsNop,
};
