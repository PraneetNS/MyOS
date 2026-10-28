#ifndef _LIBINTENT_INTENT_H
#define _LIBINTENT_INTENT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <intent_protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*intent_skill_handler_t)(const char* skill, const char* args_json,
                                      char* result_json, size_t result_cap,
                                      char* err_buf, size_t err_cap,
                                      void* udata);

int intent_connect(void);
void intent_close(int fd);

int intent_register_skill(int fd, const char* name, const char* description,
                          const char* input_schema_json, const char* output_schema_json,
                          bool sensitive, intent_skill_handler_t handler, void* udata);

int intent_unregister_skill(int fd, const char* name);

int intent_serve(int fd);
void intent_break_serve(void);

int intent_call(int fd, const char* skill, const char* json_args,
                char* json_result, size_t result_cap, int timeout_ms);

int intent_list_skills(int fd, char* out_json, size_t cap);
int intent_list_approvals(int fd, char* out_json, size_t cap);
int intent_resolve_approval(int fd, uint32_t approval_id, bool approved,
                            char* err_buf, size_t err_cap);
int intent_get_stats(int fd, char* out_json, size_t cap);

int intent_get_caller_agent(void);
int intent_get_caller_pid(void);

#ifdef __cplusplus
}
#endif

#endif /* _LIBINTENT_INTENT_H */
