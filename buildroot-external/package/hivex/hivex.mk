################################################################################
#
# hivex
#
################################################################################

HIVEX_VERSION = 1.3.23
HIVEX_SITE = https://download.libguestfs.org/hivex
HIVEX_SOURCE = hivex-$(HIVEX_VERSION).tar.gz
HIVEX_LICENSE = LGPL-2.1+
HIVEX_LICENSE_FILES = COPYING
HIVEX_INSTALL_STAGING = YES
HIVEX_DEPENDENCIES = libxml2
HIVEX_CONF_OPTS = --disable-ocaml --disable-perl --disable-python --disable-ruby

$(eval $(autotools-package))