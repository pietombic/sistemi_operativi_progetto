
void exceptionHandler() {
    int cause = getCAUSE();
    if (CAUSE_IS_INT()){
        interruptHandler();
    }else{
        if(cause >= 24 && cause <= 28){
            // TLB exception
        }
        else if(cause == 8 || cause == 11){
            // System call
        }
        else if(cause >= 0 && cause <= 7 || cause == 10 || cause >= 12 && cause <= 23){
            // Program trap
        }
        else{
            // Unrecognized exception code
            PANIC();
        }

    }
    
}