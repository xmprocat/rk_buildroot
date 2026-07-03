KMOD_LCD_SITE = $(TOPDIR)/package/kmod_lcd
KMOD_LCD_SITE_METHOD = local
KMOD_LCD_VERSION = 1.0

KMOD_LCD_KERNEL = /mnt/rk3506dev/RK3506_LINUX_SDK_0630/kernel-6.1
KMOD_LCD_CROSS  = /mnt/rk3506dev/RK3506_LINUX_SDK_0630/prebuilts/gcc/linux-x86/arm/gcc-arm-10.3-2021.07-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-

define KMOD_LCD_BUILD_CMDS
	$(MAKE) -C $(KMOD_LCD_KERNEL) M=$(@D) ARCH=arm \
		CROSS_COMPILE=$(KMOD_LCD_CROSS) modules
endef

define KMOD_LCD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/lcd.ko $(TARGET_DIR)/usr/lib/modules/extra/lcd.ko
	$(INSTALL) -D -m 0644 $(KMOD_LCD_SITE)/S02lcd.service \
		$(TARGET_DIR)/usr/lib/systemd/system/S02lcd.service
	mkdir -p $(TARGET_DIR)/etc/systemd/system/sysinit.target.wants
	ln -sf /usr/lib/systemd/system/S02lcd.service \
		$(TARGET_DIR)/etc/systemd/system/sysinit.target.wants/S02lcd.service
endef

$(eval $(generic-package))
