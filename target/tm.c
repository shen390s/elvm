#include <assert.h>
#include <ctype.h>
#include <ir/ir.h>
#include <target/util.h>

typedef enum {BLANK, START, ZERO, ONE, REGISTER, ADDRESS, VALUE, OUTPUT, SRC, DST, SCRATCH} symbol_t;
const char *symbol_names[] = {"_", "^", "0", "1", "r", "a", "v", "o", "s", "d", "."};
const int num_symbols = 11;

const int word_size = 8;

int next_state;
int q_reject;

int new_state() {
  return next_state++;
}

/* These functions take a start state and an accept state(s) as
   arguments and, as a convenience, return the accept state. */

/* Generate a transition from state q, read symbol a,
   write symbol b, move in direction d, go to state r. */

int tm_transition(int q, symbol_t a, symbol_t b, int d, int r) {
  const char *dname;
  if (d == -1)      dname = "L";
  else if (d == 0)  dname = "N"; 
  else if (d == +1) dname = "R";
  else error("invalid direction");
  emit_line("%d %s %d %s %s", 
	    q, symbol_names[a], 
	    r, symbol_names[b], dname);
  return r;
}

/* Generate transitions to write symbol b and move in direction d,
   regardless of input symbol. */

int tm_write(int q, symbol_t b, int d, int r) {
  for (symbol_t a=0; a<num_symbols; a++)
    tm_transition(q, a, b, d, r);
  return r;
}

/* Generate write transitions that do one thing for symbol a, 
   and another thing for all other symbols.

   Returns the state for the latter case. */

int tm_write_if(int q, 
		symbol_t a, int ba, int da, int ra,
		symbol_t b, int d, int r) {
  for (symbol_t s=0; s<num_symbols; s++)
    if (s == a) tm_transition(q, s, ba, da, ra); 
    else        tm_transition(q, s, b, d, r);
  return r;
}

/* Generate transitions to move in direction d, regardless of input symbol. */

int tm_move(int q, int d, int r) {
  for (symbol_t s=0; s<num_symbols; s++)
    tm_transition(q, s, s, d, r);
  return r;
}

/* Generate move transitions that do one thing for symbol a, 
   and another thing for all other symbols.

   Returns the state for the latter case. */

int tm_move_if(int q, 
	       symbol_t a, int da, int ra, 
	       int d, int r) {
  for (symbol_t s=0; s<num_symbols; s++)
    if (s == a)      tm_transition(q, s, s, da, ra);
    else             tm_transition(q, s, s, d, r);
  return r;
}

/* Generate move transitions that do one thing for symbol a, 
   another thing for symbol b,
   and another thing for all other symbols.

   Returns the state for the last case. */

int tm_move_if2(int q, 
		symbol_t a, int da, int ra, 
		symbol_t b, int db, int rb, 
		int d, int r) {
  for (symbol_t s=0; s<num_symbols; s++)
    if (s == a)      tm_transition(q, s, s, da, ra);
    else if (s == b) tm_transition(q, s, s, db, rb);
    else             tm_transition(q, s, s, d, r);
  return r;
}

/* Generate transitions that just change state and do nothing else. */
int tm_noop(int q, int r) { 
  return tm_move(q, 0, r); 
}

int tm_write_bits(int q, unsigned int x, int n, int r) {
  for (int i=n-1; i>0; i--) {
    q = tm_write(q, SCRATCH, +1, new_state());
    q = tm_write(q, (1<<i)&x?ONE:ZERO, +1, new_state());
  }
  q = tm_write(q, SCRATCH, +1, new_state());
  return tm_write(q, 1&x?ONE:ZERO, +1, r);
}

/* Generate transitions to write a machine word in binary,
   leaving a scratch cell before each bit. */
int tm_write_word(int q, unsigned int x, int r) {
  return tm_write_bits(q, x, word_size, r);
}

/* Generate transitions to write a byte in binary,
   leaving a scratch cell before each bit. */

int tm_write_byte(int q, unsigned int x, int r) {
  return tm_write_bits(q, x, 8, r);
}

/* Generate transitions to search in direction d for symbol a,
   or until the end of the used portion of the tape is reached. */

