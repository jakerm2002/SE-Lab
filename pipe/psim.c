/**************************************************************************
 * psim.c - Pipelined Y86-64 simulator
 *
 * Copyright (c) 2010, 2015. Bryant and D. O'Hallaron, All rights reserved.
 * May not be used, modified, or copied without permission.
 * 
 * Updated 2021: M. Hinton, Z. Leeper
 **************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>

#include "isa.h"
#include "pipeline.h"
#include "stages.h"
#include "sim.h"

char simname[] = "Y86-64 Processor: PIPE";

/* Parameters modifed by the command line */
char *object_filename;   /* The input object file name. */
FILE *object_file;       /* Input file handle */
int verbosity = 2;    /* Verbosity level [Non interactive Mode only] (-v) */
word_t instr_limit = 10000; /* Instruction limit [Non interactive Mode only] (-l) */

/* Log file */
FILE *dumpfile = NULL;

/* Performance monitoring */
/* How many cycles have been simulated? */
word_t cycles = 0;
/* How many instructions have passed through the WB stage? */
word_t instructions = 0;

/* Has simulator gotten past initial bubbles? */
static int starting_up = 1;





/********************************************************************************** 
 * These are all useful global variables for implementing the pipeline. They will
 * ALL be used at some point or another in one of the five stages, or in stall
 * checking. The two header files in the misc directory contain declarations for
 * byte_t, word_t, mem_t, cc_t, and stat_t. For the most part, a byte_t is 1 byte,
 * word_t is 8 bytes, mem_t is an array of bytes you can treat as an emulation of
 * memory or registers, cc_t are the condition codes, and stat_t is the machine
 * status. The remaining pipeline stage structs you can see declared in stages.h.
 **********************************************************************************/

/* Both instruction and data memory */
mem_t mem;
/* Register file */
mem_t reg;

/* Condition code register */
cc_t cc;
/* Status code */
stat_t status;

/* Pending updates to state */
word_t cc_in    = DEFAULT_CC;
word_t wb_destE = REG_NONE;
word_t wb_valE  = 0;
word_t wb_destM = REG_NONE;
word_t wb_valM  = 0;
word_t mem_addr = 0;
word_t mem_data = 0;
bool mem_write  = false;
bool mem_read   = false;

/* Output and input states of all pipeline registers */
fetch_ptr fetch_output;
decode_ptr decode_output;
execute_ptr execute_output;
memory_ptr memory_output;
writeback_ptr writeback_output;

fetch_ptr fetch_input;
decode_ptr decode_input;
execute_ptr execute_input;
memory_ptr memory_input;
writeback_ptr writeback_input;

/* Intermediate values */
word_t f_pc;
byte_t imem_icode;
byte_t imem_ifun;
bool imem_error;
bool instr_valid;
word_t d_regvala;
word_t d_regvalb;
word_t e_vala;
word_t e_valb;
bool e_bcond;
bool dmem_error;

/* The pipeline state */
pipe_ptr fetch_state, decode_state, execute_state, memory_state, writeback_state;





/***************************
 * Begin function prototypes
 ***************************/

word_t sim_run_pipe(word_t max_instr, word_t max_cycle, byte_t *statusp, cc_t *ccp);
static void usage(char *name);           /* Print helpful usage message */
static void run_tty_sim();               /* Run simulator in TTY mode */
static void sim_interactive();

/*************************
 * End function prototypes
 *************************/


/*******************************************************************
 * Part 1: This part is the initial entry point that handles general
 * initialization. It parses the command line and does any necessary
 * setup to run in TTY mode, and then starts the
 * simulation.
 * Do not change any of these.
 *******************************************************************/

/*
 * sim_main - main simulator routine. This function is called from the
 * main() routine in the HCL file.
 */
int sim_main(int argc, char **argv)
{
    int i;
    int c;
    int interactive = 0;

    /* Parse the command line arguments */
    while ((c = getopt(argc, argv, "hil:v:")) != -1) {
        switch(c) {
        case 'h':
            usage(argv[0]);
            break;
        case 'l':
            instr_limit = atoll(optarg);
            break;
        case 'v':
            verbosity = atoi(optarg);
            if (verbosity < 0 || verbosity > 2) {
                printf("Invalid verbosity %d\n", verbosity);
                usage(argv[0]);
            }
            break;
        case 'i':
            interactive = true;
            break;
        default:
            printf("Invalid option '%c'\n", c);
            usage(argv[0]);
            break;
        }
    }


    /* Do we have too many arguments? */
    if (optind < argc - 1) {
	printf("Too many command line arguments:");
	for (i = optind; i < argc; i++)
	    printf(" %s", argv[i]);
	printf("\n");
	usage(argv[0]);
    }

    /* The single unflagged argument should be the object file name */
    object_filename = NULL;
    object_file = NULL;
    if (optind < argc) {
        object_filename = argv[optind];
        object_file = fopen(object_filename, "r");
        if (!object_file) {
            fprintf(stderr, "Couldn't open object file %s\n", object_filename);
            exit(1);
        }
    }

    if (object_filename == NULL) {
        fprintf(stderr, "No object file specified\n");
        exit(1);
    }


    if (interactive) {
        sim_interactive();
    } else {
        run_tty_sim();
    }
    exit(0);
}

int main(int argc, char *argv[]){return sim_main(argc,argv);}

/*
 * run_tty_sim - Run the simulator in TTY mode
 */
