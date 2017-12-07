################################################################################
#
# mediabox
#
################################################################################

MEDIABOX_VERSION = staging

# this file is run from /sdk/build/x86_64/qemu/output/build/<tempdir>/
MEDIABOX_SITE = ../../../../../../../.git
MEDIABOX_SITE_METHOD = git
MEDIABOX_REDISTRIBUTE = NO
MEDIABOX_LICENSE = OTHER
MEDIABOX_LICENSE_FILES = COPYING
MEDIABOX_INSTALL_STAGING = NO
MEDIABOX_AUTORECONF = YES
MEDIABOX_DEPENDENCIES = libcurl libupnp directfb openssl mediatomb avmount bluealsa

MEDIABOX_CONF_OPTS = --without-systemd

ifeq ($(BR2_PACKAGE_MEDIABOX_DEBUG),y)
MEDIABOX_CONF_OPTS += --enable-debug
endif

ifeq ($(BR2_PACKAGE_MEDIABOX_BLUETOOTH),y)
MEDIABOX_CONF_OPTS += --enable-bluetooth
endif

# we need xorg here just so we can get a
# libGL. Once we support OpenGL ES we won't
# need this
ifeq ($(BR2_PACKAGE_MEDIABOX_LIBDRM),y)
MEDIABOX_CONF_OPTS += --enable-libdrm
MEDIABOX_DEPENDENCIES += libdrm mesa3d xserver_xorg-server
endif

ifeq ($(BR2_PACKAGE_MEDIABOX_X11),y)
MEDIABOX_CONF_OPTS += --enable-x11
MEDIABOX_DEPENDENCIES += xserver_xorg-server mesa3d
endif

ifeq ($(BR2_PACKAGE_MEDIABOX_DVD),y)
MEDIABOX_CONF_OPTS += --enable-dvd
MEDIABOX_DEPENDENCIES += libdvdnav
endif

# remove from package cache
define MEDIABOX_UNCACHE
rm -f $(DL_DIR)/mediabox-staging.tar.gz
endef

MEDIABOX_PRE_DOWNLOAD_HOOKS += MEDIABOX_UNCACHE

$(eval $(autotools-package))
