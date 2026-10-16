#ifndef _SYS_REBOOT_H
#define _SYS_REBOOT_H

#ifdef __cplusplus
extern "C" {
#endif

#define LINUX_REBOOT_MAGIC1       0xfee1dead
#define LINUX_REBOOT_MAGIC2       672274793

#define LINUX_REBOOT_CMD_RESTART   0x01234567
#define LINUX_REBOOT_CMD_HALT      0xcdef0123
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321fedc
#define LINUX_REBOOT_CMD_RESTART2  0xa1b2c3d4

#define RB_AUTOBOOT    LINUX_REBOOT_CMD_RESTART
#define RB_HALT_SYSTEM LINUX_REBOOT_CMD_HALT
#define RB_POWER_OFF   LINUX_REBOOT_CMD_POWER_OFF

int reboot(int cmd);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_REBOOT_H */
