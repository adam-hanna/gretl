/* gretl - The Gnu Regression, Econometrics and Time-series Library
 * Copyright (C) 1999-2000 Ramu Ramanathan and Allin Cottrell
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License 
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this software; if not, write to the 
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "libgretl.h"
#include "internal.h"

#define DEFAULT_MAX_ITER 250

static int get_maxiter (void)
{
    static int ml = 0;

    if (ml == 0) {
        char *mlstr = getenv("GRETL_MAX_ITER");

        if (mlstr != NULL && sscanf(mlstr, "%d", &ml)) ;
        else ml = DEFAULT_MAX_ITER;
    }

    return ml;
}

static void update_fcast_errs (double *e, const double *y, 
			       const double *coeff, 
			       int n, int p, int q)
{
    int i, k, t;

    for (t=0; t<n; t++) {

	if (na(y[t])) continue;
	e[t] = y[t] - coeff[0];

	for (i=0; i<p; i++) {
	    k = t - (i + 1);
	    if (k < 0 || na(y[k])) continue;
	    e[t] -= coeff[i+1] * y[k];
	}

	for (i=0; i<q; i++) {
	    k = t - (i + 1);
	    if (k < 0) continue;
	    e[t] -= coeff[i+p+1] * e[k];
	}
    }
}

/* ARMA (1,1) example:
   # partials of e wrt c, a, and m
   genr de_c = -1 - m * de_c(-1) 
   genr de_a = -y(-1) -m * de_a(-1)
   genr de_m = -e(-1) -m * de_m(-1)
*/

/* FIXME: the general case below is probably broken */

static void update_error_partials (double *e, const double *y, 
				   double **Z, const double *coeff,
				   int n, int p, int q)
{
    int i, j, k, t;

    /* the constant term */
    for (t=0; t<n; t++) {
	Z[2][t] = -1.0;
	for (i=q-1; i>=0; i--) {
	    k = t - (i + 1);
	    if (k < 0) continue;
	    Z[2][t] -= coeff[i+p+1] * Z[2][k];
	}
    }

    /* AR terms */
    for (j=0; j<p; j++) {
	int a = 3 + j;

	for (t=0; t<n; t++) {
	    k = t - (j + 1);
	    if (k < 0 || na(y[k])) continue;
	    Z[a][t] = -y[k];
	    for (i=q-1; i>=0; i--) {
		k = t - (i + 1);
		if (k < 0) continue;
		Z[a][t] -= coeff[i+p+1] * Z[a][k];
	    }
	}
    }	    

    /* MA terms */
    for (j=q-1; j>=0; j--) {
	int m = 3 + p + j;

	for (t=0; t<n; t++) {
	    k = t - (j + 1);
	    if (k < 0) continue;
	    Z[m][t] = -e[k];
	    for (i=j; i>=0; i--) {
		k = t - (i + 1);
		if (k < 0) continue;
		Z[m][t] -= coeff[i+p+1] * Z[m][k];
	    }
	}
    }    
}

/* ARMA (1,1) example:
   # partials of l wrt c, a and m
   genr sc_c = -de_c * e
   genr sc_a = -de_a * e
   genr sc_m = -de_m * e
*/

static void update_ll_partials (double *e, double **Z, 
				int n, int p, int q)
{
    int i, j, t;
    int nc = p + q + 1;

    for (i=0; i<nc; i++) {
	j = 3 + p + q + i;
	for (t=0; t<n; t++) {
	    Z[j][t] = -Z[j-nc][t] * e[t];
	}
    }
}

static double get_ll (const double *e, int n)
{
    int t;
    double ll = 0.0;

    for (t=0; t<n; t++) {
	ll += e[t] * e[t];
    }

    ll *= -0.5;

    return ll;
}

static int reallocate_hat_series (MODEL *pmod, int n)
{
    double *tmp;

    tmp = realloc(pmod->uhat, n * sizeof *pmod->uhat);
    if (tmp != NULL) {
	pmod->uhat = tmp;
    } else {
	pmod->errcode = E_ALLOC;
	return 1;
    }

    tmp = realloc(pmod->yhat, n * sizeof *pmod->yhat);
    if (tmp != NULL) {
	pmod->yhat = tmp;
    } else {
	pmod->errcode = E_ALLOC;
	return 1;
    }

    return 0;
}

