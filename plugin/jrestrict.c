/*
 *  Copyright (c) by Allin Cottrell and Riccardo "Jack" Lucchetti
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
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

/* General restrictions on beta in context of a VECM.  Based on an ox
   program by Jack, June 2007.  Rendered into C by Allin.
*/

#include "libgretl.h"
#include "random.h"
#include "johansen.h"
#include "var.h"
#include "gretl_restrict.h"
#include "jprivate.h"

#define JDEBUG 1

typedef struct Jwrap_ Jwrap;

struct Jwrap_ {
    int T;          /* length of time series used */
    int p;          /* number of equations */
    int p1;         /* number of rows in beta (>= p) */
    int rank;       /* rank, r, of VECM */
    int blen;       /* number of unrestricted coefficients in beta */
    int alen;       /* number of unrestricted coefficients in alpha */
    int bnoest;     /* fully constrained beta: no estimation required */
    int df;         /* degrees if freedom for LR test */
    int jr;         /* rank of Jacobian */
    double ldS00;   /* base component of log-likelihood */
    double ll;      /* log-likelihood */

    /* moment matrices and copies */
    const gretl_matrix *S00;
    const gretl_matrix *S01;
    const gretl_matrix *S11;

    gretl_matrix *S00i;
    gretl_matrix *S11m;

    /* restrictions on beta */
    gretl_matrix *H;
    gretl_matrix *s;

    /* homogeneous restrictions on alpha */
    gretl_matrix *G;

    /* coefficients and variances */
    gretl_matrix *beta;
    gretl_matrix *alpha;
    gretl_matrix *Omega;
    gretl_matrix *V;
    gretl_matrix *se;

    /* free parameter vector */
    gretl_matrix *theta;

    /* temp storage for beta calculation */
    gretl_matrix *phivec;

    /* alpha calculation */
    gretl_matrix *psivec;

    /* temporary storage */
    gretl_matrix *qf1;
    gretl_matrix *qf2;
};

static int compute_alpha (Jwrap *J);
static int make_beta_se (Jwrap *J);

static int make_S_matrices (Jwrap *J, const GRETL_VAR *jvar)
{
    gretl_matrix *Tmp = NULL;
    int err = 0;

    J->S00 = jvar->jinfo->S00;
    J->S01 = jvar->jinfo->S01;
    J->S11 = jvar->jinfo->S11;

    J->S00i = gretl_matrix_copy(J->S00);
    J->S11m = gretl_matrix_copy(J->S11);

    if (J->S00i == NULL || J->S11m == NULL) {
	return E_ALLOC;
    }

    J->p1 = J->S01->cols;

    Tmp = gretl_matrix_alloc(J->p1, J->p1);

    if (Tmp == NULL) {
	return E_ALLOC;
    }

    J->ldS00 = gretl_matrix_log_determinant(J->S00i, &err);

    if (!err) {
	gretl_matrix_copy_values(J->S00i, J->S00);
	err = gretl_invert_symmetric_matrix(J->S00i);
    }

    if (!err) {
	err = gretl_matrix_qform(J->S01, GRETL_MOD_TRANSPOSE,
				 J->S00i, Tmp, GRETL_MOD_NONE);
    }

    if (!err) {
	err = gretl_matrix_subtract_from(J->S11m, Tmp);
    }

    if (!err) {
	/* allocate beta and alpha while we're at it */
	J->beta = gretl_matrix_alloc(J->p1, J->rank);
	J->alpha = gretl_matrix_alloc(J->p, J->rank);
	if (J->beta == NULL || J->alpha == NULL) {
	    err = E_ALLOC;
	} else {
	    J->blen = J->p1 * J->rank;
	    J->alen = J->p * J->rank;
	}
    }

    if (!err) {
	/* and Omega too */
	J->Omega = gretl_matrix_alloc(J->p, J->p);
	if (J->Omega == NULL) {
	    err = E_ALLOC;
	}
    }

#if JDEBUG > 1
    gretl_matrix_print(J->S00, "S00");
    gretl_matrix_print(J->S01, "S01");
    gretl_matrix_print(J->S11, "S11");
    gretl_matrix_print(J->S00i, "S00i");
    gretl_matrix_print(J->S11m, "S11m");
#endif    

    gretl_matrix_free(Tmp);

    return err;
}

static int J_alloc_qfs (Jwrap *J)
{
    int r = J->rank;

    J->qf1 = gretl_matrix_alloc(r, r);
    J->qf2 = gretl_matrix_alloc(r, r);

    if (J->qf1 == NULL || J->qf2 == NULL) {
	return E_ALLOC;
    }

    return 0;
}

static void jwrap_destroy (Jwrap *J)
{
    gretl_matrix_free(J->S00i);
    gretl_matrix_free(J->S11m);

    gretl_matrix_free(J->G);
    gretl_matrix_free(J->H);
    gretl_matrix_free(J->s);

    gretl_matrix_free(J->beta);
    gretl_matrix_free(J->alpha);
    gretl_matrix_free(J->Omega);
    gretl_matrix_free(J->V);
    gretl_matrix_free(J->se);

    gretl_matrix_free(J->phivec);
    gretl_matrix_free(J->psivec);
    gretl_matrix_free(J->theta);

    gretl_matrix_free(J->qf1);
    gretl_matrix_free(J->qf2);

    free(J);
}

static Jwrap *jwrap_new (const GRETL_VAR *jvar, int *err)
{
    Jwrap *J = malloc(sizeof *J);

    if (J == NULL) {
	return NULL;
    }

    J->T = jvar->T;
    J->p = jvar->neqns;
    J->rank = jrank(jvar);
    J->blen = 0;
    J->alen = 0;
    J->bnoest = 0;
    J->df = 0;
    J->jr = 0;

    J->ll = NADBL;

    J->S00 = NULL;
    J->S01 = NULL;
    J->S11 = NULL;

    J->S00i = NULL;
    J->S11m = NULL;

    J->H = NULL;
    J->s = NULL;
    J->G = NULL;

    J->beta = NULL;
    J->alpha = NULL;
    J->Omega = NULL;
    J->V = NULL;
    J->se = NULL;

    J->phivec = NULL;
    J->psivec = NULL;
    J->theta = NULL;

    J->qf1 = NULL;
    J->qf2 = NULL;

    *err = make_S_matrices(J, jvar);

    if (!*err) {
	*err = J_alloc_qfs(J);
    }	

    if (*err) {
	jwrap_destroy(J);
	J = NULL;
    }

    return J;
}

/* produce the \beta matrix in the case where it is fully
   constrained (and therefore no estimation is needed)
*/

