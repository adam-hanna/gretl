/*
 *  Copyright (c) by Ramu Ramanathan and Allin Cottrell
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

#include "libgretl.h"
#include "gretl_string_table.h"
#include "dbwrite.h"

#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libxml/xmlmemory.h>
#include <libxml/parser.h>

#define QUOTE                  '\''
#define SCALAR_DIGITS           12
#define CSVSTRLEN               72

#define IS_DATE_SEP(c) (c == '.' || c == ':' || c == ',')

static int writelbl (const char *lblfile, const int *list, 
		     const DATAINFO *pdinfo);
static int writehdr (const char *hdrfile, const int *list, 
		     const DATAINFO *pdinfo, int opt);
static double obs_float (const DATAINFO *pdinfo, int end);
static int write_xmldata (const char *fname, const int *list, 
			  const double **Z, const DATAINFO *pdinfo, 
			  GretlDataFormat fmt, PATHS *ppaths);
static int xmlfile (const char *fname);
static int csv_time_series_check (DATAINFO *pdinfo, PRN *prn);

static char STARTCOMMENT[3] = "(*";
static char ENDCOMMENT[3] = "*)";

#define PROGRESS_BAR "progress_bar"

/* ------------------------------------------------------------- */

#ifdef ENABLE_NLS

static int printing_to_console;

char *maybe_iso_gettext (const char *msgid)
{
   if (printing_to_console) {
       return iso_gettext(msgid);
   } else {
       return gettext(msgid);
   }
} 

void check_for_console (PRN *prn)
{
    if (prn == NULL) return;

    if (printing_to_standard_stream(prn)) {
	printing_to_console = 1;
    } else {
	printing_to_console = 0;
    }
}

void console_off (void)
{
    printing_to_console = 0;
}

#endif /* ENABLE_NLS */

/* ------------------------------------------------------------- */

double get_date_x (int pd, const char *obs)
{
    double x = 1.0;

    if ((pd == 5 || pd == 6 || pd == 7 || pd == 52) && strlen(obs) > 4) { 
	/* calendar data */
	long ed = get_epoch_day(obs);

	if (ed >= 0) {
	    x = ed;
	}
    } else {
	x = obs_str_to_double(obs); 
    }

    return x;
}

static char *compact_method_to_string (int method)
{
    if (method == COMPACT_SUM) return "COMPACT_SUM";
    else if (method == COMPACT_AVG) return "COMPACT_AVG";
    else if (method == COMPACT_SOP) return "COMPACT_SOP";
    else if (method == COMPACT_EOP) return "COMPACT_EOP";
    else return "COMPACT_NONE";
}

static int compact_string_to_int (const char *str)
{
    if (!strcmp(str, "COMPACT_SUM")) return COMPACT_SUM;
    else if (!strcmp(str, "COMPACT_AVG")) return COMPACT_AVG;
    else if (!strcmp(str, "COMPACT_SOP")) return COMPACT_SOP;
    else if (!strcmp(str, "COMPACT_EOP")) return COMPACT_EOP;
    else return COMPACT_NONE;
}

/* Skip past comments in .hdr file.  Return 0 if comments found,
   otherwise 1.
*/

static int skipcomments (FILE *fp, const char *str)
{
    char word[MAXLEN];  /* should be big enough to accommodate
			   strings among the comments? */

    *word = '\0';

    if (strncmp(str, STARTCOMMENT, 2) == 0) {
        while (strcmp(word, ENDCOMMENT)) {
            fscanf(fp, "%s", word);
        }
        return 0;
    } 

    return 1;
}

static int comment_lines (FILE *fp, char **pbuf)
{
    char s[MAXLEN], *mybuf = NULL;
    int count = 0, bigger = 1;

    if (fgets(s, sizeof s, fp) == NULL) {
	return 0;
    }

    if (!strncmp(s, STARTCOMMENT, 2)) {
	*pbuf = malloc(20 * MAXLEN);

	if (*pbuf == NULL) {
	    return -1;
	}

	**pbuf = '\0';

	while (fgets(s, sizeof s, fp)) {
	    if (!strncmp(s, ENDCOMMENT, 2)) {
		break;
	    }
	    if (++count > 20 * bigger) {
		size_t bufsize = 20 * MAXLEN * ++bigger;

		mybuf = realloc(*pbuf, bufsize);
		if (mybuf == NULL) {
		    return -1;
		} else {
		    *pbuf = mybuf;
		}
	    }
	    strcat(*pbuf, s);
	} 
    }

    return count;
}

static void eatspace (FILE *fp)
{
    char c;

    while (1) {
	c = fgetc(fp);
	if (!isspace((unsigned char) c)) {
	    ungetc(c, fp);
	    return;
	}
    }
}

static int readdata (FILE *fp, const DATAINFO *pdinfo, double **Z,
		     int binary, int old_byvar)
{
    int i, t, n = pdinfo->n;
    char c, marker[OBSLEN];
    int err = 0;
    float x;

    gretl_errmsg[0] = '\0';

    if (binary == 1) { /* single-precision binary data */
	for (i=1; i<pdinfo->v; i++) {
	    for (t=0; t<n; t++) {
		if (!fread(&x, sizeof x, 1, fp)) {
		    sprintf(gretl_errmsg, _("WARNING: binary data read error at "
			    "var %d"), i);
		    return 1;
		}
		Z[i][t] = (double) x;
	    }
	}
    } else if (binary == 2) { /* double-precision binary data */
	for (i=1; i<pdinfo->v; i++) {
	    if (!fread(Z[i], sizeof(double), n, fp)) {
		sprintf(gretl_errmsg, 
			_("WARNING: binary data read error at var %d"), i);
		return 1;
	    }
	}
    } else if (old_byvar) {
	/* ascii data by variable */
	for (i=1; i<pdinfo->v; i++) {
	   for (t=0; t<n && !err; t++) {
		if ((fscanf(fp, "%lf", &Z[i][t])) != 1) {
		    sprintf(gretl_errmsg, 
			    _("WARNING: ascii data read error at var %d, "
			    "obs %d"), i, t + 1);
		    err = 1;
		    break;
		}
		if (Z[i][t] == -999.0) {
		    Z[i][t] = NADBL;
		} 
	   }
	}	       
    } else { 
	/* ascii data by observation */
	char sformat[8];

	sprintf(sformat, "%%%ds", OBSLEN - 1);

#ifdef ENABLE_NLS
	setlocale(LC_NUMERIC, "C");
#endif
	for (t=0; t<n && !err; t++) {
	    eatspace(fp);
	    c = fgetc(fp);  /* test for a #-opened comment line */
	    if (c == '#') {
		while (c != '\n') {
		    c = fgetc(fp);
		}
	    } else {
		ungetc(c, fp);
	    }
	    if (pdinfo->markers) {
		*marker = '\0';
		fscanf(fp, sformat, marker);
		strcpy(pdinfo->S[t], marker);
	    }
	    for (i=1; i<pdinfo->v; i++) {
		if ((fscanf(fp, "%lf", &Z[i][t])) != 1) {
		    sprintf(gretl_errmsg, 
			    _("WARNING: ascii data read error at var %d, "
			    "obs %d"), i, t + 1);
		    err = 1;
		    break;
		}
		if (Z[i][t] == -999.0) {
		    Z[i][t] = NADBL;
		} 
	    }
	}
#ifdef ENABLE_NLS
	setlocale(LC_NUMERIC, "");
#endif
    }

    return err;
}

static int gz_readdata (gzFile fz, const DATAINFO *pdinfo, double **Z,
			int binary)
{
    int i, t, n = pdinfo->n;
    int err = 0;
    
    gretl_errmsg[0] = '\0';

    if (binary == 1) { /* single-precision binary data */
	float xx;

	for (i=1; i<pdinfo->v; i++) {
	    for (t=0; t<n; t++) {
		if (!gzread(fz, &xx, sizeof xx)) {
		    sprintf(gretl_errmsg, _("WARNING: binary data read error at "
			    "var %d"), i);
		    return 1;
		}
		Z[i][t] = (double) xx;
	    }
	}
    } else if (binary == 2) { /* double-precision binary data */
	for (i=1; i<pdinfo->v; i++) {
	    if (!gzread(fz, &Z[i][0], n * sizeof(double))) {
		sprintf(gretl_errmsg, 
			_("WARNING: binary data read error at var %d"), i);
		return 1;
	    }
	}
    } else { /* ascii data file */
	char *line, numstr[24], sformat[8];
	int llen = pdinfo->v * 32;
	size_t offset;

	line = malloc(llen);
	if (line == NULL) {
	    return E_ALLOC;
	}

	sprintf(sformat, "%%%ds", OBSLEN - 1);

#ifdef ENABLE_NLS
	setlocale(LC_NUMERIC, "C");
#endif

	for (t=0; t<n; t++) {
	    offset = 0L;
	    if (!gzgets(fz, line, llen - 1)) {
		sprintf(gretl_errmsg, _("WARNING: ascii data read error at "
			"obs %d"), t + 1);
		err = 1;
		break;
	    }

	    chopstr(line);
	    compress_spaces(line);
	    if (line[0] == '#') {
		t--;
		continue;
	    }

	    if (pdinfo->markers) {
		if (sscanf(line, sformat, pdinfo->S[t]) != 1) {
		   sprintf(gretl_errmsg, 
			   _("WARNING: failed to read case marker for "
			   "obs %d"), t + 1);
		   err = 1;
		   break;
		}
		pdinfo->S[t][OBSLEN-1] = 0;
		offset += strlen(pdinfo->S[t]) + 1;
	    }

	    for (i=1; i<pdinfo->v; i++) {
		if (sscanf(line + offset, "%23s", numstr) != 1) {
		    sprintf(gretl_errmsg, 
			    _("WARNING: ascii data read error at var %d, "
			    "obs %d"), i, t + 1);
		    err = 1;
		    break;
		}
		numstr[23] = 0;
		Z[i][t] = atof(numstr);
		if (i < pdinfo->v - 1) {
		    offset += strlen(numstr) + 1;
		}
	    }

	    if (err) break;
	}

	free(line);

#ifdef ENABLE_NLS
	setlocale(LC_NUMERIC, "");
#endif

    }

    return err;
}

/**
 * check_varname:
 * @varname: putative name for variable.
 * 
 * Check a variable name for legality.
 * 
 * Returns: 0 if name is OK, 1 if not.
 */

int check_varname (const char *varname)
{
    int testchar = 'a';
    int ret = 0;

    *gretl_errmsg = '\0';

    if (gretl_reserved_word(varname)) {
	ret = VARNAME_RESERVED;
    } else if (!(isalpha((unsigned char) *varname))) {
	testchar = *varname;
        ret = VARNAME_FIRSTCHAR;
    } else {
	const char *p = varname;

	while (*p && testchar == 'a') {
	    if (!(isalpha((unsigned char) *p))  
		&& !(isdigit((unsigned char) *p))
		&& *p != '_') {
		testchar = *p;
		ret = VARNAME_BADCHAR;
	    }
	    p++;
	}
    }

    if (testchar != 'a') {
	if (isprint((unsigned char) testchar)) {
	    if (ret == VARNAME_FIRSTCHAR) {
		sprintf(gretl_errmsg, _("First char of varname ('%c') is bad\n"
					"(first must be alphabetical)"), 
			(unsigned char) testchar);
	    } else {
		sprintf(gretl_errmsg, _("Varname contains illegal character '%c'\n"
					"Use only letters, digits and underscore"), 
			(unsigned char) testchar);
	    }
	} else {
	    if (ret == VARNAME_FIRSTCHAR) {
		sprintf(gretl_errmsg, _("First char of varname (0x%x) is bad\n"
					"(first must be alphabetical)"), 
			(unsigned) testchar);
	    } else {
		sprintf(gretl_errmsg, _("Varname contains illegal character 0x%x\n"
					"Use only letters, digits and underscore"), 
			(unsigned) testchar);
	    }
	}
    }

    return ret;
}   

static int readhdr (const char *hdrfile, DATAINFO *pdinfo, 
		    int *binary, int *old_byvar)
{
    FILE *fp;
    int n, i = 0, panel = 0, descrip = 0;
    char str[MAXLEN], byobs[6], option[8];

    *gretl_errmsg = '\0';

    fp = gretl_fopen(hdrfile, "r");
    if (fp == NULL) {
	sprintf(gretl_errmsg, _("Couldn't open file %s"),  hdrfile);
	return E_FOPEN;
    }

    fscanf(fp, "%s", str);
    i += skipcomments(fp, str); 

    while (1) { /* find number of variables */
        if (fscanf(fp, "%s", str) != 1) {
	    fclose(fp);
	    sprintf(gretl_errmsg, _("Opened header file %s\n"
		    "Couldn't find list of variables (must "
		    "be terminated with a semicolon)"), hdrfile);
	    return 1;
	}
	n = strlen(str);
	if (str[n-1] == ';') {
	    if (n > 1) i++;
	    break;
	} else i++;
    }

    pdinfo->v = i + 1;
    fclose(fp);

    if (dataset_allocate_varnames(pdinfo)) {
	return E_ALLOC;
    }

    i = 1;
    fp = gretl_fopen(hdrfile, "r");

    str[0] = 0;
    fscanf(fp, "%s", str);
    if (skipcomments(fp, str)) {
        safecpy(pdinfo->varname[i], str, VNAMELEN - 1);
	if (check_varname(pdinfo->varname[i++])) {
	    goto varname_error;
	}
    } else {
	descrip = 1; /* comments were found */
    }

    while (1) {
        fscanf(fp, "%s", str);
	n = strlen(str);
	if (str[n-1] != ';') {
            safecpy(pdinfo->varname[i], str, VNAMELEN - 1);
	    if (check_varname(pdinfo->varname[i++])) {
		goto varname_error;
	    }
        } else {
	    if (n > 1) {
		safecpy(pdinfo->varname[i], str, n-1);
		pdinfo->varname[i][n] = '\0';
		if (check_varname(pdinfo->varname[i])) {
		    goto varname_error; 
		}
	    }
	    break;
	}
    }

    fscanf(fp, "%d", &pdinfo->pd);
    fscanf(fp, "%s", pdinfo->stobs);
    fscanf(fp, "%s", pdinfo->endobs);

    colonize_obs(pdinfo->stobs);
    colonize_obs(pdinfo->endobs);

    pdinfo->sd0 = get_date_x(pdinfo->pd, pdinfo->stobs);

    if (pdinfo->sd0 >= 2.0) {
        pdinfo->structure = TIME_SERIES; /* actual time series? */
    } else if (pdinfo->sd0 > 1.0) {
	pdinfo->structure = STACKED_TIME_SERIES; /* panel data? */
    } else {
	pdinfo->structure = CROSS_SECTION;
    }

    pdinfo->n = -1;
    pdinfo->n = dateton(pdinfo->endobs, pdinfo) + 1;

    *binary = 0;
    pdinfo->markers = NO_MARKERS;

    n = fscanf(fp, "%5s %7s", byobs, option);

    if (n == 1 && strcmp(byobs, "BYVAR") == 0) {
	*old_byvar = 1;
    } else if (n == 2) {
	if (strcmp(option, "SINGLE") == 0) {
	    *binary = 1;
	} else if (strcmp(option, "BINARY") == 0) {
	    *binary = 2;
	} else if (strcmp(option, "MARKERS") == 0) {
	    pdinfo->markers = 1;
	} else if (strcmp(option, "PANEL2") == 0) {
	    panel = 1;
	    pdinfo->structure = STACKED_TIME_SERIES;
	} else if (strcmp(option, "PANEL3") == 0) {
	    panel = 1;
	    pdinfo->structure = STACKED_CROSS_SECTION;
	}
    } 

    if (!panel && fscanf(fp, "%6s", option) == 1) {
	if (strcmp(option, "PANEL2") == 0) {
	    pdinfo->structure = STACKED_TIME_SERIES;
	} else if (strcmp(option, "PANEL3") == 0) {
	    pdinfo->structure = STACKED_CROSS_SECTION;
	}
    }

    if (fp != NULL) {
	fclose(fp);
    }

    /* last pass, to pick up data description */
    pdinfo->descrip = NULL;
    if (descrip) {
	char *dbuf = NULL;
	int lines;

	fp = gretl_fopen(hdrfile, "r");
	if (fp == NULL) return 0;
	if ((lines = comment_lines(fp, &dbuf)) > 0) {
	    delchar('\r', dbuf);
	    pdinfo->descrip = malloc(strlen(dbuf) + 1);
	    if (pdinfo->descrip != NULL) {
		strcpy(pdinfo->descrip, dbuf);
	    }
	    free(dbuf);
	}
	else if (lines < 0) {
	    fprintf(stderr, I_("Failed to store data comments\n"));
	}
	fclose(fp);
    } 
	
    return 0;

    varname_error:

    fclose(fp);
    clear_datainfo(pdinfo, CLEAR_FULL);

    return E_DATA;
}

static int bad_date_string (const char *s)
{
    int err = 0;

    *gretl_errmsg = '\0';

    while (*s && !err) {
	if (!isdigit((unsigned char) *s) && !IS_DATE_SEP(*s)) {
	    if (isprint((unsigned char) *s)) {
		sprintf(gretl_errmsg, 
			_("Bad character '%c' in date string"), *s);
	    } else {
		sprintf(gretl_errmsg, 
			_("Bad character %d in date string"), *s);
	    }
	    err = 1;
	}
	s++;
    }

    return err;
}

static void maybe_unquote_label (char *targ, const char *src)
{
    if (*src == '"' || *src == '\'') {
	int n;

	strcpy(targ, src + 1);
	n = strlen(targ);
	if (n > 0 && (targ[n-1] == '"' || targ[n-1] == '\'')) {
	    targ[n-1] = '\0';
	}
    } else {
	strcpy(targ, src);
    }
}

static int get_dot_pos (const char *s)
{
    int i, pos = 0;

    for (i=0; *s != '\0'; i++, s++) {
	if (IS_DATE_SEP(*s)) {
	    pos = i;
	    break;
	}
    }

    return pos;
}