static void rewrite_arma_model_stats (MODEL *pmod, const double *coeff,
				      const int *list, const double *y, 
				      const double *e, 
				      const DATAINFO *pdinfo)
{
    int i, t;
    int p = list[1], q = list[2];
    int maxlag = (p > q)? p : q;
    int realt1 = pmod->t1 + pdinfo->t1;
    int realt2 = pmod->t2 + pdinfo->t1;
    double emean;

    pmod->ci = ARMA;
    pmod->ifc = 1;

    pmod->nobs -= maxlag;
    pmod->dfd -= maxlag;

    for (i=0; i<pmod->ncoeff; i++) {
	pmod->coeff[i] = coeff[i];
    }

    copylist(&pmod->list, list);

    pmod->ybar = _esl_mean(realt1 + maxlag, realt2, y);
    pmod->sdy = _esl_stddev(realt1 + maxlag, realt2, y);

    /* if model was estimated on a sub-sample, we need to expand the
       residual and fitted series (which are always full-length) */
    if (pdinfo->t1 + (pdinfo->n - 1 - pdinfo->t2) > 0) {
	if (reallocate_hat_series(pmod, pdinfo->n)) {
	    return;
	}
    }

    emean = pmod->ess = 0.0;
    for (t=0; t<pdinfo->n; t++) {
	if (t < (realt1 + maxlag) || t > realt2) {
	    pmod->uhat[t] = pmod->yhat[t] = NADBL;
	} else {
	    pmod->uhat[t] = e[t - realt1];
	    pmod->yhat[t] = y[t] - pmod->uhat[t];
	    pmod->ess += pmod->uhat[t] * pmod->uhat[t];
	    emean += pmod->uhat[t];
	}
    }

    /* FIXME: make this available for printing along with the model */
    emean /= pmod->nobs;
    fprintf(stderr, "Mean of estimated errors = %g\n", emean);

    pmod->sigma = sqrt(pmod->ess / pmod->dfd);

    pmod->tss = 0.0;
    for (t=realt1 + maxlag; t<=realt2; t++) {
	pmod->tss += (y[t] - pmod->ybar) * (y[t] - pmod->ybar);
    }

    pmod->fstt = pmod->dfd * (pmod->tss - pmod->ess) / (pmod->dfn * pmod->ess);

    pmod->rsq = pmod->adjrsq = NADBL;

    if (pmod->tss > 0) {
	pmod->rsq = 1.0 - (pmod->ess / pmod->tss);
	if (pmod->dfd > 0) {
	    double den = pmod->tss * pmod->dfd;

	    pmod->adjrsq = 1.0 - (pmod->ess * (pmod->nobs - 1) / den);
	}
    }

    /* get the model start/end dates in sync with the main dataset */
    pmod->t1 = realt1 + maxlag;
    pmod->t2 += pdinfo->t1;

    gretl_aic_etc(pmod);
}

static int check_arma_list (const int *list)
{
    int err = 0;

    if (list[0] != 4) err = 1;

    /* for now we'll accept ARMA (2,2) at max */
    else if (list[1] < 0 || list[1] > 2) err = 1;
    else if (list[2] < 0 || list[2] > 2) err = 1;

    if (err) {
	gretl_errmsg_set(_("Syntax error in arma command"));
    }
    
    return err;
}