static int solve_for_beta (Jwrap *J, 
			   const gretl_matrix *R,
			   const gretl_matrix *q)
{
    gretl_matrix *b;
    int err = 0;

    b = gretl_matrix_copy(q);
    if (b == NULL) {
	return E_ALLOC;
    }

    if (!gretl_is_identity_matrix(R)) {
	gretl_matrix *Rcpy = gretl_matrix_copy(R);

	if (Rcpy == NULL) {
	    err = E_ALLOC;
	} else {
	    err = gretl_LU_solve(Rcpy, b);
	    gretl_matrix_free(Rcpy);
	}
    }

    if (!err) {
	err = gretl_matrix_copy_values_shaped(J->beta, b);
    }

    if (!err) {
	J->bnoest = 1;
    }

    gretl_matrix_free(b);
    
    return err;
}

typedef struct switcher_ switcher;

struct switcher_ {
    int finalize;
    gretl_matrix *K1;     /* holds kronecker product */
    gretl_matrix *K2;     /* holds kronecker product */
    gretl_matrix *I00;    /* chunk 0,0 of information matrix */
    gretl_matrix *I11;    /* chunk 1,1 of information matrix */
    gretl_matrix *TmpL;   /* used only in \Phi calculation */
    gretl_matrix *TmpR;   /* shared temp */
    gretl_matrix *TmpR1;  /* used when \alpha is restricted */
    gretl_matrix *Tmppp;  /* p x p temp matrix */
    gretl_matrix *Tmprp;  /* r x p temp */
    gretl_matrix *Tmprp1; /* r x p1 temp */
    gretl_matrix *HK2;    /* used in \Phi calculation */
    gretl_matrix *Pi;     /* \alpha \beta' */
    gretl_matrix *lsPi;   /* vec of unrestricted \Pi */
    gretl_matrix *iOmega; /* Omega-inverse */
};

static void switcher_free (switcher *s)
{
    gretl_matrix_free(s->K1);
    gretl_matrix_free(s->K2);
    gretl_matrix_free(s->I00);
    gretl_matrix_free(s->I11);
    gretl_matrix_free(s->TmpL);
    gretl_matrix_free(s->TmpR);
    gretl_matrix_free(s->TmpR1);
    gretl_matrix_free(s->Tmppp);
    gretl_matrix_free(s->Tmprp);
    gretl_matrix_free(s->Tmprp1);
    gretl_matrix_free(s->HK2);
    gretl_matrix_free(s->Pi);
    gretl_matrix_free(s->lsPi);
    gretl_matrix_free(s->iOmega);
}

static int switcher_init (switcher *s, Jwrap *J)
{
    gretl_matrix *S11i = NULL;
    int r = J->rank;
    int err = 0;

    s->K1 = NULL; 
    s->K2 = NULL;
    s->I00 = NULL;
    s->I11 = NULL;
    s->TmpL = NULL;
    s->TmpR = NULL;
    s->TmpR1 = NULL;
    s->Tmppp = NULL;
    s->Tmprp = NULL;
    s->Tmprp1 = NULL;
    s->HK2 = NULL;
    s->Pi = NULL;
    s->lsPi = NULL;
    s->iOmega = NULL;

    s->K1 = gretl_matrix_alloc(J->p1 * r, J->p1 * r);
    s->K2 = gretl_matrix_alloc(J->p1 * r, J->p1 * J->p1);
    s->I00 = gretl_matrix_alloc(J->alen, J->alen);
    s->I11 = gretl_matrix_alloc(J->blen, J->blen);
    s->TmpL = gretl_matrix_alloc(J->blen, J->p * J->p1);
    s->TmpR = gretl_matrix_alloc(J->p1 * J->p1, 1);
    s->Tmppp = gretl_matrix_alloc(J->p, J->p);
    s->Tmprp = gretl_matrix_alloc(r, J->p);
    s->Tmprp1 = gretl_matrix_alloc(r, J->p1);
    s->HK2 = gretl_matrix_alloc(J->blen, J->p * J->p1);
    s->Pi = gretl_matrix_alloc(J->p, J->p1);
    s->iOmega = gretl_matrix_alloc(J->p, J->p);

    if (s->K1 == NULL || s->K2 == NULL || 
	s->I00 == NULL || s->I11 == NULL ||
	s->TmpL  == NULL || s->TmpR  == NULL || 
	s->Tmppp == NULL || s->Tmprp == NULL || 
	s->Tmprp1 == NULL || s->HK2 == NULL || 
	s->Pi == NULL || s->iOmega == NULL) {
	return E_ALLOC;
    }

    if (J->G != NULL) {
	s->TmpR1 = gretl_matrix_alloc(J->alen, J->p * J->p1);
	if (s->TmpR1 == NULL) {
	    return E_ALLOC;
	}
    }

    S11i = gretl_matrix_copy(J->S11);
    s->lsPi = gretl_matrix_alloc(J->p1, J->p);
    if (S11i == NULL || s->lsPi == NULL) {
	err = E_ALLOC;
    } else {
	gretl_invert_symmetric_matrix(S11i);
	gretl_matrix_multiply_mod(S11i, GRETL_MOD_NONE,
				  J->S01, GRETL_MOD_TRANSPOSE,
				  s->lsPi, GRETL_MOD_NONE);
	/* make into vec(\Pi'_{LS}) */
	gretl_matrix_reuse(s->lsPi, J->p1 * J->p, 1);
	gretl_matrix_free(S11i);
    }

    s->finalize = 0;

    return err;
}

/* The following functions represent an attempt at implementing the
   switching algorithm as set out in Boswijk and Doornik, 2004,
   p. 455.  This does not appear to be quite right yet, but I think
   it's not very far from being right.
*/

/* 
   Update \Psi using:

   [ G'(\Omega^{-1} \otimes \beta'S_{11}\beta)G ]^{-1} 
   \times G'(\Omega^{-1} \otimes \beta'S_{11}) vec(Pi'_{LS})
*/

static int Psifun (Jwrap *J, switcher *s)
{
    int r = J->rank;
    int i, j, k;
    int err = 0;

    gretl_matrix_reuse(s->K1, J->p * r, J->p * r);
    gretl_matrix_reuse(s->K2, J->p * r, J->p * J->p1);
    gretl_matrix_reuse(s->TmpR, J->alen, 1);

    /* left-hand chunk */
    gretl_matrix_qform(J->beta, GRETL_MOD_TRANSPOSE, J->S11,
		       J->qf1, GRETL_MOD_NONE);
    gretl_matrix_kronecker_product(s->iOmega, J->qf1, s->K1);
    if (J->G != NULL) {
	gretl_matrix_qform(J->G, GRETL_MOD_TRANSPOSE, s->K1,
			   s->I00, GRETL_MOD_NONE);
    } else {
	gretl_matrix_copy_values(s->I00, s->K1);
    }
    if (s->finalize) {
	return err;
    }
    err = gretl_invert_symmetric_matrix(s->I00);
    if (err) {
	return err;
    }

    /* right-hand chunk */
    gretl_matrix_multiply_mod(J->beta, GRETL_MOD_TRANSPOSE, 
			      J->S11, GRETL_MOD_NONE,
			      s->Tmprp1, GRETL_MOD_NONE);
    gretl_matrix_kronecker_product(s->iOmega, s->Tmprp1, s->K2);
    if (J->G != NULL) {
	gretl_matrix_multiply_mod(J->G, GRETL_MOD_TRANSPOSE,
				  s->K2, GRETL_MOD_NONE,
				  s->TmpR1, GRETL_MOD_NONE);
	gretl_matrix_multiply(s->TmpR1, s->lsPi, s->TmpR);
    } else {
	gretl_matrix_multiply(s->K2, s->lsPi, s->TmpR);
    }

    /* combine */
    gretl_matrix_multiply(s->I00, s->TmpR, J->psivec);

    /* update alpha from psivec */
    k = 0;
    if (J->G != NULL) {
	gretl_matrix_reuse(s->Tmprp, J->p * r, 1);
	gretl_matrix_multiply(J->G, J->psivec, s->Tmprp);
	for (i=0; i<J->p; i++) {
	    for (j=0; j<r; j++) {
		gretl_matrix_set(J->alpha, i, j, s->Tmprp->val[k++]);
	    }
	}
	gretl_matrix_reuse(s->Tmprp, r, J->p);
    } else {
	for (i=0; i<J->p; i++) {
	    for (j=0; j<r; j++) {
		gretl_matrix_set(J->alpha, i, j, J->psivec->val[k++]);
	    }
	}
    }	

    return err;
}