int tm_find(int q, int d, int a, int r_yes, int r_no) {
  symbol_t marker = d < 0 ? START : BLANK;
  tm_move_if2(q, a, 0, r_yes, marker, 0, r_no, d, q);
  return r_yes;
}

/* Generate transitions to move to the left end of the tape. */

int tm_rewind(int q, int r) { 
  tm_move_if(q, START, 0, r, -1, q);
  return r;
}

/* Generate transitions to move to the right end of the used portion
   of the tape. */

int tm_ffwd(int q, int r) { 
  tm_move_if(q, BLANK, 0, r, +1, q);
  return r;
}

/* Generate transitions to find register reg. The head ends on the
   scratch cell to the left of reg's value. */

int tm_find_register(int q, Reg reg, int r) {
  int qstart = q;
  q = tm_find(q, +1, REGISTER, new_state(), q_reject); // .[r].0.1 ... .v.0.1
  q = tm_move(q, +1, new_state());                     // .r[.]0.1 ... .v.0.1
  for (int i=word_size-1; i>=0; i--) {
    q = tm_move(q, +1, new_state());                   // .r.[0].1 ... .v.0.1
    symbol_t bit = (1<<i)&reg ? ONE : ZERO;
    int q_match = new_state();
    tm_move_if2(q,
		bit, +1, q_match,                      // .r.0[.]1 ... .v.0.1
		BLANK, 0, q_reject,
		+1, qstart);
    q = q_match;
  }
  q = tm_move(q, +1, new_state());                     // .r.0.1 ... .[v].0.1
  tm_move_if(q,
	     VALUE, +1, r,                             // .r.0.1 ... .v[.]0.1
	     0, q_reject);
  return r;
}

int tm_copy_helper(int q, int d, int scratch, int r) {
                                                      // [.]0.1 ... dx.x
  q = tm_write(q, SRC, 0, new_state());               // [s]0.1 ... dx.x
  int q_nextbit = q;
  q = tm_write(q, SCRATCH, +1, new_state());          // .[0].1 ... dx.x
  int q0 = new_state(), q1 = new_state();
  int q_clean = tm_move_if2(q,
			   ZERO, +1, q0,              // .0[.]1 ... dx.x
			   ONE, +1, q1,
			   0, new_state());
  q = new_state();
  q0 = tm_write(q0, SRC, +1, new_state());            // .0s[1] ... dx.x
  q0 = tm_find(q0, d, DST, new_state(), q_reject);    // .0s1 ... [d]x.x
  if (scratch)
    q0 = tm_write(q0, SCRATCH, +1, new_state());      // .0s1 ... .[x].x
  q0 = tm_write(q0, ZERO, +1, q);                     // .0s1 ... .0[.]x
  q1 = tm_write(q1, SRC, +1, new_state());
  q1 = tm_find(q1, d, DST, new_state(), q_reject);
  if (scratch)
    q1 = tm_write(q1, SCRATCH, +1, new_state());
  q1 = tm_write(q1, ONE, +1, q);
  q = tm_write(q, DST, 0, new_state());               // .0s1 ... .0[d]x
  tm_find(q, -d, SRC, q_nextbit, q_reject);           // .0[s]1 ... .0dx
  q = tm_find(q_clean, d, DST, new_state(), q_reject);
  if (scratch)
    return tm_write(q, SCRATCH, 0, r);
  else
    return tm_noop(q, r);
}

/* Copy bits from current position to the position marked by DST.

   The head starts on the scratch cell to the left of the source word,
   and ends on the scratch cell to the right of the destination
   word. */

int tm_copy(int q, int d, int r) {
  return tm_copy_helper(q, d, 1, r);
}

/* Copy bits from current position to the position marked by DST.

   The head starts on the scratch cell to the left of the source word,
   and ends on the cell to the right of the destination word. The
   destination word does not have scratch cells, and the DST marker is
   also not deleted afterwards. */

int tm_compact(int q, int d, int r) {
  return tm_copy_helper(q, d, 0, r);
}

