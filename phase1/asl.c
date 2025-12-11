#include "./headers/asl.h"
#include "./headers/pcb.h"

static semd_t semd_table[MAXPROC];
static struct list_head semdFree_h;
static struct list_head semd_h;

/*Inizializza la lista semdFree inserendo al suo interno tutti gli elementi dell’array statico:
static semd_t semdTable[MAXPROC]
static semd_t semdTable[MAXPROC]*/
void initASL() {

}

/*Inserisce il PCB p in fondo alla coda dei processi bloccati sul semaforo con indirizzo semAdd,
e imposta in p il campo p->semAdd = semAdd.

Se il semaforo non esiste nell’ASL:
• alloca un nuovo descrittore dalla lista semdFree
• inizializzalo
• inseriscilo nell’ASL nella posizione corretta
• poi inserisci p nella sua coda

Se serve allocare un nuovo descrittore ma semdFree è vuota, restituisce TRUE (errore).

In tutti gli altri casi restituisce FALSE (operazione riuscita).*/
int insertBlocked(int* semAdd, pcb_t* p) {
    // inserisco il pcb p nella coda dei processi bloccati sul semaforo con indirizzo semAdd
    // cerco il descrittore di semaforo nella ASL
    semd_t* semd = NULL;
    struct list_head* pos;
    list_for_each(pos, &semd_h) {
        semd_t* current = container_of(pos, semd_t, s_link);
        if (current->s_key == semAdd) {
            semd = current;
            break;
        }
    }
    // se non l'ho trovato, devo allocarne uno nuovo
    if (semd == NULL) {
        // se la lista dei semd liberi è vuota, ritorno TRUE (errore)
        if (list_empty(&semdFree_h)) {
            return TRUE;
        }
        // altrimenti prendo il primo semd dalla lista dei semd liberi
        struct list_head* first = semdFree_h.next;
        list_del(first);
        semd = container_of(first, semd_t, s_link);
        // inizializzo il semd
        semd->s_key = semAdd;
        mkEmptyProcQ(&semd->s_procq);
        // inserisco il semd nella ASL
        struct list_head* insert_pos = &semd_h;;
        list_for_each(pos, &semd_h) {
            semd_t* current = container_of(pos, semd_t, s_link);
            if (current->s_key > semAdd) {
                break;
            }
            insert_pos = pos;
        }
        list_add(&semd->s_link, insert_pos->prev);
    }
    // inserisco p nella coda dei processi bloccati sul semaforo
    insertProcQ(&semd->s_procq, p);
    // imposto in p il campo p->semAdd = semAdd
    p->p_semAdd = semAdd;
    return FALSE;
}
/*Cerca nell’ASL il descrittore del semaforo con indirizzo semAdd.

• Se non esiste → restituisce NULL
• Se esiste → rimuove il primo PCB dalla sua coda e lo restituisce

Se, dopo la rimozione, la coda del semaforo diventa vuota, rimuove il descrittore dall’ASL e lo rimette nella semdFree.*/
pcb_t* removeBlocked(int* semAdd) {
    semd_t* semd = NULL;
    struct list_head* pos;
    list_for_each(pos, &semd_h) {
        semd_t* current = container_of(pos, semd_t, s_link);
        if (current->s_key == semAdd) {
            semd = current;
            break;
        }
    }
    if(semd == NULL) {
        return NULL;
    } else {
        pcb_t* p = outProcQ(&semd->s_procq, headProcQ(&semd->s_procq));
        // se la coda del semaforo è vuota dopo la rimozione, rimuovo il descrittore dall'ASL e lo rimetto nella semdFree
        if (emptyProcQ(&semd->s_procq)) {
            list_del(&semd->s_link);
            list_add(&semd->s_link, &semdFree_h);
        }
        return p;
    }
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
