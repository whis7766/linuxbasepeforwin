################################################################################
#
# lbpe-gui - integrated Qt6 maintenance GUI (local source)
#
################################################################################

LBPE_GUI_VERSION = 1.0
LBPE_GUI_SITE = $(BR2_EXTERNAL_LINUXBASEPE_PATH)/../src/gui
LBPE_GUI_SITE_METHOD = local
LBPE_GUI_LICENSE = GPL-3.0+
LBPE_GUI_DEPENDENCIES = qt6base wimlib hivex libbde
LBPE_GUI_INSTALL_TARGET = YES

$(eval $(cmake-package))