void target_tm(Module* module) {
  /* Every basic block's entry point is the state with the same number
     as its pc. Additional states are numbered starting after the
     highest pc. */
  next_state = 0;
  for (Inst* inst = module->text; inst; inst = inst->next)
    if (inst->pc >= next_state)
      next_state = inst->pc+1;

  printf("// beginning-of-tape marker\n");
  q_reject = new_state();
  int q = 0; // current state
  q = tm_write(q, START, +1, new_state());

  // Initialize registers
  for (Reg reg=0; reg<6; reg++) {
    printf("\n// register %s value 0\n", reg_names[reg]);
    q = tm_write(q, SCRATCH, +1, new_state());
    q = tm_write(q, REGISTER, +1, new_state());
    q = tm_write_word(q, reg, new_state());
    q = tm_write(q, SCRATCH, +1, new_state());
    q = tm_write(q, VALUE, +1, new_state());
    q = tm_write_word(q, 0, new_state());
  }

  // Initialize memory
  Data *data = module->data;
  int mp = 0;
  while (data) {
    if (isprint(data->v))
      printf("\n// address %d value %d '%c'\n", mp, data->v, data->v);
    else
      printf("\n// address %d value %d\n", mp, data->v);
    q = tm_write(q, SCRATCH, +1, new_state());
    q = tm_write(q, ADDRESS, +1, new_state());
    q = tm_write_word(q, mp, new_state());
    q = tm_write(q, SCRATCH, +1, new_state());
    q = tm_write(q, VALUE, +1, new_state());
    q = tm_write_word(q, data->v, new_state());
    data = data->next;
    mp++;
  }
  q = tm_rewind(q, new_state());

  int prev_pc = 0;
  for (Inst* inst = module->text; inst; inst = inst->next) {
    printf("\n// "); dump_inst_fp(inst, stdout);

    // If new pc, transition to state corresponding to new pc
    if (inst->pc != prev_pc && q != inst->pc)
      q = tm_noop(q, inst->pc);
    prev_pc = inst->pc;

    switch (inst->op) {

    case MOV:
      assert(inst->dst.type == REG);
      if (inst->src.type == REG) {
	if (inst->dst.reg == inst->src.reg)
	  break;
	q = tm_find_register(q, inst->dst.reg, new_state());
	q = tm_write(q, DST, -1, new_state());
	q = tm_rewind(q, new_state());
	q = tm_find_register(q, inst->src.reg, new_state());
	if (inst->dst.reg > inst->src.reg)
	  q = tm_copy(q, +1, new_state());
	else
	  q = tm_copy(q, -1, new_state());
      } else if (inst->src.type == IMM) {
	q = tm_find_register(q, inst->dst.reg, new_state());
	q = tm_write_word(q, inst->src.imm, new_state());
      } else
	error("invalid src type");
      q = tm_rewind(q, new_state());
      break;

    case JMP:
      if (inst->jmp.type == REG)
	error("not implemented");
      else if (inst->jmp.type == IMM)
	q = tm_noop(q, inst->jmp.imm);
      else
	error("invalid jmp type");
      break;

    case PUTC:
      q = tm_ffwd(q, new_state());
      q = tm_write(q, SCRATCH, +1, new_state());
      q = tm_write(q, OUTPUT, +1, new_state());
      if (inst->src.type == REG) {
	q = tm_write(q, DST, -1, new_state());
	q = tm_rewind(q, new_state());
	q = tm_find_register(q, inst->src.reg, new_state());
	q = tm_copy(q, +1, new_state());
      } else if (inst->src.type == IMM) {
	q = tm_write_byte(q, inst->src.imm, new_state());
      } else
	error("invalid src type");
      q = tm_rewind(q, new_state());
      break;

    case EXIT:
      // Consolidate output segments
      q = tm_write(q, DST, +1, new_state());              // d[.]
      int q_clear = new_state();
      int q_findo = q;
      q = tm_find(q, +1, OUTPUT, new_state(), q_clear);   // [o].0.1
      q = tm_write(q, SCRATCH, +1, new_state());          // .[.]0.1
      q = tm_compact(q, -1, q_findo);

      // Clear rest of tape
      q_clear = tm_find(q_clear, -1, DST, new_state(), q_reject);
      q_clear = tm_write_if(q_clear, 
			    BLANK, BLANK, 0, -1,
			    BLANK, +1, q_clear);
      break;

    case DUMP:
      break;

    default:
      error("not implemented");
    }
  }
}