static void run_tty_sim()
{
    word_t icount = 0;
    byte_t run_status = STAT_AOK;
    cc_t result_cc = 0;
    word_t byte_cnt = 0;
    mem_t mem0, reg0;
    state_ptr isa_state = NULL;

    if (verbosity >= 2)
	    dumpfile = stdout;
    sim_init();

    /* Emit simulator name */
    if (verbosity >= 2)
	    printf("%s\n", simname);

    byte_cnt = load_mem(mem, object_file, 1);
    if (byte_cnt == 0) {
	    fprintf(stderr, "No lines of code found\n");
	    exit(1);
    } else if (verbosity >= 2) {
	    printf("%lld bytes of code read\n", byte_cnt);
    }
    fclose(object_file);

    isa_state = new_state(0);
    free_mem(isa_state->r);
    free_mem(isa_state->m);
    isa_state->m = copy_mem(mem);
    isa_state->r = copy_mem(reg);
    isa_state->cc = cc;

    mem0 = copy_mem(mem);
    reg0 = copy_mem(reg);

    icount = sim_run_pipe(instr_limit, 5*instr_limit, &run_status, &result_cc);
    if (verbosity > 0) {
        printf("%lld instructions executed\n", icount);
        printf("Status = %s\n", stat_name(run_status));
        printf("Condition Codes: %s\n", cc_name(result_cc));
        printf("Changed Register State:\n");
        diff_reg(reg0, reg, stdout);
        printf("Changed Memory State:\n");
        diff_mem(mem0, mem, stdout);
    }

    byte_t e = STAT_AOK;
    word_t step;
    bool match = true;

    for (step = 0; step < instr_limit && e == STAT_AOK; step++) {
        e = step_state(isa_state, stdout);
    }

    if (diff_reg(isa_state->r, reg, NULL)) {
        match = false;
        if (verbosity > 0) {
            printf("ISA Register != Pipeline Register File\n");
            printf("\tISA register\t\tPipeline Register\n");
            diff_reg(isa_state->r, reg, stdout);
        }
    }

    if (diff_mem(isa_state->m, mem, NULL)) {
        match = false;
        if (verbosity > 0) {
            printf("ISA Memory != Pipeline Memory\n");
            printf("\tISA Memory\t\tPipeline Memory\n");
            diff_mem(isa_state->m, mem, stdout);
        }
    }

    if (isa_state->cc != result_cc) {
        match = false;
        if (verbosity > 0) {
            printf("ISA Cond. Codes (%s) != Pipeline Cond. Codes (%s)\n",
                cc_name(isa_state->cc), cc_name(result_cc));
        }
    }

    if (match) {
        printf("ISA Check Succeeds\n");
    } else {
        printf("ISA Check Fails\n");
    }

    /* Emit CPI statistics */
	double cpi = instructions > 0 ? (double) cycles/instructions : 1.0;
	printf("CPI: %lld cycles/%lld instructions = %.2f\n",
	       cycles, instructions, cpi);
}

/*
 * usage - print helpful diagnostic information
 */
static void usage(char *name)
{
    printf("Usage: %s [-hi] [-l m] [-v n] file.yo\n", name);
    printf("   -h     Print this message\n");
    printf("   -l m   Set instruction limit to m [non interactive mode only] (default %lld)\n", instr_limit);
    printf("   -v n   Set verbosity level to 0 <= n <= 2 [non interactive mode only] (default %d)\n", verbosity);
    printf("   -i     Runs the simulator in interactive mode\n");
    exit(0);
}


/*********************************************************
 * Part 2: This part contains the core simulator routines.
 *********************************************************/


/*****************************************************************************
 * pipeline control
 * These functions can be used to handle hazards
 *****************************************************************************/

/* bubble stage (has effect at next update) */
void sim_bubble_stage(stage_id_t stage)
{
    switch (stage)
	{
	case FETCH_STAGE     : fetch_state->op     = P_BUBBLE; break;
	case DECODE_STAGE    : decode_state->op    = P_BUBBLE; break;
	case EXECUTE_STAGE   : execute_state->op   = P_BUBBLE; break;
	case MEMORY_STAGE    : memory_state->op    = P_BUBBLE; break;
	case WRITEBACK_STAGE : writeback_state->op = P_BUBBLE; break;
	}
}

/* stall stage (has effect at next update) */
void sim_stall_stage(stage_id_t stage) {
    switch (stage)
	{
	case FETCH_STAGE     : fetch_state->op     = P_STALL; break;
	case DECODE_STAGE    : decode_state->op    = P_STALL; break;
	case EXECUTE_STAGE   : execute_state->op   = P_STALL; break;
	case MEMORY_STAGE    : memory_state->op    = P_STALL; break;
	case WRITEBACK_STAGE : writeback_state->op = P_STALL; break;
	}
}

static int initialized = 0;

void sim_init()
{
    /* Create memory and register files */
    initialized = 1;
    mem = init_mem(MEM_SIZE);
    reg = init_reg();

    /* create 5 pipe registers */
    fetch_state     = new_pipe(sizeof(fetch_ele), (void *) &bubble_fetch);
    decode_state    = new_pipe(sizeof(decode_ele), (void *) &bubble_decode);
    execute_state   = new_pipe(sizeof(execute_ele), (void *) &bubble_execute);
    memory_state    = new_pipe(sizeof(memory_ele), (void *) &bubble_memory);
    writeback_state = new_pipe(sizeof(writeback_ele), (void *) &bubble_writeback);

    /* connect them to the pipeline stages */
    fetch_input      = fetch_state->input;
    fetch_output     = fetch_state->output;

    decode_input     = decode_state->input;
    decode_output    = decode_state->output;

    execute_input    = execute_state->input;
    execute_output   = execute_state->output;

    memory_input     = memory_state->input;
    memory_output    = memory_state->output;

    writeback_input  = writeback_state->input;
    writeback_output = writeback_state->output;

    sim_reset();
    clear_mem(mem);
}

void sim_reset()
{
    if (!initialized)
	    sim_init();
    clear_pipes();
    clear_mem(reg);
    starting_up = 1;
    cycles = instructions = 0;
    cc = DEFAULT_CC;
    status = STAT_AOK;

    cc = cc_in = DEFAULT_CC;
    wb_destE  = REG_NONE;
    wb_valE   = 0;
    wb_destM  = REG_NONE;
    wb_valM   = 0;
    mem_addr  = 0;
    mem_data  = 0;
    mem_write = false;
}

static void print_state(word_t cyc) {
    sim_log("\nCycle = %lld. CC = %s, Stat = %s\n", cyc, cc_name(cc), stat_name(status));
}

static void print_fetch() {
    sim_log("F: predPC = 0x%llx\n", fetch_output->predPC);
}

static void print_decode() {
    sim_log("D: instr = %s, rA = %s, rB = %s, valC = 0x%llx, valP = 0x%llx, Stat = %s, Stage PC = 0x%llx\n",
	    iname(HPACK(decode_output->icode, decode_output->ifun)),
	    reg_name(decode_output->ra), reg_name(decode_output->rb),
	    decode_output->valc, decode_output->valp,
	    stat_name(decode_output->status), decode_output->stage_pc);
}

static void print_execute() {
    sim_log("E: instr = %s, valC = 0x%llx, valA = 0x%llx, valB = 0x%llx\n   srcA = %s, srcB = %s, dstE = %s, dstM = %s, Stat = %s, Stage PC = 0x%llx\n",
	    iname(HPACK(execute_output->icode, execute_output->ifun)),
	    execute_output->valc, execute_output->vala, execute_output->valb,
	    reg_name(execute_output->srca), reg_name(execute_output->srcb),
	    reg_name(execute_output->deste), reg_name(execute_output->destm),
	    stat_name(execute_output->status), execute_output->stage_pc);
}

static void print_memory() {
    sim_log("M: instr = %s, Cnd = %d, valE = 0x%llx, valA = 0x%llx\n   dstE = %s, dstM = %s, Stat = %s, Stage PC = 0x%llx\n",
	    iname(HPACK(memory_output->icode, memory_output->ifun)),
	    memory_output->takebranch,
	    memory_output->vale, memory_output->vala,
	    reg_name(memory_output->deste), reg_name(memory_output->destm),
	    stat_name(memory_output->status), memory_output->stage_pc);
}

