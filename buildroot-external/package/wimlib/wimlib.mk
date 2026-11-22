################################################################################
#
# wimlib
#
################################################################################

WIMLIB_VERSION = 1.14.4
WIMLIB_SITE = https://wimlib.net/downloads
WIMLIB_SOURCE = wimlib-$(WIMLIB_VERSION).tar.gz
WIMLIB_LICENSE = GPL-3.0+ / LGPL-3.0+
WIMLIB_LICENSE_FILES = COPYING COPYING.GPLv3 COPYING.LGPLv3
WIMLIB_INSTALL_STAGING = YES
WIMLIB_DEPENDENCIES = libxml2 openssl ntfs-3g
WIMLIB_CONF_OPTS = --with-ntfs-3g --without-fuse

$(eval $(autotools-package))