static int 
real_dateton (const char *date, const DATAINFO *pdinfo,
	      int nolimit)
{
    int t, n = -1;

    /* first check if this is calendar data and if so,
       treat accordingly */

    if (calendar_data(pdinfo)) {
	if (pdinfo->markers && pdinfo->S != NULL) {
	    /* "hard-wired" calendar dates as strings */
	    for (t=0; t<pdinfo->n; t++) {
		if (!strcmp(date, pdinfo->S[t])) {
		    /* handled */
		    return t;
		}
	    }
	    /* try allowing for 2- versus 4-digit years? */
	    if (strlen(pdinfo->S[0]) == 10 &&
		(!strncmp(pdinfo->S[0], "19", 2) || 
		 !strncmp(pdinfo->S[0], "20", 2))) {
		for (t=0; t<pdinfo->n; t++) {
		    if (!strcmp(date, pdinfo->S[t] + 2)) {
			/* handled */
			return t;
		    }
		}		
	    }
	    /* out of options: abort */
	    return -1;
	} else {
	    /* automatic calendar dates */
	    n = calendar_obs_number(date, pdinfo);
	} 
    } 

    /* now try treating as an undated time series */

    else if (dataset_is_daily(pdinfo) ||
	     dataset_is_weekly(pdinfo) ||
	     custom_time_series(pdinfo)) {
	if (sscanf(date, "%d", &t) && t > 0) {
	    n = t - 1;
	}
    } 

    /* string observation markers other than dates? */

    else if (pdinfo->markers && pdinfo->S != NULL) {
	char test[OBSLEN];

	maybe_unquote_label(test, date);
	for (t=0; t<pdinfo->n; t++) {
	    if (!strcmp(test, pdinfo->S[t])) {
		/* handled */
		return t;
	    }
	}
	/* else maybe just a straight obs number */
	if (sscanf(date, "%d", &t) && t > 0) {
	    n = t - 1;
	}
    }

    /* decennial data? */

    else if (dataset_is_decennial(pdinfo)) {
	if (sscanf(date, "%d", &t) && t > 0) {
	    n = (t - pdinfo->sd0) / 10;
	}	
    }

    /* treat as "regular" numeric obs number or date */

    else {
	int dotpos1, dotpos2;

	if (bad_date_string(date)) {
	    return -1;
	}

	dotpos1 = get_dot_pos(date);
	dotpos2 = get_dot_pos(pdinfo->stobs);

	if ((dotpos1 && !dotpos2) || (dotpos2 && !dotpos1)) {
	    sprintf(gretl_errmsg, _("Date strings inconsistent"));
	} else if (!dotpos1 && !dotpos2) {
	    n = atoi(date) - atoi(pdinfo->stobs);
	} else {
	    char majstr[5] = {0};
	    char minstr[3] = {0};
	    char majstr0[5] = {0};
	    char minstr0[3] = {0};

	    int maj, min;
	    int maj0, min0;

	    strncat(majstr, date, dotpos1);
	    maj = atoi(majstr);
	    strncat(minstr, date + dotpos1 + 1, 2);
	    min = atoi(minstr);	    

	    strncat(majstr0, pdinfo->stobs, dotpos2);
	    maj0 = atoi(majstr0);
	    strncat(minstr0, pdinfo->stobs + dotpos2 + 1, 2);
	    min0 = atoi(minstr0);
    
	    n = pdinfo->pd * (maj - maj0) + (min - min0);
	}
    }

    if (!nolimit && pdinfo->n > 0 && n >= pdinfo->n) {
	sprintf(gretl_errmsg, _("Observation number out of bounds"));
	n = -1; 
    }

    return n;
}

/**
 * dateton:
 * @date: string representation of date for processing.
 * @pdinfo: pointer to data information struct.
 * 
 * Given a "current" date string, a periodicity, and a starting
 * date string, returns the observation number corresponding to
 * the current date string, counting from zero.
 * 
 * Returns: integer observation number.
 *
 */

int dateton (const char *date, const DATAINFO *pdinfo)
{
    return real_dateton(date, pdinfo, 0);
}

/* special for appending data: allow the date to be outside of
   the range of the current dataset */

static int merge_dateton (const char *date, const DATAINFO *pdinfo)
{
    return real_dateton(date, pdinfo, 1);
}

static char *
real_ntodate (char *datestr, int t, const DATAINFO *pdinfo, int full)
{
    double x;

#if 0
    fprintf(stderr, "real_ntodate: t=%d, pd=%d, sd0=%g\n",
	    t, pdinfo->pd, pdinfo->sd0);
#endif

    if (calendar_data(pdinfo)) {
	/* handles both daily and dated weekly data */
	if (pdinfo->markers && pdinfo->S != NULL) {
	    strcpy(datestr, pdinfo->S[t]);
	} else {
	    calendar_date_string(datestr, t, pdinfo);
	}
	if (!full && strlen(datestr) > 8) {
	    char tmp[12];

	    strcpy(tmp, datestr);
	    strcpy(datestr, tmp + 2);
	}
	return datestr;
    } else if (dataset_is_daily(pdinfo) || 
	       dataset_is_weekly(pdinfo) ||
	       custom_time_series(pdinfo)) {
	/* undated time series */
	x = date(t, 1, pdinfo->sd0);
	sprintf(datestr, "%d", (int) x);
	return datestr;
    } else if (dataset_is_decennial(pdinfo)) {
	x = pdinfo->sd0 + 10 * t;
	sprintf(datestr, "%d", (int) x);
	return datestr;
    }	

    x = date(t, pdinfo->pd, pdinfo->sd0);

    if (pdinfo->pd == 1) {
        sprintf(datestr, "%d", (int) x);
    } else {
	int pdp = pdinfo->pd, len = 1;
	char fmt[8];

	while ((pdp = pdp / 10)) len++;
	sprintf(fmt, "%%.%df", len);
	sprintf(datestr, fmt, x);
	colonize_obs(datestr);
    }
    
    return datestr;
}

/**
 * ntodate:
 * @datestr: string to which date is to be printed.
 * @nt: an observation number (zero-based).
 * @pdinfo: data information struct.
 * 
 * print to @datestr the calendar representation of observation
 * number nt.
 * 
 * Returns: the observation string.
 */

char *ntodate (char *datestr, int t, const DATAINFO *pdinfo)
{
    return real_ntodate(datestr, t, pdinfo, 0);
}

char *ntodate_full (char *datestr, int t, const DATAINFO *pdinfo)
{
    return real_ntodate(datestr, t, pdinfo, 1);
}

/* .......................................................... */

static int blank_check (FILE *fp)
{
    int i, deflt = 1;
    char s[MAXLEN];

    for (i=0; i<3 && deflt && fgets(s, MAXLEN-1, fp); i++) {
	if (i == 0 && strncmp(s, "(*", 2)) {
	    deflt = 0;
	} else if (i == 1 && strncmp(s, _("space for comments"), 18)) {
	    deflt = 0;
	} else if (i == 2 && strncmp(s, "*)", 2)) {
	    deflt = 0;
	}
    }

    fclose(fp);

    return deflt;
}

/**
 * get_info:
 * @hdrfile: name of data header file
 * @prn: gretl printing struct.
 * 
 * print to @prn the informative comments contained in the given
 * data file (if any).
 * 
 * Returns: 0 on successful completion, non-zero on error or if there
 * are no informative comments.
 * 
 */

int get_info (const char *hdrfile, PRN *prn)
{      
    char s[MAXLEN];
    int i = 0;
    FILE *hdr;

    if ((hdr = gretl_fopen(hdrfile, "r")) == NULL) {
	pprintf(prn, _("Couldn't open %s\n"), hdrfile); 
	return 1;
    }

    /* see if it's just the default "space for comments" */
    if (blank_check(hdr)) { /* yes */
	pprintf(prn, _("No info in %s\n"), hdrfile);
	return 2;
    } 

    /* no, so restart the read */
    if ((hdr = gretl_fopen(hdrfile, "r")) == NULL) {
	pprintf(prn, _("Couldn't open %s\n"), hdrfile); 
	return 1;
    }    

    pprintf(prn, _("Data info in file %s:\n\n"), hdrfile);

    if (fgets(s, MAXLEN-1, hdr) != NULL && !strncmp(s, STARTCOMMENT, 2)) {
	do {
	    if (fgets(s, MAXLEN-1, hdr) != NULL && strncmp(s, "*)", 2)) {
#ifndef WIN32
		delchar('\r', s);
#endif
		pputs(prn, s);
		i++;
	    }
	} while (s != NULL && strncmp(s, ENDCOMMENT, 2));
    }

    if (i == 0) {
	pputs(prn, _(" (none)\n"));
    }

    pputc(prn, '\n');

    if (hdr != NULL) {
	fclose(hdr);
    }

    return 0;
}

static int writehdr (const char *hdrfile, const int *list, 
		     const DATAINFO *pdinfo, int opt)
{
    FILE *fp;
    char startdate[OBSLEN], enddate[OBSLEN];
    int i, binary = 0;

    if (opt == GRETL_DATA_FLOAT) {
	binary = 1;
    } else if (opt == GRETL_DATA_DOUBLE) {
	binary = 2;
    }

    ntodate_full(startdate, pdinfo->t1, pdinfo);
    ntodate_full(enddate, pdinfo->t2, pdinfo);

    fp = gretl_fopen(hdrfile, "w");
    if (fp == NULL) {
	return 1;
    }

    /* write description of data set, if any */
    if (pdinfo->descrip != NULL) {
	size_t len = strlen(pdinfo->descrip);

	if (len > 2) {
	    fprintf(fp, "(*\n%s%s*)\n", pdinfo->descrip,
		    (pdinfo->descrip[len-1] == '\n')? "" : "\n");
	}
    }

    /* then list of variables */
    for (i=1; i<=list[0]; i++) {
	if (list[i] == 0) {
	    continue;
	}
	fprintf(fp, "%s ", pdinfo->varname[list[i]]);
	if (i && i <list[0] && (i+1) % 8 == 0) {
	    fputc('\n', fp);
	}
    }  
  
    fputs(";\n", fp);

    /* then obs line */
    fprintf(fp, "%d %s %s\n", pdinfo->pd, startdate, enddate);
    
    /* and flags as required */
    if (binary == 1) {
	fputs("BYVAR\nSINGLE\n", fp);
    } else if (binary == 2) {
	fputs("BYVAR\nBINARY\n", fp);
    } else { 
	fputs("BYOBS\n", fp);
	if (pdinfo->markers) {
	    fputs("MARKERS\n", fp);
	}
    }

    if (pdinfo->structure == STACKED_TIME_SERIES) {
	fputs("PANEL2\n", fp);
    } else if (pdinfo->structure == STACKED_CROSS_SECTION) {
	fputs("PANEL3\n", fp);
    }
    
    fclose(fp);

    return 0;
}

/**
 * get_precision:
 * @x: data vector.
 * @n: length of x.
 * @placemax: maximum number of decimal places to try.
 *
 * Find the number of decimal places required to represent a given
 * data series uniformly.
 * 
 * Returns: the required number of decimal places.
 */

int get_precision (const double *x, int n, int placemax)
{
    int t, p, pmax = 0;
    char *s, numstr[48];
    int n_ok = 0;
    double z;

    for (t=0; t<n; t++) {
	if (na(x[t])) {
	    continue;
	}

	n_ok++;

	z = fabs(x[t]);

	/* escape clause: numbers are too big or too small for
	   this treatment */
	if (z > 0 && (z < 1.0e-6 || z > 1.0e+8)) {
	    return PMAX_NOT_AVAILABLE;
	}

	p = placemax;
	sprintf(numstr, "%.*f", p, z);
	s = numstr + strlen(numstr) - 1;
	while (*s-- == '0') {
	    p--;
	}
	if (p > pmax) {
	    pmax = p;
	}
    }

    if (n_ok == 0) {
	pmax = PMAX_NOT_AVAILABLE;
    }

    return pmax;
}

static GretlDataFormat 
format_from_opt_or_name (gretlopt opt, const char *fname)
{
    GretlDataFormat fmt = 0;
    
    if (opt & OPT_S) {
	fmt = GRETL_DATA_FLOAT;	
    } else if (opt & OPT_T) {
	fmt = GRETL_DATA_TRAD;
    } else if (opt & OPT_O) {
	fmt = GRETL_DATA_DOUBLE;
    } else if (opt & OPT_M) {
	fmt = GRETL_DATA_OCTAVE;
    } else if (opt & OPT_R) {
	fmt = GRETL_DATA_R;
    } else if (opt & OPT_C) {
	fmt = GRETL_DATA_CSV;
    } else if (opt & OPT_Z) {
	fmt = GRETL_DATA_GZIPPED;
    } else if (opt & OPT_D) {
	fmt = GRETL_DATA_DB;
    } else if (opt & OPT_G) {
	fmt = GRETL_DATA_DAT;
    }

    if (fmt == 0) {
	if (has_suffix(fname, ".R")) {
	    fmt = GRETL_DATA_R;
	} else if (has_suffix(fname, ".csv")) {
	    fmt = GRETL_DATA_CSV;
	} else if (has_suffix(fname, ".m")) {
	    fmt = GRETL_DATA_OCTAVE;
	}
    }

    return fmt;
}

/**
 * write_data:
 * @fname: name of file to write.
 * @list: list of variables to write.
 * @Z: data matrix.
 * @pdinfo: data information struct.
 * @opt: option flag indicating format in which to write the data.
 * @ppaths: pointer to paths information (should be NULL when not
 * called from gui).
 * 
 * Write out a data file containing the values of the given set
 * of variables.
 * 
 * Returns: 0 on successful completion, non-zero on error.
 */

