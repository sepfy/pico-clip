/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pico/bootrom.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "Rebooting...");
	k_sleep(K_MSEC(100));
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

static int cmd_uf2_bootloader(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Rebooting to RP2350 BOOTSEL/UF2 mode...");
	k_sleep(K_MSEC(100));

	reset_usb_boot(0, 0);

	CODE_UNREACHABLE;
}

SHELL_CMD_REGISTER(uf2, NULL, "Reboot to RP2350 BOOTSEL/UF2 mode.", cmd_uf2_bootloader);
SHELL_CMD_REGISTER(bootsel, NULL, "Reboot to RP2350 BOOTSEL/UF2 mode.", cmd_uf2_bootloader);
SHELL_CMD_REGISTER(reboot, NULL, "Cold reboot the device.", cmd_reboot);
