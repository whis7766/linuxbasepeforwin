# Include custom package makefiles
include $(sort $(wildcard $(BR2_EXTERNAL_LINUXBASEPE_PATH)/package/*/*.mk))