int write_data (const char *fname, const int *list, 
		const double **Z, const DATAINFO *pdinfo, 
		gretlopt opt, PATHS *ppaths)
{
    int i, t, v, l0;
    GretlDataFormat fmt;
    char datfile[MAXLEN], hdrfile[MAXLEN], lblfile[MAXLEN];
    int tsamp = pdinfo->t2 - pdinfo->t1 + 1;
    int n = pdinfo->n;
    FILE *fp = NULL;
    int *pmax = NULL;
    double xx;

    *gretl_errmsg = 0;

    if (list == NULL || list[0] == 0) {
	return 1;
    }

    l0 = list[0];

    fmt = format_from_opt_or_name(opt, fname);

    if (fmt == 0 || fmt == GRETL_DATA_GZIPPED) {
	return write_xmldata(fname, list, Z, pdinfo, fmt, ppaths);
    }

    if (fmt == GRETL_DATA_DB) {
	return write_db_data(fname, list, opt, Z, pdinfo);
    }

    if (fmt == GRETL_DATA_CSV && pdinfo->delim == ',' && 
	',' == pdinfo->decpoint) {
	sprintf(gretl_errmsg, _("You can't use the same character for "
				"the column delimiter and the decimal point"));
	return 1;
    }

    strcpy(datfile, fname);

    if (fmt == GRETL_DATA_R && pdinfo->structure == TIME_SERIES) {
	fmt = GRETL_DATA_R_ALT;
    }

    /* write header and label files if not exporting to other formats */
    if (fmt != GRETL_DATA_R && fmt != GRETL_DATA_R_ALT && 
	fmt != GRETL_DATA_CSV && fmt != GRETL_DATA_OCTAVE &&
	fmt != GRETL_DATA_DAT) {
	if (!has_suffix(datfile, ".gz")) {
	    switch_ext(hdrfile, datfile, "hdr");
	    switch_ext(lblfile, datfile, "lbl");
	} else {
	    gz_switch_ext(hdrfile, datfile, "hdr");
	    gz_switch_ext(lblfile, datfile, "lbl");
	}
	if (writehdr(hdrfile, list, pdinfo, fmt)) {
	    fprintf(stderr, I_("Write of header file failed"));
	    return 1;
	}
	if (writelbl(lblfile, list, pdinfo)) {
	    fprintf(stderr, I_("Write of labels file failed"));
	    return 1;
	}
    }

    /* open file for output */
    if (fmt == GRETL_DATA_FLOAT || fmt == GRETL_DATA_DOUBLE) {
	fp = gretl_fopen(datfile, "wb");
    } else {
	fp = gretl_fopen(datfile, "w");
    }

    if (fp == NULL) return 1;

    if (fmt == GRETL_DATA_FLOAT) { /* single-precision binary */
	float x;

	for (i=1; i<=l0; i++) {
	    v = list[i];
	    x = (float) Z[v][0];
	    for (t=0; t<n; t++) {
		if (pdinfo->vector[v]) {
		    x = (float) Z[v][t];
		}
		fwrite(&x, sizeof x, 1, fp);
	    }
	}
    } else if (fmt == GRETL_DATA_DOUBLE) { /* double-precision binary */
	for (i=1; i<=l0; i++) {
	    v = list[i];
	    if (pdinfo->vector[v]) {
		fwrite(&Z[v][0], sizeof(double), n, fp);
	    } else {
		for (t=0; t<n; t++) {
		    fwrite(&Z[v][0], sizeof(double), 1, fp);
		}
	    }
	}
    }

    if (fmt == GRETL_DATA_CSV || fmt == GRETL_DATA_OCTAVE || 
	GRETL_DATA_R || fmt == GRETL_DATA_TRAD || fmt == GRETL_DATA_DAT) { 
	/* an ASCII variant of some sort */
	pmax = malloc(l0 * sizeof *pmax);
	if (pmax == NULL) {
	    fclose(fp);
	    return 1;
	}
	for (i=1; i<=l0; i++) {
	    v = list[i];
	    if (pdinfo->vector[v]) {
		pmax[i-1] = get_precision(&Z[v][pdinfo->t1], tsamp, 10);
	    } else {
		pmax[i-1] = SCALAR_DIGITS;
	    }
	}	
    }

#ifdef ENABLE_NLS
    if (fmt == GRETL_DATA_CSV && pdinfo->decpoint == ',') ;
    else setlocale(LC_NUMERIC, "C");
#endif

    if (fmt == GRETL_DATA_TRAD) { /* plain ASCII */
	for (t=pdinfo->t1; t<=pdinfo->t2; t++) {
	    if (pdinfo->markers && pdinfo->S != NULL) {
		fprintf(fp, "%s ", pdinfo->S[t]);
	    }
	    for (i=1; i<=l0; i++) {
		v = list[i];
		if (na(Z[v][t])) {
		    fprintf(fp, "-999 ");
		} else if (pmax[i-1] == PMAX_NOT_AVAILABLE) {
		    fprintf(fp, "%.12g ", 
			    (pdinfo->vector[v])? 
			    Z[v][t] : Z[v][0]);
		} else {
		    fprintf(fp, "%.*f ", pmax[i-1], 
			    (pdinfo->vector[v])? 
			    Z[v][t] : Z[v][0]);
		}
	    }
	    fputc('\n', fp);
	}
	fputc('\n', fp);
    } else if (fmt == GRETL_DATA_CSV || fmt == GRETL_DATA_R) { 
	/* export CSV or GNU R (dataframe) */
	char delim;
	
	if (fmt == GRETL_DATA_CSV) delim = pdinfo->delim;
	else delim = ' ';

	/* variable names */
	if (fmt == GRETL_DATA_CSV && 
	    (pdinfo->S != NULL || pdinfo->structure != CROSS_SECTION)) {
	    fprintf(fp, "obs%c", delim);
	}
	for (i=1; i<l0; i++) {
	    fprintf(fp, "%s%c", pdinfo->varname[list[i]], delim);
	}
	fprintf(fp, "%s\n", pdinfo->varname[list[l0]]);
	
	for (t=pdinfo->t1; t<=pdinfo->t2; t++) {
	    if (pdinfo->S != NULL) {
		fprintf(fp, "\"%s\"%c", pdinfo->S[t], delim);
	    } else if (pdinfo->structure != CROSS_SECTION) {
		char tmp[OBSLEN];

		ntodate_full(tmp, t, pdinfo);
		fprintf(fp, "\"%s\"%c", tmp, delim);
	    }
	    for (i=1; i<=l0; i++) { 
		v = list[i];
		xx = (pdinfo->vector[v])? Z[v][t] : Z[v][0];
		if (na(xx)) {
		    fprintf(fp, "NA");
		} else if (pmax[i-1] == PMAX_NOT_AVAILABLE) {
		    fprintf(fp, "%.12g", xx);
		} else {
		    fprintf(fp, "%.*f", pmax[i-1], xx);
		}
		if (i < l0) {
		    fputc(delim, fp);
		} else {
		    fputc('\n', fp);
		}
	    }
	}
	fputc('\n', fp);
    } else if (fmt == GRETL_DATA_R_ALT && pdinfo->structure == TIME_SERIES) {
	/* October, 2003: attempt at improved R time-series structure */
	char *p, datestr[OBSLEN];
	int subper = 1;

	fprintf(fp, "\"%s\" <- ts (t (matrix (data = c(\n", "gretldata");

	for (t=pdinfo->t1; t<=pdinfo->t2; t++) {
	    for (i=1; i<=l0; i++) {
		v = list[i];
		xx = (pdinfo->vector[v])? Z[v][t] : Z[v][0];
		if (na(xx)) {
		    fputs("NA", fp);
		} else {
		    fprintf(fp, "%g", xx);
		}
		if (i == l0) {
		    if (t == pdinfo->t2) {
			fputs("),\n", fp);
		    } else {
			fputs(" ,\n", fp);
		    }
		} else {
		    fputs(" , ", fp);
		}
	    }
	}

	ntodate_full(datestr, pdinfo->t1, pdinfo);
	p = strchr(datestr, ':');
	if (p != NULL) {
	    subper = atoi(p + 1);
	}
	fprintf(fp, "nrow = %d, ncol = %d)), start = c(%d,%d), frequency = %d)\n",
		l0, pdinfo->t2 - pdinfo->t1 + 1, 
		atoi(datestr), subper, pdinfo->pd);
	fprintf(fp, "colnames(%s) <- c(", "gretldata");
	for (i=1; i<=l0; i++) {
	    fprintf(fp, "\"%s\"", pdinfo->varname[list[i]]);
	    if (i < l0) {
		fputs(", ", fp);
	    } else {
		fputs(")\n", fp);
	    }
	}
    } else if (fmt == GRETL_DATA_R_ALT) { 
	/* export GNU R (structure) */
	for (i=1; i<=l0; i++) {
	    v = list[i];
	    fprintf(fp, "\"%s\" <-\n", pdinfo->varname[v]);
	    fprintf(fp, "structure(c(");
	    for (t=pdinfo->t1; t<=pdinfo->t2; t++) {
		xx = (pdinfo->vector[v])? Z[v][t] : Z[v][0];
		if (na(xx)) {
		    fprintf(fp, "NA");
		} else {
		    fprintf(fp, "%g", xx);
		}
		if (t < pdinfo->t2) {
		    fprintf(fp, ", "); 
		}
		if (t > pdinfo->t1 && (t - pdinfo->t1) % 8 == 0 && t < pdinfo->t2) {
		    fputc('\n', fp);
		}
	    }
	    fputc(')', fp);
	    if (pdinfo->structure == TIME_SERIES) { 
		fprintf(fp, ",\n.Tsp = c(%f, %f, %d), class = \"ts\"",
			obs_float(pdinfo, 0), obs_float(pdinfo, 1), 
			pdinfo->pd);
	    }
	    fprintf(fp, ")\n");
	}
    } else if (fmt == GRETL_DATA_OCTAVE) { 
	/* GNU Octave: write out data as a matrix */
	fprintf(fp, "# name: X\n# type: matrix\n# rows: %d\n# columns: %d\n", 
		n, list[0]);
	for (t=pdinfo->t1; t<=pdinfo->t2; t++) {
	    for (i=1; i<=list[0]; i++) {
		v = list[i];
		if (pmax[i-1] == PMAX_NOT_AVAILABLE) {
		    fprintf(fp, "%.12g ", Z[v][t]);
		} else {
		    fprintf(fp, "%.*f ", pmax[i-1], 
			    (pdinfo->vector[v])? 
			    Z[v][t] : Z[v][0]);
		}
	    }
	    fputc('\n', fp);
	}
	fputc('\n', fp);
    } else if (fmt == GRETL_DATA_DAT) { 
	/* PcGive: data file with load info */
	int pd = pdinfo->pd;

	for (i=1; i<=list[0]; i++) {
	    fprintf(fp, ">%s ", pdinfo->varname[list[i]]);
	    if (pdinfo->structure == TIME_SERIES &&
		(pd == 1 || pd == 4 || pd == 12)) {
		double ts = obs_float(pdinfo, 0);
		double te = obs_float(pdinfo, 1);
			   
		fprintf(fp, "%d %d %d %d %d\n",
			(int) (floor(ts)), (int) (ts - floor(ts)) * pd + 1,
			(int) (floor(te)), (int) (te - floor(te)) * pd + 1, pd);
	    } else {
		fprintf(fp, "%d 1 %d 1 1", pdinfo->t1, pdinfo->t2);
	    }
			   
	    fputc('\n', fp);

	    for (t=pdinfo->t1; t<=pdinfo->t2; t++) {
		v = list[i];
		xx = (pdinfo->vector[v])? Z[v][t] : Z[v][0];
		if (na(xx)) {
		    fprintf(fp, "-9999.99");
		} else if (pmax[i-1] == PMAX_NOT_AVAILABLE) {
		    fprintf(fp, "%.12g", xx);;
		} else {
		    fprintf(fp, "%.*f", pmax[i-1], xx);
		}
		fputc('\n', fp);
	    }
	    fputc('\n', fp);
	}
	fputc('\n', fp);
    }

#ifdef ENABLE_NLS
    setlocale(LC_NUMERIC, "");
#endif

    if (pmax != NULL) {
	free(pmax);
    }

    if (fp != NULL) {
	fclose(fp);
    }

    return 0;
}

static void dataset_type_string (char *str, const DATAINFO *pdinfo)
{
    if (dataset_is_time_series(pdinfo)) {
	strcpy(str, _("time series"));
    } else if (dataset_is_panel(pdinfo)) {
        strcpy(str, _("panel"));
	strcat(str, " (");
	strcat(str, (pdinfo->structure == STACKED_TIME_SERIES)? 
	       _("Stacked time series") : _("Stacked cross sections"));
	strcat(str, ")");       
    } else {
        strcpy(str, _("undated"));
    }
}

static void pd_string (char *str, const DATAINFO *pdinfo)
{
    if (custom_time_series(pdinfo)) {
	strcpy(str, _("special"));
    } else {
	switch (pdinfo->pd) {
	case 1:
	    strcpy(str, _("annual")); break;
	case 4:
	    strcpy(str, _("quarterly")); break;
	case 12:
	    strcpy(str, _("monthly")); break;
	case 24:
	    strcpy(str, _("hourly")); break;
	case 52:
	    strcpy(str, _("weekly")); break;
	case 5:
	    strcpy(str, _("daily")); break;
	case 7:
	    strcpy(str, _("daily")); break;
	default:
	    strcpy(str, _("unknown")); break;
	}
    }
}

/**
 * data_report:
 * @pdinfo: data information struct.
 * @ppaths: path information struct.
 * @prn: gretl printing struct.
 * 
 * Write out a summary of the content of the current data set.
 * 
 * Returns: 0 on successful completion, non-zero on error.
 * 
 */

int data_report (const DATAINFO *pdinfo, PATHS *ppaths, PRN *prn)
{
    char startdate[OBSLEN], enddate[OBSLEN], tmp[MAXLEN];
    time_t prntime = time(NULL);
    int i;

    ntodate_full(startdate, 0, pdinfo);
    ntodate_full(enddate, pdinfo->n - 1, pdinfo);

    sprintf(tmp, _("Data file %s\nas of"), 
	    strlen(ppaths->datfile)? ppaths->datfile : _("(unsaved)"));

    pprintf(prn, "%s %s\n\n", tmp, print_time(&prntime));

    if (pdinfo->descrip != NULL && *pdinfo->descrip != '\0') {
	pprintf(prn, "%s:\n\n", _("Description"));
	pputs(prn, pdinfo->descrip);
	pputs(prn, "\n\n");
    }

    dataset_type_string(tmp, pdinfo);
    pprintf(prn, "%s: %s\n", _("Type of data"), tmp);
    
    if (dataset_is_time_series(pdinfo)) {
	pd_string(tmp, pdinfo);
	pprintf(prn, "%s: %s\n", _("Frequency"), tmp);
    }	

    pprintf(prn, "%s: %s - %s (n = %d)\n\n", _("Range"),
	    startdate, enddate, pdinfo->n);

    pprintf(prn, "%s:\n\n", _("Listing of variables"));

    for (i=1; i<pdinfo->v; i++) {
	pprintf(prn, "%*s  %s\n", VNAMELEN, pdinfo->varname[i], 
		VARLABEL(pdinfo, i));
    }

    return 0;
}

/* ................................................. */

static double obs_float (const DATAINFO *pdinfo, int end)
{
    double xx, xx2 = 0.0;
    int i, x1, x2 = 0;

    if (end) {
	xx = obs_str_to_double(pdinfo->endobs);
	if ((i = haschar(':', pdinfo->endobs)) > 0) {
	   x2 = atoi(pdinfo->endobs + i + 1) - 1;
	}
    } else {
	xx = obs_str_to_double(pdinfo->stobs);
	if ((i = haschar(':', pdinfo->stobs)) > 0) {
	   x2 = atoi(pdinfo->stobs + i + 1) - 1;
	}
    }

    x1 = (int) xx;
    if (x2 > 0) {
	xx2 = (double) x2 / pdinfo->pd;
    }
    
    return (double) x1 + xx2;
}

/* ................................................. */

static int readlbl (const char *lblfile, DATAINFO *pdinfo)
     /* read data "labels" from file */
{
    FILE * fp;
    char line[MAXLEN], *label, varname[VNAMELEN];
    int v;
    
    *gretl_errmsg = '\0';

    fp = gretl_fopen(lblfile, "r");
    if (fp == NULL) return 0;

    while (1) {
        if (fgets(line, MAXLEN-1, fp) == NULL) {
            fclose(fp);
            return 0;
        }
        if (sscanf(line, "%s", varname) != 1) {
            fclose(fp);
	    sprintf(gretl_errmsg, _("Bad data label in %s"), lblfile); 
            return 0;
        }
        label = line + strlen(varname);
        if (top_n_tail(label) == E_ALLOC) {
            fclose(fp);
            return E_ALLOC;
        }
	v = varindex(pdinfo, varname);
	if (v < pdinfo->v) {
	    strcpy(VARLABEL(pdinfo, v), label);
	} else {
	    fprintf(stderr, I_("extraneous label for var '%s'\n"), varname);
	}
    }

    if (fp != NULL) fclose(fp);

    return 0;
}

/* ................................................ */

static int writelbl (const char *lblfile, const int *list, 
		     const DATAINFO *pdinfo)
{
    FILE *fp;
    int i, lblcount = 0;

    for (i=1; i<=list[0]; i++) {
	if (list[i] == 0) {
	    continue;
	}
	if (strlen(VARLABEL(pdinfo, list[i])) > 2) {
	    lblcount++;
	    break;
	}
    }

    if (lblcount == 0) return 0;

    fp = gretl_fopen(lblfile, "w");
    if (fp == NULL) return 1;

    /* spit out varnames and labels (if filled out) */
    for (i=1; i<=list[0]; i++) {
	if (list[i] == 0) {
	    continue;
	}
	if (strlen(VARLABEL(pdinfo, list[i])) > 2) {
	    fprintf(fp, "%s %s\n", pdinfo->varname[list[i]],
		    VARLABEL(pdinfo, list[i]));
	}
    }
    
    if (fp != NULL) fclose(fp);

    return 0;
}

/**
 * is_gzipped:
 * @fname: filename to examine.
 * 
 * Determine if the given file is gzipped.
 * 
 * Returns: 1 in case of a gzipped file, 0 if not gzipped or
 * inaccessible.
 * 
 */

int is_gzipped (const char *fname)
{
    FILE *fp;
    int gz = 0;

    if (fname == NULL || *fname == '\0') {
	return 0;
    }

    fp = gretl_fopen(fname, "rb");
    if (fp == NULL) {
	return 0;
    }

    if (fgetc(fp) == 037 && fgetc(fp) == 0213) {
	gz = 1;
    }

    fclose(fp);

    return gz;
}

/**
 * gz_switch_ext:
 * @targ: target or "output" filename (must be pre-allocated).
 * @src: "source or "input" filename.
 * @ext: suffix to add to filename.
 * 
 * Copy @src filename to @targ, without the existing suffix (if any),
 * and adding the supplied extension or suffix.
 * 
 */

void gz_switch_ext (char *targ, char *src, char *ext)
{
    size_t i = dotpos(src), j = slashpos(src), k;

    strcpy(targ, src);
    targ[i] = '\0';

    k = dotpos(targ);
    if (j > 0 && k < strlen(targ) && k > j) {
	i = k;
    }

    targ[i] = '.';
    targ[i + 1] = '\0';
    strcat(targ, ext);
}

/* ................................................ */

static void try_gdt (char *fname)
{
    char *suff;

    if (fname != NULL) {
	suff = strrchr(fname, '.');
	if (suff != NULL && !strcmp(suff, ".dat")) {
	    strcpy(suff, ".gdt");
	} else {
	    strcat(fname, ".gdt");
	}
    }
}

static void data_read_message (const char *fname, DATAINFO *pdinfo, PRN *prn)
{
    pprintf(prn, M_("\nRead datafile %s\n"), fname);
    pprintf(prn, M_("periodicity: %d, maxobs: %d,\n"
		    "observations range: %s-%s\n"), 
	    (custom_time_series(pdinfo))? 1 : pdinfo->pd, 
	    pdinfo->n, pdinfo->stobs, pdinfo->endobs);
    pputc(prn, '\n');
}

/**
 * gretl_get_data:
 * @pZ: pointer to data set.
 * @ppdinfo: pointer to data information struct.
 * @datfile: name of file to try.
 * @ppaths: path information struct.
 * @code: option.
 * @prn: where messages should be written.
 * 
 * Read data from file into gretl's work space, allocating space as
 * required.
 * 
 * Returns: 0 on successful completion, non-zero otherwise.
 *
 */

int gretl_get_data (double ***pZ, DATAINFO **ppdinfo, char *datfile, PATHS *ppaths, 
		    DataOpenCode code, PRN *prn) 
{
    DATAINFO *tmpdinfo = NULL;
    double **tmpZ = NULL;
    FILE *dat = NULL;
    gzFile fz = NULL;
    int err = 0, gzsuff = 0, add_gdt = 0;
    int binary = 0, old_byvar = 0;
    char hdrfile[MAXLEN], lblfile[MAXLEN];

    *gretl_errmsg = '\0';

    /* get filenames organized */
    *hdrfile = '\0';
    gzsuff = has_suffix(datfile, ".gz");

    if (addpath(datfile, ppaths, 0) == NULL) { /* not found yet */
	char tryfile[MAXLEN];
	int found = 0;

	/* try using the .gdt suffix? */
	*tryfile = '\0';
	strncat(tryfile, datfile, MAXLEN-1);
	try_gdt(tryfile); 
	found = (addpath(tryfile, ppaths, 0) != NULL);
	if (found) {
	    add_gdt = 1;
	}

	/* or maybe the file is gzipped but lacks a .gz extension? */
	if (!found && !gzsuff) { 
	    sprintf(tryfile, "%s.gz", datfile);
	    if (addpath(tryfile, ppaths, 0) != NULL) {
		gzsuff = 1;
		found = 1;
	    }
	}

	if (!found) {
	    sprintf(gretl_errmsg, _("Couldn't open file %s"),  datfile);
	    return E_FOPEN;
	} else {
	    strcpy(datfile, tryfile);
	}
    }

    /* catch XML files that have strayed in here? */
    if (add_gdt && xmlfile(datfile)) {
	return get_xmldata(pZ, ppdinfo, datfile, ppaths, 
			   code, prn, 0);
    }

    tmpdinfo = datainfo_new();
    if (tmpdinfo == NULL) {
	return E_ALLOC;
    }
	
    if (!gzsuff) {
	switch_ext(hdrfile, datfile, "hdr");
	switch_ext(lblfile, datfile, "lbl");
    } else {
	gz_switch_ext(hdrfile, datfile, "hdr");
	gz_switch_ext(lblfile, datfile, "lbl");
    }

    /* read data header file */
    err = readhdr(hdrfile, tmpdinfo, &binary, &old_byvar);
    if (err == E_FOPEN) {
	/* no header file, so maybe it's just an ascii datafile */
	return import_csv(pZ, ppdinfo, datfile, prn);
    } else if (err) {
	return err;
    } else { 
	pprintf(prn, I_("\nReading header file %s\n"), hdrfile);
    }

    /* deal with case where first col. of data file contains
       "marker" strings */
    tmpdinfo->S = NULL;
    if (tmpdinfo->markers && dataset_allocate_obs_markers(tmpdinfo)) {
	return E_ALLOC; 
    }
    
    /* allocate dataset */
    if (allocate_Z(&tmpZ, tmpdinfo)) {
	err = E_ALLOC;
	goto bailout;
    }

    /* Invoke data (Z) reading function */
    if (gzsuff) {
	fz = gretl_gzopen(datfile, "rb");
	if (fz == NULL) {
	    err = E_FOPEN;
	    goto bailout;
	}
    } else {
	if (binary) {
	    dat = gretl_fopen(datfile, "rb");
	} else {
	    dat = gretl_fopen(datfile, "r");
	}
	if (dat == NULL) {
	    err = E_FOPEN;
	    goto bailout;
	}
    }

    /* print out basic info from the files read */
    pprintf(prn, I_("periodicity: %d, maxobs: %d,\n"
	   "observations range: %s-%s\n"), tmpdinfo->pd, tmpdinfo->n,
	   tmpdinfo->stobs, tmpdinfo->endobs);

    pputs(prn, I_("\nReading "));
    pputs(prn, (tmpdinfo->structure == TIME_SERIES) ? 
	    I_("time-series") : _("cross-sectional"));
    pputs(prn, I_(" datafile"));
    if (strlen(datfile) > 40) {
	pputc(prn, '\n');
    }
    pprintf(prn, " %s\n\n", datfile);

    if (gzsuff) {
	err = gz_readdata(fz, tmpdinfo, tmpZ, binary); 
	gzclose(fz);
    } else {
	err = readdata(dat, tmpdinfo, tmpZ, binary, old_byvar); 
	fclose(dat);
    }

    if (err) goto bailout;

    /* Set sample range to entire length of dataset by default */
    tmpdinfo->t1 = 0; 
    tmpdinfo->t2 = tmpdinfo->n - 1;

    strcpy(ppaths->datfile, datfile);

    err = readlbl(lblfile, tmpdinfo);
    if (err) goto bailout;

    if (code == DATA_APPEND) {
	err = merge_data(pZ, *ppdinfo, tmpZ, tmpdinfo, prn);
    } else {
	free_Z(*pZ, *ppdinfo);
	if (code == DATA_CLEAR) {
	    clear_datainfo(*ppdinfo, CLEAR_FULL);
	}
	free(*ppdinfo);
	*ppdinfo = tmpdinfo;
	*pZ = tmpZ;
    }

 bailout:

    if (err) {
	free_Z(tmpZ, tmpdinfo);
	clear_datainfo(tmpdinfo, CLEAR_FULL);
	free(tmpdinfo);
    }

    return err;
}