/* update beta based on \Phi vector */

static void beta_from_phivec (Jwrap *J)
{
    int r = J->rank;

    if (J->H != NULL) {
	gretl_matrix_reuse(J->beta, J->p1 * r, 1);
	gretl_matrix_multiply(J->H, J->phivec, J->beta);
	if (!gretl_is_zero_matrix(J->s)) {
	    gretl_matrix_add_to(J->beta, J->s);
	}
	gretl_matrix_reuse(J->beta, J->p1, r);
    } else {
	gretl_matrix_copy_values_shaped(J->beta, J->phivec);
    }
}

/*
    Update \Phi using:

    [H'(\alpha'\Omega^{-1}\alpha \otimes S_{11})H]^{-1} \times
       H'(\alpha'\Omega^{-1} \otimes S_{11}) \times
         [vec(Pi'_{LS}) - (\alpha \otimes I_{p1})h_0]
 */

static int Phifun (Jwrap *J, switcher *s)
{
    int r = J->rank;
    int err = 0;

    gretl_matrix_reuse(s->K1, r * J->p1, r * J->p1);
    gretl_matrix_reuse(s->K2, r * J->p1, J->p * J->p1);
    gretl_matrix_reuse(s->TmpR, J->p * J->p1, 1);

    /* first big inverse */
    gretl_matrix_qform(J->alpha, GRETL_MOD_TRANSPOSE, s->iOmega,
		       J->qf1, GRETL_MOD_NONE);
    gretl_matrix_kronecker_product(J->qf1, J->S11, s->K1);
    if (J->H != NULL) {
	gretl_matrix_qform(J->H, GRETL_MOD_TRANSPOSE, s->K1,
			   s->I11, GRETL_MOD_NONE);
    } else {
	gretl_matrix_copy_values(s->I11, s->K1);
    }
    if (s->finalize) {
	return err;
    }
    err = gretl_invert_symmetric_matrix(s->I11);
    if (err) {
	return err;
    }

    /* second chunk */
    gretl_matrix_multiply_mod(J->alpha, GRETL_MOD_TRANSPOSE,
			      s->iOmega, GRETL_MOD_NONE,
			      s->Tmprp, GRETL_MOD_NONE);
    gretl_matrix_kronecker_product(s->Tmprp, J->S11, s->K2);
    if (J->H != NULL) {
	gretl_matrix_multiply_mod(J->H, GRETL_MOD_TRANSPOSE,
				  s->K2, GRETL_MOD_NONE,
				  s->HK2, GRETL_MOD_NONE);
    } else {
	gretl_matrix_copy_values(s->HK2, s->K2);
    }

    /* combine first and second chunks */
    gretl_matrix_multiply(s->I11, s->HK2, s->TmpL);

    /* right-hand chunk */
    gretl_matrix_copy_values(s->TmpR, s->lsPi);
    if (J->s != NULL && !gretl_is_zero_matrix(J->s)) {
	gretl_matrix_reuse(s->K2, J->p * J->p1, r * J->p1);
	gretl_matrix_kronecker_I(J->alpha, J->p1, s->K2);
	gretl_matrix_multiply_mod(s->K2, GRETL_MOD_NONE,
				  J->s, GRETL_MOD_NONE,
				  s->TmpR, GRETL_MOD_DECUMULATE);
    }

    /* combine */
    gretl_matrix_multiply(s->TmpL, s->TmpR, J->phivec);

    /* update beta */
    beta_from_phivec(J);

    return err;
}

/* 
    Update Omega using:

    S_{00} - S_{01} \beta\alpha' - \alpha\beta' S_{10} +
       \alpha\beta' S_{11} \beta\alpha'

    then invert into iOmega.
 */

static int Omegafun (Jwrap *J, switcher *s)
{
    int err = 0;

    gretl_matrix_copy_values(J->Omega, J->S00);

    gretl_matrix_multiply_mod(J->alpha, GRETL_MOD_NONE,
			      J->beta, GRETL_MOD_TRANSPOSE,
			      s->Pi, GRETL_MOD_NONE);

    gretl_matrix_multiply_mod(J->S01, GRETL_MOD_NONE,
			      s->Pi, GRETL_MOD_TRANSPOSE,
			      s->Tmppp, GRETL_MOD_NONE);

    gretl_matrix_add_self_transpose(s->Tmppp);
    gretl_matrix_subtract_from(J->Omega, s->Tmppp);

    gretl_matrix_qform(s->Pi, GRETL_MOD_NONE, J->S11,
		       J->Omega, GRETL_MOD_CUMULATE);

    gretl_matrix_copy_values(s->iOmega, J->Omega);
    err = gretl_invert_symmetric_matrix(s->iOmega);

    return err;
}

/* reduced version of log-likelihood calculation for
   switching algorithm loop */

static int switcher_ll (Jwrap *J, switcher *s)
{
    int err = 0;

    gretl_matrix_copy_values(s->Tmppp, J->Omega);
    J->ll = gretl_matrix_log_determinant(s->Tmppp, &err);
    if (!err) {
	J->ll *= -J->T / 2.0;
    }
    
    return err;
}

/* Use the information matrix to compute standard errors for
   beta (we could do alpha as well).  Should we be using the
   whole matrix or just the diagonal block(s)??
*/

#define PHI_BLOCK_ONLY 1

#if PHI_BLOCK_ONLY

static int info_matrix (Jwrap *J, switcher *s)
{
    int r = J->rank;
    int err = 0;

    gretl_matrix_divide_by_scalar(s->I11, J->T);

    if (J->H != NULL) {
	int nb = r * J->p1;

	J->V = gretl_matrix_alloc(nb, nb);
	if (J->V == NULL) {
	    err = E_ALLOC;
	} else {
	    gretl_matrix_qform(J->H, GRETL_MOD_NONE, s->I11,
			       J->V, GRETL_MOD_NONE);
	}
    } else {
	J->V = s->I11;
	s->I11 = NULL;
    }

    if (!err) {
	err = make_beta_se(J);
    }

    return err;
}