static void print_writeback() {
    sim_log("W: instr = %s, valE = 0x%llx, valM = 0x%llx, dstE = %s, dstM = %s, Stat = %s, Stage PC = 0x%llx\n",
	    iname(HPACK(writeback_output->icode, writeback_output->ifun)),
	    writeback_output->vale, writeback_output->valm,
	    reg_name(writeback_output->deste), reg_name(writeback_output->destm),
	    stat_name(writeback_output->status), writeback_output->stage_pc);
}

/* Text representation of status */
void tty_report(word_t cyc) {
    print_state(cyc);
    print_fetch();
    print_decode();
    print_execute();
    print_memory();
    print_writeback();
}

/******************************************************************
 * This function runs the pipeline for one cycle. max_instr
 * indicates maximum number of instructions that want to complete
 * during this simulation run.
 * You should update intermediate values for each stage, update
 * global state values after all stages, and finally return the
 * correct state.
 * The diagrams in section 4.5 of your textbook will be of great
 * help in implementing these stages.
 ******************************************************************/

/* Run pipeline for one cycle */
/* Return status of processor */
/* Max_instr indicates maximum number of instructions that
   want to complete during this simulation run.  */
static byte_t sim_step_pipe(word_t ccount)
{
    /* Update pipe registers */
    update_pipes();
    /* print status report in TTY mode */
    tty_report(ccount);
    /* error checking */
    if (fetch_state->op == P_ERROR)
	    fetch_output->status = STAT_PIP;
    if (decode_state->op == P_ERROR)
	    decode_output->status = STAT_PIP;
    if (execute_state->op == P_ERROR)
	    execute_output->status = STAT_PIP;
    if (memory_state->op == P_ERROR)
	    memory_output->status = STAT_PIP;
    if (writeback_state->op == P_ERROR)
	    writeback_output->status = STAT_PIP;

    /****************** Stage implementations ******************
     * TODO: implement the following functions to simulate the
     * executations in each stage.
     * You should also implement stalling, forwarding and branch
     * prediction to handle data hazards and control hazards.
     *
     * Since C code is executed sequencially, we are calling the
     * decode stage after execute & memory stages, and memory
     * stage before execute, in order to propagate forwarding
     * values properly. Note that on real hardware, these would
     * all happen simultaneously.
     * 
     * Note that these functions do not have input parameters!
     * You need to use the given global pipeline register
     * structs.
     ***********************************************************/

    do_writeback_stage();
    do_memory_stage();
    do_execute_stage();
    do_decode_stage();
    do_fetch_stage();

    do_stall_check();

    /* Performance monitoring. Do not change anything below */
    if (writeback_output->status != STAT_BUB) {
        starting_up = 0;
        instructions++;
        cycles++;
    } else {
	    if (!starting_up)
	        cycles++;
    }

    return status;
}

/*************************** Fetch stage ***************************
 * TODO: update [*decode_input, f_pc, *fetch_input]
 * you may find these functions useful:
 * HPACK(), get_byte_val(), get_word_val(), HI4(), LO4()
 *
 * imem_error is defined for logging purpose, you can use it to help
 * with your design, but it's also fine to neglect it
 *******************************************************************/
void do_fetch_stage()
{
    /* your implementation */

    byte_t instr; //byte 0, contains icode, ifun
    byte_t byte1; //byte 1, contains rA, rB

    //Select PC
    if (memory_output->icode == I_JMP && !memory_output->takebranch) {
        f_pc = memory_output->vala;
    } else if (writeback_output->icode == I_RET) {
        f_pc = writeback_output->valm;
    } else {
        f_pc = fetch_output->predPC;
    }

    //fetch instruction from memory
    imem_error |= !get_byte_val(mem, f_pc, &instr); //get first byte from memory
    decode_input->icode = HI4(instr);
    decode_input->ifun = LO4(instr);
    decode_input->ra = REG_NONE;
    decode_input->rb = REG_NONE;
    decode_input->valc = 0;
    decode_input->status = STAT_AOK;


    switch (instr) {
        case HPACK(I_NOP, F_NONE):
            decode_input->valp = f_pc + 1;
            fetch_input->predPC = decode_input->valp;
            break;
        case HPACK(I_HALT, F_NONE):
            decode_input->valp = f_pc + 1;
            fetch_input->predPC = decode_input->valp;
            decode_input->status = STAT_HLT;
            break;

        case HPACK(I_RRMOVQ, F_NONE):
        case HPACK(I_RRMOVQ, C_LE):
        case HPACK(I_RRMOVQ, C_L):
        case HPACK(I_RRMOVQ, C_E):
        case HPACK(I_RRMOVQ, C_NE):
        case HPACK(I_RRMOVQ, C_GE):
        case HPACK(I_RRMOVQ, C_G):
            imem_error |= !get_byte_val(mem, f_pc + 1, &byte1);
            decode_input->ra = HI4(byte1);
            decode_input->rb = LO4(byte1);
            decode_input->valp = f_pc + 2;
            fetch_input->predPC = decode_input->valp;
            break;

        case HPACK(I_IRMOVQ, F_NONE):
            imem_error |= !get_byte_val(mem, f_pc + 1, &byte1);
            decode_input->rb = LO4(byte1);
            imem_error |= !get_word_val(mem, f_pc + 2, &decode_input->valc);
            decode_input->valp = f_pc + 10;
            fetch_input->predPC = decode_input->valp;
            break;

        case HPACK(I_RMMOVQ, F_NONE):
            imem_error |= !get_byte_val(mem, f_pc + 1, &byte1);
            decode_input->ra = HI4(byte1);
            decode_input->rb = LO4(byte1);
            imem_error |= !get_word_val(mem, f_pc + 2, &decode_input->valc);
            decode_input->valp = f_pc + 10;
            fetch_input->predPC = decode_input->valp;
            break;

        case HPACK(I_MRMOVQ, F_NONE):
            imem_error |= !get_byte_val(mem, f_pc + 1, &byte1);
            decode_input->ra = HI4(byte1);
            decode_input->rb = LO4(byte1);
            imem_error |= !get_word_val(mem, f_pc + 2, &decode_input->valc);
            decode_input->valp = f_pc + 10;
            fetch_input->predPC = decode_input->valp;
            break;

        case HPACK(I_ALU, A_ADD):
        case HPACK(I_ALU, A_SUB):
        case HPACK(I_ALU, A_AND):
        case HPACK(I_ALU, A_XOR):
            imem_error |= !get_byte_val(mem, f_pc + 1, &byte1);
            decode_input->ra = HI4(byte1);
            decode_input->rb = LO4(byte1);
            decode_input->valp = f_pc + 2;
            fetch_input->predPC = decode_input->valp;
            break;

        case HPACK(I_JMP, C_YES):
        case HPACK(I_JMP, C_LE):
        case HPACK(I_JMP, C_L):
        case HPACK(I_JMP, C_E):
        case HPACK(I_JMP, C_NE):
        case HPACK(I_JMP, C_GE):
        case HPACK(I_JMP, C_G):
            imem_error |= !get_word_val(mem, f_pc + 1, &decode_input->valc);
            decode_input->valp = f_pc + 9;
            fetch_input->predPC = decode_input->valc;
            break;

        case HPACK(I_CALL, F_NONE):
            imem_error |= !get_word_val(mem, f_pc + 1, &decode_input->valc);
            decode_input->valp = f_pc + 9;
            fetch_input->predPC = decode_input->valc;
            break;

        case HPACK(I_RET, F_NONE):
            decode_input->valp = f_pc + 1;
            fetch_input->predPC = decode_input->valp;
            break;

        case HPACK(I_PUSHQ, F_NONE):
            imem_error |= !get_byte_val(mem, f_pc + 1, &byte1);
            decode_input->ra = HI4(byte1);
            decode_input->rb = LO4(byte1);
            decode_input->valp = f_pc + 2;
            fetch_input->predPC = decode_input->valp;
            break;

        case HPACK(I_POPQ, F_NONE):
            imem_error |= !get_byte_val(mem, f_pc + 1, &byte1);
            decode_input->ra = HI4(byte1);
            decode_input->rb = LO4(byte1);
            decode_input->valp = f_pc + 2;
            fetch_input->predPC = decode_input->valp;
            break;

        case HPACK(I_LEAQ, F_NONE):
            imem_error |= !get_byte_val(mem, f_pc + 1, &byte1);
            decode_input->ra = HI4(byte1);
            decode_input->rb = LO4(byte1);
            imem_error |= !get_word_val(mem, f_pc + 2, &decode_input->valc);
            decode_input->valp = f_pc + 10;
            fetch_input->predPC = decode_input->valp;
            break;

        case HPACK(I_VECADD, F_NONE):
            imem_error |= !get_byte_val(mem, f_pc + 1, &byte1);
            decode_input->ra = HI4(byte1);
            decode_input->rb = LO4(byte1);
            decode_input->valp = f_pc + 2;
            fetch_input->predPC = decode_input->valp;
            break;

        case HPACK(I_SHF, S_HL):
        case HPACK(I_SHF, S_HR):
        case HPACK(I_SHF, S_AR):
            imem_error |= !get_byte_val(mem, f_pc + 1, &byte1);
            decode_input->ra = HI4(byte1);
            decode_input->rb = LO4(byte1);
            decode_input->valp = f_pc + 2;
            fetch_input->predPC = decode_input->valp;
            break;

        default:
            decode_input->status = imem_error ? STAT_ADR : STAT_INS;
            instr_valid = false;
            printf("Invalid instruction\n");
            break;
    }


    /* logging function, do not change this */
    if (!imem_error) {
        sim_log("\tFetch: f_pc = 0x%llx, f_instr = %s\n",
            f_pc, iname(HPACK(decode_input->icode, decode_input->ifun)));
    }
}


