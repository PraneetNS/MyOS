#ifndef ERRNO_H
#define ERRNO_H

#define EPERM        1   /* Operation not permitted */
#define ENOENT       2   /* No such file or directory */
#define ESRCH        3   /* No such process */
#define EINTR        4   /* Interrupted system call */
#define EIO          5   /* I/O error */
#define ENXIO        6   /* No such device or address */
#define E2BIG        7   /* Argument list too long */
#define ENOEXEC      8   /* Exec format error */
#define EBADF        9   /* Bad file number */
#define ECHILD      10   /* No child processes */
#define EAGAIN      11   /* Try again */
#define ENOMEM      12   /* Out of memory */
#define EACCES      13   /* Permission denied */
#define EFAULT      14   /* Bad address */
#define EEXIST      17   /* File exists */
#define ENODEV      19   /* No such device */
#define ENOTDIR     20   /* Not a directory */
#define EISDIR      21   /* Is a directory */
#define EINVAL      22   /* Invalid argument */
#define ENFILE      23   /* File table overflow */
#define EMFILE      24   /* Too many open files */
#define ENOTTY      25   /* Not a typewriter */
#define EFBIG        27   /* File too large */
#define ENOSPC      28   /* No space left on device */
#define ESPIPE      29   /* Illegal seek */
#define EROFS       30   /* Read-only file system */
#define EPIPE       32   /* Broken pipe */
#define ERANGE      34   /* Math result not representable */
#define ENAMETOOLONG 36  /* File name too long */
#define ENOSYS      38   /* Function not implemented */
#define ENOTEMPTY   39   /* Directory not empty */
#define ENOTSOCK     108  /* Socket operation on non-socket */
#define EDESTADDRREQ 121  /* Destination address required */
#define EMSGSIZE     122  /* Message too long */
#define EPROTOTYPE   107  /* Protocol wrong type for socket */
#define ENOPROTOOPT  109  /* Protocol not available */
#define EPROTONOSUPPORT 123 /* Protocol not supported */
#define ESOCKTNOSUPPORT 124 /* Socket type not supported */
#define EOPNOTSUPP   95   /* Operation not supported on transport endpoint */
#define EPFNOSUPPORT 96   /* Protocol family not supported */
#define EAFNOSUPPORT 106  /* Address family not supported by protocol */
#define EADDRINUSE   112  /* Address already in use */
#define EADDRNOTAVAIL 125 /* Cannot assign requested address */
#define ENETDOWN     115  /* Network is down */
#define ENETUNREACH  114  /* Network is unreachable */
#define ENETRESET    126  /* Network dropped connection because of reset */
#define ECONNABORTED 113  /* Software caused connection abort */
#define ECONNRESET   104  /* Connection reset by peer */
#define ENOBUFS      105  /* No buffer space available */
#define EISCONN      127  /* Transport endpoint is already connected */
#define ENOTCONN     128  /* Transport endpoint is not connected */
#define ESHUTDOWN    110  /* Cannot send after transport endpoint shutdown */
#define ETOOMANYREFS 129  /* Too many references: cannot splice */
#define ETIMEDOUT    116  /* Connection timed out */
#define ECONNREFUSED 111  /* Connection refused */
#define EDQUOT       132  /* Quota exceeded */

#endif