#else

/* build and invert the entire information matrix */

static int info_matrix (Jwrap *J, switcher *s)
{
    gretl_matrix *M = NULL;
    gretl_matrix *I01 = NULL;
    gretl_matrix *GK1 = NULL;
    int npar = J->alen + J->blen;
    int r = J->rank;
    int err = 0;

    s->finalize = 1;

    /* re-create block 0,0 */
    Psifun(J, s);

    /* re-create block 1,1 */
    Phifun(J, s);

    M = gretl_zero_matrix_new(npar, npar);
    gretl_matrix_inscribe_matrix(M, s->I00, 0, 0, GRETL_MOD_NONE);
    gretl_matrix_inscribe_matrix(M, s->I11, J->alen, J->alen,
				 GRETL_MOD_NONE);

    /* off-diagonal block 0,1: 
         G'(\Omega^{-1}\alpha \otimes \beta'S_{11})H 
    */

    gretl_matrix_reuse(s->Tmprp, J->p, r);
    gretl_matrix_reuse(s->K1, J->p * r, r * J->p1);

    if (J->G != NULL) {
	GK1 = gretl_matrix_alloc(J->alen, r * J->p1);
    } else {
	GK1 = s->K1;
    }

    I01 = gretl_matrix_alloc(J->alen, J->blen);

    gretl_matrix_multiply(s->iOmega, J->alpha, s->Tmprp);
    gretl_matrix_multiply_mod(J->beta, GRETL_MOD_TRANSPOSE,
			      J->S11, GRETL_MOD_NONE,
			      s->Tmprp1, GRETL_MOD_NONE);
    gretl_matrix_kronecker_product(s->Tmprp, s->Tmprp1, s->K1);

    if (J->G != NULL) {
	gretl_matrix_multiply_mod(J->G, GRETL_MOD_TRANSPOSE,
				  s->K1, GRETL_MOD_NONE,
				  GK1, GRETL_MOD_NONE);
    } 
    if (J->H != NULL) {
	gretl_matrix_multiply(GK1, J->H, I01);
    } else {
	gretl_matrix_copy_values(I01, GK1);
    }

    gretl_matrix_inscribe_matrix(M, I01, 0, J->alen, GRETL_MOD_NONE);
    gretl_matrix_inscribe_matrix(M, I01, J->alen, 0, GRETL_MOD_TRANSPOSE);

    gretl_matrix_multiply_by_scalar(M, J->T);
#if JDEBUG
    gretl_matrix_print(M, "observed information matrix");
#endif
    err = gretl_invert_symmetric_indef_matrix(M);

    if (!err) {
	gretl_matrix_extract_matrix(s->I11, M, J->alen, J->alen, 
				    GRETL_MOD_NONE);
	
	if (J->H != NULL) {
	    int nb = r * J->p1;

	    J->V = gretl_matrix_alloc(nb, nb);
	    gretl_matrix_qform(J->H, GRETL_MOD_NONE, s->I11,
			       J->V, GRETL_MOD_NONE);
	} else {
	    J->V = s->I11;
	    s->I11 = NULL;
	}

	make_beta_se(J);
    }

 bailout:

    if (J->G != NULL) {
	gretl_matrix_free(GK1);
    }
    gretl_matrix_free(I01);
    gretl_matrix_free(M);

    return err;
}

#endif /* info matrix variants */

static int switchit (Jwrap *J, PRN *prn)
{
    switcher s;
    double lldiff = NADBL;
    double llbak = -1.0e+200;
    double tol = 4.0e-11;
    int j, jmax = 100000;
    int conv = 0;
    int err;

    err = switcher_init(&s, J);

#if 0
    if (!err && J->b != NULL) {
	/* initialize beta: not sure about this */
	gretl_matrix_copy_values(J->phivec, J->b);
	beta_from_phivec(J);
    }
#endif

    gretl_matrix_print(J->phivec, "switchit: initial Phi");
    gretl_matrix_print(J->beta, "switchit: initial beta");

    if (!err) {
	/* initialize alpha */
	err = compute_alpha(J);
    }

    gretl_matrix_print(J->alpha, "switchit: initial alpha");

    if (!err) {
	/* initialize Omega */
	err = Omegafun(J, &s);
    }

    for (j=0; j<jmax && !err; j++) {
#if SDEBUG
	fprintf(stderr, "switcher: j = %d\n", j);
#endif
	err = Phifun(J, &s);
	if (!err) {
	    err = Psifun(J, &s);
	}
	if (!err) {
	    err = Omegafun(J, &s);
	}
	if (!err) {
	    err = switcher_ll(J, &s);
#if SDEBUG
	    fprintf(stderr, " -(T/2)log|Omega| = %.8g\n", J->ll);
#endif
	}
	if (!err && j > 1) {
	    lldiff = (J->ll - llbak) / fabs(llbak);
	    if (lldiff < tol) {
		conv = 1;
		break;
	    }
	}
	llbak = J->ll;
    }

    pprintf(prn, "Switching algorithm: %d iterations\n", j);
    pprintf(prn, " -(T/2)log|Omega| = %.8g, lldiff = %g\n", J->ll, lldiff);

    if (!err && !conv) {
	err = E_NOCONV;
    }

    if (!err) {
	J->ll -= J->T * 0.5 * (J->p * (1.0 + LN_2_PI));
	if (J->jr >= J->alen + J->blen) {
	    /* model is identified */
	    info_matrix(J, &s);
	}
    }

    switcher_free(&s);

    return err;
}

/* 
   J = [(I_p \otimes \beta)G : (\alpha \otimes I_{p1})H]

   Boswijk and Doornik (2004), equation (40), page 455.
*/