/*************************** Decode stage ***************************
 * TODO: update [*execute_input]
 * you may find these functions useful:
 * get_reg_val()
 *******************************************************************/
void do_decode_stage()
{
    /* your implementation */
    execute_input->icode = decode_output->icode;
    execute_input->ifun = decode_output->ifun;
    execute_input->srca = REG_NONE;
    execute_input->srcb = REG_NONE;
    execute_input->deste = REG_NONE;
    execute_input->destm = REG_NONE;
    execute_input->vala = 0;
    execute_input->valb = 0;
    execute_input->valc = decode_output->valc;
    execute_input->status = decode_output->status;
    
    switch (decode_output->icode) {
        case I_HALT: break;

        case I_NOP: break;

        case I_RRMOVQ: // aka CMOVQ
            execute_input->srca = decode_output->ra;
            execute_input->deste = decode_output->rb;
            break;

        case I_IRMOVQ:
            execute_input->deste = decode_output->rb;
            break;

        case I_RMMOVQ:
            execute_input->srca = decode_output->ra;
            execute_input->srcb = decode_output->rb;
            break;

        case I_MRMOVQ:
            execute_input->srcb = decode_output->rb;
            execute_input->destm = decode_output->ra;
            break;

        case I_ALU:
            execute_input->srca = decode_output->ra;
            execute_input->srcb = decode_output->rb;
            execute_input->deste = decode_output->rb;
            break;

        case I_JMP: break;

        case I_CALL:
            execute_input->srcb = REG_RSP;
            execute_input->deste = REG_RSP;
            break;

        case I_RET:
            execute_input->srca = REG_RSP;
            execute_input->srcb = REG_RSP;
            execute_input->deste = REG_RSP;
            break;

        case I_PUSHQ:
            execute_input->srca = decode_output->ra;
            execute_input->srcb = REG_RSP;
            execute_input->deste = REG_RSP;
            break;

        case I_POPQ:
            execute_input->srca = REG_RSP;
            execute_input->srcb = REG_RSP;
            execute_input->deste = REG_RSP;
            execute_input->destm = decode_output->ra;
            break;

        case I_LEAQ:
            execute_input->srcb = decode_output->rb;
            execute_input->deste = decode_output->ra;
            break;

        case I_VECADD:
            execute_input->srca = decode_output->ra;
            execute_input->srcb = decode_output->rb;
            execute_input->deste = decode_output->rb;
            break;

        case I_SHF:
            execute_input->srca = decode_output->ra;
            execute_input->srcb = decode_output->rb;
            execute_input->deste = decode_output->rb;
            break;

        default:
            printf("icode is not valid (%d)", decode_output->icode);
            break;
    }


    //update valA
    if (decode_output->icode == I_CALL || decode_output->icode == I_JMP) {
        execute_input->vala = decode_output->valp;
    } else if (execute_input->srca == memory_input->deste) {
        execute_input->vala = memory_input->vale;
    } else if (execute_input->srca == memory_output->destm) {
        execute_input->vala = writeback_input->valm;
    } else if (execute_input->srca == memory_output->deste) {
        execute_input->vala = memory_output->vale;
    } else if (execute_input->srca == writeback_output->destm) {
        execute_input->vala = writeback_output->valm;
    } else if (execute_input->srca == writeback_output->deste) {
        execute_input->vala = writeback_output->vale;
    } else {
        d_regvala = get_reg_val(reg, execute_input->srca);
        execute_input->vala = d_regvala;
    }

    //update valB
    if (execute_input->srcb == memory_input->deste) {
        execute_input->valb = memory_input->vale;
    } else if (execute_input->srcb == memory_output->destm) {
        execute_input->valb = writeback_input->valm;
    } else if (execute_input->srcb == memory_output->deste) {
        execute_input->valb = memory_output->vale;
    } else if (execute_input->srcb == writeback_output->destm) {
        execute_input->valb = writeback_output->valm;
    } else if (execute_input->srcb == writeback_output->deste) {
        execute_input->valb = writeback_output->vale;
    } else {
        d_regvalb = get_reg_val(reg, execute_input->srcb);
        execute_input->valb = d_regvalb;
    }

}

