#include <stdio.h>
#include "../headers/const.h"
#include "../headers/types.h"
#include "./headers/pcb.h"


int main() {
    // 1. inizializza la lista dei PCB liberi
    initPcbs();

    // Verifica initPcbs + allocPcb
    pcb_t *p1 = allocPcb();
    if (p1 == NULL) {
        printf("ERRORE: allocPcb() ha restituito NULL!\n");
        return 1;
    }
    printf("allocPcb OK: PCB ottenuto\n");

    // 2. Test di listPcbFree + freePcb
    freePcb(p1);
    printf("freePcb OK: PCB rimesso nella pcbFree\n");

    // 3. Crea una coda e testala
    struct list_head queue;
    mkEmptyProcQ(&queue);

    if (emptyProcQ(&queue))
        printf("Coda inizialmente vuota OK\n");
    else
        printf("ERRORE: coda non vuota!\n");

    // 4. Inserisci processi con priorità diverse
    removeProcQ(&queue);
    pcb_t *a = allocPcb(); a->p_prio = 20;
    pcb_t *b = allocPcb(); b->p_prio = 30;
    pcb_t *c = allocPcb(); c->p_prio = 1;
    pcb_t *d = allocPcb(); d->p_prio = 15;
    pcb_t *e = allocPcb(); e->p_prio = 40;
    pcb_t *f = allocPcb(); e->p_prio = 0;
    pcb_t *g = allocPcb(); e->p_prio = 2;
    

    insertProcQ(&queue, a);
    insertProcQ(&queue, b);
    insertProcQ(&queue, c);
    insertProcQ(&queue, d);
    insertProcQ(&queue, e);
    insertProcQ(&queue, f);
    insertProcQ(&queue, g);

    removeProcQ(&queue);

    printf("Inseriti A(20), B(10), C(0)\n");

    // 5. Test ordine corretto
    pcb_t *iter;
    printf("Ordine nella coda:\n");
    list_for_each_entry(iter, &queue, p_list) {
        printf("%d ", iter->p_prio);
    }
    printf("\n");

    printf("prova headProcQ:\n");
    pcb_t* head = container_of(list_next(&queue), pcb_t, p_list);
    printf("Priorita' del primo elemento della coda: %d\n", head->p_prio);
    printf("\n");

    pcb_t* prova = removeProcQ(&queue);
    printf("Dopo una rimozione, nuovo ordine:\n");
    list_for_each_entry(iter, &queue, p_list) {
        printf("%d ", iter->p_prio);
    }
    printf("\n");
    printf("Priorita' del processo rimosso: %d\n", prova->p_prio);




    return 0;
}
