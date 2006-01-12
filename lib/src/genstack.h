/*
 *  Copyright (c) by Allin Cottrell
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#ifndef GENSTACK_H
#define GENSTACK_H

#define GENR_DEBUG 0

#if GENR_DEBUG
void dprintf (const char *format, ...);
# define DPRINTF(x) dprintf x
#else 
# define DPRINTF(x)
#endif /* GENR_DEBUG */

#define ATOMLEN 32  /* length of auxiliary string in genr atom */

enum {
    ATOM_SERIES = 0,
    ATOM_SCALAR = 1 << 0,
    ATOM_MATRIX = 1 << 1,
    ATOM_TMP    = 1 << 2
};

typedef struct genatom_ genatom;
typedef struct atomset_ atomset;

struct genatom_ {
    char level;
    char atype;
    int varnum;
    int varobs;
    int tmpvar;
    char lag;
    double val;
    char func;
    char op;
    char popped;
    char str[ATOMLEN];
    gretl_matrix *M;
    genatom *parent;
    atomset *aset;
};

/* below: matches funcs[] in generate.c -- these are in addition to
   the (mostly) standard math functions in the GretlMathFunc
   enumeration
*/
enum transformations {
    T_DIFF = T_MATHMAX,
    T_LDIFF, 
    T_SDIFF,
    T_MEAN, 
    T_SD, 
    T_MIN,
    T_MAX,
    T_SORT, 
    T_COEFF,
    T_RHO, 
    T_SUM, 
    T_NOBS,
    T_T1,
    T_T2,
    T_STDERR,
    T_CUM, 
    T_MISSING,
    T_OK,
    T_MISSZERO,
    T_CORR,
    T_VCV,
    T_VAR,
    T_SST,
    T_COV,
    T_MEDIAN,
    T_GINI,
    T_ZEROMISS,
    T_PVALUE,
    T_CRIT,
    T_OBSNUM,
    T_MPOW,
#ifdef HAVE_MPFR
    T_MLOG,
#endif
    T_RESAMPLE,
    T_HPFILT,
    T_BKFILT,
    T_FRACDIFF,
    T_VARNUM,
    T_VECTOR,
    T_ISLIST,
    T_NELEM,
    T_DET,
    T_INV,
    T_LDET,
    T_TRACE,
    T_DIAG,
    T_ROWS,
    T_COLS,
    T_TRANSP,
    T_IMAT,
    T_ZEROS,
    T_ONES,
    T_IDENTITY
};

#define VALSTACK_SIZE 32
#define MATSTACK_SIZE 32

enum genr_flags {
    GENR_SAVE         = 1 << 0,
    GENR_SCALAR       = 1 << 1,
    GENR_FORCE_VECTOR = 1 << 2,
    GENR_NEED_SCALAR  = 1 << 3,
    GENR_WARN         = 1 << 4,
    GENR_SIMPLE_SORT  = 1 << 5,
    GENR_PRIVATE      = 1 << 6,
    GENR_MATRIX       = 1 << 7
};

struct _GENERATOR {
    int err;                        /* error code */  
    int done;                       /* completion indicator */
    char orig_s[MAXLINE];           /* formula provided by user */
    char lhs[USER_VLEN];            /* left-hand side of formula */
    unsigned char flags;            /* option flags */
    double *xvec;                   /* temporary storage */
    int varnum;                     /* number of variable generated */
    int obs;                        /* observation number, if generating
				       a single observation */
    char varname[32];               /* name of variable generated */
    char label[MAXLABEL];           /* descriptive label for variable,
				       or submatrix specification */
    int tmpv;                       /* temporary variable ID */
    double **tmpZ;                  /* temporary dataset */
    DATAINFO *pdinfo;               /* pointer to "outer" data info */
    double ***pZ;                   /* pointer to "outer" dataset */
    atomset *aset;                  /* set of atomic compoenents */
    double valstack[VALSTACK_SIZE]; /* stack of temporary values */
    int nvals;                      /* number of temporary values */
    gretl_matrix **mstack;          /* stack of temporary matrices */
    int nmats;                      /* number of same */
    char **S;                       /* array of obs strings, used in
				       certain cases of sorting */
};

int attach_atomset (GENERATOR *genr);
int push_atom (genatom *atom);
genatom *pop_atom (GENERATOR *genr);
genatom *pop_child_atom (genatom *atom);
genatom *peek_child_atom (genatom *atom);
void reset_atom_stack (GENERATOR *genr);
void destroy_atom_stack (GENERATOR *genr);
void atom_stack_set_parentage (GENERATOR *genr);
void atom_eat_children (genatom *atom);
void atom_stack_bookmark (GENERATOR *genr);
void atom_stack_resume (GENERATOR *genr);
int atom_stack_check_for_scalar (GENERATOR *genr);
void atom_stack_nullify_matrix (gretl_matrix *M, GENERATOR *genr);

int calc_push (double x, GENERATOR *genr);
double calc_pop (GENERATOR *genr);
void reset_calc_stack (GENERATOR *genr);

int matrix_calc_push (gretl_matrix *M, GENERATOR *genr);
gretl_matrix *matrix_calc_pop (GENERATOR *genr);
void reset_matrix_calc_stack (GENERATOR *genr);

const char *get_genr_func_word (int fnum);

#endif /* GENSTACK_H */