MODEL arma_model (int *list, const double **Z, DATAINFO *pdinfo, 
		  PRN *prn)
{
    int an = pdinfo->t2 - pdinfo->t1 + 1;
    int nc, v, p, q;
    int iters, itermax;
    int subiters, subitermax;
    int i, t;
    double tol, crit, ll = 0.0;
    double steplength, ll_prev = -1.0e+8;
    double *e, *coeff, *d_coef;
    const double *y;
    double **aZ;
    DATAINFO *ainfo;
    int *alist;
    MODEL armod;

    _init_model(&armod, pdinfo);  

    if (check_arma_list(list)) {
	armod.errcode = E_UNSPEC;
	return armod;
    }

    v = list[4]; /* the dependent variable */
    p = list[1]; /* AR order */
    q = list[2]; /* MA order */

    /* number of coefficients */
    nc = 1 + p + q;

    alist = malloc((nc + 2) * sizeof *alist);
    if (alist == NULL) {
	armod.errcode = E_ALLOC;
	return armod;
    }

    alist[0] = nc + 1;
    alist[1] = 0; /* dep var is constant, in OPG */
    for (i=0; i<=p+q; i++) {
	alist[i+2] = 3 + p + q + i;
    }

    /* coefficient vector */
    coeff = malloc(nc * sizeof *coeff);
    if (coeff == NULL) {
	free(alist);
	armod.errcode = E_ALLOC;
	return armod;
    }

    /* and its change on each iteration */
    d_coef = malloc(nc * sizeof *d_coef);
    if (d_coef == NULL) {
	free(alist);
	free(coeff);
	armod.errcode = E_ALLOC;
	return armod;
    }

    /* temporary dataset: we need two series for each coefficient,
       plus one for the forecast errors and one for the const */
    ainfo = create_new_dataset(&aZ, nc * 2 + 2, an, 0);
    if (ainfo == NULL) {
	free(alist);
	free(coeff);
	armod.errcode = E_ALLOC;
	return armod;
    }
    ainfo->extra = 1; /* ? can't remember what this does */

    /* initialize the coefficients (FIXME: make configurable?) */
    coeff[0] = 0.0;
    for (i=0; i<p; i++) coeff[i+1] = 0.1;
    for (i=0; i<q; i++) coeff[i+p+1] = 0.1;

    /* initialize forecast errors and derivatives */
    for (t=0; t<an; t++) {
	for (i=1; i<=nc+1; i++) {
	    aZ[i][t] = 0.0;
	}
    }

    /* forecast errors */
    e = aZ[1];
    /* dependent variable */
    y = &Z[v][pdinfo->t1];

    crit = 1.0;
    tol = 1.0e-9;
    iters = 0;
    itermax = get_maxiter();
    subitermax = 20;
    steplength = 1.0;

    /* generate one-step forecast errors */
    update_fcast_errs(e, y, coeff, an, p, q);

    /* calculate log-likelihood */
    ll = get_ll(e, an);

    while (crit > tol && iters++ < itermax) {

	pprintf(prn, "Iteration %d\n", iters);
        pprintf(prn, "  log likelihood = %g\n", ll);

        /* partials of e wrt coeffs */
	update_error_partials(e, y, aZ, coeff, an, p, q);

	/* partials of l wrt coeffs */
	update_ll_partials(e, aZ, an, p, q);

	/* OPG regression */
	armod = lsq(alist, &aZ, ainfo, OLS, 1, 0.0);
	if (armod.errcode) {
	    goto arma_bailout;
	}

	/* compute direction */
	for (i=0; i<armod.ncoeff; i++) {
	    d_coef[i] = armod.coeff[i];
	}

	/* update parameters */
	for (i=0; i<armod.ncoeff; i++) {
	  coeff[i] += d_coef[i] * steplength;
	}

	/* compute log-likelihood at new point */
	update_fcast_errs(e, y, coeff, an, p, q);
	ll = get_ll(e, an);

	/* subiterations for best steplength */
	subiters = 0;
	while (steplength > 1.0e-08 && subiters++ < subitermax && ll < ll_prev) {

	    /* if we've gone down, halve steplength and go back */
	    steplength *= 0.5;
	    for (i=0; i<armod.ncoeff; i++) {
		coeff[i] -= d_coef[i] * steplength;
	    }

	    /* compute loglikelihood again */
	    update_fcast_errs(e, y, coeff, an, p, q);
	    ll = get_ll(e, an);
	}

	if (subiters > subitermax) {
	    gretl_errmsg_set(_("Maximum number of subiterations reached: quitting"));
	    armod.errcode = E_UNSPEC;
	    goto arma_bailout;
	}

	pprintf(prn, _("  constant        = %.8g (gradient = %#.6g)\n"), 
		coeff[0], armod.coeff[0]);
	for (i=0; i<p; i++) {
	    pprintf(prn, _("  ar%d coefficient = %.8g (gradient = %#.6g)\n"), 
		    i + 1, coeff[1+i], armod.coeff[1+i]);
	}
	for (i=0; i<q; i++) {
	    pprintf(prn, _("  ma%d coefficient = %.8g (gradient = %#.6g)\n"), 
		    i + 1, coeff[1+p+i], armod.coeff[1+p+i]);
	}
	pprintf(prn, _("  steplength = %.6g (subiterations = %d)\n"), 
		steplength, subiters);

	/* update criterion */
	crit = armod.nobs - armod.ess;
	pprintf(prn, _("  criterion = %g\n\n"), crit);

	/* try and re-double the steplength for next iteration */
	if (subiters == 1 && steplength < 1.0){
	    steplength *= 2.0;
	} else if (subiters == 0) {
	    /* time to quit */
	    pputs(prn, _("Warning: convergence criterion was not met\n"));
	    break;
	}

	ll_prev = ll;

    }

    y = Z[v];
    armod.lnL = ll;
    rewrite_arma_model_stats(&armod, coeff, list, y, e, pdinfo);

 arma_bailout:

    free(coeff);
    free(d_coef);
    free_Z(aZ, ainfo);
    clear_datainfo(ainfo, CLEAR_FULL);
    free(ainfo);

    return armod;
}



    

    

    
    
