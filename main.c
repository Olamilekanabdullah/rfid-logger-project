/*
 * main.c - entry point
 *
 * Phase 1: a small command menu (dump log, add tag, clear log).
 * Phase 2: the scan loop, which runs the state machine until input ends.
 */

#include <stdio.h>
#include <stdlib.h>
#include "tags.h"

int main(void)
{
    /* open the log file, load the whitelist, reset the ring buffer */
    system_init();

    int choice = 0;
    /* menu repeats until "Start scanning" (4); option 5 exits the program
     * from inside close_program()
     */
    while (choice != 4) {
        printf("\n1: Dump log\n2: Add tag\n3: Clear log\n4: Start scanning\n5: Close program\n> ");
        /* NOTE: return value not checked yet - non-numeric input isn't handled */
        scanf("%d", &choice);
        getchar();   /* swallow the Enter left behind by scanf */

        switch (choice) {
            case 1:
                dump_log();
                break;
            case 2:
                add_tag();
                break;
            case 3:
                clear_log();
                break;
            case 4:
                break;   /* falls out of the loop; scanning starts below */
            case 5:
                close_program();
                break;
            default:
                printf("Invalid choice.\n");
                break;
        }
    }

    /* The state machine. Each pass calls the current state's handler through
     * the dispatch table and moves to the state it returns. The loop ends when
     * scan_ended is set (EOF, or 'q' typed as a quit key while testing).
     */
    system_state current_state = STATE_IDLE;
    while (scan_ended == false) {
        current_state = sp[current_state]();
    }

    return 0;
}
