#ifndef _APPROVAL_PROTOCOL_H
#define _APPROVAL_PROTOCOL_H

#include <stdint.h>

#define APPROVAL_SOCK_PATH "/run/approval.sock"

#define APPROVAL_MSG_REQUEST  1
#define APPROVAL_MSG_RESOLVE  2
#define APPROVAL_MSG_CANCEL   3

#define APPROVAL_STATUS_PENDING 0
#define APPROVAL_STATUS_ALLOW   1
#define APPROVAL_STATUS_DENY    2
#define APPROVAL_STATUS_SESSION 3

struct approval_msg_header {
    uint32_t len;
    uint16_t type;
    uint16_t flags;
    uint32_t call_id;
};

struct approval_msg_request {
    uint32_t approval_id;
    uint32_t agent_id;
    char agent_name[32];
    char skill[64];
    char args_summary[256];
    char args_hash[64];
    uint32_t sensitive;
    uint32_t timeout_ms;
};

struct approval_msg_resolve {
    uint32_t approval_id;
    uint32_t status; /* 1=allow_once, 2=deny, 3=allow_session */
};

struct approval_msg_cancel {
    uint32_t approval_id;
};

#endif /* _APPROVAL_PROTOCOL_H */