/************************** Execute stage **************************
 * TODO: update [*memory_input, cc_in]
 * you may find these functions useful:
 * cond_holds(), compute_alu(), compute_cc()
 *******************************************************************/
void do_execute_stage()
{
    /* some useful variables for logging purpose */
    bool setcc = false;
    alu_t alufun = execute_output->icode == I_ALU ? execute_output->ifun : A_ADD;
    word_t alua, alub;
    alua = alub = 0;

    /* your implementation */

    //Set CC
    if ( execute_output->icode == I_ALU && 
    !(writeback_input->status == STAT_ADR || writeback_input->status == STAT_INS || writeback_input->status == STAT_HLT) && 
    !(writeback_output->status == STAT_ADR || writeback_output->status == STAT_INS || writeback_output->status == STAT_HLT) ) {
        setcc = true;
    }
    
    memory_input->vale = 0;
    memory_input->icode = execute_output->icode;
    memory_input->ifun = execute_output->ifun;
    // memory_input->srca = REG_NONE;
    // memory_input->srcb = REG_NONE;
    memory_input->deste = execute_output->deste;
    memory_input->destm = execute_output->destm;
    memory_input->vala = execute_output->vala;
    memory_input->status = execute_output->status;
    memory_input->takebranch = 0;

    // cc_in = cc;
    bool cnd = false;

    switch (execute_output->icode) {
        case I_HALT:
            break;

        case I_NOP: break;

        case I_RRMOVQ: // aka CMOVQ
            cnd = cond_holds(cc, execute_output->ifun);
            alua = execute_output->vala;
            memory_input->vale = execute_output->vala;
            if (!cnd) {
                memory_input->deste = REG_NONE;
            }
            memory_input->takebranch = cnd;
            break;

        case I_IRMOVQ:
            alua = execute_output->valc;
            memory_input->vale = execute_output->valc;
            break;

        case I_RMMOVQ:
            alua = execute_output->valc;
            alub = execute_output->valb;
            memory_input->vale = execute_output->valb + execute_output->valc;
            break;

        case I_MRMOVQ:
            alua = execute_output->valc;
            alub = execute_output->valb;
            memory_input->vale = execute_output->valb + execute_output->valc;
            break;

        case I_ALU:
            alua = execute_output->vala;
            alub = execute_output->valb;
            memory_input->vale = compute_alu(execute_output->ifun, execute_output->vala, execute_output->valb);
            if (setcc) {
                cc_in = compute_cc(execute_output->ifun, execute_output->vala, execute_output->valb);
            }
            break;

        case I_JMP:
            cnd = cond_holds(cc, execute_output->ifun);
            memory_input->takebranch = cnd;
            break;

        case I_CALL:
            alua = -8;
            alub = execute_output->valb;
            memory_input->vale = execute_output->valb - 8;
            break;

        case I_RET:
            alua = 8;
            alub = execute_output->valb;
            memory_input->vale = execute_output->valb + 8;
            break;

        case I_PUSHQ:
            alua = -8;
            alub = execute_output->valb;
            memory_input->vale = execute_output->valb - 8;
            break;

        case I_POPQ:
            alua = 8;
            alub = execute_output->valb;
            memory_input->vale = execute_output->valb + 8;
            break;

        case I_LEAQ:
            alua = execute_output->valc;
            alub = execute_output->valb;
            memory_input->vale = execute_output->valb + execute_output->valc;
            break;

        case I_VECADD:
            alua = execute_output->vala;
            alub = execute_output->valb;

            //uses code already included in isa.c
            int j = 1;
            int x = 0;
            word_t out = 0;
            char* v = (char*)&execute_output->vala;
            char* t = (char*)&execute_output->valb;
            char* u = (char*)&out;
            for (size_t i = 0; i < 8; i++) {
                u[i] = v[i] + t[i];
                j &= (u[i] == 0);
                x |= ((u[i] >> 7) != 0);
            }
            memory_input->vale = out;
            setcc = true;
            cc_in = PACK_CC(j, x, 0);
            break;

        case I_SHF:
            alua = execute_output->vala;
            alub = execute_output->valb;
            if (execute_output->ifun == S_HL) {
                memory_input->vale = execute_output->valb << execute_output->vala;
            } else if (execute_output->ifun == S_HR) {
                memory_input->vale = ((unsigned long long)execute_output->valb) >> execute_output->vala;
            } else if (execute_output->ifun == S_AR) {
                memory_input->vale = execute_output->valb >> execute_output->vala;
            }
            setcc = true;
            //uses code already included in isa.c
            cc_in = PACK_CC(!memory_input->vale, (memory_input->vale >> 63) & 0x1, 0);
            break;

        default:
            printf("icode is not valid (%d)", execute_output->icode);
            break;
    }


    /* logging functions, do not change these */
    if (execute_output->icode == I_JMP) {
        sim_log("\tExecute: instr = %s, cc = %s, branch %staken\n",
            iname(HPACK(execute_output->icode, execute_output->ifun)),
            cc_name(cc),
            memory_input->takebranch ? "" : "not ");
    }
    sim_log("\tExecute: ALU: %c 0x%llx 0x%llx --> 0x%llx\n",
        op_name(alufun), alua, alub, memory_input->vale);
    if (setcc) {
        cc = cc_in;
	    sim_log("\tExecute: New cc=%s\n", cc_name(cc_in));
    }
}

/*************************** Memory stage **************************
 * TODO: update [*writeback_input, mem_addr, mem_data, mem_write]
 * you may find these functions useful:
 * get_word_val()
 *******************************************************************/
