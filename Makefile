default : all

######################################
## General configuration
######################################
XEN_VER		?= 4.2.0
XEN_ROOT	?= $(realpath ../../xen-$(XEN_VER))
TOOLCHAIN_ROOT	?= $(realpath ../../toolchain.xen.$(XEN_VER))
GCC_VERSION	?= 4.5.0
verbose		?=
stubdom		 = y

#CFLAGS          += -Winline -Wtype-limits -Wcast-align -DDEBUG_SHELL
CFLAGS          += -Winline -Wtype-limits -Wcast-align --param large-stack-frame=256 --param large-stack-frame-growth=16 -DDEBUG_SHELL

CONFIG_START_NETWORK		= n
# use 'vale' for xenbus driver instead of 'vif'
CONFIG_NETMAP_XENBUS		= y
# POSIX netmap implementation
CONFIG_NETMAP			= y
CONFIG_NETMAP_API		= 4
CONFIG_MEMPOOL			= y
CONFIG_LWIP			= y
CONFIG_LWIP_MINIMAL		= y
CONFIG_LWIP_SINGLETHREADED 	= y

# enable NM_WRAP API/lwip-netif only
CONFIG_NETFRONT			= n
CONFIG_NETFRONT_NETMAP2		= n
CONFIG_NMWRAP			= y
CONFIG_NMWRAP_SYNCRX		= n

######################################
## Debugging options
######################################
CONFIG_DEBUG			= y
CONFIG_DEBUG_LWIP		= n

######################################
## MiniOS path
######################################
MINI_OS_ROOT	= $(realpath ./mini-os/)

######################################
## Stubdomain
######################################
STUBDOM_NAME	= minicache
STUBDOM_ROOT	= $(realpath .)
STUB_APP_OBJS0  = main.o pktbuf.o blkdev.o
STUB_APP_OBJS	= $(addprefix $(STUB_APP_OBJ_DIR)/,$(STUB_APP_OBJS0))

include $(MINI_OS_ROOT)/stub.mk
