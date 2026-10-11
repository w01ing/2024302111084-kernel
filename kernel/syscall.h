#ifndef LAB2_SYSCALL_H
#define LAB2_SYSCALL_H

/*
 * LAB2-STAGE1
 *
 * Stage 1 needs one known syscall for the round-trip self-test.  The number
 * comes from the supplied ABI file, where SYS_sync is 22.  Unknown numbers
 * must still return -1, so this tiny header is enough before the full
 * syscall table is implemented.
 */
#define SYS_sync 22

#endif