void do_memory_stage()
{
    /* dummy placeholders, replace them with your implementation */
    mem_addr   = 0;
    mem_data   = 0;
    word_t *mem_dest = 0;
    mem_write  = false;
    mem_read   = false;
    dmem_error = false;
    writeback_input->icode = memory_output->icode;
    writeback_input->vale = memory_output->vale;
    writeback_input->valm = 0;
    writeback_input->deste = memory_output->deste;
    writeback_input->destm = memory_output->destm;
    writeback_input->status = memory_output->status;

    /* your implementation */

    switch (memory_output->icode) {
        case I_HALT:
            writeback_input->status = STAT_HLT;
            break;

        case I_NOP: break;

        case I_RRMOVQ: break; // aka CMOVQ

        case I_IRMOVQ: break;

        case I_RMMOVQ:
            mem_write = true;
            mem_addr = memory_output->vale;
            mem_data = memory_output->vala;
            break;

        case I_MRMOVQ:
            mem_read = true;
            mem_addr = memory_output->vale;
            mem_data = writeback_input->valm;
            mem_dest = &writeback_input->valm;
            // dmem_error |= !get_word_val(mem, mem_addr, &mem_data);
            break;

        case I_ALU: break;

        case I_JMP: break;

        case I_CALL:
            mem_write = true;
            mem_addr = memory_output->vale;
            mem_data = memory_output->vala;
            break;

        case I_RET:
            mem_read = true;
            mem_addr = memory_output->vala;
            mem_data = writeback_input->valm;
            mem_dest = &writeback_input->valm;
            // dmem_error |= !get_word_val(mem, mem_addr, &mem_data);
            break;

        case I_PUSHQ:
            mem_write = true;
            mem_addr = memory_output->vale;
            mem_data = memory_output->vala;
            break;

        case I_POPQ:
            mem_read = true;
            mem_addr = memory_output->vala;
            mem_data = writeback_input->valm;
            mem_dest = &writeback_input->valm;
            // dmem_error |= !get_word_val(mem, mem_addr, &mem_data);
            break;

        case I_LEAQ: break;

        case I_VECADD: break;

        case I_SHF: break;

        default:
            printf("icode is not valid (%d)", memory_output->icode);
            break;
    }

    if (mem_read) {
        if ((dmem_error |= !get_word_val(mem, mem_addr, mem_dest))) {
            writeback_input->status = STAT_ADR;
            sim_log("\tMemory: Couldn't Read from 0x%llx\n", mem_addr);
        } else {
            sim_log("\tMemory: Read 0x%llx from 0x%llx\n",
                *mem_dest, mem_addr);
        }
    }

    if (mem_write) {
        if ((dmem_error |= !set_word_val(mem, mem_addr, mem_data))) {
            writeback_input->status = STAT_ADR;
            sim_log("\tMemory: Couldn't write to address 0x%llx\n", mem_addr);
        } else {
            sim_log("\tMemory: Wrote 0x%llx to address 0x%llx\n", mem_data, mem_addr);
        }
    }
}

/******************** Writeback stage *********************
 * TODO: update [wb_destE, wb_valE, wb_destM, wb_valM, status]
 *******************************************************************/
void do_writeback_stage()
{

    /* your implementation */
    status = writeback_output->status;
    wb_destE = writeback_output->deste;
    wb_destM = writeback_output->destm;
    wb_valE = writeback_output->vale;
    wb_valM = writeback_output->valm;


    if (wb_destE != REG_NONE &&  writeback_output -> status == STAT_AOK) {
	    sim_log("\tWriteback: Wrote 0x%llx to register %s\n",
		    wb_valE, reg_name(wb_destE));
	    set_reg_val(reg, wb_destE, wb_valE);
    }
    if (wb_destM != REG_NONE &&  writeback_output -> status == STAT_AOK) {
	    sim_log("\tWriteback: Wrote 0x%llx to register %s\n",
		    wb_valM, reg_name(wb_destM));
	    set_reg_val(reg, wb_destM, wb_valM);
    }

}

/* given stall and bubble flag, return the correct control operation */
p_stat_t pipe_cntl(char *name, word_t stall, word_t bubble)
{
    if (stall) {
        if (bubble) {
            sim_log("%s: Conflicting control signals for pipe register\n",
                name);
            return P_ERROR;
        } else
            return P_STALL;
    } else {
	    return bubble ? P_BUBBLE : P_LOAD;
    }
}

/******************** Pipeline Register Control ********************
 * TODO: implement stalling or insert a bubble for different stages
 * by modifying the control operations of the pipeline registers
 * you may find the util function pipe_cntl() useful
 *
 * update_pipes() will handle the real control behavior later
 * make sure you have a working PIPE before implementing this
 *******************************************************************/
void do_stall_check()
{
    /* your implementation */
    // dummy placeholders to show the usage of pipe_cntl()

    bool br_mispred = execute_output->icode == I_JMP && !memory_input->takebranch;
    bool load_use = (execute_output->icode == I_MRMOVQ ||  execute_output->icode == I_POPQ) 
    && (execute_output->destm == execute_input->srca || execute_output->destm == execute_input->srcb);
    bool ret_pass = (I_RET == decode_output->icode || I_RET == execute_output->icode || I_RET == memory_output->icode);
    bool m_excep = writeback_input->status == STAT_ADR || writeback_input->status == STAT_INS || writeback_input->status == STAT_HLT;
    bool W_excep = writeback_output->status == STAT_ADR || writeback_output->status == STAT_INS || writeback_output->status == STAT_HLT;


    word_t stall_fetch = load_use || ret_pass;
    word_t stall_decode = load_use;
    word_t stall_writeback = W_excep;

    word_t d_bubble = br_mispred || (!load_use && ret_pass);
    word_t e_bubble = br_mispred || load_use;
    word_t m_bubble = m_excep || W_excep;


    fetch_state->op     = pipe_cntl("PC", stall_fetch, false);
    decode_state->op    = pipe_cntl("ID", stall_decode, d_bubble);
    execute_state->op   = pipe_cntl("EX", false, e_bubble);
    memory_state->op    = pipe_cntl("MEM", false, m_bubble);
    writeback_state->op = pipe_cntl("WB", stall_writeback, false);



}

/*
  Run pipeline until one of following occurs:
  - An error status is encountered in WB.
  - max_instr instructions have completed through WB
  - max_cycle cycles have been simulated

  Return number of instructions executed.
  if statusp nonnull, then will be set to status of final instruction
  if ccp nonnull, then will be set to condition codes of final instruction
*/
word_t sim_run_pipe(word_t max_instr, word_t max_cycle, byte_t *statusp, cc_t *ccp)
{
    word_t icount     = 0;
    word_t ccount     = 0;
    byte_t run_status = STAT_AOK;
    while (icount < max_instr && ccount < max_cycle) {
        run_status = sim_step_pipe(ccount);
        if (run_status != STAT_BUB)
            icount++;
        if (run_status != STAT_AOK && run_status != STAT_BUB)
            break;
        ccount++;
    }
    if (statusp)
	    *statusp = run_status;
    if (ccp)
	    *ccp = cc;
    return icount;
}



void sim_run_cycle(word_t *icount, word_t* ccount, byte_t *statusp, cc_t *ccp)
{

    byte_t run_status = STAT_AOK;
    run_status = sim_step_pipe(*ccount);
    if (run_status != STAT_BUB)
        (*icount)++;
    (*ccount)++;
    if (statusp)
	    *statusp = run_status;
    if (ccp)
	    *ccp = cc;
}

