/* Build guard for navd, force-included before navd.c (build.sh -include).
 *
 * navd reads the DVD VM's registers through libdvdnav's internal headers
 * (vm/vm.h, dvdnav_internal.h), which change between releases without
 * notice. It is therefore pinned to libdvdnav 7.0.0: any other version fails
 * the build here instead of misreading registers at run time.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NAVD_VERSION_CHECK_H
#define NAVD_VERSION_CHECK_H
#include <dvdnav/version.h>
#if !defined(DVDNAV_VERSION_MAJOR) || DVDNAV_VERSION_MAJOR != 7 || DVDNAV_VERSION_MINOR != 0 || DVDNAV_VERSION_MICRO != 0
#error "navd needs libdvdnav 7.0.0 exactly: it reads the VM through internal headers"
#endif
#endif
