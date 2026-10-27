#ifndef _INTENT_PROTOCOL_H
#define _INTENT_PROTOCOL_H

#include <stdint.h>

#define INTENT_SOCK_PATH        "/run/intent.sock"
#define INTENT_MAX_MSG_LEN      16384
#define INTENT_DEFAULT_TIMEOUT  5000   /* 5s */
#define INTENT_APPROVAL_TIMEOUT 30000  /* 30s */

/* 12-byte header matching win_proto style */
struct intent_header {
    uint32_t len;      /* Total size including header */
    uint16_t type;     /* INTENT_MSG_* */
    uint16_t flags;    /* Reserved / flags */
    uint32_t call_id;  /* Correlation ID */
};

enum {
    /* Registration */
    INTENT_MSG_REGISTER             = 1,
    INTENT_MSG_REGISTER_ACK         = 2,
    INTENT_MSG_UNREGISTER           = 3,
    INTENT_MSG_UNREGISTER_ACK       = 4,

    /* Discovery */
    INTENT_MSG_LIST                 = 5,
    INTENT_MSG_LIST_ACK             = 6,

    /* Invocation */
    INTENT_MSG_CALL                 = 7,
    INTENT_MSG_CALL_FORWARD         = 8,
    INTENT_MSG_RESULT               = 9,

    /* Approvals */
    INTENT_MSG_LIST_APPROVALS       = 10,
    INTENT_MSG_LIST_APPROVALS_ACK   = 11,
    INTENT_MSG_RESOLVE_APPROVAL     = 12,
    INTENT_MSG_RESOLVE_APPROVAL_ACK = 13,

    /* Bus stats */
    INTENT_MSG_STATS                = 14,
    INTENT_MSG_STATS_ACK            = 15,

    /* Errors */
    INTENT_MSG_ERROR                = 99
};

#endif /* _INTENT_PROTOCOL_H */