/**
 * open_nulldata:
 * @pZ: pointer to data set.
 * @pdinfo: data information struct.
 * @data_status: indicator for whether a data file is currently open
 * in gretl's work space (1) or not (0).
 * @length: desired length of data series.
 * @prn: gretl printing struct.
 * 
 * Create an empty "dummy" data set, suitable for Monte Carlo simulations.
 * 
 * Returns: 0 on successful completion, non-zero otherwise.
 *
 */

int open_nulldata (double ***pZ, DATAINFO *pdinfo, 
		   int data_status, int length,
		   PRN *prn) 
{
    int t;

    /* clear any existing data info */
    if (data_status) {
	clear_datainfo(pdinfo, CLEAR_FULL);
    }

    /* dummy up the data info */
    pdinfo->n = length;
    pdinfo->v = 2;
    dataset_obs_info_default(pdinfo);

    if (dataset_allocate_varnames(pdinfo)) {
	return E_ALLOC;
    }

    /* allocate dataset */
    if (allocate_Z(pZ, pdinfo)) {
	return E_ALLOC;
    }

    /* add an index var */
    strcpy(pdinfo->varname[1], "index");
    strcpy(VARLABEL(pdinfo, 1), _("index variable"));
    for (t=0; t<pdinfo->n; t++) {
	(*pZ)[1][t] = (double) (t + 1);
    }

    /* print out basic info */
    pprintf(prn, I_("periodicity: %d, maxobs: %d,\n"
	   "observations range: %s-%s\n"), pdinfo->pd, pdinfo->n,
	   pdinfo->stobs, pdinfo->endobs);

    /* Set sample range to entire length of data-set by default */
    pdinfo->t1 = 0; 
    pdinfo->t2 = pdinfo->n - 1;

    return 0;
}

static int check_daily_dates (DATAINFO *pdinfo, int *pd)
{
    int fulln = 0, n, t;
    int oldpd = pdinfo->pd;
    double oldsd0 = pdinfo->sd0;
    long ed1, ed2;
    int nmiss = 0, err = 0;

    *pd = 0;
    
    ed1 = get_epoch_day(pdinfo->S[0]);
    if (ed1 < 0) {
	err = 1;
    }

    pdinfo->pd = guess_daily_pd(pdinfo);
    pdinfo->structure = TIME_SERIES;

#if 0    
    fprintf(stderr, "guessed daily pd = %d\n", pdinfo->pd);
#endif

    if (!err) {
	ed2 = get_epoch_day(pdinfo->S[pdinfo->n - 1]);
	if (ed2 <= ed1) {
	    err = 1;
	} else {
	    pdinfo->sd0 = ed1;
	}
    }

    if (!err) {
	int n1 = calendar_obs_number(pdinfo->S[0], pdinfo);
	int n2 = calendar_obs_number(pdinfo->S[pdinfo->n - 1], pdinfo);

	fulln = n2 - n1 + 1;
	if (pdinfo->n > fulln) {
	    err = 1;
	} else {
	    nmiss = fulln - pdinfo->n;
	    fprintf(stderr, "Observations: %d; days in sample: %d\n", 
		    pdinfo->n, fulln);
	    if (nmiss > 300 * pdinfo->n) {
		fprintf(stderr, "Probably annual data\n");
		*pd = 1;
	    } else if (nmiss > 50 * pdinfo->n) {
		fprintf(stderr, "Probably quarterly data\n");
		*pd = 4;
	    } else if (nmiss > 20 * pdinfo->n) {
		fprintf(stderr, "Probably monthly data\n");
		*pd = 12;
	    } else if (nmiss > 5 * pdinfo->n) {
		fprintf(stderr, "Probably weekly data\n");
		*pd = pdinfo->pd = 52;
	    } else {
		fprintf(stderr, "Missing daily observations: %d\n", nmiss);
	    }
	}
    }

    for (t=0; t<pdinfo->n && !err; t++) {
	n = calendar_obs_number(pdinfo->S[t], pdinfo);
	if (n < t) {
	    fprintf(stderr, "Error: n = %d < t = %d\n", n, t);
	    err = 1;
	} else if (n > fulln - 1) {
	    fprintf(stderr, "Error: n = %d >= fulln = %d\n", n, fulln);
	    err = 1;
	}
    }

    if (err) {
	pdinfo->pd = oldpd;
	pdinfo->sd0 = oldsd0;
	pdinfo->structure = CROSS_SECTION;
    } else {
	strcpy(pdinfo->stobs, pdinfo->S[0]);
	strcpy(pdinfo->endobs, pdinfo->S[pdinfo->n - 1]);
	pdinfo->t2 = pdinfo->n - 1;
	if (nmiss > 0 && *pd == 0) {
	    pdinfo->markers = DAILY_DATE_STRINGS;
	}
    }

#if 0
    fprintf(stderr, "check_daily_dates: pd = %d, err = %d\n", 
	    pdinfo->pd, err);
#endif

    return (err)? -1 : pdinfo->pd;
}

/* ......................................................... */

static int complete_year_labels (DATAINFO *pdinfo)
{
    int t, yr, yrbak = atoi(pdinfo->S[0]);
    int ret = 1;

    for (t=1; t<pdinfo->n; t++) {
	yr = atoi(pdinfo->S[t]);
	if (yr != yrbak + 1) {
	    ret = 0;
	    break;
	}
	yrbak = yr;
    }

    return  ret;
}

enum date_orders {
    YYYYMMDD = 1,
    MMDDYYYY,
    DDMMYYYY
};

int get_date_order (int f0, int fn) 
{
    if (f0 > 31 || fn > 31) {
	/* first field must be year */
	return YYYYMMDD;
    } else if (f0 > 12 || fn > 12) {
	/* first field must be day */
	return DDMMYYYY;
    } else {
	return MMDDYYYY;
    }
}

static int compress_daily (DATAINFO *pdinfo, int pd)
{
    int t, yr, mon, day;

    for (t=0; t<pdinfo->n; t++) {
	sscanf(pdinfo->S[t], "%d/%d/%d", &yr, &mon, &day);
	if (pd == 1) {
	    sprintf(pdinfo->S[t], "%d", yr);
	} else if (pd == 12) {
	    sprintf(pdinfo->S[t], "%d:%02d", yr, mon);
	} else if (pd == 4) {
	    sprintf(pdinfo->S[t], "%d:%d", yr, mon / 3 + (mon % 3 != 0));
	} 
    }

    return 0;
}

static int transform_daily_dates (DATAINFO *pdinfo, int dorder)
{
    int t, yr, mon, day;
    int sret, err = 0;

    for (t=0; t<pdinfo->n && !err; t++) {
	if (dorder == DDMMYYYY) {
	    sret = sscanf(pdinfo->S[t], "%d/%d/%d", &day, &mon, &yr);
	} else {
	    sret = sscanf(pdinfo->S[t], "%d/%d/%d", &mon, &day, &yr);
	}
	if (sret == 3) {
	    sprintf(pdinfo->S[t], "%02d/%02d/%02d", yr, mon, day);
	} else {
	    err = 1;
	}
    }

    return err;
}

static int csv_weekly_data (DATAINFO *pdinfo)
{
    int ret = 1;
    int t, tc;

    for (t=0; t<pdinfo->n; t++) {
	tc = calendar_obs_number(pdinfo->S[t], pdinfo);
	if (tc != t) {
	    ret = 0;
	    break;
	}
    }
    
    return ret;
}

static int csv_daily_date_check (DATAINFO *pdinfo, PRN *prn)
{
    int d1[3], d2[3];
    char *lbl1 = pdinfo->S[0];
    char *lbl2 = pdinfo->S[pdinfo->n - 1];

    if (sscanf(lbl1, "%d/%d/%d", &d1[0], &d1[1], &d1[2]) == 3 &&
	sscanf(lbl2, "%d/%d/%d", &d2[0], &d2[1], &d2[2]) == 3) {
	int yr1, mon1, day1;
	int yr2, mon2, day2;
	int dorder = get_date_order(d1[0], d2[0]);
	int pd, ret = 0;

	if (dorder == YYYYMMDD) {
	    pputs(prn, "Trying date order YYYYMMDD\n");
	    yr1 = d1[0];
	    mon1 = d1[1];
	    day1 = d1[2];
	    yr2 = d2[0];
	    mon2 = d2[1];
	    day2 = d2[2];
	} else if (dorder == DDMMYYYY) {
	    pputs(prn, "Trying date order DDMMYYYY\n");
	    day1 = d1[0];
	    mon1 = d1[1];
	    yr1 = d1[2];
	    day2 = d2[0];
	    mon2 = d2[1];
	    yr2 = d2[2];
	} else {
	    pputs(prn, "Trying date order MMDDYYYY\n");
	    mon1 = d1[0];
	    day1 = d1[1];
	    yr1 = d1[2];
	    mon2 = d2[0];
	    day2 = d2[1];
	    yr2 = d2[2];
	}		
	    
	if (yr2 >= yr1 && 
	    mon1 > 0 && mon1 < 13 &&
	    mon2 > 0 && mon2 < 13 && 
	    day1 > 0 && day1 < 32 &&
	    day2 > 0 && day2 < 32) {
	    /* looks promising for calendar dates */
	    if (dorder != YYYYMMDD) {
		if (transform_daily_dates(pdinfo, dorder)) {
		    return -1;
		}
	    }
	    pprintf(prn, "? %s - %s\n", lbl1, lbl2);
	    ret = check_daily_dates(pdinfo, &pd);
	    if (ret >= 0 && pd > 0) {
		if (pd == 52) {
		    if (csv_weekly_data(pdinfo)) {
			ret = 52;
		    } else {
			ret = -1;
		    }
		} else {
		    compress_daily(pdinfo, pd);
		    ret = csv_time_series_check(pdinfo, prn);
		}
	    } 
	    return ret;
	}
    } 

    return -1;
}

static void make_endobs_string (char *endobs, const char *s)
{
    *endobs = 0;
    strncat(endobs, s, 4);
    strcat(endobs, ":");
    strncat(endobs, s + 5, 2);
}

static int csv_time_series_check (DATAINFO *pdinfo, PRN *prn)
{
    char year[5];
    char *lbl1 = pdinfo->S[0];
    char *lbl2 = pdinfo->S[pdinfo->n - 1];
    int len = strlen(lbl1);
    int try, pd = -1;

    *year = '\0';
    strncat(year, lbl1, 4);
    try = atoi(year);

    if (try > 0 && try < 3000) {
	pprintf(prn, M_("   %s: probably a year... "), year);
    } else {
	pprintf(prn, M_("   %s: out of bounds for a year?\n"), year);
    }

    if (len == 5) {
	pputs(prn, M_("   but I can't make sense of the extra bit\n"));
    } else if (len == 4) {
	pputs(prn, M_("and just a year\n"));
	if (complete_year_labels(pdinfo)) {
	    strcpy(pdinfo->stobs, year);
	    pdinfo->sd0 = atof(pdinfo->stobs);
	    strcpy(pdinfo->endobs, lbl2);
	    pd = pdinfo->pd = 1;
	} else {
	    pputs(prn, M_("   but the dates are not complete and consistent\n"));
	    return pd;
	}
    } else if (lbl1[4] == '.' || 
	       lbl1[4] == ':' || 
	       lbl1[4] == 'Q' || 
	       lbl1[4] == 'P') {
	char subper[3];

	*subper = '\0';
	strncat(subper, lbl1 + 5, 2);
	if (len == 6) {
	    pprintf(prn, M_("quarter %s?\n"), subper);
	    sprintf(pdinfo->stobs, "%s:%s", year, subper);
	    pdinfo->sd0 = obs_str_to_double(pdinfo->stobs);
	    make_endobs_string(pdinfo->endobs, lbl2);
	    pd = pdinfo->pd = 4;
	} else if (len == 7) {
	    pprintf(prn, M_("month %s?\n"), subper);
	    sprintf(pdinfo->stobs, "%s:%s", year, subper);
	    pdinfo->sd0 = obs_str_to_double(pdinfo->stobs);
	    make_endobs_string(pdinfo->endobs, lbl2);
	    pd = pdinfo->pd = 12;
	}
    }

    return pd;
}

/* attempt to parse csv row labels as dates.  Return -1 if this
   doesn't work out, or 0 if the labels seem to be just integer
   observation numbers, else return the inferred data frequency 
*/

static int test_markers_for_dates (DATAINFO *pdinfo, PRN *prn)
{
    char endobs[OBSLEN];
    char *lbl1 = pdinfo->S[0];
    int n1 = strlen(lbl1);

    pprintf(prn, M_("   first row label \"%s\", last label \"%s\"\n"), 
	    lbl1, pdinfo->S[pdinfo->n - 1]);

    /* are the labels (probably) just 1, 2, 3 etc.? */
    sprintf(endobs, "%d", pdinfo->n);
    if (!strcmp(pdinfo->S[0], "1") && !strcmp(pdinfo->S[pdinfo->n - 1], endobs)) {
	return 0;
    }

    /* labels are of different lengths? */
    if (n1 != strlen(pdinfo->S[pdinfo->n - 1])) {
	pputs(prn, M_("   label strings can't be consistent dates\n"));
	return -1;
    }

    pputs(prn, M_("trying to parse row labels as dates...\n"));

    if (n1 == 8 || n1 == 10) {
	/* daily data? */
	return csv_daily_date_check(pdinfo, prn);
    } else if (n1 >= 4) {
	/* annual, quarterly, monthly? */
	if (isdigit((unsigned char) lbl1[0]) &&
	    isdigit((unsigned char) lbl1[1]) &&
	    isdigit((unsigned char) lbl1[2]) && 
	    isdigit((unsigned char) lbl1[3])) {
	    return csv_time_series_check(pdinfo, prn);
	} else {
	    pputs(prn, M_("   definitely not a four-digit year\n"));
	}
    }

    return -1;
}

static int extend_markers (DATAINFO *pdinfo, int old_n, int new_n)
{
    char **S = realloc(pdinfo->S, new_n * sizeof *S);
    int t, err = 0;
	   
    if (S == NULL) {
	err = 1;
    } else {
	pdinfo->S = S;
	for (t=old_n; t<new_n && !err; t++) {
	    S[t] = malloc(OBSLEN);
	    if (S[t] == NULL) {
		err = 1;
	    } 
	}
    }

    return err;
}

static int count_add_vars (const DATAINFO *pdinfo, const DATAINFO *addinfo)
{
    int addvars = addinfo->v - 1;
    int i, j;

    for (i=1; i<addinfo->v; i++) {
	for (j=1; j<pdinfo->v; j++) {
	    if (!strcmp(addinfo->varname[i], pdinfo->varname[j])) {
		if (!pdinfo->vector[j]) {
		    addvars = -1;
		} else {
		    addvars--;
		}
		break;
	    }
	}
	if (addvars < 0) {
	    fprintf(stderr, "%s: can't replace scalar with vector\n",
		    addinfo->varname[i]);
	    break;
	}
    }

    return addvars;
}

static int compare_ranges (const DATAINFO *pdinfo,
			   const DATAINFO *addinfo,
			   int *offset)
{
    int ed0, sd1, ed1;
    int addobs = -1;

    ed0 = dateton(pdinfo->endobs, pdinfo);
    sd1 = merge_dateton(addinfo->stobs, pdinfo);
    ed1 = merge_dateton(addinfo->endobs, pdinfo);

#if 1
    fprintf(stderr, "compare_ranges:\n"
	    " pdinfo->n = %d, addinfo->n = %d\n"
	    " pdinfo->stobs = '%s', addinfo->stobs = '%s'\n" 
	    " sd1 = %d, ed1 = %d\n",
	    pdinfo->n, addinfo->n, pdinfo->stobs, addinfo->stobs,
	    sd1, ed1);
#endif

    if (sd1 < 0) {
	fprintf(stderr, "addinfo->stobs: '%s', can't figure\n", 
		addinfo->stobs);
	addobs = -1;
    } else if (sd1 == 0 && ed1 == ed0) {
	/* case: exact match of ranges */
	*offset = 0;
	addobs = 0;
    } else if (sd1 == 0) {
	/* case: starting obs the same */
	*offset = 0;
	if (ed1 > ed0) {
	    addobs = ed1 - ed0;
	}
    } else if (sd1 == ed0 + 1) {
	/* case: new data start right after end of old */
	*offset = sd1;
	addobs = addinfo->n;
    } else if (sd1 > 0) {
	/* case: new data start later than old */
	if (sd1 <= ed0) {
	    /* but there's some overlap */
	    *offset = sd1;
	    if (ed1 > ed0) {
		addobs = ed1 - ed0;
	    } else {
		addobs = 0;
	    }
	}
    }

    if (sd1 < 0) {
	fputs("compare_ranges: returning error\n", stderr);
    }

    return addobs;
}

static void merge_error (char *msg, PRN *prn)
{
    pputs(prn, msg);
    strcpy(gretl_errmsg, msg);
}

/**
 * merge_data:
 * @pZ: pointer to data set.
 * @pdinfo: data information struct.
 * @addZ: new data set to be merged in.
 * @addinfo: data information associated with @addZ.
 * @prn: print struct to accept messages.
 * 
 * Attempt to merge the content of a newly opened data file into
 * gretl's current working data set.
 * 
 * Returns: 0 on successful completion, non-zero otherwise.
 *
 */