/*
 * sim_log dumps a formatted string to the dumpfile, if it exists
 * accepts variable argument list
 */
void sim_log( const char *format, ... ) {
    if (dumpfile) {
        va_list arg;
        va_start( arg, format );
        vfprintf( dumpfile, format, arg );
        va_end( arg );
    }
}

/**************************************************************
 * Part 4: Code for implementing pipelined processor simulators
 * Do not change any of these
 *************************************************************/

/******************************************************************************
 *	defines
 ******************************************************************************/

#define MAX_STAGE 10

/******************************************************************************
 *	static variables
 ******************************************************************************/

static pipe_ptr pipes[MAX_STAGE];
static int pipe_count = 0;

/******************************************************************************
 *	function definitions
 ******************************************************************************/

/* Create new pipe with count bytes of state */
/* bubble_val indicates state corresponding to pipeline bubble */
pipe_ptr new_pipe(int count, void *bubble_val)
{
    pipe_ptr result = (pipe_ptr) malloc(sizeof(pipe_ele));
    result->output = malloc(count);
    result->input = malloc(count);
    memcpy(result->output, bubble_val, count);
    memcpy(result->input, bubble_val, count);
    result->count = count;
    result->op = P_LOAD;
    result->bubble_val = bubble_val;
    pipes[pipe_count++] = result;
    return result;
}

/* Update all pipes */
void update_pipes()
{
    int s;
    for (s = 0; s < pipe_count; s++) {
    pipe_ptr p = pipes[s];
    switch (p->op)
    {
    case P_BUBBLE:
        /* insert a bubble into the next stage */
        memcpy(p->output, p->bubble_val, p->count);
        break;

    case P_LOAD:
        /* copy calculated state from previous stage */
        memcpy(p->output, p->input, p->count);
        break;
    case P_ERROR:
        /* Like a bubble, but insert error condition */
        memcpy(p->output, p->bubble_val, p->count);
        break;
    case P_STALL:
    default:
        /* do nothing: next stage gets same instr again */
        ;
    }
    if (p->op != P_ERROR)
        p->op = P_LOAD;
    }
}

/* Set all pipes to bubble values */
void clear_pipes()
{
  int s;
    for (s = 0; s < pipe_count; s++) {
        pipe_ptr p = pipes[s];
        memcpy(p->output, p->bubble_val, p->count);
        memcpy(p->input, p->bubble_val, p->count);
        p->op = P_LOAD;
    }
}

/*************** Bubbled version of stages *************/

fetch_ele bubble_fetch = {0,STAT_AOK};
decode_ele bubble_decode = { I_NOP, 0, REG_NONE,REG_NONE,
			   0, 0, STAT_BUB, 0};
execute_ele bubble_execute = { I_NOP, 0, 0, 0, 0,
			   REG_NONE, REG_NONE, REG_NONE, REG_NONE,
			   STAT_BUB, 0};

memory_ele bubble_memory = { I_NOP, 0, false, 0, 0,
			     REG_NONE, REG_NONE, STAT_BUB, 0};

writeback_ele bubble_writeback = { I_NOP, 0, 0, 0, REG_NONE, REG_NONE,
			     STAT_BUB, 0};

typedef struct pipe_restore_struct {
    processor_state_t state;
    word_t cycles;
    pipe_ptr pipes[5];
    struct pipe_restore_struct *next;
} pipe_restore_t;

/*
 * help - Prints the help information for the Trace Runner.
 */
void help() {
    printf("----------------psim Help-----------------------\n");
    printf("go                -  run program to completion\n");
    printf("next n            -  advance n instructions\n");
    printf("cycle n           -  advance n cycles\n");
    printf("memory            -  display differences in memory\n");
    printf("registers         -  display differences in registers\n");
    printf("arch              -  display processor state\n");
    printf("undo n            -  steps back n instructions\n");
    printf("back n            -  steps back n cycles\n");
    printf("pipe X            -  displays pipeline info for stage X (f, d, e, m , w) \n");
    printf("quit              -  exit the program\n\n");
}

static pipe_restore_t *create_pipe_restore_point(word_t *icount, word_t* ccount, byte_t *statusp, cc_t *ccp) {
    pipe_restore_t *pipe_restore_point = malloc(sizeof(pipe_restore_t));
    pipe_restore_point->state.cc = *ccp;
    pipe_restore_point->state.status = *statusp;
    pipe_restore_point->cycles = *ccount;
    pipe_restore_point->state.icount = *icount;
    for (int s = 0; s < pipe_count; s++) {
        pipe_ptr p = pipes[s];
        pipe_restore_point->pipes[s] = (pipe_ptr) malloc(sizeof(pipe_ele));
        pipe_restore_point->pipes[s]->bubble_val = p->bubble_val;
        pipe_restore_point->pipes[s]->op = p->op;
        pipe_restore_point->pipes[s]->count = p->count;
        pipe_restore_point->pipes[s]->output = malloc(p->count);
        pipe_restore_point->pipes[s]->input = malloc(p->count);
        memcpy(pipe_restore_point->pipes[s]->input, p->input, p->count);
        memcpy(pipe_restore_point->pipes[s]->output, p->output, p->count);
    }
    mem_t previous_memory = copy_mem(mem);
    mem_t previous_registers = copy_mem(reg);
    sim_run_cycle(icount, ccount, statusp, ccp);
    pipe_restore_point->state.memory = create_memory_restore(previous_memory, mem);
    pipe_restore_point->state.registers = create_memory_restore(previous_registers, reg);
    return pipe_restore_point;
}

static void restore_pipes_and_free(pipe_restore_t *pipe_restore_point) {
    for (int s = 0; s < pipe_count; s++) {
        pipe_ptr p = pipes[s];
        p->bubble_val = pipe_restore_point->pipes[s]->bubble_val;
        p->op = pipe_restore_point->pipes[s]->op;
        p->count = pipe_restore_point->pipes[s]->count;
        memcpy(p->input, pipe_restore_point->pipes[s]->input, p->count);
        memcpy(p->output, pipe_restore_point->pipes[s]->output, p->count);
        free(pipe_restore_point->pipes[s]->output);
        free(pipe_restore_point->pipes[s]->input);
        free(pipe_restore_point->pipes[s]);
    }

    if (pipe_restore_point->state.memory->positions != NULL)
        free(pipe_restore_point->state.memory->positions);
    if (pipe_restore_point->state.memory->values != NULL)
        free(pipe_restore_point->state.memory->values);
    free(pipe_restore_point->state.memory);

    if (pipe_restore_point->state.registers->positions != NULL)
        free(pipe_restore_point->state.registers->positions);
    if (pipe_restore_point->state.registers->values != NULL)
        free(pipe_restore_point->state.registers->values);
    free(pipe_restore_point->state.registers);

    free(pipe_restore_point);
}

