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
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "libgretl.h"
#include "gretl_matrix.h"

static void 
print_system_vcv (const gretl_matrix *m, int triangle, PRN *prn)
{
    int i, j, jmax;
    int mcols = gretl_matrix_cols(m);
    int mrows = gretl_matrix_rows(m);
    double x;
    char numstr[16];

    jmax = (triangle)? 1 : mcols;

    for (i=0; i<mrows; i++) {
	for (j=0; j<jmax; j++) {
	    pprintf(prn, "%#10.5g ", gretl_matrix_get(m, i, j));
	}
	for (j=jmax; j<mcols; j++) {
	    x = gretl_matrix_get(m, i, i) * gretl_matrix_get(m, j, j);
	    x = sqrt(x);
	    x = gretl_matrix_get(m, i, j) / x;
	    sprintf(numstr,"(%.3f)", x); 
	    pprintf(prn, "%11s", numstr);
	}
	pputc(prn, '\n');
	if (triangle && jmax < mcols) jmax++;
    }
}

static void kronecker_place (gretl_matrix *X, 
			     const gretl_matrix *M,
			     int startrow, int startcol,
			     int k, double scale)
{
    int i, j;
    int row, col;
    double x;
    
    for (i=0; i<k; i++) {
	row = startrow * k + i;
	for (j=0; j<k; j++) {
	    col = startcol * k + j;
	    x = gretl_matrix_get(M, i, j);
	    gretl_matrix_set(X, row, col, x * scale);
	}
    }
}

static int make_sys_X_block (gretl_matrix *X, 
			     const MODEL *pmod,
			     const double **Z, 
			     int t1, int T, int systype)
{
    int i, t;
    const double *Xi;

    for (i=0; i<pmod->ncoeff; i++) {
	if (systype == THREESLS) {
	    Xi = tsls_get_Xi(pmod, Z, i);
	} else {
	    Xi = Z[pmod->list[i+2]];
	}
	if (Xi == NULL) return 1;
	for (t=0; t<T; t++) {
	    gretl_matrix_set(X, t, i, Xi[t+t1]);
	}
    }

    return 0;
}

static int
gls_sigma_from_uhat (gretl_matrix *sigma, const gretl_matrix *e, 
		     int m, int T)
{
    int i, j, t;
    double xx;

    /* construct sigma: s_{ij} = e'_i * e_j / T  */
    for (i=0; i<m; i++) {
	for (j=0; j<m; j++) {
	    xx = 0.0;
	    for (t=0; t<T; t++) {
		xx += gretl_matrix_get(e, i, t) * gretl_matrix_get(e, j, t);
	    }
	    gretl_matrix_set(sigma, i, j, xx / T);
	}
    }

    return 0;
}

static gretl_matrix *
gls_sigma_inverse_from_uhat (const gretl_matrix *e, int m, int T)
{
    gretl_matrix *sigma;

    sigma = gretl_matrix_alloc(m, m);
    if (sigma == NULL) return NULL;

    gls_sigma_from_uhat(sigma, e, m, T);
    gretl_invert_general_matrix(sigma);

    return sigma;
}

/* m = number of equations 
   k = number of indep vars per equation (FIXME: allow this to vary)
*/

static void sys_resids (MODEL *pmod, const double **Z, gretl_matrix *uhat,
			int systype)
{
    int i, t;
    double fit;

    pmod->ess = 0.0;
    for (t=pmod->t1; t<=pmod->t2; t++) {
	fit = 0.0;
	for (i=0; i<pmod->ncoeff; i++) {
	    fit += pmod->coeff[i] * Z[pmod->list[i+2]][t];
	}
	pmod->yhat[t] = fit;
	pmod->uhat[t] = Z[pmod->list[1]][t] - fit;
	/* for cross-equation vcv */
	gretl_matrix_set(uhat, pmod->ID, t - pmod->t1, pmod->uhat[t]);
	pmod->ess += pmod->uhat[t] * pmod->uhat[t];
    }

    pmod->sigma = sqrt(pmod->ess / pmod->dfd);
}