static int check_jacobian (Jwrap *J)
{
    gretl_matrix *A = NULL;
    gretl_matrix *B = NULL;
    gretl_matrix *Jac = NULL;
    int err = 0;

    /* form both beta = H \phi + s, and alpha, for randomized 
       \theta
    */

    /* FIXME phi/psi vs J->phivec, J->psivec */

    if (J->H != NULL) {
	gretl_matrix *phi = gretl_column_vector_alloc(J->H->cols);

	gretl_matrix_random_fill(phi, D_NORMAL);
	gretl_matrix_reuse(J->beta, J->p1 * J->rank, 1);
	gretl_matrix_multiply(J->H, phi, J->beta);
	gretl_matrix_add_to(J->beta, J->s);
	gretl_matrix_reuse(J->beta, J->p1, J->rank);
	gretl_matrix_free(phi);
    } else {
	gretl_matrix_random_fill(J->beta, D_NORMAL);
    }

    if (J->G != NULL) {
	gretl_matrix *psi = gretl_column_vector_alloc(J->G->cols);

	gretl_matrix_random_fill(psi, D_NORMAL);
	gretl_matrix_reuse(J->alpha, J->p * J->rank, 1);
	gretl_matrix_multiply(J->G, psi, J->alpha);
	gretl_matrix_reuse(J->alpha, J->p, J->rank);
	gretl_matrix_free(psi);
    } else {	
	compute_alpha(J);
    }

    if (!err) {
	A = gretl_matrix_I_kronecker_new(J->p, J->beta, &err);
    }

    if (!err) {
	B = gretl_matrix_kronecker_I_new(J->alpha, J->p1, &err);
    }

#if JDEBUG > 1
    gretl_matrix_print(A, "I_p \\otimes \\beta");
    gretl_matrix_print(B, "\\alpha \\otimes I_{p1}");
#endif

    if (!err && J->G != NULL) {
	/* alpha is restricted */
	gretl_matrix *AG = gretl_matrix_multiply_new(A, J->G, &err);

	if (!err) {
	    gretl_matrix_free(A);
	    A = AG;
	}
#if JDEBUG > 1
	gretl_matrix_print(A, "A*G");
#endif
    }

    if (!err && J->H != NULL) {
	/* beta is restricted */
	gretl_matrix *BH = gretl_matrix_multiply_new(B, J->H, &err);

	if (!err) {
	    gretl_matrix_free(B);
	    B = BH;
	}
#if JDEBUG > 1
	gretl_matrix_print(B, "B*H");
#endif
    }

    if (!err) {
	Jac = gretl_matrix_col_concat(A, B, &err);
    }

    if (!err) {
	J->jr = gretl_matrix_rank(Jac, &err);
#if JDEBUG
	gretl_matrix_print(Jac, "Jacobian");
#endif
    }

    gretl_matrix_free(A);
    gretl_matrix_free(B);
    gretl_matrix_free(Jac);

    return err;
}

/* Doornik's approach to checking identification */

static int 
vecm_id_check (Jwrap *J, GRETL_VAR *jvar, PRN *prn)
{
    int npar = J->blen + J->alen;
    int err;

    err = check_jacobian(J);

    if (!err) {
	pprintf(prn, "Rank of Jacobian = %d, number of free "
		"parameters = %d\n", J->jr, npar);
	if (J->jr < npar) {
	    pputs(prn, "Model is not fully identified\n");
	} else {
	    pputs(prn, "Model is fully identified\n");
	}

	J->df = (J->p + J->p1 - J->rank) * J->rank - J->jr;
	pprintf(prn, "Based on Jacobian, df = %d\n", J->df);

	/* system was subject to a prior restriction? */
	if (jvar->jinfo->bdf > 0) {
	    J->df -= jvar->jinfo->bdf;
	    pprintf(prn, "Allowing for prior restriction, df = %d\n", 
		    J->df);
	}
    }

    return err;
}

/* set up restrictions for alpha */

static int set_up_G (Jwrap *J, GRETL_VAR *jvar,
		     const gretl_restriction_set *rset,
		     PRN *prn)
{
    const gretl_matrix *Ra = rset_get_Ra_matrix(rset);
    const gretl_matrix *qa = rset_get_qa_matrix(rset);
    gretl_matrix *R = NULL;
    int err = 0;

#if JDEBUG
    gretl_matrix_print(Ra, "Ra, in set_up_G");
    gretl_matrix_print(qa, "qa, in set_up_G");
#endif

    if (!gretl_is_zero_matrix(qa)) {
	pprintf(prn, "alpha restriction is not homogeneous: not supported\n");
	return E_NOTIMP;
    }

    R = gretl_matrix_copy(Ra);
    if (R == NULL) {
	return E_ALLOC;
    }

    /* remap R, for conformity with vec(\alpha') = G\Psi */
    if (J->rank > 1 && R->cols > J->p) {
	double x;
	int c0, c1 = 0;
	int i, j, k;

	for (i=0; i<J->p; i++) {
	    for (j=0; j<J->rank; j++) {
		/* write column of Ra into remapped column of R */
		c0 = i + j * J->p;
		fprintf(stderr, "col %d -> col %d\n", c0, c1);
		for (k=0; k<R->rows; k++) {
		    x = gretl_matrix_get(Ra, k, c0);
		    gretl_matrix_set(R, k, c1, x);
		}
		c1++;
	    }
	}
#if JDEBUG
	gretl_matrix_print(R, "Re-mapped R, in set_up_G");
#endif
    }

    if (J->rank > 1 && R->cols == J->p) {
	/* common alpha restriction */
	gretl_matrix *Rtmp = gretl_matrix_I_kronecker_new(J->rank, R, &err);

	if (!err) {
	    J->G = gretl_matrix_right_nullspace(Rtmp, &err);
	    gretl_matrix_free(Rtmp);
	}
    } else {
	J->G = gretl_matrix_right_nullspace(R, &err);
    }

    if (!err) {
	J->alen = J->G->cols;
    }

#if JDEBUG
    gretl_matrix_print(J->G, "G, in set_up_G");
#endif

    gretl_matrix_free(R);

    return err;
}

/* for use with switching algorithm */

static int set_up_H_h0 (Jwrap *J, GRETL_VAR *jvar,
			const gretl_restriction_set *rset,
			PRN *prn)
{
    const gretl_matrix *R = rset_get_R_matrix(rset);
    const gretl_matrix *q = rset_get_q_matrix(rset);
    gretl_matrix *RRT = NULL;
    gretl_matrix *Tmp = NULL;
    int err = 0;

#if JDEBUG
    gretl_matrix_print(R, "R, in set_up_H_h0");
    gretl_matrix_print(q, "q, in set_up_H_h0");
#endif

    if (R->rows == J->p1 * J->rank) {
	/* number of restrictions = total betas */
	return solve_for_beta(J, R, q);
    }

    if (J->rank > 1 && R->cols == J->p1) {
	/* common beta restriction */
	gretl_matrix *Rtmp = gretl_matrix_I_kronecker_new(J->rank, R, &err);

	if (!err) {
	    J->H = gretl_matrix_right_nullspace(Rtmp, &err);
	    gretl_matrix_free(Rtmp);
	}
    } else {
	J->H = gretl_matrix_right_nullspace(R, &err);
    }

    if (!err) {
	J->blen = J->H->cols;
    }

    RRT = gretl_matrix_alloc(R->rows, R->rows);
    Tmp = gretl_matrix_alloc(R->cols, R->rows);
    if (RRT == NULL || Tmp == NULL) {
	err = E_ALLOC;
    }

    /* now for h_0, or "s" */

    if (!err) {
	err = gretl_matrix_multiply_mod(R, GRETL_MOD_NONE,
					R, GRETL_MOD_TRANSPOSE,
					RRT, GRETL_MOD_NONE);
    }
    
    if (!err) {
	err = gretl_invert_symmetric_matrix(RRT);
    }

    if (!err) {
	err = gretl_matrix_multiply_mod(R, GRETL_MOD_TRANSPOSE,
					RRT, GRETL_MOD_NONE,
					Tmp, GRETL_MOD_NONE);
    }

    if (!err) {
	J->s = gretl_matrix_multiply_new(Tmp, q, &err);
    }

#if JDEBUG
    gretl_matrix_print(J->H, "H, in set_up_H_h0");
    gretl_matrix_print(J->s, "h_0, in set_up_H_h0");
#endif

    gretl_matrix_free(RRT);
    gretl_matrix_free(Tmp);

    return err;
}

