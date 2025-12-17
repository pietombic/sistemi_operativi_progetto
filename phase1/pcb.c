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

//inserisce il PCB p nella lista dei PCB liberi
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

// Restituisce un puntatore ad un PCB libero
// se non ci sono PCB liberi ritorna NULL
pcb_t* allocPcb() {
    if(list_empty(&pcbFree_h)) {
        return NULL;
    
    }
    // altrimenti prendo un elemento dalla lista, lo rimuovo e lo ritorno
    // prendo il primo elemento della lista
    struct list_head* first = pcbFree_h.next;
    list_del(first);
    // Come se dicessi "quale struttura pcb_t contiene questo campo p_list?" 
    // -> ci restituisce il puntatore al pcb_t che dobbiamo ritornare
    pcb_t* p = container_of(first, pcb_t, p_list);
    
    // inizializzo il PCB
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

// inizializza la coda dei processi puntata da head
void mkEmptyProcQ(struct list_head* head) {
    INIT_LIST_HEAD(head);
}
// Restituisce true se la coda dei processi puntata da head è vuota, false altrimenti
int emptyProcQ(struct list_head* head) {
    return list_empty(head);
}
// inserisce il PCB p nella coda dei processi puntata da head, tenendo conto della priorità di p
void insertProcQ(struct list_head* head, pcb_t* p) {
    //se la coda è vuota, inserisco direttamente
    if (emptyProcQ(head)) {
        list_add(&p->p_list, head);
        return;
    }
    
    struct list_head* pos; 
    list_for_each(pos, head) {
        pcb_t* current = container_of(pos, pcb_t, p_list); // ottengo il pcb corrente dall'iteratore
        // Se troviamo un processo con priorità minore, inseriamo prima di lui
        if (p->p_prio > current->p_prio) {
            __list_add(&p->p_list, pos->prev, pos);
            return;
        }
    }
    
    // nessuno ha priorità minore -> mettiamo in coda
    list_add_tail(&p->p_list, head);
}

//ritorna un puntatore al PCB che si trova in testa (max priorità) alla coda dei processi puntata da head 
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

//rimuove e ritorna il PCB che si trova in testa (max priorità) alla coda dei processi puntata da head
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

// Rimuove il PCB p dalla lista dei figli del suo genitore
// Ritorna NULL se p non ha un genitore
// Altrimenti ritorna p dopo averlo rimosso dalla lista dei figli del genitore
pcb_t* outChild(pcb_t* p) {
    if (p == NULL || p->p_parent == NULL) {
        return NULL;
    }else{
        list_del(&p->p_sib);
        p->p_parent = NULL;
        return p;
    }
}
