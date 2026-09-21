.section .text
.global switch_task
.type switch_task, @function
/* void switch_task(uint32_t* old_esp_store, uint32_t new_esp) */
switch_task:
    push %ebp
    push %ebx
    push %esi
    push %edi
    pushf

    mov 24(%esp), %eax   /* old_esp_store */
    mov %esp, (%eax)

    mov 28(%esp), %ecx   /* new_esp -- read before we clobber %esp */
    mov %ecx, %esp

    popf
    pop %edi
    pop %esi
    pop %ebx
    pop %ebp
    ret
.size switch_task, . - switch_task