static int 
normalize_initial_beta (Jwrap *J, const gretl_restriction_set *rset, 
			gretl_matrix *b)
{
    const gretl_matrix *R = rset_get_R_matrix(rset);
    const gretl_matrix *d = rset_get_q_matrix(rset);

    gretl_matrix *tmp = NULL;
    gretl_matrix *tmp_sq = NULL;
    gretl_matrix *tmp_b = NULL;
    gretl_matrix *X = NULL;

    double x;
    int i, j, ii;
    int br = b->rows;
    int bc = J->rank;
    int bc2 = bc * bc;
    int err = 0;

    if (bc2 > d->rows) {
	fprintf(stderr, "*** normalize_initial_beta: df = %d\n", d->rows - bc2);
	return 0;
    }

    tmp_b = gretl_matrix_alloc(br, bc);
    tmp_sq = gretl_matrix_alloc(bc, bc);
    X = gretl_matrix_alloc(R->rows, bc2);
    if (tmp_b == NULL || tmp_sq == NULL || X == NULL) {
	err = E_ALLOC;
	goto bailout;
    }

    tmp = gretl_matrix_I_kronecker_new(bc, b, &err);
    if (err) {
	goto bailout;
    }

    gretl_matrix_multiply(R, tmp, X);
    gretl_matrix_reuse(tmp, bc2, 1);

    err = gretl_matrix_multi_ols(d, X, tmp, NULL);
    if (err) {
	fprintf(stderr, "beta initialization: gretl_matrix_multi_ols failed\n");
	err = 0;
	goto bailout;
    }

    ii = 0;
    for (i=0; i<bc; i++) {
	for (j=0; j<bc; j++) {
	    x = gretl_matrix_get(tmp, ii++, 0);
	    gretl_matrix_set(tmp_sq, j, i, x);
	}
    }
	    
    gretl_matrix_copy_values(tmp_b, b);
    gretl_matrix_multiply(tmp_b, tmp_sq, b);

 bailout:

    gretl_matrix_free(tmp);
    gretl_matrix_free(tmp_sq);
    gretl_matrix_free(tmp_b);
    gretl_matrix_free(X);

    return err;
}

/* solution to unrestricted eigenvalue problem */

static int case0 (Jwrap *J)
{
    gretl_matrix *M = NULL;
    gretl_matrix *Tmp = NULL;
    gretl_matrix *evals = NULL;
    int n = J->S11->cols;
    int err = 0;

    Tmp = gretl_matrix_alloc(n, n);
    M = gretl_matrix_alloc(n, n);

    if (Tmp == NULL || M == NULL) {
	err = E_ALLOC;
    }

    if (!err) {
	err = gretl_matrix_qform(J->S01, GRETL_MOD_TRANSPOSE, 
				 J->S00i, Tmp, GRETL_MOD_NONE);
    }

    if (!err) {
	evals = gretl_gensymm_eigenvals(Tmp, J->S11, M, &err);
    }

    if (!err) {
	err = gretl_symmetric_eigen_sort(evals, M, J->rank);
    }

#if JDEBUG
    gretl_matrix_print(evals, "case0: evals");
    gretl_matrix_print(M, "case0: M");
    fprintf(stderr, "(err = %d)\n", err);
#endif

    if (!err) {
	gretl_matrix_copy_values(J->beta, M);
    }

    gretl_matrix_free(Tmp);
    gretl_matrix_free(M);
    gretl_matrix_free(evals);

    return err;
}

static int initval (Jwrap *J, const gretl_restriction_set *rset)
{
    gretl_matrix *HHi = NULL;
    gretl_matrix *tmp = NULL;
    int err;

    err = case0(J);
    if (err) {
	return err;
    }

    if (rset_get_R_matrix(rset) == NULL || J->rank == 1) {
	return 0;
    }

    err = normalize_initial_beta(J, rset, J->beta);
    if (err) {
	return err;
    }

#if JDEBUG
    gretl_matrix_print(J->beta, "initval: 'normalized' beta");
#endif

    HHi = gretl_matrix_alloc(J->blen, J->blen);
    J->theta = gretl_column_vector_alloc(J->H->rows);
    tmp = gretl_column_vector_alloc(J->blen);

    if (HHi == NULL || J->theta == NULL || tmp == NULL) {
	err = E_ALLOC;
    }

    if (!err) {
	err = gretl_matrix_multiply_mod(J->H, GRETL_MOD_TRANSPOSE,
					J->H, GRETL_MOD_NONE,
					HHi, GRETL_MOD_NONE);
    }
    
    if (!err) {
	err = gretl_invert_symmetric_matrix(HHi);
    }

    if (!err) {
	err = gretl_matrix_vectorize(J->theta, J->beta);
    }

    if (!err) {
	err = gretl_matrix_subtract_from(J->theta, J->s);
    }

    if (!err) {
	err = gretl_matrix_multiply_mod(J->H, GRETL_MOD_TRANSPOSE,
					J->theta, GRETL_MOD_NONE,
					tmp, GRETL_MOD_NONE);
    }

    if (!err) {
	gretl_matrix_reuse(J->theta, tmp->rows, 1);
	err = gretl_matrix_multiply(HHi, tmp, J->theta);
    }

#if JDEBUG
    gretl_matrix_print(J->theta, "initval: final vecb");
#endif

    gretl_matrix_free(HHi);
    gretl_matrix_free(tmp);
    
    return err;
}

static int make_omega (Jwrap *J)
{
    gretl_matrix *tmp = NULL;
    int err = 0;

    tmp = gretl_matrix_alloc(J->rank, J->rank);
    if (tmp == NULL) {
	return E_ALLOC;
    }

    gretl_matrix_copy_values(J->Omega, J->S00);

    err = gretl_matrix_qform(J->beta, GRETL_MOD_TRANSPOSE,
			     J->S11, tmp, GRETL_MOD_NONE);

    if (!err) {
	gretl_matrix_qform(J->alpha, GRETL_MOD_NONE,
			   tmp, J->Omega, GRETL_MOD_DECUMULATE);
    }

    if (!err) {
	gretl_matrix_divide_by_scalar(J->Omega, J->T);
    }

    gretl_matrix_free(tmp);

    return err;
}

static int make_zero_variance (Jwrap *J)
{
    int npar = J->p1 * J->rank;

    J->V = gretl_zero_matrix_new(npar, npar);
    J->se = gretl_zero_matrix_new(J->p1, J->rank);

    if (J->V == NULL || J->se == NULL) {
	return E_ALLOC;
    }

    return 0;
}

