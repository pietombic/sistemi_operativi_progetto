#include "./headers/pcb.h"

static struct list_head pcbFree_h;
static pcb_t pcbFree_table[MAXPROC];
static int next_pid = 1;

void initPcbs() {
}

void freePcb(pcb_t* p) {
    list_add(&p->p_list, &pcbFree_h);
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
}

int emptyProcQ(struct list_head* head) {
    return list_empty(head);
}

void insertProcQ(struct list_head* head, pcb_t* p) {
    //se la coda dei processi non è vuota cerco la posizione giusta
    if (!emptyProcQ(head)){
        struct list_head* pos;
        //macro che itera su ogni elemento della lista head attraverso il puntatore pos
        list_for_each(pos, head) { 
            //prendo la struttura dati puntata da pos 
            pcb_t* current = container_of(pos, pcb_t, p_list);
            //caso in cui siamo arrivati alla fine della lista -> aggiungiamo in coda
            if (list_is_last(pos, head)) {
                list_add_tail(&p->p_list, head);
                return;
            }
            //controllo se la priorità del processo da inserire è minore di quella del processo corrente
            //in caso positivo inserisco
            else if (p->p_prio < current->p_prio) {
                //funzione che inserisce l'elemento new tra prev (di dove mi sono fermato) e dove mi sono fermato (pos)
                __list_add(&p->p_list, pos->prev, pos);
                return;
            }
        }
    }
    else {
        list_add_tail(&p->p_list, head);
    }
}

pcb_t* headProcQ(struct list_head* head) {
}

pcb_t* removeProcQ(struct list_head* head) {
}

pcb_t* outProcQ(struct list_head* head, pcb_t* p) {
}

int emptyChild(pcb_t* p) {
}

void insertChild(pcb_t* prnt, pcb_t* p) {
}

pcb_t* removeChild(pcb_t* p) {
}

pcb_t* outChild(pcb_t* p) {
}