static int 
calculate_sys_coefficients (MODEL **models, const double **Z,
			    gretl_matrix *X, gretl_matrix *uhat,
			    double *tmp_y, int m, int k, 
			    int systype)
{
    gretl_vector *coeff;
    gretl_matrix *vcv;
    int i, j;
    int ncoeff = m * k;

    coeff = gretl_vector_alloc(ncoeff);
    if (coeff == NULL) return 1;

    for (i=0; i<ncoeff; i++) {
	gretl_vector_set(coeff, i, tmp_y[i]);
    }

    vcv = gretl_matrix_copy(X);
    if (vcv == NULL) {
	gretl_vector_free(coeff);
	return 1;
    }

    gretl_LU_solve(X, coeff);
    gretl_invert_general_matrix(vcv); 

    for (i=0; i<m; i++) {
	int nci = (models[i])->ncoeff;

	for (j=0; j<nci; j++) {
	    (models[i])->coeff[j] = gretl_vector_get(coeff, i * k + j);
	    (models[i])->sderr[j] = 
		sqrt(gretl_matrix_get(vcv, i * k + j, i * k + j));
	}
	sys_resids(models[i], Z, uhat, systype);
    }

    gretl_vector_free(coeff);
    gretl_matrix_free(vcv);

    return 0;
}

static void add_results_to_dataset (gretl_equation_system *sys, 
				    MODEL *pmod, int i, int *pj,
				    double **Z, DATAINFO *pdinfo,
				    int systype)
{
    int t;

    if (system_save_uhat(sys)) {
	for (t=0; t<pdinfo->n; t++) {
	    Z[*pj][t] = pmod->uhat[t];
	}
	sprintf(pdinfo->varname[*pj], "uhat_s%02d", i + 1);
	if (systype == SUR) {
	    sprintf(VARLABEL(pdinfo, *pj), _("SUR residual, equation %d"), 
		    i + 1);
	} else {
	    sprintf(VARLABEL(pdinfo, *pj), _("3SLS residual, equation %d"), 
		    i + 1);
	}	    
	*pj += 1;
    }
    if (system_save_yhat(sys)) {
	for (t=0; t<pdinfo->n; t++) {
	    Z[*pj][t] = pmod->yhat[t];
	}	
	sprintf(pdinfo->varname[*pj], "yhat_s%02d", i + 1);
	if (systype == SUR) {
	    sprintf(VARLABEL(pdinfo, *pj), _("SUR fitted value, equation %d"), 
		    i + 1);
	} else {
	    sprintf(VARLABEL(pdinfo, *pj), _("3SLS fitted value, equation %d"), 
		    i + 1);
	}	    
	*pj += 1;
    }
}

static void restore_sample (DATAINFO *pdinfo, int t1, int t2)
{
    pdinfo->t1 = t1;
    pdinfo->t2 = t2;
}