static int make_beta_se (Jwrap *J)
{
    double x;
    int i;

    J->se = gretl_matrix_alloc(J->p1, J->rank);
    if (J->se == NULL) {
	return E_ALLOC;
    }

    for (i=0; i<J->V->rows; i++) {
	x = gretl_matrix_get(J->V, i, i);
	J->se->val[i] = sqrt(x);
    }

    return 0;
}

static int make_beta_variance (Jwrap *J)
{
    gretl_matrix *K = NULL;
    gretl_matrix *Vphi = NULL;
    int r = J->rank;
    int err;

    err = gretl_invert_symmetric_matrix(J->Omega);
    if (err) {
	return err;
    }

    K = gretl_matrix_alloc(r * J->p1, r * J->p1);
    Vphi = gretl_matrix_alloc(J->blen, J->blen);
    if (K == NULL || Vphi == NULL) {
	err = E_ALLOC;
	goto bailout;
    }

    gretl_matrix_qform(J->alpha, GRETL_MOD_TRANSPOSE, J->Omega,
		       J->qf1, GRETL_MOD_NONE);
    gretl_matrix_kronecker_product(J->qf1, J->S11, K);
    if (J->H != NULL) {
	gretl_matrix_qform(J->H, GRETL_MOD_TRANSPOSE, K,
			   Vphi, GRETL_MOD_NONE);
    } else {
	gretl_matrix_copy_values(Vphi, K);
    }

    err = gretl_invert_symmetric_matrix(Vphi);
    if (err) {
	goto bailout;
    }

    if (J->H != NULL) {
	int nb = r * J->p1;

	J->V = gretl_matrix_alloc(nb, nb);
	if (J->V == NULL) {
	    err = E_ALLOC;
	} else {
	    gretl_matrix_qform(J->H, GRETL_MOD_NONE, Vphi,
			       J->V, GRETL_MOD_NONE);
	}
    } else {
	J->V = Vphi;
	Vphi = NULL;
    }

    if (!err) {
	err = make_beta_se(J);
    }

 bailout:

    gretl_matrix_free(K);
    gretl_matrix_free(Vphi);

    return err;
}

static int make_beta (Jwrap *J, const double *phi)
{
    int i, err = 0;

    gretl_matrix_reuse(J->beta, J->p1 * J->rank, 1);

    for (i=0; i<J->blen; i++) {
	J->phivec->val[i] = phi[i];
    }
    err = gretl_matrix_multiply(J->H, J->phivec, J->beta);

    if (!err) {
	gretl_matrix_add_to(J->beta, J->s);
    }

    gretl_matrix_reuse(J->beta, J->p1, J->rank);

#if JDEBUG > 1
    gretl_matrix_print(J->phivec, "phi");
    gretl_matrix_print(J->beta, "beta");
#endif

    return err;
}

static int real_compute_ll (Jwrap *J)
{
    double ll0 = J->ldS00;
    int err = 0;

    err = gretl_matrix_qform(J->beta, GRETL_MOD_TRANSPOSE,
			     J->S11m, J->qf1, GRETL_MOD_NONE);

    if (!err) {
	err = gretl_matrix_qform(J->beta, GRETL_MOD_TRANSPOSE,
				 J->S11, J->qf2, GRETL_MOD_NONE);
    }

    if (!err) {
	ll0 += gretl_matrix_log_determinant(J->qf1, &err);
    }

    if (!err) {
	ll0 -= gretl_matrix_log_determinant(J->qf2, &err);
    }

    if (err) {
	J->ll = NADBL;
    } else {
	J->ll = -J->T * 0.5 * (J->p * (1.0 + LN_2_PI) + ll0);
    }

    return err;
}

/* BFGS callback function */

static double Jloglik (const double *phi, void *data)
{
    Jwrap *J = (Jwrap *) data;
    int err;

    err = make_beta(J, phi);

    if (!err) {
	err = real_compute_ll(J);
    }

    return J->ll;
}

#define VECM_WIDTH 13

static int printres (Jwrap *J, GRETL_VAR *jvar, const DATAINFO *pdinfo,
		     PRN *prn)
{
    JohansenInfo *jv = jvar->jinfo;
    const gretl_matrix *b = J->beta;
    const gretl_matrix *a = J->alpha;
    const gretl_matrix *sd = J->se;
    char vname[32], s[16];
    int n = b->rows;
    int r = b->cols;
    int i, j;

    pprintf(prn, _("Unrestricted loglikelihood (lu) = %g\n"), jvar->ll);
    pprintf(prn, _("Restricted loglikelihood (lr) = %g\n"), J->ll);
    if (J->df > 0) {
	double x = 2.0 * (jvar->ll - J->ll);

	pprintf(prn, "2 * (lu - lr) = %g\n", x);
	pprintf(prn, _("P(Chi-Square(%d) > %g = %g\n"), J->df, x, 
		chisq_cdf_comp(x, J->df));
    }

    pputs(prn, "\n\n");
    pputs(prn, _("beta (cointegrating vectors)"));
    if (sd != NULL) {
	pprintf(prn, " (%s)", _("standard errors in parentheses"));
    }
    pputs(prn, "\n\n");

    for (i=0; i<n; i++) {
	if (i < jv->list[0]) {
	    sprintf(vname, "%s(-1)", pdinfo->varname[jv->list[i+1]]);
	} else if (jv->code == J_REST_CONST) {
	    strcpy(vname, "const");
	} else if (jv->code == J_REST_TREND) {
	    strcpy(vname, "trend");
	}
	pprintf(prn, "%-12s", vname); /* FIXME */

	for (j=0; j<r; j++) {
	    pprintf(prn, "%#12.5g ", gretl_matrix_get(b, i, j));
	}
	pputc(prn, '\n');

	if (sd != NULL) {
	    bufspace(VECM_WIDTH, prn);
	    for (j=0; j<r; j++) {
		sprintf(s, "(%#.5g)", gretl_matrix_get(sd, i, j));
		pprintf(prn, "%12s ", s);
	    }
	    pputc(prn, '\n');
	}
    }

    pputc(prn, '\n');
    pputs(prn, _("alpha (adjustment vectors)"));
    pputs(prn, "\n\n");

    for (i=0; i<J->p; i++) {
	sprintf(vname, "%s", pdinfo->varname[jv->list[i+1]]);
	pprintf(prn, "%-12s", vname);
	for (j=0; j<r; j++) {
	    pprintf(prn, "%#12.5g ", gretl_matrix_get(a, i, j));
	}
	pputc(prn, '\n');
    }

    pputc(prn, '\n');

    return 0;
}

