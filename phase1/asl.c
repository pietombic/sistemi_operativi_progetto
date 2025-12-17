#include "./headers/asl.h"
#include "./headers/pcb.h"

static semd_t semd_table[MAXPROC];
static struct list_head semdFree_h;
static struct list_head semd_h;

/*Inizializza la lista semdFree inserendo al suo interno tutti gli elementi dell’array statico:
static semd_t semdTable[MAXPROC]
static semd_t semdTable[MAXPROC]*/

void initASL() {
    // inizializzo la lista dei semd liberi
    INIT_LIST_HEAD(&semdFree_h);

    // inizializzo la ASL (vuota all'inizio)
    INIT_LIST_HEAD(&semd_h);

    // inserisco tutti i semd dell'array nella lista dei liberi
    for (int i = 0; i < MAXPROC; i++) {
        list_add(&semd_table[i].s_link, &semdFree_h);
    }
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
        if (list_empty(&semdFree_h)) {
            return TRUE;
        }
        // prendo il primo semd dalla lista dei semd liberi
        struct list_head* first = semdFree_h.next;
        list_del(first);
        semd = container_of(first, semd_t, s_link);
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

////rimuove e ritorna il PCB in testa alla coda dei processi bloccati sul semaforo con indirizzo semAdd
pcb_t* outBlocked(pcb_t* p) {
    // Cerca il descrittore di semaforo nella ASL
    semd_t* semd = NULL;
    struct list_head* pos;
    //itero tutta la lista dei semd per trovare quello con chiave = p->p_semAdd
    list_for_each(pos, &semd_h) {
        semd_t* current = container_of(pos, semd_t, s_link);
        if (current->s_key == p->p_semAdd) {
            semd = current;
            break;
        }
    }
    
    if (semd == NULL) {
        return NULL;
    }
    //lo rimuovo dalla coda dei processi bloccati sul semaforo e lo ritorno
    return outProcQ(&semd->s_procq, p);
}

//ritorna il puntatore al PCB in testa alla coda dei processi bloccati sul semaforo con indirizzo semAdd
pcb_t* headBlocked(int* semAdd) {
    // Cerca il descrittore di semaforo nella ASL
    semd_t* semd = NULL;
    struct list_head* pos;
    //itero tutta la lista dei semd per trovare quello con chiave = semAdd
    list_for_each(pos, &semd_h) {
        semd_t* current = container_of(pos, semd_t, s_link);
        if (current->s_key == semAdd) {
            semd = current;
            break;
        }
    }
    
    if (semd == NULL) {
        return NULL;
    }
    
    // Se trovato, ritorna la testa della coda di processi
    return headProcQ(&semd->s_procq);
}