int merge_data (double ***pZ, DATAINFO *pdinfo,
		double **addZ, DATAINFO *addinfo,
		PRN *prn)
{
    int addvars = 0;
    int addobs = 0;
    int offset = 0;
    int err = 0;

    /* first check for conformability */

    if (pdinfo->pd != addinfo->pd) {
	merge_error(_("Data frequency does not match\n"), prn);
	err = 1;
    }

    if (!err) {
	addobs = compare_ranges(pdinfo, addinfo, &offset);
	addvars = count_add_vars(pdinfo, addinfo);
    }

    if (!err && (addobs < 0 || addvars < 0)) {
	merge_error(_("New data not conformable for appending\n"), prn);
	err = 1;
    }

    if (!err && pdinfo->markers != addinfo->markers) {
	if (addinfo->n != pdinfo->n) {
	    merge_error(_("Inconsistency in observation markers\n"), prn);
	    err = 1;
	} else if (addinfo->markers && !pdinfo->markers) {
	    dataset_destroy_obs_markers(addinfo);
	}
    }
	
    /* if checks are passed, try merging the data */

    if (!err && addobs > 0) { 
	int i, t, new_n = pdinfo->n + addobs;

	if (pdinfo->markers) {
	    err = extend_markers(pdinfo, pdinfo->n, new_n);
	    if (!err) {
		for (t=pdinfo->n; t<new_n; t++) {
		    strcpy(pdinfo->S[t], addinfo->S[t - offset]);
		}
	    }
	}

	for (i=0; i<pdinfo->v && !err; i++) {
	    double *x;

	    if (!pdinfo->vector[i]) {
		continue;
	    }

	   x = realloc((*pZ)[i], new_n * sizeof *x);
	   if (x == NULL) {
	       err = 1;
	       break;
	   }

	   for (t=pdinfo->n; t<new_n; t++) {
	       if (i == 0) {
		   x[t] = 1.0;
	       } else {
		   x[t] = NADBL;
	       }
	   }
	   (*pZ)[i] = x;
       }

       if (err) { 
	   merge_error(_("Out of memory adding data\n"), prn);
       } else {
	   pdinfo->n = new_n;
	   ntodate_full(pdinfo->endobs, new_n - 1, pdinfo);
	   pdinfo->t2 = pdinfo->n - 1;
       }
   }

   if (!err) { 
       int k = pdinfo->v;
       int i, t;

       if (addvars > 0 && dataset_add_series(addvars, pZ, pdinfo)) {
	   merge_error(_("Out of memory adding data\n"), prn);
	   err = 1;
       }

       for (i=1; i<addinfo->v && !err; i++) {
	   int v = varindex(pdinfo, addinfo->varname[i]);
	   int newvar = 0;

	   if (v >= k) {
	       /* a  new variable */
	       v = k++;
	       newvar = 1;
	       strcpy(pdinfo->varname[v], addinfo->varname[i]);
	   } 

	   for (t=0; t<pdinfo->n; t++) {
	       if (t >= offset && t - offset < addinfo->n) {
		   (*pZ)[v][t] = addZ[i][t - offset];
	       } else if (newvar) {
		   (*pZ)[v][t] = NADBL;
	       }
	   }
       }
   }

   if (!err && (addvars || addobs)) {
       pputs(prn, _("Data appended OK\n"));
   }

   free_Z(addZ, addinfo);
   clear_datainfo(addinfo, CLEAR_FULL);

   return err;
}

/* The function below checks for the maximum line length in the given
   file.  It also checks for extraneous binary data (the file is 
   supposed to be plain text), and checks whether the 'delim'
   character is present in the file, on a non-comment line (where
   a comment line is one that starts with '#').  Finally, checks
   whether the file has a trailing comma on every line.
*/

static int get_max_line_length (FILE *fp, char delim, int *gotdelim, 
				int *gottab, int *trail, PRN *prn)
{
    int c, cbak = 0, cc = 0;
    int comment = 0, maxlen = 0;

    if (trail != NULL) {
	*trail = 1;
    }

    while ((c = fgetc(fp)) != EOF) {
	if (c == '\n') {
	    if (cc > maxlen) {
		maxlen = cc;
	    }
	    cc = 0;
	    if (trail != NULL && cbak != 0 && cbak != ',') {
		*trail = 0;
	    }
	    continue;
	}
	cbak = c;
	if (!isspace((unsigned char) c) && !isprint((unsigned char) c) &&
	    !(c == CTRLZ)) {
	    pprintf(prn, M_("Binary data (%d) encountered: this is not a valid "
			   "text file\n"), c);
	    return -1;
	}
	if (cc == 0) {
	    comment = (c == '#');
	}
	if (!comment) {
	    if (gottab != NULL && *gottab == 0 && c == '\t') {
		*gottab = 1;
	    }
	    if (gotdelim != NULL && *gotdelim == 0 && c == delim) {
		*gotdelim = 1;
	    }
	}
	cc++;
    }

    if (maxlen == 0) {
	pprintf(prn, M_("Data file is empty\n"));
    } else if (trail != NULL && *trail) {
	pprintf(prn, M_("Data file has trailing commas\n"));
    }

    if (maxlen > 0) {
	/* allow for newline and null terminator */
	maxlen += 2;
    }

    return maxlen;
}

static int count_csv_fields (const char *line, char delim)
{
    int cbak, nf = 0;
    const char *p = line;

    if (*p == delim && *p == ' ') p++;

    while (*p) {
	if (*p == delim) nf++;
	cbak = *p;
	p++;
	/* Problem: (when) should trailing delimiter be read as implicit "NA"? */
	if (*p == '\0' && cbak == delim && cbak != ',') {
	    nf--;
	}
    }

    return nf + 1;
}

static void remove_quoted_commas (char *s)
{
    int inquote = 0;

    while (*s) {
	if (*s == '"') {
	    inquote = !inquote;
	}
	if (inquote && *s == ',') {
	    *s = ' ';
	}
	s++;
    }
}

static void compress_csv_line (char *line, char delim, int trail)
{
    int n = strlen(line);
    char *p = line + n - 1;

    if (*p == '\n') {
	*p = '\0';
	p--;
    }

    if (*p == '\r') *p = '\0';

    if (delim == ',') {
	remove_quoted_commas(line);
    }

    if (delim != ' ') {
	delchar(' ', line);
    } else {
	compress_spaces(line);
    }

    delchar('"', line);

    if (trail) {
	/* chop trailing comma */
	n = strlen(line);
	if (n > 0) {
	    line[n-1] = '\0';
	}
    }
}

static void check_first_field (const char *line, char delim, 
			       int *blank_1, int *obs_1, 
			       PRN *prn)
{
    *blank_1 = 0;
    *obs_1 = 0;
    
    if (delim != ' ' && *line == delim) {
	*blank_1 = 1;
    } else {
	char field1[16];
	int i = 0;

	if (delim == ' ' && *line == ' ') line++;

	while (*line && i < 15) {
	    if (*line == delim) break;
	    field1[i++] = *line++;
	}
	field1[i] = '\0';
	iso_to_ascii(field1);

	pprintf(prn, M_("   first field: '%s'\n"), field1);
	lower(field1);

	if (!strcmp(field1, "obs") || !strcmp(field1, "date") ||
	    !strcmp(field1, "year")) {
	    pputs(prn, M_("   seems to be observation label\n"));
	    *obs_1 = 1;
	}
    }
}

#define ISNA(s) (!strcmp(s, "NA") || \
                 !strcmp(s, "N.A.") || \
                 !strcmp(s, "n.a.") || \
                 !strcmp(s, "na") || \
                 !strcmp(s, ".") || \
                 !strcmp(s, "..") || \
                 !strncmp(s, "-999", 4))

static int csv_missval (const char *str, int i, int t, PRN *prn)
{
    int miss = 0;

    if (*str == '\0') {
	if (t < 100) {
	    pprintf(prn, M_("   the cell for variable %d, obs %d "
			    "is empty: treating as missing value\n"), 
		    i, t);
	}
	miss = 1;
    }

    if (ISNA(str)) {
	if (t < 100) {
	    pprintf(prn, M_("   warning: missing value for variable "
			    "%d, obs %d\n"), i, t);
	}
	miss = 1;
    }

    return miss;
}

static int add_obs_marker (DATAINFO *pdinfo, int n)
{
    char **S;

    S = realloc(pdinfo->S, n * sizeof *S);
    if (S == NULL) {
	return 1;
    }

    pdinfo->S = S;

    pdinfo->S[n-1] = malloc(OBSLEN);
    if (pdinfo->S[n-1] == NULL) {
	return 1;
    }

    strcpy(pdinfo->S[n-1], "NA");

    return 0;
}

static int dataset_add_obs (double ***pZ, DATAINFO *pdinfo)
{
    int i;
    int err = 0;

    for (i=0; i<pdinfo->v; i++) {
	if (pdinfo->vector[i]) {
	    double *tmp = realloc((*pZ)[i], (pdinfo->n + 1) * sizeof ***pZ);

	    if (tmp != NULL) {
		(*pZ)[i] = tmp;
	    } else {
		return 1;
	    }
	}
    }

    pdinfo->n += 1;

    (*pZ)[0][pdinfo->n - 1] = 1.0;

    for (i=1; i<pdinfo->v; i++) {
	if (pdinfo->vector[i]) {
	    (*pZ)[i][pdinfo->n - 1] = NADBL;
	}
    }

    if (pdinfo->S != NULL) {
	err = add_obs_marker(pdinfo, pdinfo->n);
    }

    return err;
}

static int blank_so_far (double *x, int obs)
{
    int t;

    for (t=0; t<obs; t++) {
	if (!na(x[t])) return 0;
    }

    return 1;
}

static int process_csv_obs (const char *str, int i, int t, 
			    double **Z, gretl_string_table **pst,
			    PRN *prn)
{
    int err = 0;

    if (csv_missval(str, i, t+1, prn)) {
	Z[i][t] = NADBL;
    } else {
	if (check_atof(str)) {
	    int ix = 0;
	    int addcol = 0;

	    if (t == 0 && *pst == NULL) {
		*pst = gretl_string_table_new();
	    }
	    if (blank_so_far(Z[i], t)) {
		addcol = 1;
	    }
	    if (*pst != NULL) {
		ix = gretl_string_table_index(*pst, str, i, addcol, prn);
	    }
	    if (ix >= 0) {
		Z[i][t] = (double) ix;
	    } else {
		pprintf(prn, M_("At variable %d, observation %d:\n"), i, t+1);
		pprintf(prn, " %s\n", gretl_errmsg);
		*gretl_errmsg = '\0';
		err = 1;
	    }
	} else {
	    Z[i][t] = atof(str);
	}
    }

    return err;
}

/* pick up any comments following the data block in a CSV file */

static char *get_csv_descrip (FILE *fp)
{
    char line[MAXLEN];
    char *desc = NULL;

    while (fgets(line, MAXLEN, fp)) {
	tailstrip(line);
	if (desc == NULL) {
	    desc = malloc(strlen(line) + 2);
	    if (desc == NULL) {
		return NULL;
	    }
	    sprintf(desc, "%s\n", line);
	} else {
	    char *tmp;

	    tmp = realloc(desc, strlen(desc) + strlen(line) + 2);
	    if (tmp == NULL) {
		free(desc);
		return NULL;
	    }
	    desc = tmp;
	    strcat(desc, line);
	    strcat(desc, "\n");
	}
    }

    if (string_is_blank(desc)) {
	free(desc);
	desc = NULL;
    }

    return desc;
}

static int 
csv_reconfigure_for_markers (double ***pZ, DATAINFO *pdinfo)
{
    if (dataset_allocate_obs_markers(pdinfo)) {
	return 1;
    }

    return dataset_drop_last_variables(1, pZ, pdinfo);
}

#define starts_number(c) (isdigit((unsigned char) c) || c == '-' || \
                          c == '+' || c == '.')

#define obs_labels_no_varnames(o,c,n)  (!o && c->v > 3 && n == c->v - 2)

/**
 * import_csv:
 * @pZ: pointer to data set.
 * @ppdinfo: pointer to data information struct.
 * @fname: name of CSV file.
 * @prn: gretl printing struct (can be NULL).
 * 
 * Open a Comma-Separated Values data file and read the data into
 * the current work space.
 * 
 * Returns: 0 on successful completion, non-zero otherwise.
 *
 */

int import_csv (double ***pZ, DATAINFO **ppdinfo, 
		const char *fname, PRN *prn)
{
    int ncols, chkcols, nrows;
    int gotdata = 0, gotdelim = 0, gottab = 0, markertest = -1;
    int blank_1 = 0, obs_1 = 0;
    int i, k, t, trail, maxlen;
    char csvstr[CSVSTRLEN];
    FILE *fp = NULL;
    DATAINFO *csvinfo = NULL;
    double **csvZ = NULL;
    char *line = NULL, *p = NULL, *descrip = NULL;

    const char *msg = M_("\nPlease note:\n"
	"- The first row of the CSV file should contain the "
	"names of the variables.\n"
	"- The first column may optionally contain date "
	"strings or other 'markers':\n  in that case its row 1 entry "
	"should be blank, or should say 'obs' or 'date'.\n"
	"- The remainder of the file must be a rectangular "
	"array of data.\n");

    char delim = '\t';
    int numcount, auto_name_vars = 0;
    gretl_string_table *st = NULL;

    if (*ppdinfo != NULL) {
	delim = (*ppdinfo)->delim;
    }

    if (prn != NULL) {
	check_for_console(prn);
    }

    fp = gretl_fopen(fname, "r");
    if (fp == NULL) {
	pprintf(prn, M_("Couldn't open %s\n"), fname);
	goto csv_bailout;
    }

    csvinfo = datainfo_new();
    if (csvinfo == NULL) {
	fclose(fp);
	pputs(prn, M_("Out of memory\n"));
	goto csv_bailout;
    }

    csvinfo->delim = delim;

    pprintf(prn, "%s %s...\n", M_("parsing"), fname);

    /* get line length, also check for binary data, etc. */
    maxlen = get_max_line_length(fp, delim, &gotdelim, &gottab, &trail, prn);    
    if (maxlen <= 0) {
	goto csv_bailout;
    } 

    if (!gotdelim) {
	/* set default delimiter */
	if (gottab) {
	    delim = csvinfo->delim = '\t';
	} else {
	    delim = csvinfo->delim = ' ';
	}
    }

    pprintf(prn, M_("using delimiter '%c'\n"), delim);
    pprintf(prn, M_("   longest line: %d characters\n"), maxlen - 1);

    if (trail && delim != ',') trail = 0;

    /* create buffer to hold lines */
    line = malloc(maxlen);
    if (line == NULL) {
	pputs(prn, M_("Out of memory\n"));
	goto csv_bailout;
    }  
    
    rewind(fp);
    
    /* read lines, check for consistency in number of fields */
    chkcols = ncols = nrows = gotdata = 0;
    while (fgets(line, maxlen, fp)) {
	/* skip comment lines */
	if (*line == '#') {
	    continue;
	}
	/* skip blank lines -- but finish if the blank comes after data */
	if (string_is_blank(line)) {
	    if (gotdata) {
		if (*pZ == NULL) {
		    descrip = get_csv_descrip(fp);
		}
		break;
	    } else {
		continue;
	    }
	}
	nrows++;
	compress_csv_line(line, delim, trail);
	if (!gotdata) {
	    /* scrutinize first "real" line */
	    check_first_field(line, delim, &blank_1, &obs_1, prn);
	    gotdata = 1;
	} 
	chkcols = count_csv_fields(line, delim);
	if (ncols == 0) {
	    ncols = chkcols;
	    pprintf(prn, M_("   number of columns = %d\n"), ncols);	    
	} else {
	    if (chkcols != ncols) {
		pprintf(prn, M_("   ...but row %d has %d fields: aborting\n"),
			nrows, chkcols);
		pputs(prn, msg);
		goto csv_bailout;
	    }
	}
    }

    /* allow for var headings */
    csvinfo->n = nrows - 1;

    csvinfo->v = (blank_1 || obs_1)? ncols : ncols + 1;
    pprintf(prn, M_("   number of variables: %d\n"), csvinfo->v - 1);
    pprintf(prn, M_("   number of non-blank lines: %d\n"), nrows);

    /* end initial checking */

    if (csvinfo->n == 0) {
	pputs(prn, M_("Invalid data file\n"));
	goto csv_bailout;
    }

    /* initialize datainfo and Z */
    if (start_new_Z(&csvZ, csvinfo, 0)) {
	pputs(prn, M_("Out of memory\n"));
	goto csv_bailout;
    }

    if (blank_1 || obs_1) {
	if (dataset_allocate_obs_markers(csvinfo)) {
	    pputs(prn, M_("Out of memory\n"));
	    goto csv_bailout;
	}
    }

    /* second pass */

    rewind(fp);

    /* parse the line containing variable names */
    pputs(prn, M_("scanning for variable names...\n"));

    while (fgets(line, maxlen, fp)) {
	if (*line == '#' || string_is_blank(line)) {
	    continue;
	} else {
	    break;
	}
    }

    compress_csv_line(line, delim, trail);   

    p = line;
    if (delim == ' ' && *p == ' ') p++;
    iso_to_ascii(p);
    pprintf(prn, M_("   line: %s\n"), p);
    
    numcount = 0;
    for (k=0; k<ncols; k++) {
	int nv = 0;

	i = 0;
	while (*p && *p != delim) {
	    if (i < CSVSTRLEN - 1) {
		csvstr[i++] = *p;
	    }
	    p++;
	}
	if (*p == delim) p++;

	csvstr[i] = 0;

	if (k == 0 && (blank_1 || obs_1)) {
	    ;
	} else {
	    nv = (blank_1 || obs_1)? k : k + 1;

	    if (*csvstr == '\0') {
		pprintf(prn, M_("   variable name %d is missing: aborting\n"), nv);
		pputs(prn, msg);
		goto csv_bailout;
	    } else {
		csvinfo->varname[nv][0] = 0;
		/* was VNAMELEN below */
		strncat(csvinfo->varname[nv], csvstr, USER_VLEN - 1);
		if (starts_number(*csvstr)) {
		    numcount++;
		} else {
		    iso_to_ascii(csvinfo->varname[nv]);
		    if (check_varname(csvinfo->varname[nv])) {
			pprintf(prn, "%s\n", gretl_errmsg);
			*gretl_errmsg = '\0';
			goto csv_bailout;
		    }
		}
	    }
	}
	if (nv == csvinfo->v - 1) break;
    }

    if (numcount == csvinfo->v - 1 || 
	obs_labels_no_varnames(obs_1, csvinfo, numcount)) {
	pputs(prn, M_("it seems there are no variable names\n"));
	/* then we undercounted the observations by one */
	if (dataset_add_obs(&csvZ, csvinfo)) {
	    pputs(prn, _("Out of memory\n"));
	    goto csv_bailout;
	}
	auto_name_vars = 1;
	rewind(fp);
	if (obs_labels_no_varnames(obs_1, csvinfo, numcount)) {
	    if (csv_reconfigure_for_markers(&csvZ, csvinfo)) {
		pputs(prn, _("Out of memory\n"));
		goto csv_bailout;
	    } else {
		obs_1 = 1;
	    }
	}
    } else if (numcount > 0) {
	for (i=1; i<csvinfo->v; i++) {
	    if (check_varname(csvinfo->varname[i])) {
		pprintf(prn, "%s\n", gretl_errmsg);
		*gretl_errmsg = '\0';
		break;
	    }
	}	    
	goto csv_bailout;
    }
    
#ifdef ENABLE_NLS
    if (*ppdinfo != NULL && (*ppdinfo)->decpoint != ',') {
	setlocale(LC_NUMERIC, "C");
    }
#endif

    pputs(prn, M_("scanning for row labels and data...\n"));

    t = 0;
    while (fgets(line, maxlen, fp)) {
	int nv;

	if (*line == '#' || string_is_blank(line)) {
	    continue;
	}

	compress_csv_line(line, delim, trail);
	p = line;
	if (delim == ' ' && *p == ' ') p++;

	for (k=0; k<ncols; k++) {
	    i = 0;
	    while (*p && *p != delim) {
		if (i < CSVSTRLEN - 1) {
		    csvstr[i++] = *p;
		} else {
		    pprintf(prn, M_("warning: truncating data at row %d, column %d\n"),
			    t+1, k+1);
		}
		p++;
	    }
	    if (*p == delim) {
		p++;
	    }
	    csvstr[i] = 0;
	    if (k == 0 && (blank_1 || obs_1) && csvinfo->S != NULL) {
		csvinfo->S[t][0] = 0;
		strncat(csvinfo->S[t], csvstr, OBSLEN - 1);
		iso_to_ascii(csvinfo->S[t]);
	    } else {
		nv = (blank_1 || obs_1)? k : k + 1;
		if (process_csv_obs(csvstr, nv, t, csvZ, &st, prn)) {
		    goto csv_bailout;
		}
	    }
	}
	if (++t == csvinfo->n) {
	    break;
	}
    }

#ifdef ENABLE_NLS
    setlocale(LC_NUMERIC, "");
#endif

    if (st != NULL) {
	gretl_string_table_print(st, csvinfo, fname, prn);
    }

    csvinfo->t1 = 0;
    csvinfo->t2 = csvinfo->n - 1;

    if (blank_1 || obs_1) {
	markertest = test_markers_for_dates(csvinfo, prn);
    }
    if (markertest > 0) {
	pputs(prn, M_("taking date information from row labels\n\n"));
    } else {
	pputs(prn, M_("treating these as undated data\n\n"));
	dataset_obs_info_default(csvinfo);
    }

    if (csvinfo->pd != 1 || strcmp(csvinfo->stobs, "1")) { 
        csvinfo->structure = TIME_SERIES;
    }

    /* If there were observation labels and they were not interpretable
       as dates, and they weren't simply "1, 2, 3, ...", then they 
       should probably be preserved; otherwise discard them. 
    */
    if (csvinfo->S != NULL && markertest >= 0 && 
	csvinfo->markers != DAILY_DATE_STRINGS) {
	csvinfo->markers = NO_MARKERS;
	for (i=0; i<csvinfo->n; i++) {
	    free(csvinfo->S[i]);
	}
	free(csvinfo->S);
	csvinfo->S = NULL;
    }

    if (auto_name_vars) {
	/* no variable names were found */
	for (i=1; i<csvinfo->v; i++) {
	    sprintf(csvinfo->varname[i], "v%d", i);
	}
    } else if (fix_varname_duplicates(csvinfo)) {
	pputs(prn, M_("warning: some variable names were duplicated\n"));
    }

    if (*pZ == NULL) {
	/* no dataset currently in place */
	*pZ = csvZ;
	if (*ppdinfo != NULL) {
	    free(*ppdinfo);
	}
	if (descrip != NULL) {
	    csvinfo->descrip = descrip;
	    descrip = NULL;
	}
	*ppdinfo = csvinfo;
    } else if (merge_data(pZ, *ppdinfo, csvZ, csvinfo, prn)) {
	goto csv_bailout;
    }

    fclose(fp); 
    free(line);

    console_off();

    return 0;

 csv_bailout:

    if (fp != NULL) {
	fclose(fp);
    }
    if (line != NULL) {
	free(line);
    }
    if (descrip != NULL) {
	free(descrip);
    }
    if (csvinfo != NULL) {
	clear_datainfo(csvinfo, CLEAR_FULL);
    }
    if (st != NULL) {
	gretl_string_table_destroy(st);
    }

    console_off();

    return 1;
}

