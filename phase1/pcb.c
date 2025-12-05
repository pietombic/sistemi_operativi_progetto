#include "./headers/pcb.h"

static struct list_head pcbFree_h;
static pcb_t pcbFree_table[MAXPROC];
static int next_pid = 1;

void initPcbs() {
    INIT_LIST_HEAD(&pcbFree_h);                             // inizializzo la lista dei pcb liberi  

    for(int x = 0; x < MAXPROC; x++) {
        list_add(&pcbFree_table[x].p_list, &pcbFree_h);     // aggiungo ogni pcb della table[maxproc] alla lista dei pcb liberi
    }
}

void freePcb(pcb_t* p) {
    list_add(&p->p_list, &pcbFree_h);
}

#include <stddef.h>
/* semplice implementatione di memset per toolchain freestanding */
void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

pcb_t* allocPcb() {
    // se la lista è vuota ritorno NULL
    if(list_empty(&pcbFree_h)) {
        return NULL;
    
    }
    // altrimenti prendo un elemento dalla lista, lo rimuovo e lo ritorno
    // prendo il primo elemento della lista
    struct list_head* first = pcbFree_h.next;
    // rimuovo l'elemento dalla lista
    list_del(first);
    // Come se dicessi "quale struttura pcb_t contiene questo campo p_list?" 
    // -> ci restituisce il puntatore al pcb_t che dobbiamo ritornare
    pcb_t* p = container_of(first, pcb_t, p_list);
    
    // inizializzo i campi del PCB
    p->p_parent = NULL;
    INIT_LIST_HEAD(&p->p_child);
    INIT_LIST_HEAD(&p->p_sib);
    p->p_s = (state_t){0};
    p->p_time = 0;
    p->p_semAdd = NULL;
    p->p_supportStruct = NULL;
    p->p_prio = 0;
    p->p_pid = next_pid++;
    return p;
}

void mkEmptyProcQ(struct list_head* head) {
    INIT_LIST_HEAD(head);                       // inizializzo la lista passata come parametro
}

int emptyProcQ(struct list_head* head) {
    return list_empty(head);
}

void insertProcQ(struct list_head* head, pcb_t* p) {
    if (emptyProcQ(head)) {
        list_add(&p->p_list, head);
        return;
    }
    
    struct list_head* pos;
    list_for_each(pos, head) {
        pcb_t* current = container_of(pos, pcb_t, p_list);
        // Se troviamo un processo con priorità minore, inseriamo prima di lui
        if (p->p_prio > current->p_prio) {
            __list_add(&p->p_list, pos->prev, pos);
            return;
        }
    }
    
    // Se arriviamo qui, nessuno ha priorità minore -> mettiamo in coda
    list_add_tail(&p->p_list, head);
}

pcb_t* headProcQ(struct list_head* head) {
    // coda vuota -> ritorno NULL
    if (emptyProcQ(head)) {
        return NULL;
    }
    else {
        struct list_head* first = head->next;
        pcb_t* p = container_of(first, pcb_t, p_list);
        return p;
    }
}

pcb_t* removeProcQ(struct list_head* head) {
    // coda vuota -> ritorno NULL
    if (emptyProcQ(head)) {
        return NULL;
    }
    else {
        //puntatore al primo elemento della lista (da rimuovere)
        struct list_head* first = head->next;
        list_del(first);
        //ritorno il puntatore al pcb corrispondente
        pcb_t* p = container_of(first, pcb_t, p_list);
        return p;
    }
}



// rimuove dalla coda puntata da head il puntatore p
// - se p non è presente restituisce null
// - se p è presente lo rimuove e restituisce
pcb_t* outProcQ(struct list_head* head, pcb_t* p) {
    // se la lista è vuota ritorno NULL
    if(list_empty(head)) {
        return NULL;
    
    }

    struct list_head* pos;
    list_for_each(pos, head) {
        pcb_t* current = container_of(pos, pcb_t, p_list);
        // caso in cui si sia trovato l'elemento
        if(current == p){
            list_del(&p->p_list);
            return p;
        }
    }
    // se arrivo qui non ho trovato l'elemento
    return NULL;

}

int emptyChild(pcb_t* p) {
    return list_empty(&p->p_child); // controllo se la lista dei figli è vuota
}

void insertChild(pcb_t* prnt, pcb_t* p) {
    p->p_parent = prnt; // imposto il genitore del processo p

    list_add_tail(&p->p_sib, &prnt->p_child); // inserisco p nella lista dei figli di prnt usando il campo p_child come testa della lista e p_sib come nodo della lista
}

// Rimuove e ritorna il primo figlio del processo puntato da p
pcb_t* removeChild(pcb_t* p) {
    // Se p non ha figli ritorno NULL
    if(emptyChild(p)) {
        return NULL;
    }
    // prendo il primo figlio dalla lista dei figli
    struct list_head* first = p->p_child.next;
    list_del(first);
    pcb_t* child = container_of(first, pcb_t, p_sib);
    child->p_parent = NULL; // il figlio non ha più un genitore
    return child;

}

/*
make the PCB pointed to by p no longer the
child of its parent. If the PCB pointed to by p has no parent, return NULL;
otherwise, return p. Note that the element pointed to by p could be in an
arbitrary position (i.e. not be the first child of its parent).
*/

pcb_t* outChild(pcb_t* p) {
    if (p == NULL || p->p_parent == NULL) {
        return NULL;
    }else{
        list_del(&p->p_sib);
        p->p_parent = NULL;
        return p;
    }
}
