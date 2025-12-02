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
    pcb_t *a = allocPcb(); a->p_prio = 20;
    pcb_t *b = allocPcb(); b->p_prio = 10;
    pcb_t *c = allocPcb(); c->p_prio = 0;

    insertProcQ(&queue, a);
    insertProcQ(&queue, b);
    insertProcQ(&queue, c);

    printf("Inseriti A(20), B(10), C(0)\n");

    // 5. Test ordine corretto
    pcb_t *iter;
    printf("Ordine nella coda:\n");
    list_for_each_entry(iter, &queue, p_list) {
        printf("%d ", iter->p_prio);
    }
    printf("\n");

    return 0;
}