/**
 * add_obs_markers_from_file:
 * @pdinfo: data information struct.
 * @fname: name of file containing case markers.
 * 
 * Read case markers (strings of %OBSLEN - 1 characters or less that identify
 * the observations) from a file, and associate tham with the 
 * current data set.
 * 
 * Returns: 0 on successful completion, non-zero otherwise.
 */

int add_obs_markers_from_file (DATAINFO *pdinfo, const char *fname)
{
    char **Sbak = NULL;
    FILE *fp;
    char marker[OBSLEN], sformat[8];
    int t, err = 0;

    fp = gretl_fopen(fname, "r");
    if (fp == NULL) {
	return E_FOPEN;
    }

    if (pdinfo->S != NULL) {
	/* keep backup copy */
	Sbak = pdinfo->S;
	pdinfo->S = NULL;
    }

    if (dataset_allocate_obs_markers(pdinfo)) {
	err = E_ALLOC;
	goto bailout;
    }
    
    sprintf(sformat, "%%%ds", OBSLEN - 1);

    for (t=0; t<pdinfo->n; t++) {
	eatspace(fp);
	if (fscanf(fp, sformat, marker)) {
	    strcat(pdinfo->S[t], marker);
	} else {
	    err = E_DATA;
	    goto bailout;
	}
    }

 bailout:

    fclose(fp);

    if (Sbak != NULL) {
	if (err) {
	    /* restore old markers */
	    pdinfo->S = Sbak;
	} else {
	    /* destroy them */
	    free_strings_array(Sbak, pdinfo->n);
	}
    }

    return err;
}

static void 
octave_varname (char *name, const char *s, int nnum, int v)
{
    char nstr[8];
    int len, tr;

    if (nnum == 0) {
	strcpy(name, s);
    } else {
	sprintf(nstr, "%d", nnum);
	len = strlen(nstr);
	tr = VNAMELEN - len;

	if (tr > 0) {
	    strncat(name, s, tr);
	    strcat(name, nstr);
	} else {
	    sprintf(name, "v%d", v);
	}
    }
}

/**
 * import_octave:
 * @pZ: pointer to data set.
 * @ppdinfo: pointer to data information struct.
 * @fname: name of GNU octave ascii data file.
 * @prn: gretl printing struct (can be NULL).
 * 
 * Open a GNU octave ascii data file (matrix type) and read the data into
 * the current work space.
 * 
 * Returns: 0 on successful completion, non-zero otherwise.
 *
 */

int import_octave (double ***pZ, DATAINFO **ppdinfo, 
		   const char *fname, PRN *prn)
{
    DATAINFO *octinfo = NULL;
    double **octZ = NULL;
    
    FILE *fp = NULL;
    char *line = NULL;

    char tmp[8], name[32];
    int nrows = 0, ncols = 0, nblocks = 0;
    int brows = 0, bcols = 0, oldbcols = 0;
    int maxlen, got_type = 0, got_name = 0;
    int err = 0;

    int i, t;

    if (prn != NULL) {
	check_for_console(prn);
    }

    fp = gretl_fopen(fname, "r");
    if (fp == NULL) {
	goto oct_bailout;
    }   

    pprintf(prn, "%s %s...\n", M_("parsing"), fname);

    maxlen = get_max_line_length(fp, 0, NULL, NULL, NULL, prn);
    if (maxlen <= 0) {
	goto oct_bailout;
    }
 
    line = malloc(maxlen);
    if (line == NULL) {
	goto oct_bailout;
    }

    pprintf(prn, M_("   longest line: %d characters\n"), maxlen - 1);

    rewind(fp);

    while (fgets(line, maxlen, fp) && !err) {
	if (*line == '#') {
	    if (!got_name) {
		if (sscanf(line, "# name: %31s", name) == 1) {
		    got_name = 1;
		    nblocks++;
		    continue;
		}
	    }
	    if (!got_type) {
		if (sscanf(line, "# type: %7s", tmp) == 1) {
		    if (!got_name || strcmp(tmp, "matrix")) {
			err = 1;
		    } else {
			got_type = 1;
		    }
		    continue;
		}
	    }
	    if (brows == 0) {
		if (sscanf(line, "# rows: %d", &brows) == 1) {
		    if (!got_name || !got_type || brows <= 0) {
			err = 1;
		    } else if (nrows > 0 && brows != nrows) {
			err = 1;
		    } else {
			nrows = brows;
		    }
		    continue;
		}	    
	    } 
	    if (bcols == 0) {
		if (sscanf(line, "# columns: %d", &bcols) == 1) {
		    if (!got_name || !got_type || bcols <= 0) {
			err = 1;
		    } else {
			ncols += bcols;
			pprintf(prn, M_("   Found matrix '%s' with "
					"%d rows, %d columns\n"), name, brows, bcols);
		    }
		    continue;
		}
	    }
	} else if (string_is_blank(line)) {
	    continue;
	} else {
	    got_name = 0;
	    got_type = 0;
	    brows = 0;
	    bcols = 0;
	}
    }

    if (err || nrows == 0 || ncols == 0) {
	pputs(prn, M_("Invalid data file\n"));
	goto oct_bailout;
    } 

    /* initialize datainfo and Z */

    octinfo = datainfo_new();
    if (octinfo == NULL) {
	fclose(fp);
	pputs(prn, M_("Out of memory\n"));
	goto oct_bailout;
    }

    octinfo->n = nrows;
    octinfo->v = ncols + 1;

    if (start_new_Z(&octZ, octinfo, 0)) {
	pputs(prn, M_("Out of memory\n"));
	goto oct_bailout;
    }  

    rewind(fp);

    pprintf(prn, M_("   number of variables: %d\n"), ncols);
    pprintf(prn, M_("   number of observations: %d\n"), nrows);
    pprintf(prn, M_("   number of data blocks: %d\n"), nblocks); 

    i = 1;
    t = 0;

    while (fgets(line, maxlen, fp) && !err) {
	char *s = line;
	int j;

	if (*s == '#') {
	    if (sscanf(line, "# name: %8s", name) == 1) {
		;
	    } else if (sscanf(line, "# rows: %d", &brows) == 1) {
		t = 0;
	    } else if (sscanf(line, "# columns: %d", &bcols) == 1) {
		i += oldbcols;
		oldbcols = bcols;
	    }
	} 

	if (*s == '#' || string_is_blank(s)) {
	    continue;
	}

	if (t >= octinfo->n) {
	    err = 1;
	}

	for (j=0; j<bcols && !err; j++) {
	    double x;
	    int v = i + j;

	    if (t == 0) {
		int nnum = (bcols > 1)? j + 1 : 0;

		octave_varname(octinfo->varname[i+j], name, nnum, v);
	    }

	    while (isspace(*s)) s++;
	    if (sscanf(s, "%lf", &x) != 1) {
		fprintf(stderr, "error: '%s', didn't get double\n", s);
		err = 1;
	    } else {
		octZ[v][t] = x;
		while (!isspace(*s)) s++;
	    }	
	}
	t++;
    }

    if (err) {
	pputs(prn, M_("Invalid data file\n"));
	goto oct_bailout;
    } 
    
    if (*pZ == NULL) {
	/* no dataset currently in place */
	*pZ = octZ;
	if (*ppdinfo != NULL) {
	    free(*ppdinfo);
	}
	*ppdinfo = octinfo;
    } else if (merge_data(pZ, *ppdinfo, octZ, octinfo, prn)) {
	goto oct_bailout;
    }

    fclose(fp); 
    free(line);

    console_off();

    return 0;

 oct_bailout:

    if (fp != NULL) {
	fclose(fp);
    }

    if (line != NULL) {
	free(line);
    }

    if (octinfo != NULL) {
	clear_datainfo(octinfo, CLEAR_FULL);
    }

    console_off();

    return 1;
}

/* ................................................. */

static char *unspace (char *s)
{
    int i;
    size_t n = strlen(s);

    for (i=n-1; i>=0; i--) { 
	if (s[i] == ' ') s[i] = '\0';
	else break;
    }
    return s;
}

/* #define BOX_DEBUG 1 */

/**
 * import_box:
 * @pZ: pointer to data set.
 * @ppdinfo: pointer to data information struct.
 * @fname: name of CSV file.
 * @prn: gretl printing struct.
 * 
 * Open a BOX1 data file (as produced by the US Census Bureau's
 * Data Extraction Service) and read the data into
 * the current work space.
 * 
 * Returns: 0 on successful completion, non-zero otherwise.
 *
 */

int import_box (double ***pZ, DATAINFO **ppdinfo, 
		const char *fname, PRN *prn)
{
    int c, cc, i, t, v, realv, gotdata;
    int maxline, dumpvars;
    char tmp[48];
    unsigned *varsize = NULL, *varstart = NULL;
    char *line = NULL;
    double x;
    FILE *fp;
    DATAINFO *boxinfo;
    double **boxZ = NULL;
    int err = 0;

    check_for_console(prn);

    fp = gretl_fopen(fname, "r");
    if (fp == NULL) {
	pprintf(prn, M_("Couldn't open %s\n"), fname);
	err = E_FOPEN;
	goto box_bailout;
    }

    boxinfo = datainfo_new();
    if (boxinfo == NULL) {
	pputs(prn, M_("Out of memory\n"));
	err = E_ALLOC;
	goto box_bailout;
    }

    pprintf(prn, "%s %s...\n", M_("parsing"), fname);

    /* first pass: find max line length, number of vars and number
       of observations, plus basic sanity check */
    cc = maxline = 0;
    boxinfo->v = 1;
    while ((c = getc(fp)) != EOF) {
	if (c != 10 && !isprint((unsigned char) c)) {
	    pprintf(prn, M_("Binary data (%d) encountered: this is not a valid "
		   "BOX1 file\n"), c);
	    fclose(fp);
	    err = 1;
	    goto box_bailout;
	}
	if (c == '\n') {
	    if (cc > maxline) {
		maxline = cc;
	    }
	    cc = 0;
	    if ((c = getc(fp)) != EOF) {
		tmp[0] = c; cc++;
	    } else {
		break;
	    }
	    if ((c = getc(fp)) != EOF) {
		tmp[1] = c; cc++;
	    } else {
		break;
	    }
	    tmp[2] = '\0';
	    if (!strcmp(tmp, "03")) {
		boxinfo->v += 1;
	    } else if (!strcmp(tmp, "99")) {
		boxinfo->n += 1;
	    }
	} else {
	    cc++;
	}
    } 

    fclose(fp);

    pprintf(prn, M_("   found %d variables\n"), boxinfo->v - 1);
    pprintf(prn, M_("   found %d observations\n"), boxinfo->n);
    pprintf(prn, M_("   longest line = %d characters\n"), maxline); 
    maxline += 2;

    /* allocate space for data etc */
    pputs(prn, M_("allocating memory for data... "));

    if (start_new_Z(&boxZ, boxinfo, 0)) {
	err = E_ALLOC;
	goto box_bailout;
    }

    varstart = malloc((boxinfo->v - 1) * sizeof *varstart);
    varsize = malloc((boxinfo->v - 1) * sizeof *varsize);
    line = malloc(maxline);

    if (varstart == NULL || varsize == NULL || line == NULL) {
	free(varstart);
	free(varsize);
	free(line);
	err = E_ALLOC;
	goto box_bailout;
    }

    pputs(prn, M_("done\n"));

    fp = gretl_fopen(fname, "r");
    if (fp == NULL) {
	err = E_FOPEN;
	goto box_bailout;
    }
    pputs(prn, M_("reading variable information...\n"));

#ifdef ENABLE_NLS
    setlocale(LC_NUMERIC, "C");
#endif

    /* second pass: get detailed info on variables */
    v = 0; realv = 1; t = 0;
    dumpvars = 0; gotdata = 0;
    while (fgets(line, maxline, fp)) {
	strncpy(tmp, line, 2);
	tmp[2] = '\0';
	switch (atoi(tmp)) {
	case 0: /* comment */
	    break;
	case 1: /* BOX info (ignored for now) */
	    break;
	case 2: /* raw data records types (ignored for now) */
	    break;
	case 3: /* variable info */
	    boxinfo->varname[realv][0] = '\0';
	    strncat(boxinfo->varname[realv], line+11, VNAMELEN - 1);
	    unspace(boxinfo->varname[realv]);
	    lower(boxinfo->varname[realv]);
	    pprintf(prn, M_(" variable %d: '%s'\n"), v+1, boxinfo->varname[realv]);
#ifdef notdef  
	    /* This is wrong!  How do you identify character data? */
	    if (line[51] != '2') {
		pputs(prn, M_("   Non-numeric data: will be skipped\n"));
		varstart[v] = 0;
		varsize[v] = 0;
		v++;
		break;
	    }
#endif
	    strncpy(tmp, line+52, 6);
	    tmp[6] = '\0';
	    varstart[v] = atoi(tmp) - 1;
	    pprintf(prn, M_("   starting col. %d, "), varstart[v]);
	    strncpy(tmp, line+58, 4);
	    tmp[4] = '\0';
	    varsize[v] = atoi(tmp);
	    pprintf(prn, M_("field width %d, "), varsize[v]);
	    strncpy(tmp, line+62, 2);
	    tmp[2] = '\0';
	    pprintf(prn, M_("decimal places %d\n"), atoi(tmp));
	    tmp[0] = '\0';
	    strncpy(tmp, line+64, 20);
	    tmp[20] = '\0';
	    unspace(tmp);
	    if (strlen(tmp)) {
		pprintf(prn, M_("   Warning: coded variable (format '%s' "
			"in BOX file)\n"), tmp);
	    }
	    *VARLABEL(boxinfo, realv) = 0;
	    strncat(VARLABEL(boxinfo, realv), line + 87, 99);
	    unspace(VARLABEL(boxinfo, realv));
	    pprintf(prn, M_("   definition: '%s'\n"), VARLABEL(boxinfo, realv));
	    realv++;
	    v++;
	    break;
	case 4: /* category info (ignored for now) */
	    break;
	case 99: /* data line */
	    realv = 1;
 	    for (i=0; i<v; i++) {
		if (varstart[i] == 0 && varsize[i] == 0) {
		    if (!gotdata) dumpvars++;
		    continue;
		}
		strncpy(tmp, line + varstart[i], varsize[i]);
		tmp[varsize[i]] = '\0';
		top_n_tail(tmp);

		if (check_atof(tmp)) {
		    pprintf(prn, "%s\n", gretl_errmsg);
		    x = NADBL;
		} else {
		    x = atof(tmp);
		}
#ifdef BOX_DEBUG
		fprintf(stderr, "read %d chars from pos %d: '%s' -> %g\n",
			varsize[i], varstart[i], tmp, x); 
#endif
		boxZ[realv][t] = x;
#ifdef BOX_DEBUG
		fprintf(stderr, "setting Z[%d][%d] = %g\n", realv, t, x);
#endif
		realv++;
	    }
	    t++;
	    gotdata = 1;
	    break;
	default:
	    break;
	}
    }

#ifdef ENABLE_NLS
    setlocale(LC_NUMERIC, "");
#endif

    pputs(prn, M_("done reading data\n"));
    fclose(fp);

    free(varstart);
    free(varsize);
    free(line);

    dataset_obs_info_default(boxinfo);

    if (dumpvars) {
	dataset_drop_last_variables(dumpvars, &boxZ, boxinfo);
	pprintf(prn, M_("Warning: discarded %d non-numeric variable(s)\n"), 
		dumpvars);
    }

    if (*pZ == NULL) {
	*pZ = boxZ;
	if (*ppdinfo != NULL) free(*ppdinfo);
	*ppdinfo = boxinfo;
    }

 box_bailout:

    console_off();

    return err;
}

