/* 
 *  gretl -- Gnu Regression, Econometrics and Time-series Library
 *  Copyright (C) 2001 Allin Cottrell and Riccardo "Jack" Lucchetti
 * 
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 * 
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 * 
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 */

#ifndef COMMANDS_H
#define COMMANDS_H

typedef enum {
    SEMIC = 0,    
    ADD,
    ADF,
    APPEND,
    AR,  
    AR1,
    ARBOND,
    ARCH,
    ARMA,
    BREAK,
    BXPLOT,
    CHOW,
    COEFFSUM,
    COINT,
    COINT2,
    CORR,     
    CORRGM,   
    CRITERIA,
    CUSUM,
    DATA,
    DATAMOD,
    DELEET,
    DIFF,
    DIFFTEST,
    DISCRETE,
    DUMMIFY,
    ELIF,
    ELSE,
    END,
    ENDIF,
    ENDLOOP,
    EQNPRINT, 
    EQUATION,
    ESTIMATE,
    FCAST,  
    FOREIGN,
    FREQ, 
    FUNC,
    FUNCERR,
    GARCH,
    GENR,  
    GMM,
    GNUPLOT, 
    GRAPH,
    HAUSMAN,
    HECKIT,
    HELP,    
    HSK,
    HURST,
    IF,
    INCLUDE,
    INFO,
    INTREG,
    KALMAN,
    KPSS,
    LABELS, 
    LAD,
    LAGS,    
    LDIFF,
    LEVERAGE,
    LMTEST, 
    LOGISTIC,
    LOGIT,
    LOGS,
    LOOP,
    MAHAL,
    MEANTEST,
    MLE,
    MODELTAB,
    MODPRINT,
    MPOLS,
    NLS,
    NORMTEST,
    NULLDATA,
    OLS,     
    OMIT,
    OPEN,
    ORTHDEV,
    OUTFILE,
    PANEL,
    PCA,
    PERGM,
    PLOT,    
    POISSON,
    PRINT, 
    PRINTF,
    PROBIT,
    PVALUE, 
    QUANTREG,
    QLRTEST,
    QUIT,
    RENAME,
    RESET,
    RESTRICT,
    RMPLOT,
    RUN,
    RUNS,
    SCATTERS,
    SDIFF,
    SET,
    SETINFO,
    SETOBS,
    SETMISS,
    SHELL,  
    SMPL,
    SPEARMAN,
    SPRINTF,
    SQUARE,
    SSCANF,
    STORE, 
    SUMMARY,
    SYSTEM,
    TABPRINT,
    TESTUHAT,
    TOBIT,
    IVREG,
    VAR,
    VARLIST,
    VARTEST,
    VECM,
    VIF,
    WLS,
    XCORRGM,
    XTAB,
    NC
} GretlCmdIndex;

#define NEEDS_TWO_VARS(c)  ((c) == AR || (c) == ARCH || (c) == COINT || \
                            (c) == AR1 || (c) == CORR || (c) == HSK || \
                            (c) == LOGIT || (c) == PROBIT || \
                            (c) == SPEARMAN || (c) == OLS || \
                            (c) == IVREG || (c) == VAR || (c) == WLS || \
			    (c) == XTAB)

#define TEXTSAVE_OK(c) (c == ADD || \
                        c == ADF || \
                        c == CHOW || \
                        c == COEFFSUM || \
                        c == COINT || \
                        c == COINT2 || \
                        c == CORR || \
                        c == CORRGM || \
                        c == CRITERIA || \
                        c == CUSUM || \
                        c == FCAST || \
                        c == FREQ || \
                        c == GRAPH || \
                        c == HAUSMAN || \
                        c == KPSS || \
                        c == LEVERAGE || \
                        c == LMTEST || \
                        c == MEANTEST || \
                        c == MPOLS || \
			c == NORMTEST || \
                        c == OMIT || \
                        c == PCA || \
                        c == PERGM || \
                        c == PLOT || \
                        c == PRINT || \
                        c == PRINTF || \
                        c == PVALUE || \
                        c == QLRTEST || \
                        c == RESET || \
                        c == RMPLOT || \
                        c == RUNS || \
                        c == SPEARMAN || \
                        c == SUMMARY || \
                        c == TESTUHAT || \
                        c == VIF || \
                        c == VARTEST || \
                        c == XCORRGM || \
                        c == XTAB)

#define NEEDS_MODEL_CHECK(c) (c == ADD || \
                              c == CHOW || \
                              c == COEFFSUM || \
                              c == CUSUM || \
                              c == EQNPRINT || \
                              c == FCAST || \
                              c == HAUSMAN || \
                              c == LEVERAGE || \
                              c == LMTEST || \
                              c == OMIT || \
                              c == QLRTEST || \
                              c == RESET || \
                              c == TABPRINT || \
                              c == TESTUHAT || \
                              c == VIF)

#define USES_BFGS(c) ((c) == ARMA || \
                      (c) == GARCH || \
                      (c) == GMM || \
		      (c) == HECKIT || \
		      (c) == INTREG || \
                      (c) == MLE || \
                      (c) == TOBIT)

int gretl_command_number (const char *s);

const char *gretl_command_word (int i);

const char *gretl_command_complete_next (const char *s, int idx);

const char *gretl_command_complete (const char *s);

void gretl_command_hash_cleanup (void);

#endif /* COMMANDS_H */