int sur (gretl_equation_system *sys, double ***pZ, DATAINFO *pdinfo, 
	 PRN *prn)
{
    int i, j, k, m, T, t, l;
    int t1 = pdinfo->t1, t2 = pdinfo->t2;
    int orig_t1 = t1, orig_t2 = t2;
    gretl_matrix *X = NULL, *Xi = NULL, *Xj = NULL, *M = NULL;
    gretl_matrix *uhat = NULL, *sigma = NULL;
    double *tmp_y = NULL;
    int v, bigrows;
    int systype = system_get_type(sys);
    MODEL **models = NULL;
    int err = 0;

    if (system_adjust_t1t2(sys, &t1, &t2, (const double **) *pZ)) {
	return E_DATA;
    }   

    /* reset sample temporarily */
    pdinfo->t1 = t1;
    pdinfo->t2 = t2;

    /* number of equations */
    m = system_n_equations(sys);

    /* (max) number of indep vars per equation */
    k = system_n_indep_vars(sys);

    /* number of observations per series */
    T = t2 - t1 + 1;

    bigrows = m * k;

    models = malloc(m * sizeof *models);
    if (models == NULL) {
	restore_sample(pdinfo, orig_t1, orig_t2);
	return E_ALLOC;
    }

    for (i=0; i<m; i++) {
	models[i] = gretl_model_new(pdinfo);
	if (models[i] == NULL) {
	    for (j=0; j<i; j++) {
		free_model(models[j]);
	    }
	    restore_sample(pdinfo, orig_t1, orig_t2);
	    return E_ALLOC;
	}
    }

    X = gretl_matrix_alloc(bigrows, bigrows);
    Xi = gretl_matrix_alloc(T, k);
    Xj = gretl_matrix_alloc(T, k);
    M = gretl_matrix_alloc(k, k);
    uhat = gretl_matrix_alloc(m, T);

    if (X == NULL || Xi == NULL || Xj == NULL || M == NULL ||
	uhat == NULL) {
	err = E_ALLOC;
	goto bailout;
    }

    gretl_matrix_zero(X);

    /* first grab the single-equation residuals */
    for (i=0; i<m; i++) {
	if (systype == SUR) {
	    *models[i] = lsq(system_get_list(sys, i), pZ, pdinfo, OLS, OPT_A, 0.0);
	} else if (systype == THREESLS) {
	    *models[i] = tsls_func(system_get_list(sys, i), 0, pZ, pdinfo, OPT_S);
	}
	if ((models[i])->errcode) {
	    fprintf(stderr, "model failed on lists[%d], code=%d\n",
		    i, (models[i])->errcode);
	    err = (models[i])->errcode;
	    goto bailout;
	}

	(models[i])->ID = i;
	(models[i])->aux = AUX_SYS;
	gretl_model_set_int(models[i], "systype", systype);

	for (t=0; t<T; t++) {
	    gretl_matrix_set(uhat, i, t, (models[i])->uhat[t + pdinfo->t1]);
	}
    }

    sigma = gls_sigma_inverse_from_uhat(uhat, m, T);
    if (sigma == NULL) {
	err = E_ALLOC;
	goto bailout;
    }

    /* Xi = data matrix for equation i, specified in lists[i] */
    for (i=0; i<m; i++) {
	const gretl_matrix *Y;
	
	make_sys_X_block(Xi, models[i], (const double **) *pZ, 
			 pdinfo->t1, T, systype);
	for (j=0; j<m; j++) { 
	    if (i != j) {
		make_sys_X_block(Xj, models[j], (const double **) *pZ, 
				 pdinfo->t1, T, systype);
	    }
	    Y = (i == j)? Xi : Xj;
	    gretl_matrix_multiply_mod ((const gretl_matrix *) Xi, 
				       GRETL_MOD_TRANSPOSE,
				       Y, GRETL_MOD_NONE, M);
	    kronecker_place (X, (const gretl_matrix *) M,
			     i, j, k, 
			     gretl_matrix_get(sigma, i, j)); 
	}
    }

    tmp_y = malloc((m * k) * sizeof *tmp_y);
    if (tmp_y == NULL) {
	err = E_ALLOC;
	goto bailout;
    }

    /* form Y column vector (m x k) */
    v = 0;
    for (i=0; i<m; i++) { /* loop over the m vertically arranged
			     blocks in the final column vector */
	double xx;
	const double *y;

	make_sys_X_block(Xi, models[i], (const double **) *pZ, 
			 pdinfo->t1, T, systype);
	for (j=0; j<k; j++) { /* loop over the k rows within each of 
				 the m blocks */
	    tmp_y[v] = 0.0;
	    for (l=0; l<m; l++) { /* loop over the m components that
				     must be added to form each element */
		int depvar = system_get_depvar(sys, l);

		y = (*pZ)[depvar];
		/* multiply X'[l] into y */
		xx = 0.0;
		for (t=0; t<T; t++) {
		    xx += gretl_matrix_get(Xi, t, j) * y[t + pdinfo->t1];
		}
		xx *= gretl_matrix_get(sigma, i, l);
		tmp_y[v] += xx;
	    }
	    v++;
	}
    }

    calculate_sys_coefficients(models, (const double **) *pZ, X, uhat, 
			       tmp_y, m, k, systype);
    gls_sigma_from_uhat(sigma, uhat, m, T);

    j = 0;
    if (system_save_uhat(sys)) {
	j = pdinfo->v;
	err = dataset_add_vars(m, pZ, pdinfo);
    }
    if (system_save_yhat(sys)) {
	if (j == 0) j = pdinfo->v;
	err = dataset_add_vars(m, pZ, pdinfo);
    }

    for (i=0; i<m; i++) {
	printmodel(models[i], pdinfo, prn);
	add_results_to_dataset(sys, models[i], i, &j, *pZ, pdinfo, systype);
	if (systype == THREESLS) {
	    tsls_free_data(models[i]);
	}
    }

    if (!err) {
	pprintf(prn, "%s\n(%s)\n\n",
		_("Cross-equation VCV for residuals"),
		_("correlations above the diagonal"));
	print_system_vcv(sigma, 1, prn);
    }

 bailout:

    gretl_matrix_free(X);
    gretl_matrix_free(Xi);
    gretl_matrix_free(Xj);
    gretl_matrix_free(M);
    gretl_matrix_free(sigma);
    gretl_matrix_free(uhat);

    free(tmp_y);

    for (i=0; i<m; i++) {
	free_model(models[i]);
    }
    free(models);

    restore_sample(pdinfo, orig_t1, orig_t2);

    return err;
}