static int xmlfile (const char *fname)
{
    gzFile fz;
    char test[6];
    int ret = 0;

    fz = gretl_gzopen(fname, "rb");
    if (fz != Z_NULL) {
	if (gzread(fz, test, 5)) {
	    test[5] = '\0';
	    if (!strcmp(test, "<?xml")) ret = 1;
	} 
	gzclose(fz);
    } 
    return ret;
} 

/**
 * detect_filetype:
 * @fname: name of file to examine.
 * @ppaths: path information struct.
 * @prn: gretl printing struct.
 * 
 * Attempt to determine the type of a file to be opened in gretl:
 * data file (native, CSV or BOX), or command script.
 * 
 * Returns: integer code indicating the type of file.
 *
 */

GretlFileType detect_filetype (char *fname, PATHS *ppaths, PRN *prn)
{
    int i, c, ftype = GRETL_NATIVE_DATA;
    char teststr[5];
    FILE *fp;

    /* might be a script file? (watch out for DOS-mangled names) */
    if (has_suffix(fname, ".inp") ||
	has_suffix(fname, ".gre"))
	return GRETL_SCRIPT;
    if (has_suffix(fname, ".gretl"))
	return GRETL_SCRIPT; 
    if (has_suffix(fname, ".gnumeric"))
	return GRETL_GNUMERIC;
    if (has_suffix(fname, ".xls"))
	return GRETL_EXCEL;
    if (has_suffix(fname, ".wf1"))
	return GRETL_WF1;
    if (has_suffix(fname, ".dta"))
	return GRETL_DTA;
    if (has_suffix(fname, ".bin"))
	return GRETL_NATIVE_DB;
    if (has_suffix(fname, ".rat"))
	return GRETL_RATS_DB;
    if (has_suffix(fname, ".csv"))
	return GRETL_CSV_DATA;
    if (has_suffix(fname, ".txt"))
	return GRETL_CSV_DATA;
    if (has_suffix(fname, ".m"))
	return GRETL_OCTAVE;

    addpath(fname, ppaths, 0); 

    if (xmlfile(fname)) {
	return GRETL_XML_DATA;  
    } 

    fp = gretl_fopen(fname, "r");
    if (fp == NULL) { 
	/* may be native file in different location */
	return GRETL_NATIVE_DATA; 
    }

    /* look at extension */
    if (has_suffix(fname, ".box")) {
	ftype = GRETL_BOX_DATA;
    }

    /* take a peek at content */
    for (i=0; i<80; i++) {
	c = getc(fp);
	if (c == EOF || c == '\n') {
	    break;
	}
	if (!isprint(c) && c != '\r' && c != '\t') {
	    ftype = GRETL_NATIVE_DATA; /* native binary data? */
	    break;
	}
	if (i < 4) {
	    teststr[i] = c;
	}
    }

    fclose(fp);
    teststr[4] = 0;

    if (ftype == GRETL_BOX_DATA) {
	if (strcmp(teststr, "00**")) {
	    pputs(prn, M_("box file seems to be malformed\n"));
	    ftype = GRETL_UNRECOGNIZED;
	}
    }

    return ftype;
}

#define UTF const xmlChar *

#undef XML_DEBUG

static char *simple_fname (char *dest, const char *src)
{
    char *p;
    const char *s;

    s = strrchr(src, SLASH);

    /* take last part of src filename */
    if (s != NULL) {
        strcpy(dest, s + 1);
    } else {
        strcpy(dest, src);
    }

    /* trash any extension */
    p = strrchr(dest, '.');
    if (p != NULL && strlen(dest) > 3) {
	*p = '\0';
    }

    return dest;
}

static int alt_puts (const char *s, FILE *fp, gzFile *fz)
{
    int ret = 0;

    if (fp != NULL) {
	ret = fputs(s, fp);
    } else if (fz != NULL) {
	ret = gzputs(fz, s);
    } 

    return ret;
}

/**
 * write_xmldata:
 * @fname: name of file to write.
 * @list: list of variables to write.
 * @Z: data matrix.
 * @pdinfo: data information struct.
 * @fmt: if %GRETL_DATA_GZIPPED write gzipped data, else uncompressed.
 * @ppaths: pointer to paths information (or NULL).
 * 
 * Write out in xml a data file containing the values of the given set
 * of variables.
 * 
 * Returns: 0 on successful completion, non-zero on error.
 * 
 */

static int write_xmldata (const char *fname, const int *list, 
			  const double **Z, const DATAINFO *pdinfo, 
			  GretlDataFormat fmt, PATHS *ppaths)
{
    FILE *fp = NULL;
    gzFile *fz = Z_NULL;
    int gz = (fmt == GRETL_DATA_GZIPPED);
    int tsamp = pdinfo->t2 - pdinfo->t1 + 1;
    int *pmax = NULL;
    char startdate[OBSLEN], enddate[OBSLEN];
    char datname[MAXLEN], type[32], freqstr[16];
    char numstr[128];
    char *xmlbuf = NULL;
    void *handle = NULL;
    int (*show_progress) (long, long, int) = NULL;
    long sz = 0L;
    int i, t;
    int err = 0;

#ifdef ENABLE_NLS
    int clocale = 0;
#endif

#ifdef USE_GTK2
    const char *enc = "UTF-8";
#else
    const char *enc = get_gretl_charset();

    if (enc == NULL) {
	enc = "ISO-8859-1";
    }
#endif

    if (gz) {
	fz = gretl_gzopen(fname, "wb");
	if (fz == Z_NULL) err = 1;
    } else {
	fp = gretl_fopen(fname, "wb");
	if (fp == NULL) err = 1;
    }

    if (err) {
	sprintf(gretl_errmsg, _("Couldn't open %s for writing"), fname);
	return 1;
    }

    pmax = malloc(list[0] * sizeof *pmax);
    if (pmax == NULL) {
	sprintf(gretl_errmsg, _("Out of memory"));
	err = 1;
	goto cleanup;
    } 

    sz = (tsamp * pdinfo->v * sizeof(double));
    if (sz > 100000) {
	fprintf(stderr, I_("Writing %ld Kbytes of data\n"), sz / 1024);
	if (ppaths == NULL) {
	    sz = 0L;
	}
    } else {
	sz = 0L;
    }

    if (sz) {
	show_progress = get_plugin_function("show_progress", &handle);
	if (show_progress == NULL) {
	    sz = 0L;
	}
    }

    if (sz) (*show_progress)(0, sz, SP_SAVE_INIT); 

    for (i=1; i<=list[0]; i++) {
	if (pdinfo->vector[list[i]]) {
	    pmax[i-1] = get_precision(&Z[list[i]][pdinfo->t1], tsamp, 10);
	} else {
	    pmax[i-1] = SCALAR_DIGITS;
	}
    }

    ntodate_full(startdate, pdinfo->t1, pdinfo);
    ntodate_full(enddate, pdinfo->t2, pdinfo);

    simple_fname(datname, fname);
    xmlbuf = gretl_xml_encode(datname);
    if (xmlbuf == NULL) {
	err = 1;
	goto cleanup;
    }

    if (custom_time_series(pdinfo)) {
	strcpy(freqstr, "special");
    } else {
	sprintf(freqstr, "%d", pdinfo->pd);
    }

    if (gz) {
	gzprintf(fz, "<?xml version=\"1.0\" encoding=\"%s\"?>\n"
		 "<!DOCTYPE gretldata SYSTEM \"gretldata.dtd\">\n\n"
		 "<gretldata name=\"%s\" frequency=\"%s\" "
		 "startobs=\"%s\" endobs=\"%s\" ", 
		 enc, datname, freqstr, startdate, enddate);
    } else {
	fprintf(fp, "<?xml version=\"1.0\" encoding=\"%s\"?>\n"
		"<!DOCTYPE gretldata SYSTEM \"gretldata.dtd\">\n\n"
		"<gretldata name=\"%s\" frequency=\"%s\" "
		"startobs=\"%s\" endobs=\"%s\" ", 
		enc, datname, freqstr, startdate, enddate);
    }

    free(xmlbuf);

    switch (pdinfo->structure) {
    case 0:
	strcpy(type, "cross-section"); break;
    case TIME_SERIES:
    case SPECIAL_TIME_SERIES:
	strcpy(type, "time-series"); break;
    case STACKED_TIME_SERIES:
	strcpy(type, "stacked-time-series"); break;
    case STACKED_CROSS_SECTION:
	strcpy(type, "stacked-cross-section"); break;
    default:
	strcpy(type, "cross-section"); break;
    }

    if (gz) {
	gzprintf(fz, "type=\"%s\">\n", type);
    } else {
	fprintf(fp, "type=\"%s\">\n", type);
    }

    /* first deal with description, if any */
    if (pdinfo->descrip != NULL) {
	xmlbuf = gretl_xml_encode(pdinfo->descrip);
	if (xmlbuf == NULL) {
	    err = 1;
	    goto cleanup;
	} else {
	    if (gz) {
		gzputs(fz, "<description>");
		gzputs(fz, xmlbuf);
		gzputs(fz, "</description>\n");
	    } else {
		fprintf(fp, "<description>%s</description>\n", xmlbuf);
	    }
	    free(xmlbuf);
#ifdef XML_DEBUG
	    fprintf(stderr, "xmlbuf encoded buffer freed\n");
#endif
	}
    }

#ifdef ENABLE_NLS
    setlocale(LC_NUMERIC, "C");
    clocale = 1;
#endif

    /* then listing of variable names and labels */
    if (gz) {
	gzprintf(fz, "<variables count=\"%d\">\n", list[0]);
    } else {
	fprintf(fp, "<variables count=\"%d\">\n", list[0]);
    }

    for (i=1; i<=list[0]; i++) {
	xmlbuf = gretl_xml_encode(pdinfo->varname[list[i]]);

	if (xmlbuf == NULL) {
	    err = 1;
	    goto cleanup;
	} else {
	    if (gz) {
		gzprintf(fz, "<variable name=\"%s\"", xmlbuf);
	    } else {
		fprintf(fp, "<variable name=\"%s\"", xmlbuf);
	    }
	    free(xmlbuf);
	}

	if (!pdinfo->vector[list[i]] && !na(Z[list[i]][0])) {
	    if (pmax[i-1] == PMAX_NOT_AVAILABLE) {
		sprintf(numstr, "\n role=\"scalar\" value=\"%.12g\"",
			Z[list[i]][0]);
	    } else {
		sprintf(numstr, "\n role=\"scalar\" value=\"%.*f\"",
			pmax[i-1], Z[list[i]][0]);
	    }
	    alt_puts(numstr, fp, fz);
	}

	if (*VARLABEL(pdinfo, list[i])) {
	    xmlbuf = gretl_xml_encode(VARLABEL(pdinfo, list[i]));
	    if (xmlbuf == NULL) {
		err = 1;
		goto cleanup;
	    } else {
		if (gz) {
		    gzprintf(fz, "\n label=\"%s\"", xmlbuf);
		} else {
		    fprintf(fp, "\n label=\"%s\"", xmlbuf);
		}
		free(xmlbuf);
	    }
	} 

	if (*DISPLAYNAME(pdinfo, list[i])) {
	    xmlbuf = gretl_xml_encode(DISPLAYNAME(pdinfo, list[i]));
	    if (xmlbuf == NULL) {
		err = 1;
		goto cleanup;
	    } else {
		if (gz) {
		    gzprintf(fz, "\n displayname=\"%s\"", xmlbuf);
		} else {
		    fprintf(fp, "\n displayname=\"%s\"", xmlbuf);
		}
		free(xmlbuf);
	    }
	} 

	if (COMPACT_METHOD(pdinfo, list[i]) != COMPACT_NONE) {
	    const char *meth = compact_method_to_string(COMPACT_METHOD(pdinfo, list[i]));

	    if (gz) {
		gzprintf(fz, "\n compact-method=\"%s\"", meth);
	    } else {
		fprintf(fp, "\n compact-method=\"%s\"", meth);
	    }
	} 

	alt_puts("\n/>\n", fp, fz);
    }

    alt_puts("</variables>\n", fp, fz);

    /* then listing of observations */
    if (gz) {
	gzprintf(fz, "<observations count=\"%d\" labels=\"%s\">\n",
		tsamp, (pdinfo->markers && pdinfo->S != NULL)? "true" : "false");
    } else {
	fprintf(fp, "<observations count=\"%d\" labels=\"%s\">\n",
		tsamp, (pdinfo->markers && pdinfo->S != NULL)? "true" : "false");
    }

    for (t=pdinfo->t1; t<=pdinfo->t2; t++) {
	alt_puts("<obs", fp, fz);
	if (pdinfo->markers && pdinfo->S != NULL) {
	    if (gz) {
		gzprintf(fz, " label=\"%s\">", pdinfo->S[t]);
	    } else {
		fprintf(fp, " label=\"%s\">", pdinfo->S[t]);
	    }
	} else {
	    alt_puts(">", fp, fz);
	}
	for (i=1; i<=list[0]; i++) {
	    if (!pdinfo->vector[list[i]]) {
		continue;
	    }
	    if (na(Z[list[i]][t])) {
		strcpy(numstr, "NA ");
	    } else if (pmax[i-1] == PMAX_NOT_AVAILABLE) {
		sprintf(numstr, "%.12g ", Z[list[i]][t]);
	    } else {
		sprintf(numstr, "%.*f ", pmax[i-1], Z[list[i]][t]);
	    }
	    alt_puts(numstr, fp, fz);
	}

	alt_puts("</obs>\n", fp, fz);

	if (sz && t && ((t - pdinfo->t1) % 50 == 0)) { 
	    (*show_progress) (50, tsamp, SP_NONE);
	}
    }

    alt_puts("</observations>\n</gretldata>\n", fp, fz);

 cleanup: 

#ifdef ENABLE_NLS
    if (clocale) {
	setlocale(LC_NUMERIC, "");
    }
#endif

    if (sz) {
	(*show_progress)(0, pdinfo->t2 - pdinfo->t1 + 1, SP_FINISH);
	close_plugin(handle);
    } 

    if (pmax) free(pmax);
    if (fp != NULL) fclose(fp);
    if (fz != Z_NULL) gzclose(fz);

    return err;
}

static int transcribe_string (char *targ, const char *src, int maxlen,
			      int convert)
{
    *targ = '\0';

#ifndef USE_GTK2
    if (convert) {
	char tmp[128] = {0};

	if (maxlen > 128) {
	    maxlen = 128;
	}
	strncat(tmp, src, maxlen - 1);
	utf8_to_iso_latin_1(targ, maxlen, tmp, maxlen);
    } else {
	strncat(targ, src, maxlen - 1);
    }
#else
    strncat(targ, src, maxlen - 1);
#endif

    return 0;
}

static int process_varlist (xmlNodePtr node, DATAINFO *pdinfo, double ***pZ,
			    int to_iso_latin)
{
    xmlNodePtr cur;
    char *tmp = xmlGetProp(node, (UTF) "count");
    int i, err = 0;

    if (tmp != NULL) {
	int v;

	if (sscanf(tmp, "%d", &v) == 1) {
	    pdinfo->v = v + 1;
	} else {
	    sprintf(gretl_errmsg, _("Failed to parse count of variables"));
	    err = 1;
	}
	if (!err && dataset_allocate_varnames(pdinfo)) {
	    sprintf(gretl_errmsg, _("Out of memory reading data file"));
	    err = 1;
	}
	if (!err) {
	    *pZ = malloc(pdinfo->v * sizeof **pZ);
	    if (*pZ == NULL) {
		sprintf(gretl_errmsg, _("Out of memory reading data file"));
		err = 1;
	    }
	}		
	free(tmp);
    } else {
	sprintf(gretl_errmsg, _("Got no variables"));
	err = 1;
    }

    if (err) return 1;

    /* now get individual variable info: names and labels */
    cur = node->xmlChildrenNode;
    while (cur && xmlIsBlankNode(cur)) {
	cur = cur->next;
    }

    if (cur == 0) {
	sprintf(gretl_errmsg, _("Got no variables"));
	return 1;
    }

    i = 1;
    while (cur != NULL) {
        if (!xmlStrcmp(cur->name, (UTF) "variable")) {
	    tmp = xmlGetProp(cur, (UTF) "name");
	    if (tmp != NULL) {
		transcribe_string(pdinfo->varname[i], tmp, VNAMELEN,
				  to_iso_latin); 
		free(tmp);
	    } else {
		sprintf(gretl_errmsg, _("Variable %d has no name"), i);
		return 1;
	    }
	    tmp = xmlGetProp(cur, (UTF) "label");
	    if (tmp != NULL) {
		transcribe_string(VARLABEL(pdinfo, i), tmp, MAXLABEL,
				  to_iso_latin);
		free(tmp);
	    }
	    tmp = xmlGetProp(cur, (UTF) "displayname");
	    if (tmp != NULL) {
		transcribe_string(DISPLAYNAME(pdinfo, i), tmp, MAXDISP,
				  to_iso_latin);
		free(tmp);
	    }
	    tmp = xmlGetProp(cur, (UTF) "compact-method");
	    if (tmp != NULL) {
		COMPACT_METHOD(pdinfo, i) = compact_string_to_int(tmp);
		free(tmp);
	    }
	    tmp = xmlGetProp(cur, (UTF) "role");
	    if (tmp != NULL) {
		if (!strcmp(tmp, "scalar")) {
		    char *val = xmlGetProp(cur, (UTF) "value");
		    
		    if (val) {
			double xx = atof(val);

			free(val);
			(*pZ)[i] = malloc(sizeof ***pZ);
			(*pZ)[i][0] = xx;
			pdinfo->vector[i] = 0;
		    }
		}
		free(tmp);
	    }
	    i++;
	}	    
	cur = cur->next;
    }
   
    if (i != pdinfo->v) {
	sprintf(gretl_errmsg, _("Number of variables does not match declaration"));
	err = 1;
    } 

    return err;
}