static int simann (Jwrap *J, gretlopt opt, PRN *prn)
{
    gretl_matrix *b = J->theta;
    int i, SAiter = 4096;
    double f0, f1;
    double fbest, fworst;
    double rndu;
    int jump;

    gretl_matrix *b0 = NULL;
    gretl_matrix *b1 = NULL;
    gretl_matrix *d = NULL;

    double Temp = 1.0;
    double radius = 1.0;
    int hdr = 0, err = 0;

    b0 = gretl_matrix_copy(b);
    b1 = gretl_matrix_copy(b);
    d = gretl_column_vector_alloc(b->rows);

    if (b0 == NULL || b1 == NULL || d == NULL) {
	err = E_ALLOC;
	goto bailout;
    }

    f0 = fbest = fworst = Jloglik(b->val, J);

    for (i=0; i<SAiter; i++) {
	gretl_matrix_random_fill(d, D_NORMAL);
	gretl_matrix_multiply_by_scalar(d, radius);
	gretl_matrix_add_to(b1, d);
	f1 = Jloglik(b1->val, J);

	if (f1 > f0) {
	    jump = 1;
	} else {
	    rndu = ((double) rand()) / RAND_MAX;
	    jump = (Temp < rndu);
	}

	if (jump) {
	    f0 = f1;
	    gretl_matrix_copy_values(b0, b1);
	    if (f0 > fbest) {
		fbest = f0;
		gretl_matrix_copy_values(b, b0);
		if (opt & OPT_V) {
		    if (!hdr) {
			pputs(prn, "\nSimulated annealing:\n");
			pprintf(prn, "%6s %12s %12s %12s\n",
				"iter", "temp", "radius", "fbest");
			hdr = 1;
		    }
		    pprintf(prn, "%6d %#12.6g %#12.6g %#12.6g\n", 
			    i, Temp, radius, fbest);
		}
	    } else if (f0 < fworst) {
		fworst = f0;
	    }
	} else {
	    gretl_matrix_copy_values(b1, b0);
	    f1 = f0;
	}

	Temp *= 0.999;
	radius *= 0.9999;
    }

    if (hdr) {
	pputc(prn, '\n');
    }
    
    if (fbest - fworst < 1.0e-9) {
	pprintf(prn, "Warning: likelihood seems to be flat\n");
    }

 bailout:

    gretl_matrix_free(b0);
    gretl_matrix_free(b1);
    gretl_matrix_free(d);

    return err;
}

static int compute_alpha (Jwrap *J)
{
    gretl_matrix *S01b = NULL;
    int err = 0;

    S01b = gretl_matrix_multiply_new(J->S01, J->beta, &err);

    if (!err) {
	err = gretl_matrix_qform(J->beta, GRETL_MOD_TRANSPOSE,
				 J->S11, J->qf1, GRETL_MOD_NONE);
    }

    if (!err) {
	gretl_invert_symmetric_matrix(J->qf1);
    }

    if (!err) {
	gretl_matrix_multiply(S01b, J->qf1, J->alpha);
    }

    gretl_matrix_free(S01b);

    return err;
}

static int allocate_phivec (Jwrap *J)
{
    if (J->H != NULL) {
	J->phivec = gretl_column_vector_alloc(J->H->cols);
    } else {
	J->phivec = gretl_column_vector_alloc(J->p1 * J->rank);
    }

    return (J->phivec == NULL)? E_ALLOC : 0;
}

static int allocate_psivec (Jwrap *J)
{
    if (J->G != NULL) {
	J->psivec = gretl_column_vector_alloc(J->G->cols);
    } else {
	J->psivec = gretl_column_vector_alloc(J->p * J->rank);
    }

    return (J->psivec == NULL)? E_ALLOC : 0;
}

/* write info from the temporary Jwrap structure into
   the "permanent" GRETL_VAR structure */

static void transcribe_to_jvar (Jwrap *J, GRETL_VAR *jvar)
{
    jvar->jinfo->ll0 = jvar->ll;
    jvar->ll = J->ll;
    jvar->jinfo->bdf += J->df; /* ?? */

    gretl_matrix_free(jvar->jinfo->Beta);
    jvar->jinfo->Beta = J->beta;
    J->beta = NULL;

    gretl_matrix_free(jvar->jinfo->Alpha);
    jvar->jinfo->Alpha = J->alpha;
    J->alpha = NULL;

    gretl_matrix_free(jvar->jinfo->Bvar);
    jvar->jinfo->Bvar = J->V;
    J->V = NULL;

    gretl_matrix_free(jvar->jinfo->Bse);
    jvar->jinfo->Bse = J->se;
    J->se = NULL;
}

/* public entry point (OPT_W == use switching algorithm) */

int general_vecm_analysis (GRETL_VAR *jvar, 
			   const gretl_restriction_set *rset,
			   const DATAINFO *pdinfo,
			   gretlopt opt,
			   PRN *prn)
{
    Jwrap *J = NULL;
    int err = 0;

    J = jwrap_new(jvar, &err);
    if (err) {
	return err;
    }

    if (rset_VECM_bcols(rset) > 0) {
	err = set_up_H_h0(J, jvar, rset, prn);
    }

    if (!err) {
	err = allocate_phivec(J);
    }

    if (!err && rset_VECM_acols(rset) > 0) {
	err = set_up_G(J, jvar, rset, prn);
    }

    if (!err) {
	err = allocate_psivec(J);
    }    

    if (!err && J->bnoest) {
	/* nothing to be estimated (FIXME alpha) */
	err = real_compute_ll(J);
	goto skipest;
    }

    if (!err) {
	err = vecm_id_check(J, jvar, prn);
	if (!(opt & OPT_W)) {
	    if (!err && J->jr < J->alen + J->blen) {
		err = E_NOIDENT;
	    }
	}
    }

    if (!err) {
	err = initval(J, rset);
#if JDEBUG
	fprintf(stderr, "after initval: err = %d\n", err);
#endif
    }

    if (opt & OPT_W) {
	if (!err) {
	    err = switchit(J, prn);
	}
    } else {	
	if (!err) {
	    err = simann(J, opt, prn);
	}    
	if (!err) {
	    int maxit = 4000;
	    double reltol = 1.0e-11;
	    int fncount = 0;
	    int grcount = 0;
	    int nn = J->theta->rows;

	    err = LBFGS_max(J->theta->val, nn, maxit, reltol, 
			    &fncount, &grcount, Jloglik, C_LOGLIK,
			    NULL, J, opt, prn);
	}
    }

 skipest:

    if (!err && !(opt & OPT_W)) {
	err = compute_alpha(J);
	if (!err) {
	    err = make_omega(J);
	}
    }

    if (!err) {
	if (opt & OPT_F) {
	    gretl_matrix_free(jvar->S);
	    jvar->S = gretl_matrix_copy(J->Omega);
	}
	if (J->bnoest) {
	    err = make_zero_variance(J);
	} else if (!(opt & OPT_W)) {
	    err = make_beta_variance(J);
	}
    }

    if (!err) {
	if (opt & OPT_F) {
	    transcribe_to_jvar(J, jvar);
	} else {
	    printres(J, jvar, pdinfo, prn);
	}
    } 

    jwrap_destroy(J);

    return err;
}

