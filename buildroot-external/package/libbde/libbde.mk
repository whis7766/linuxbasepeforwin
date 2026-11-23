################################################################################
#
# libbde
#
################################################################################

LIBBDE_VERSION = 20260916
LIBBDE_SITE = https://github.com/libyal/libbde/releases/download/$(LIBBDE_VERSION)
LIBBDE_SOURCE = libbde-alpha-$(LIBBDE_VERSION).tar.gz
LIBBDE_LICENSE = LGPL-3.0+
LIBBDE_LICENSE_FILES = COPYING
LIBBDE_INSTALL_STAGING = YES
LIBBDE_DEPENDENCIES = libopenssl
LIBBDE_CONF_OPTS = --disable-nls --without-libfuse

$(eval $(autotools-package))