static int process_values (double **Z, DATAINFO *pdinfo, int t, char *s)
{
    char valstr[32];
    double x;
    int i, err = 0;

    *gretl_errmsg = '\0';

    for (i=1; i<pdinfo->v && !err; i++) {
	if (!pdinfo->vector[i]) {
	    continue;
	}
	s = strpbrk(s, "01234567890+-NA");
	if (s == NULL) {
	    fprintf(stderr, "i = %d: s == NULL in process_values()\n", i);
	    err = 1;
	} else {
	    if (*s == '\0' || sscanf(s, "%31s", valstr) != 1) {
		fputs("s is blank in process_values()\n", stderr);
		err = 1;
	    } else {
		if (!strcmp(valstr, "NA")) {
		    x = NADBL;
		} else if (check_atof(valstr)) {
		    err = 1;
		} else {
		    sscanf(valstr, "%lf", &x);
		}
	    }
	}
	if (!err) {
	    if (t < pdinfo->n) {
		Z[i][t] = x;
	    }
	    s = strpbrk(s, " \t\n\r");
	}
    }

    if (err && *gretl_errmsg == '\0') {
	sprintf(gretl_errmsg, _("Failed to parse data values at obs %d"), t+1);
    }

    return err;
}

static int process_observations (xmlDocPtr doc, xmlNodePtr node, 
				 double ***pZ, DATAINFO *pdinfo,
				 long progress, int to_iso_latin)
{
    xmlNodePtr cur;
    char *tmp;
    int n, i, t;
    void *handle;
    int (*show_progress) (long, long, int) = NULL;

    tmp = xmlGetProp(node, (UTF) "count");
    if (tmp == NULL) {
	return 1;
    } 

    if (sscanf(tmp, "%d", &n) == 1) {
	pdinfo->n = n;
	free(tmp);
    } else {
	sprintf(gretl_errmsg, _("Failed to parse number of observations"));
	free(tmp);
	return 1;
    }

    if (progress > 0) {
	show_progress = get_plugin_function("show_progress", &handle);
	if (show_progress == NULL) {
	    progress = 0L;
	}
    }

    tmp = xmlGetProp(node, (UTF) "labels");
    if (tmp) {
	if (!strcmp(tmp, "true")) {
	    if (dataset_allocate_obs_markers(pdinfo)) {
		sprintf(gretl_errmsg, "Out of memory");
		return 1;
	    }
	} else if (strcmp(tmp, "false")) {
	    sprintf(gretl_errmsg, _("labels attribute for observations must be "
		    "'true' or 'false'"));
	    return 1;
	}
	free(tmp);
    } else {
	return 1;
    }

    if (pdinfo->endobs[0] == '\0') {
	sprintf(pdinfo->endobs, "%d", pdinfo->n);
    }

    pdinfo->t2 = pdinfo->n - 1;

    for (i=0; i<pdinfo->v; i++) {
	if (!pdinfo->vector[i]) {
	    continue;
	}
	(*pZ)[i] = malloc(pdinfo->n * sizeof ***pZ);
	if ((*pZ)[i] == NULL) {
	    return 1;
	}
    }

    for (t=0; t<pdinfo->n; t++) {
	(*pZ)[0][t] = 1.0;
    }

    /* now get individual obs info: labels and values */
    cur = node->xmlChildrenNode;
    while (cur && xmlIsBlankNode(cur)) {
	cur = cur->next;
    }
    if (cur == 0) {
	sprintf(gretl_errmsg, _("Got no observations\n"));
	return 1;
    }

    if (progress) {
	(*show_progress)(0L, progress, SP_LOAD_INIT);
    }

    t = 0;
    while (cur != NULL) {
        if (!xmlStrcmp(cur->name, (UTF) "obs")) {
	    if (pdinfo->markers) {
		tmp = xmlGetProp(cur, (UTF) "label");
		if (tmp) {
		    transcribe_string(pdinfo->S[t], tmp, OBSLEN,
				      to_iso_latin); 
		    free(tmp);
		} else {
		    sprintf(gretl_errmsg, _("Case marker missing at obs %d"), t+1);
		    return 1;
		}
	    }
	    tmp = xmlNodeListGetString(doc, cur->xmlChildrenNode, 1);
	    if (tmp) {
		if (process_values(*pZ, pdinfo, t, tmp)) {
		    return 1;
		}
		free(tmp);
		t++;
	    } else {
		sprintf(gretl_errmsg, _("Values missing at observation %d"), t+1);
		return 1;
	    }
	}	    
	cur = cur->next;
	if (progress && t > 0 && t % 50 == 0) {
	    (*show_progress) (50L, (long) pdinfo->n, SP_NONE);
	}
    }

    if (progress) {
	(*show_progress)(0L, (long) pdinfo->n, SP_FINISH);
	close_plugin(handle);
    }

    if (t != pdinfo->n) {
	sprintf(gretl_errmsg, _("Number of observations does not match declaration"));
	return 1;
    }

    else return 0;
}

static long get_filesize (const char *fname)
{
    struct stat buf;

    if (stat(fname, &buf) == 0) {
        return buf.st_size;
    } else {
        return -1;
    }
}

/**
 * get_xmldata:
 * @pZ: pointer to data set.
 * @ppdinfo: pointer to data information struct.
 * @fname: name of file to try.
 * @ppaths: path information struct.
 * @data_status: DATA_NONE: no datafile currently open; DATA_CLEAR: datafile
 * is open, should be cleared; DATA_APPEND: add to current dataset.
 * @prn: where messages should be written.
 * @gui: should = 1 if the function is launched from the GUI, else 0.
 * 
 * Read data from file into gretl's work space, allocating space as
 * required.
 * 
 * Returns: 0 on successful completion, non-zero otherwise.
 *
 */

int get_xmldata (double ***pZ, DATAINFO **ppdinfo, char *fname,
		 PATHS *ppaths, int data_status, PRN *prn, int gui) 
{
    DATAINFO *tmpdinfo;
    double **tmpZ = NULL;
    xmlDocPtr doc;
    xmlNodePtr cur;
    char *tmp;
    int gotvars = 0, gotobs = 0, err = 0;
    int to_iso_latin = 0;
    long fsz, progress = 0L;

    *gretl_errmsg = '\0';

    check_for_console(prn);

    tmpdinfo = datainfo_new();
    if (tmpdinfo == NULL) {
	err = E_ALLOC;
	goto bailout;
    }

    /* COMPAT: Do not generate nodes for formatting spaces */
    LIBXML_TEST_VERSION
	xmlKeepBlanksDefault(0);

    fsz = get_filesize(fname);
    if (fsz > 100000) {
	fprintf(stderr, "%s %ld bytes %s...\n", 
		(is_gzipped(fname))? I_("Uncompressing") : I_("Reading"),
		fsz, I_("of data"));
	if (gui) progress = fsz;
    }

    doc = xmlParseFile(fname);
    if (doc == NULL) {
	sprintf(gretl_errmsg, _("xmlParseFile failed on %s"), fname);
	err = 1;
	goto bailout;
    }

#ifndef USE_GTK2
    if (doc->encoding != NULL && strstr(doc->encoding, "UTF")) {
	to_iso_latin = 1;
    }
#endif

    cur = xmlDocGetRootElement(doc);
    if (cur == NULL) {
        sprintf(gretl_errmsg, _("%s: empty document"), fname);
	xmlFreeDoc(doc);
	err = 1;
	goto bailout;
    }

    if (xmlStrcmp(cur->name, (UTF) "gretldata")) {
        sprintf(gretl_errmsg, _("File of the wrong type, root node not gretldata"));
	xmlFreeDoc(doc);
	err = 1;
	goto bailout;
    }

    /* set some datainfo parameters */
    tmp = xmlGetProp(cur, (UTF) "type");
    if (tmp == NULL) {
	sprintf(gretl_errmsg, 
		_("Required attribute 'type' is missing from data file"));
	err = 1;
	goto bailout;
    } else {
	if (!strcmp(tmp, "cross-section")) {
	    tmpdinfo->structure = CROSS_SECTION;
	} else if (!strcmp(tmp, "time-series")) {
	    tmpdinfo->structure = TIME_SERIES;
	} else if (!strcmp(tmp, "stacked-time-series")) {
	    tmpdinfo->structure = STACKED_TIME_SERIES;
	} else if (!strcmp(tmp, "stacked-cross-section")) {
	    tmpdinfo->structure = STACKED_CROSS_SECTION;
	} else {
	    sprintf(gretl_errmsg, _("Unrecognized type attribute for data file"));
	    free(tmp);
	    err = 1;
	    goto bailout;
	}
	free(tmp);
    }

    tmpdinfo->pd = 1;

    tmp = xmlGetProp(cur, (UTF) "frequency");
    if (tmp) {
	int pd = 0;

	if (!strcmp(tmp, "special")) {
	    tmpdinfo->structure = SPECIAL_TIME_SERIES;
	    tmpdinfo->pd = 1;
	} else if (sscanf(tmp, "%d", &pd) == 1) {
	    tmpdinfo->pd = pd;
	} else {
	    strcpy(gretl_errmsg, _("Failed to parse data frequency"));
	    free(tmp);
	    err = 1;
	    goto bailout;
	}
	free(tmp);
    }

#ifdef ENABLE_NLS
    setlocale(LC_NUMERIC, "C");
#endif

    strcpy(tmpdinfo->stobs, "1");

    tmp = xmlGetProp(cur, (UTF) "startobs");

    if (tmp != NULL) {
	char obstr[16];

	obstr[0] = '\0';
	strncat(obstr, tmp, 15);
	charsub(obstr, ':', '.');
	
	if (strchr(obstr, '/') != NULL && 
	    (dataset_is_daily(tmpdinfo) || 
	     dataset_is_weekly(tmpdinfo))) {
	    long ed = get_epoch_day(tmp);

	    if (ed < 0) {
		err = 1;
	    } else {
		tmpdinfo->sd0 = ed;
	    }
	} else {
	    double x;

	    if (sscanf(obstr, "%lf", &x) != 1) {
		err = 1;
	    } else {
		tmpdinfo->sd0 = x;
	    }
	}
	if (err) {
	    strcpy(gretl_errmsg, _("Failed to parse startobs"));
	    free(tmp);
	    err = 1;
	    goto bailout;
	}
	tmpdinfo->stobs[0] = '\0';
	strncat(tmpdinfo->stobs, tmp, OBSLEN - 1);
	colonize_obs(tmpdinfo->stobs);
	free(tmp);
    }

    *tmpdinfo->endobs = '\0';

    tmp = xmlGetProp(cur, (UTF) "endobs");

    if (tmp!= NULL) {
	if (calendar_data(tmpdinfo)) {
	    long ed = get_epoch_day(tmp);

	    if (ed < 0) err = 1;
	} else {
	    double x;

	    if (sscanf(tmp, "%lf", &x) != 1) {
		err = 1;
	    }
	} 
	if (err) {
	    strcpy(gretl_errmsg, _("Failed to parse endobs"));
	    free(tmp);
	    err = 1;
	    goto bailout;
	}
	tmpdinfo->endobs[0] = '\0';
	strncat(tmpdinfo->endobs, tmp, OBSLEN - 1);
	colonize_obs(tmpdinfo->endobs);
	free(tmp);
    }

    /* Now walk the tree */
    cur = cur->xmlChildrenNode;
    while (cur != NULL && !err) {
        if (!xmlStrcmp(cur->name, (UTF) "description")) {
	    tmpdinfo->descrip = 
		xmlNodeListGetString(doc, cur->xmlChildrenNode, 1);
        } else if (!xmlStrcmp(cur->name, (UTF) "variables")) {
	    if (process_varlist(cur, tmpdinfo, &tmpZ, to_iso_latin)) {
		err = 1;
	    } else {
		gotvars = 1;
	    }
	}
        else if (!xmlStrcmp(cur->name, (UTF) "observations")) {
	    if (!gotvars) {
		sprintf(gretl_errmsg, _("Variables information is missing"));
		err = 1;
	    }
	    if (process_observations(doc, cur, &tmpZ, tmpdinfo, 
				     progress, to_iso_latin)) {
		err = 1;
	    } else {
		gotobs = 1;
	    }
	}
	if (!err) cur = cur->next;
    }

#ifdef ENABLE_NLS
    setlocale(LC_NUMERIC, "");
#endif

    xmlFreeDoc(doc);
    xmlCleanupParser();

    if (err) {
	goto bailout;
    }

    if (!gotvars) {
	sprintf(gretl_errmsg, _("Variables information is missing"));
	err = 1;
	goto bailout;
    }
    if (!gotobs) {
	sprintf(gretl_errmsg, _("No observations were found"));
	err = 1;
	goto bailout;
    }

    if (ppaths != NULL && fname != ppaths->datfile) {
	strcpy(ppaths->datfile, fname);
    }

    data_read_message(fname, tmpdinfo, prn);

    if (data_status == DATA_APPEND) {
	err = merge_data(pZ, *ppdinfo, tmpZ, tmpdinfo, prn);
	if (err) {
	    tmpZ = NULL;
	    free(tmpdinfo);
	    tmpdinfo = NULL;
	}
    } else {
	free_Z(*pZ, *ppdinfo);
	if (data_status == DATA_CLEAR) {
	    clear_datainfo(*ppdinfo, CLEAR_FULL);
	}
	free(*ppdinfo);
	*ppdinfo = tmpdinfo;
	*pZ = tmpZ;
    }

 bailout:

    if (err) {
	free_Z(tmpZ, tmpdinfo);
	clear_datainfo(tmpdinfo, CLEAR_FULL);
	free(tmpdinfo);
    }

    console_off();

    return err;
}

/**
 * get_xml_description:
 * @fname: name of file to try.
 * 
 * Read data description for gretl xml data file.
 * 
 * Returns: buffer containing description, or NULL on failure.
 *
 */

char *get_xml_description (const char *fname)
{
    xmlDocPtr doc;
    xmlNodePtr cur;
    char *buf = NULL;

    *gretl_errmsg = '\0';

    LIBXML_TEST_VERSION
	xmlKeepBlanksDefault(0);

    doc = xmlParseFile(fname);
    if (doc == NULL) {
	sprintf(gretl_errmsg, _("xmlParseFile failed on %s"), fname);
	return NULL;
    }

    cur = xmlDocGetRootElement(doc);
    if (cur == NULL) {
        sprintf(gretl_errmsg, _("%s: empty document"), fname);
	xmlFreeDoc(doc);
	return NULL;
    }

    if (xmlStrcmp(cur->name, (UTF) "gretldata")) {
        sprintf(gretl_errmsg, _("File of the wrong type, root node not gretldata"));
	xmlFreeDoc(doc);
	return NULL;
    }

    cur = cur->xmlChildrenNode;
    while (cur != NULL) {
        if (!xmlStrcmp(cur->name, (UTF) "description")) {
	    buf = xmlNodeListGetString(doc, cur->xmlChildrenNode, 1);
	    break;
        }
	cur = cur->next;
    }

    xmlFreeDoc(doc);
    xmlCleanupParser();

    return buf;
}

int check_atof (const char *numstr)
{
    char *test;

    /* accept blank entries */
    if (*numstr == '\0') return 0;

    strtod(numstr, &test);

    if (*test == '\0' && errno != ERANGE) return 0;

    if (!strcmp(numstr, test)) {
	sprintf(gretl_errmsg, M_("'%s' -- no numeric conversion performed!"), numstr);
	return 1;
    }

    if (*test != '\0') {
	if (isprint(*test)) {
	    sprintf(gretl_errmsg, M_("Extraneous character '%c' in data"), *test);
	} else {
	    sprintf(gretl_errmsg, M_("Extraneous character (0x%x) in data"), *test);
	}
	return 1;
    }

    if (errno == ERANGE) {
	sprintf(gretl_errmsg, M_("'%s' -- number out of range!"), numstr);
    }

    return 1;
}

int transpose_data (double ***pZ, DATAINFO *pdinfo)
{
    double **tZ = NULL;
    DATAINFO *tinfo;
    int k = pdinfo->n + 1;
    int T = pdinfo->v - 1;
    int i, t;

    for (i=1; i<pdinfo->v; i++) {
	if (!pdinfo->vector[i]) {
	    strcpy(gretl_errmsg, _("Dataset contains scalars, can't transpose"));
	    return E_DATA;
	}
    }

    tinfo = create_new_dataset(&tZ, k, T, 0);
    if (tinfo == NULL) {
	return E_ALLOC;
    }

    for (i=1; i<pdinfo->v; i++) {
	for (t=0; t<pdinfo->n; t++) {
	    tZ[t+1][i-1] = (*pZ)[i][t];
	}
    }

    for (t=0; t<pdinfo->n; t++) {
	if (pdinfo->S != NULL && pdinfo->S[t][0] != '\0') {
	    tinfo->varname[t+1][0] = '\0';
	    strncat(tinfo->varname[t+1], pdinfo->S[t], 8);
	} else {
	    sprintf(tinfo->varname[t+1], "v%d", t+1);
	}
    }

    free_Z(*pZ, pdinfo);
    *pZ = tZ;

    clear_datainfo(pdinfo, CLEAR_FULL);

    pdinfo->v = k;
    pdinfo->n = T;
    pdinfo->t1 = 0;
    pdinfo->t2 = pdinfo->n - 1;

    pdinfo->varname = tinfo->varname;
    pdinfo->varinfo = tinfo->varinfo;
    pdinfo->vector = tinfo->vector;

    dataset_obs_info_default(pdinfo);

    free(tinfo);

    return 0;
}

void dataset_set_regular_markers (DATAINFO *pdinfo)
{
    pdinfo->markers = REGULAR_MARKERS;
}