void sim_interactive()
{
    word_t ccount = 0, icount = 0, ucount = 0;
    status = STAT_AOK;
    word_t byte_cnt = 0;
    int instructions_to_run, cycles_to_run;
    int instructions_to_undo, cycles_to_undo;
    word_t ccount_stored = 0, icount_stored = 0;
    cc_t curr_cc = DEFAULT_CC;

	dumpfile = stdout;
    sim_init();

    /* Emit simulator name */
    printf("%s\n", simname);

    byte_cnt = load_mem(mem, object_file, 1);
    if (byte_cnt == 0) {
	    fprintf(stderr, "No lines of code found\n");
	    exit(1);
    } else if (verbosity >= 2) {
	    printf("%lld bytes of code read\n", byte_cnt);
    }
    fclose(object_file);

    mem_t mem0, reg0;
    mem0 = copy_mem(mem);
    reg0 = copy_mem(reg);

    pipe_restore_t *restore_head = NULL;

    char buffer[20];
    char stage_buffer[20];
    byte_t run_status = STAT_AOK;

    while(1) {
        printf("PIPE> ");

        int size = scanf("%s", buffer);
            if (size == 0) {
            buffer[0] = 'x';
        }
        printf("\n");

        switch(buffer[0]) {
        case 'G':
        case 'g':
            if(run_status == STAT_AOK || run_status == STAT_BUB) {
                ccount_stored = ccount;
                icount_stored = icount;
                while ((run_status == STAT_AOK || run_status == STAT_BUB)) {
                    pipe_restore_t *new_restore_point = create_pipe_restore_point(&icount, &ccount, &run_status, &curr_cc);
                    new_restore_point->next = restore_head;
                    restore_head = new_restore_point;
                }

                printf("Simulator ran %lld instructions in %lld cycles\n", icount - icount_stored, ccount - ccount_stored);
            } else {
                printf("Simulator is in a non AOK state\n");
            }

            printf("Simulator ran to completion\n");
            break;

        case 'h':
        case 'H':
            help();
            break;

        case 'Q':
        case 'q':
            printf("Bye.\n");
            exit(0);

        case 'N':
        case 'n':
            size = scanf("%d", &instructions_to_run);
            if (size == 0) {
                break;
            }

            if(run_status == STAT_AOK || run_status == STAT_BUB) {
                icount_stored = icount;
                ccount_stored = ccount;
                while ((run_status == STAT_AOK || run_status == STAT_BUB) && icount - icount_stored < instructions_to_run) {
                    pipe_restore_t *new_restore_point = create_pipe_restore_point(&icount, &ccount, &run_status, &curr_cc);
                    new_restore_point->next = restore_head;
                    restore_head = new_restore_point;
                }

                printf("Simulator ran %lld instructions in %lld cycles\n", icount - icount_stored, ccount - ccount_stored);
            } else {
                printf("Simulator is in a non AOK state\n");
            }

            if (run_status != STAT_AOK && run_status != STAT_BUB) {
                printf("Simulator ran to completion\n");
            }

            break;

        case 'C':
        case 'c':
            size = scanf("%d", &cycles_to_run);
            if (size == 0) {
                break;
            }

            if(run_status == STAT_AOK || run_status == STAT_BUB) {
                icount_stored = icount;
                ccount_stored = ccount;
                while ((run_status == STAT_AOK || run_status == STAT_BUB) && ccount - ccount_stored < cycles_to_run) {
                    pipe_restore_t *new_restore_point = create_pipe_restore_point(&icount, &ccount, &run_status, &curr_cc);
                    new_restore_point->next = restore_head;
                    restore_head = new_restore_point;
                }

                printf("Simulator ran %lld instructions in %lld cycles\n", icount - icount_stored, ccount - ccount_stored);
            } else {
                printf("Simulator is in a non AOK state\n");
            }

            if (run_status != STAT_AOK && run_status != STAT_BUB) {
                printf("Simulator ran to completion\n");
            }

            break;

        case 'M':
        case 'm':
            diff_mem(mem0, mem, stdout);
            break;

        case 'R':
        case 'r':
            diff_reg(reg0, reg, stdout);
            break;

        case 'U':
        case 'u':
            size = scanf("%d", &instructions_to_undo);
            if (size == 0) {
                break;
            }
            ucount = 0;
            icount_stored = icount;
            ccount_stored = ccount;
            while (restore_head != NULL && (icount_stored - icount) < instructions_to_undo) {
                cc = restore_head->state.cc;
                icount = restore_head->state.icount;
                ccount = restore_head->cycles;
                apply_restore(mem, restore_head->state.memory);
                apply_restore(reg, restore_head->state.registers);
                status = restore_head->state.status;
                run_status = status;
                pipe_restore_t *temp = restore_head;
                restore_head = restore_head->next;
                restore_pipes_and_free(temp);
                ucount++;
            }
            printf("Instructions undone: %lld Cycles undone: %lld\n", icount_stored - icount, ccount_stored - ccount);


        case 'A':
        case 'a':
            print_state(ccount);
            dump_reg_display(stdout, reg);
            break;

        case 'B':
        case 'b':
            size = scanf("%d", &cycles_to_undo);
            if (size == 0) {
                break;
            }
            ucount = 0;
            ccount_stored = ccount;
            icount_stored = icount;
            while (restore_head != NULL && (ccount_stored - ccount) < cycles_to_undo) {
                cc = restore_head->state.cc;
                icount = restore_head->state.icount;
                ccount = restore_head->cycles;
                apply_restore(mem, restore_head->state.memory);
                apply_restore(reg, restore_head->state.registers);
                status = restore_head->state.status;
                run_status = status;
                pipe_restore_t *temp = restore_head;
                restore_head = restore_head->next;
                restore_pipes_and_free(temp);
                ucount++;
            }
            printf("Instructions undone: %lld Cycles undone: %lld\n", icount_stored - icount, ccount_stored - ccount);
            print_state(ccount);
            dump_reg_display(stdout, reg);
            break;

        case 'P':
        case 'p':
            size = scanf("%s", stage_buffer);
            if (size == 0) {
                stage_buffer[0] = 'x';
            }

            switch(stage_buffer[0]) {
                case 'f':
                case 'F':
                    print_fetch();
                    break;
                case 'd':
                case 'D':
                    print_decode();
                    break;
                case 'e':
                case 'E':
                    print_execute();
                    break;
                case 'm':
                case 'M':
                    print_memory();
                    break;
                case 'w':
                case 'W':
                    print_writeback();
                    break;
                default:
                    printf("Invalid Stage\n");
                    break;
            }
            break;

        default:
            printf("Invalid Command\n");
            break;
        }
    }
}