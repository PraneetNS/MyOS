#ifndef _LINUX_KD_H
#define _LINUX_KD_H

#define KD_TEXT         0x00
#define KD_GRAPHICS     0x01
#define KDGETMODE       0x4B3B
#define KDSETMODE       0x4B3C

#define K_RAW           0x00
#define K_XLATE         0x01
#define K_MEDIUMRAW     0x02
#define K_UNICODE       0x03
#define K_OFF           0x04
#define KDGKBMODE       0x4B44
#define KDSKBMODE       0x4B45

#endif
