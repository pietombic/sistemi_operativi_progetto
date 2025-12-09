#include "./headers/asl.h"
#include "./headers/pcb.h"

static semd_t semd_table[MAXPROC];
static struct list_head semdFree_h;
static struct list_head semd_h;

void initASL() {
}

int insertBlocked(int* semAdd, pcb_t* p) {
}

pcb_t* removeBlocked(int* semAdd) {
}
/*
remove the PCB pointed to by p from the
process queue associated with p’s semaphore on the ASL. If PCB pointed to by p
does not appear in the process queue associated with p’s semaphore, which is an
error condition, return NULL; otherwise, return p
*/
pcb_t* outBlocked(pcb_t* p) {
    // Cerca il descrittore di semaforo nella ASL
    semd_t* semd = NULL;
    struct list_head* pos;
    
    list_for_each(pos, &semd_h) {
        semd_t* current = container_of(pos, semd_t, s_link);
        if (current->s_key == p->p_semAdd) {
            semd = current;
            break;
        }
    }
    
    // Se non trovato, ritorna NULL
    if (semd == NULL) {
        return NULL;
    }
    
    return outProcQ(&semd->s_procq, p);
}
/*
return a pointer to the PCB that is at
the head of the process queue associated with the semaphore semAdd. Return
NULL if semAdd is not found on the ASL or if the process queue associated with
semAdd is empty.
*/
pcb_t* headBlocked(int* semAdd) {
    // Cerca il descrittore di semaforo nella ASL
    semd_t* semd = NULL;
    struct list_head* pos;
    
    list_for_each(pos, &semd_h) {
        semd_t* current = container_of(pos, semd_t, s_link);
        if (current->s_key == semAdd) {
            semd = current;
            break;
        }
    }
    
    // Se non trovato, ritorna NULL
    if (semd == NULL) {
        return NULL;
    }
    
    // Se trovato, ritorna la testa della coda di processi
    // (equivalente a headProcQ su semd->s_procQ)
    return headProcQ(&semd->s_procq);
}
