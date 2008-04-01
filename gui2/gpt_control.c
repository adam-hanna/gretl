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

/* gpt_control.c for gretl -- gnuplot controller */

#include "gretl.h"
#include "plotspec.h"
#include "gpt_control.h"
#include "session.h"
#include "gpt_dialog.h"
#include "fileselect.h"
#include "calculator.h"
#include "guiprint.h"
#include "textbuf.h"

#define GPDEBUG 0
#define POINTS_DEBUG 0

#ifdef G_OS_WIN32
# include <io.h>
# include "gretlwin32.h"
#endif

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdkkeysyms.h>

enum {
    PLOT_SAVED          = 1 << 0,
    PLOT_HAS_CONTROLLER = 1 << 1,
    PLOT_ZOOMED         = 1 << 2,
    PLOT_ZOOMING        = 1 << 3,
    PLOT_NO_MARKERS     = 1 << 4,
    PLOT_PNG_COORDS     = 1 << 5,
    PLOT_HAS_XRANGE     = 1 << 6,
    PLOT_HAS_YRANGE     = 1 << 7,
    PLOT_DONT_ZOOM      = 1 << 8,
    PLOT_DONT_EDIT      = 1 << 9,
    PLOT_DONT_MOUSE     = 1 << 10,
    PLOT_POSITIONING    = 1 << 11
} plot_status_flags;

enum {
    PLOT_TITLE          = 1 << 0,
    PLOT_XLABEL         = 1 << 1,
    PLOT_YLABEL         = 1 << 2,
    PLOT_Y2AXIS         = 1 << 3,
    PLOT_Y2LABEL        = 1 << 4,
    PLOT_MARKERS_UP     = 1 << 5,
    PLOT_POLAR          = 1 << 6
} plot_format_flags;

#define MAX_MARKERS 120

#define plot_is_saved(p)        (p->status & PLOT_SAVED)
#define plot_has_controller(p)  (p->status & PLOT_HAS_CONTROLLER)
#define plot_is_zoomed(p)       (p->status & PLOT_ZOOMED)
#define plot_is_zooming(p)      (p->status & PLOT_ZOOMING)
#define plot_has_no_markers(p)  (p->status & PLOT_NO_MARKERS)
#define plot_has_png_coords(p)  (p->status & PLOT_PNG_COORDS)
#define plot_has_xrange(p)      (p->status & PLOT_HAS_XRANGE)
#define plot_has_yrange(p)      (p->status & PLOT_HAS_YRANGE)
#define plot_not_zoomable(p)    (p->status & PLOT_DONT_ZOOM)
#define plot_not_editable(p)    (p->status & PLOT_DONT_EDIT)
#define plot_doing_position(p)  (p->status & PLOT_POSITIONING)

#define plot_has_title(p)        (p->format & PLOT_TITLE)
#define plot_has_xlabel(p)       (p->format & PLOT_XLABEL)
#define plot_has_ylabel(p)       (p->format & PLOT_YLABEL)
#define plot_has_y2axis(p)       (p->format & PLOT_Y2AXIS)
#define plot_has_y2label(p)      (p->format & PLOT_Y2LABEL)
#define plot_has_data_markers(p) (p->format & PLOT_MARKERS_UP)
#define plot_is_polar(p)         (p->format & PLOT_POLAR)

#define plot_is_range_mean(p)   (p->spec->code == PLOT_RANGE_MEAN)
#define plot_is_hurst(p)        (p->spec->code == PLOT_HURST)
#define plot_is_roots(p)        (p->spec->code == PLOT_VAR_ROOTS)

#define plot_has_regression_list(p) (p->spec->reglist != NULL)
#define plot_show_all_markers(p)    (p->spec->flags & GPT_ALL_MARKERS)

enum {
    PNG_START,
    PNG_ZOOM,
    PNG_UNZOOM,
    PNG_REDISPLAY
} png_zoom_codes;

struct png_plot_t {
    GtkWidget *shell;
    GtkWidget *canvas;
    GtkWidget *popup;
    GtkWidget *statusarea;    
    GtkWidget *statusbar;
    GtkWidget *cursor_label;
    GtkWidget *labelpos_entry;
    GdkPixmap *pixmap;
    GdkGC *invert_gc;
    GPT_SPEC *spec;
    double xmin, xmax;
    double ymin, ymax;
    int pixel_width, pixel_height;
    int pixel_xmin, pixel_xmax;
    int pixel_ymin, pixel_ymax;
    int xint, yint;
    int pd;
    int err;
    guint cid;
    double zoom_xmin, zoom_xmax;
    double zoom_ymin, zoom_ymax;
    int screen_xmin, screen_ymin;
    unsigned long status; 
    unsigned char format;
};

static int render_pngfile (png_plot *plot, int view);
static int zoom_unzoom_png (png_plot *plot, int view);
static void create_selection_gc (png_plot *plot);
static int get_plot_ranges (png_plot *plot);
static void graph_display_pdf (GPT_SPEC *spec);
#ifdef G_OS_WIN32
static void win32_process_graph (GPT_SPEC *spec, int color, int dest);
#endif

enum {
    GRETL_PNG_OK,
    GRETL_PNG_NO_OPEN,
    GRETL_PNG_NOT_PNG,
    GRETL_PNG_NO_COMMENTS,
    GRETL_PNG_BAD_COMMENTS,
    GRETL_PNG_NO_COORDS
};

typedef struct png_bounds_t png_bounds;

struct png_bounds_t {
    int xleft;
    int xright;
    int ybot;
    int ytop;
    double xmin;
    double xmax;
    double ymin;
    double ymax;
};

static int get_png_bounds_info (png_bounds *bounds);

#define PLOTSPEC_DETAILS_IN_MEMORY(s)  (s->data != NULL)

static void terminate_plot_positioning (png_plot *plot)
{
    plot->status ^= PLOT_POSITIONING;
    plot->labelpos_entry = NULL;
    gdk_window_set_cursor(plot->canvas->window, NULL);
    gtk_statusbar_pop(GTK_STATUSBAR(plot->statusbar), plot->cid);
    raise_gpt_control_window();
}

void plot_remove_controller (png_plot *plot) 
{
    if (plot_has_controller(plot)) {
	plot->status ^= PLOT_HAS_CONTROLLER;
	if (plot_doing_position(plot)) {
	    terminate_plot_positioning(plot);
	}
    }
}

GtkWidget *plot_get_shell (png_plot *plot) 
{
    return plot->shell;
}

GPT_SPEC *plot_get_spec (png_plot *plot) 
{
    return plot->spec;
}

int plot_is_mouseable (const png_plot *plot)
{
    return !(plot->status & PLOT_DONT_MOUSE);
}

double plot_get_xmin (png_plot *plot)
{
    return (plot != NULL)? plot->xmin : -1;
}

double plot_get_ymin (png_plot *plot)
{
    return (plot != NULL)? plot->ymin : -1;
}

void set_plot_has_y2_axis (png_plot *plot, gboolean s)
{
    if (s == TRUE) {
	plot->format |= PLOT_Y2AXIS;
    } else {
	plot->format &= ~PLOT_Y2AXIS;
    }
}

void plot_label_position_click (GtkWidget *w, png_plot *plot)
{
    if (plot != NULL) {
	GtkWidget *entry;
	GdkCursor* cursor;

	cursor = gdk_cursor_new(GDK_CROSSHAIR);
	gdk_window_set_cursor(plot->canvas->window, cursor);
	gdk_cursor_destroy(cursor);
	entry = g_object_get_data(G_OBJECT(w), "labelpos_entry");
	plot->labelpos_entry = entry;
	plot->status |= PLOT_POSITIONING;
	gtk_statusbar_push(GTK_STATUSBAR(plot->statusbar), plot->cid, 
			   _(" Click to set label position"));
    }
}

static FILE *open_gp_file (const char *fname, const char *mode)
{
    FILE *fp = gretl_fopen(fname, mode);

    if (fp == NULL) {
	if (*mode == 'w') {
	    file_write_errbox(fname);
	} else {
	    file_read_errbox(fname);
	}
    }

    return fp;
}

static int commented_term_line (const char *s)
{
    return !strncmp(s, "# set term png", 14);
}

static int set_output_line (const char *s)
{
    return !strncmp(s, "set output", 10);
}

static int set_print_line (const char *s)
{
    return (!strncmp(s, "set print ", 10) ||
	    !strncmp(s, "print \"pixe", 11) ||
	    !strncmp(s, "print \"data", 11));
}

enum {
    REMOVE_PNG,
    ADD_PNG
};

static int 
add_or_remove_png_term (const char *fname, int action, GPT_SPEC *spec)
{
    FILE *fsrc, *ftmp;
    char temp[MAXLEN], fline[MAXLEN];
    char restore_line[MAXLEN] = {0};
    GptFlags flags = 0;

    sprintf(temp, "%sgpttmp", paths.dotdir);
    ftmp = gretl_tempfile_open(temp);
    if (ftmp == NULL) {
	return 1;
    }

    fsrc = open_gp_file(fname, "r");
    if (fsrc == NULL) {
	fclose(ftmp);
	return 1;
    }

    if (action == ADD_PNG) {
	/* see if there's already a png term setting, possibly commented
	   out, that can be reused */
	while (fgets(fline, sizeof fline, fsrc)) {
	    if (!strncmp(fline, "set term png", 12)) {
		strcat(restore_line, fline);
		break;
	    } else if (commented_term_line(fline) && *restore_line == '\0') {
		strcat(restore_line, fline + 2);
		break;
	    } else if (strstr(fline, "letterbox")) {
		flags = GPT_LETTERBOX;
	    } else if (!strncmp(fline, "plot", 4)) {
		break;
	    }
	}

	rewind(fsrc);

	if (*restore_line) {
	    fputs(restore_line, ftmp);
	} else {
	    int ptype = (spec != NULL)? spec->code : PLOT_REGULAR;
	    const char *pline;

	    if (spec != NULL) {
		flags = spec->flags;
	    }
	    pline = get_gretl_png_term_line(ptype, flags);
	    fprintf(ftmp, "%s\n", pline);
	}	    
	fprintf(ftmp, "set output '%sgretltmp.png'\n", 
		paths.dotdir);
    }

    /* now for the body of the plot file */

    if (action == ADD_PNG) {
	int got_set_print = 0;

	while (fgets(fline, sizeof fline, fsrc)) {
	    if (set_print_line(fline)) {
		got_set_print = 1;
		fputs(fline, ftmp);
	    } else if (!commented_term_line(fline) && !set_output_line(fline)) {
		fputs(fline, ftmp);
	    }
	}
	if (gnuplot_has_bbox() && !got_set_print) {
	    print_plot_bounding_box_request(ftmp);
	}
    } else {
	/* we're removing the png term line */
	int printit, png_line_saved = 0;
	
	while (fgets(fline, sizeof fline, fsrc)) {
	    printit = 1;
	    if (!strncmp(fline, "set term png", 12)) {
		if (!png_line_saved) {
		    /* comment it out, for future reference */
		    fprintf(ftmp, "# %s", fline);
		    png_line_saved = 1;
		} 
		printit = 0;
	    } else if (commented_term_line(fline)) {
		if (png_line_saved) {
		    printit = 0;
		}
	    } else if (set_output_line(fline)) {
		printit = 0;
	    } else if (spec != NULL && (spec->flags & GPT_FIT_HIDDEN)
		       && is_auto_fit_string(fline)) {
		printit = 0;
	    } else if (set_print_line(fline)) {
		printit = 0;
	    }
	    if (printit) {
		fputs(fline, ftmp);
	    }
	}
    }

    fclose(fsrc);
    fclose(ftmp);
    remove(fname);

    return rename(temp, fname);
}

static int add_png_term_to_plotfile (const char *fname)
{
    return add_or_remove_png_term(fname, ADD_PNG, NULL);
}

static int remove_png_term_from_plotfile (const char *fname, GPT_SPEC *spec)
{
    return add_or_remove_png_term(fname, REMOVE_PNG, spec);
}

/* public because called from session.c when editing plot commands */

int remove_png_term_from_plotfile_by_name (const char *fname)
{
    return add_or_remove_png_term(fname, REMOVE_PNG, NULL);
}

static void mark_plot_as_saved (GPT_SPEC *spec)
{
    png_plot *plot = (png_plot *) spec->ptr;

    plot->status |= PLOT_SAVED;
}

static int gnuplot_png_init (GPT_SPEC *spec, FILE **fpp)
{
    *fpp = gretl_fopen(spec->fname, "w");

    if (*fpp == NULL) {
	file_write_errbox(spec->fname);
	return 1;
    }

    fprintf(*fpp, "%s\n", get_gretl_png_term_line(spec->code, spec->flags));
    fprintf(*fpp, "set output '%sgretltmp.png'\n", paths.dotdir);

    return 0;
}

int gp_term_code (gpointer p)
{
    GPT_SPEC *spec = (GPT_SPEC *) p;
    const char *s = spec->termtype;

    if (!strncmp(s, "postscript", 10)) 
	return GP_TERM_EPS;
    else if (!strncmp(s, "PDF", 3) || !strncmp(s, "pdf", 3))
	return GP_TERM_PDF;
    else if (!strcmp(s, "fig")) 
	return GP_TERM_FIG;
    else if (!strcmp(s, "latex")) 
	return GP_TERM_TEX;
    else if (!strncmp(s, "png", 3)) 
	return GP_TERM_PNG;
    else if (!strncmp(s, "emf", 3)) 
	return GP_TERM_EMF;
    else if (!strncmp(s, "svg", 3)) 
	return GP_TERM_SVG;
    else if (!strcmp(s, "plot commands")) 
	return GP_TERM_PLT;
    else 
	return GP_TERM_NONE;
}

#define PDF_CAIRO_STRING "pdfcairo font \"sans,5\""

static void 
get_full_term_string (const GPT_SPEC *spec, char *termstr, int *cmds)
{
    if (!strcmp(spec->termtype, "postscript color")) {
	strcpy(termstr, "postscript eps color"); 
    } else if (!strcmp(spec->termtype, "postscript")) {
	strcpy(termstr, "postscript eps"); 
    } else if (!strcmp(spec->termtype, "PDF")) {
	if (gnuplot_pdf_terminal() == GP_PDF_CAIRO) {
	    strcpy(termstr, PDF_CAIRO_STRING);
	} else {
	    strcpy(termstr, "pdf");
	}
    } else if (!strcmp(spec->termtype, "fig")) {
	strcpy(termstr, "fig");
    } else if (!strcmp(spec->termtype, "latex")) {
	strcpy(termstr, "latex");
    } else if (!strncmp(spec->termtype, "png", 3)) { 
	const char *png_str = 
	    get_gretl_png_term_line(spec->code, spec->flags);

	strcpy(termstr, png_str + 9);
    } else if (!strcmp(spec->termtype, "emf color")) {
	const char *emf_str = 
	    get_gretl_emf_term_line(spec->code, 1);

	strcpy(termstr, emf_str + 9);
    } else if (!strcmp(spec->termtype, "plot commands")) {
	strcpy(termstr, spec->termtype);
	*cmds = 1;
    } else {
	strcpy(termstr, spec->termtype);
    }
}

static char *gp_contd_string (char *s)
{
    char *p = strstr(s, ", \\");
    int n = 0;

    if (p != NULL) {
	n = 3;
    } else {
	p = strstr(s, ",\\");
	if (p != NULL) {
	    n = 2;
	}
    }

    if (p != NULL) {
	/* ensure we've really got '\' at end of line */
	char c = *(p+n);

	if (c != '\0' && c != '\n' && c != '\r') {
	    p = NULL;
	}
    }

    return p;
}

static char *get_insert_point (char *s, char *p)
{
    if (p == NULL) {
	p = s + strlen(s) - 1;
    }

    if (p - s > 0 && *(p-1) == ' ') {
	p--;
    }

    return p;
}

/* old gnuplot: can't do "rgb" line spec; we'll do what we can,
   namely switch line type 2 for 3
*/

static void maybe_recolor_line (char *s, int lnum)
{
    const gretlRGB *color = get_graph_color(lnum - 1);

    if (color != NULL) {
	char *contd = gp_contd_string(s);
	char cstr[8];

	print_rgb_hash(cstr, color);

#if GPDEBUG
	fprintf(stderr, "lnum=%d, cstr='%s', rgb=%d\n", lnum, cstr, rgb);
	fprintf(stderr, "s='%s'\n", s);
#endif
    
	if (lnum == 2 && strcmp(cstr, "#00ff00") && !strstr(s, " lt ")) {
	    char *p = get_insert_point(s, contd);

	    *p = '\0';
	    strcpy(p, " lt 3");
	    if (contd != NULL) {
		strcat(s, ", \\\n");
	    } else {
		strcat(s, "\n");
	    }
	} 
    } 
}

static void dataline_check (char *s, int *d)
{
    if (!strncmp(s, "plot \\", 6)) {
	*d = 0;
    } else {
	if (!strncmp(s, "plot ", 5)) {
	    *d = 0;
	}
	if (*d == 0 && !gp_contd_string(s)) {
	    *d = 1;
	}
    }
}

/* for postscript output, e.g. in Latin-2, or EMF output in CP125X */

static int maybe_recode_gp_line (char *s, int ttype, FILE *fp)
{
    int err = 0;

#ifdef ENABLE_NLS    
    if (!gretl_is_ascii(s) && g_utf8_validate(s, -1, NULL)) {
	char *tmp;
	
	if (ttype == GP_TERM_EMF) {
	    tmp = utf8_to_cp(s);
	} else {
	    tmp = utf8_to_latin(s);
	}

	if (tmp == NULL) {
	    err = 1;
	} else {
	    fputs(tmp, fp);
	    free(tmp);
	}
    } else {
	fputs(s, fp);
    }
#else
    fputs(s, fp);
#endif

    return err;
}

#ifdef ENABLE_NLS

/* check for non-ASCII strings in plot file: these may
   require special treatment */

static int non_ascii_gp_file (FILE *fp)
{
    char pline[512];
    int dataline = -1;
    int ret = 0;

    while (fgets(pline, sizeof pline, fp) && dataline <= 0) {
	if (set_print_line(pline)) {
	    break;
	}
	if (*pline == '#') {
	    continue;
	}
	if (!gretl_is_ascii(pline)) {
	    ret = 1;
	    break;
	}
	dataline_check(pline, &dataline);
    }

    rewind(fp);

    return ret;
}

static int term_uses_utf8 (int ttype)
{
    if (ttype == GP_TERM_PNG || 
	ttype == GP_TERM_SVG ||
	ttype == GP_TERM_PLT) {
	return 1;
    } else if (ttype == GP_TERM_PDF && 
	       gnuplot_pdf_terminal() == GP_PDF_CAIRO) {
	return 1;
    } else {
	return 0;
    }
}

#endif

#define is_color_line(s) (strstr(s, "set style line") && strstr(s, "rgb"))

void filter_gnuplot_file (int ttype, int latin, int mono, int recolor, 
			  FILE *fpin, FILE *fpout)
{
    char pline[512];
    int dataline = -1;
    int lnum = -1;
    int err = 0;

    while (fgets(pline, sizeof pline, fpin)) {
	if (set_print_line(pline)) {
	    break;
	}

	if (!strncmp(pline, "set term", 8) ||
	    !strncmp(pline, "set enco", 8) ||
	    !strncmp(pline, "set outp", 8)) {
	    continue;
	}

	if (mono) {
	    if (is_color_line(pline)) {
		continue;
	    } else if (strstr(pline, "set style fill solid")) {
		fputs("set style fill solid 0.3\n", fpout);
		continue;
	    }
	}

	if (recolor) {
	    if (!strncmp(pline, "plot ", 5)) {
		lnum = 0;
	    } else if (lnum >= 0) {
		lnum++;
	    }
	    if (lnum > 0 && dataline <= 0) {
		maybe_recolor_line(pline, lnum);
	    }
	}

	if (latin && dataline <= 0 && *pline != '#') {
	    err += maybe_recode_gp_line(pline, ttype, fpout);
	    if (err == 1) {
		gui_errmsg(err);
	    }
	} else {
	    fputs(pline, fpout);
	} 

	dataline_check(pline, &dataline);
    }
}

/* for non-UTF-8 plot formats: print a "set encoding" string
   if appropriate, but only if gnuplot won't choke on it.
*/

static void maybe_print_gp_encoding (int ttype, int latin, FILE *fp)
{
    if (ttype == GP_TERM_EMF) {
	if (latin == 2 && gnuplot_has_cp1250()) {
	    fputs("set encoding cp1250\n", fp);
	} else if (latin == 9 && gnuplot_has_cp1254()) {
	    fputs("set encoding cp1254\n", fp);
	}
    } else {
	if (latin != 1 && latin != 2 && latin != 15 && latin != 9) {
	    /* unsupported by gnuplot */
	    latin = 0;
	}
	if (latin == 9 && !gnuplot_has_latin5()) {
	    /* Turkish not supported */
	    latin = 0;
	}
	if (latin) {
	    fprintf(fp, "set encoding iso_8859_%d\n", latin);
	}
    }
} 

static int revise_plot_file (const char *inname, 
			     const char *pltname,
			     const char *outtarg,
			     const char *term)
{
    FILE *fpin = NULL;
    FILE *fpout = NULL;
    int ttype = 0, latin = 0;
    int mono = 0, recolor = 0;
    int err = 0;

    fpin = gretl_fopen(inname, "r");
    if (fpin == NULL) {
	file_read_errbox(inname);
	return 1;
    }

    fpout = gretl_fopen(pltname, "w");
    if (fpout == NULL) {
	fclose(fpin);
	file_write_errbox(pltname);
	return 1;
    }

    if (!strncmp(term, "emf", 3)) {
	ttype = GP_TERM_EMF;
    } else if (!strncmp(term, "png", 3)) {
	ttype = GP_TERM_PNG;
    } else if (!strncmp(term, "post", 4)) {
	ttype = GP_TERM_EPS;
    } else if (!strncmp(term, "pdf", 3)) {
	ttype = GP_TERM_PDF;
    } else if (strstr(term, "commands")) {
	ttype = GP_TERM_PLT;
    }

#ifdef ENABLE_NLS
    if (non_ascii_gp_file(fpin)) {
	/* plot contains UTF-8 strings */
	if (!term_uses_utf8(ttype)) {
	    latin = iso_latin_version();
	    maybe_print_gp_encoding(ttype, latin, fpout);
	} else if (gnuplot_has_utf8()) {
	    fputs("set encoding utf8\n", fpout);
	}
    }
#endif

    if (outtarg != NULL && *outtarg != '\0') {
	fprintf(fpout, "set term %s\n", term);
	fprintf(fpout, "set output '%s'\n", outtarg);
    }	

    if (strstr(term, " mono") || 
	(strstr(term, "postscr") && !strstr(term, "color"))) {
	mono = 1;
    }    

    if (!mono && (ttype == GP_TERM_EPS || ttype == GP_TERM_PDF)
	&& !gnuplot_has_rgb()) {
	recolor = 1;
    }

    filter_gnuplot_file(ttype, latin, mono, recolor, fpin, fpout);

    fclose(fpin);
    fclose(fpout);

    return err;
}

void save_graph_to_file (gpointer data, const char *fname)
{
    GPT_SPEC *spec = (GPT_SPEC *) data;
    char term[MAXLEN];
    char pltname[MAXLEN];
    int cmds = 0;
    int err = 0;

    get_full_term_string(spec, term, &cmds);

    if (cmds) {
	/* saving plot commands to file */
	strcpy(pltname, fname);
	err = revise_plot_file(spec->fname, pltname, NULL, term);
    } else {
	/* saving some form of gnuplot output */
	build_path(pltname, paths.dotdir, "gptout.tmp", NULL);
	err = revise_plot_file(spec->fname, pltname, fname, term);
    }

    if (!err && !cmds) {
	gchar *plotcmd;

	plotcmd = g_strdup_printf("\"%s\" \"%s\"", paths.gnuplot, 
				  pltname);
	err = gretl_spawn(plotcmd);
	remove(pltname);
	g_free(plotcmd);
	if (err) {
	    gui_errmsg(err);
	} 
    }
}

#define GRETL_PDF_TMP "gretltmp.pdf"

static void graph_display_pdf (GPT_SPEC *spec)
{
    char pdfname[FILENAME_MAX];
    char plttmp[FILENAME_MAX];
    static char term[32];
    gchar *plotcmd;
    int err = 0;

    if (*term == '\0') {
	if (gnuplot_pdf_terminal() == GP_PDF_CAIRO) {
	    fprintf(stderr, "gnuplot: using pdfcairo driver\n");
	    strcpy(term, PDF_CAIRO_STRING);
	} else {
	    strcpy(term, "pdf");
	}
    }

    build_path(plttmp, paths.dotdir, "gptout.tmp", NULL);
    build_path(pdfname, paths.dotdir, GRETL_PDF_TMP, NULL);

    err = revise_plot_file(spec->fname, plttmp, pdfname, term);
    if (err) {
	return;
    }

    plotcmd = g_strdup_printf("\"%s\" \"%s\"", paths.gnuplot, plttmp);
    err = gretl_spawn(plotcmd);
    remove(plttmp);
    g_free(plotcmd);

    if (err) {
	gui_errmsg(err);
	return;
    } 

#if defined(G_OS_WIN32)
    win32_open_file(pdfname);
#elif defined(OSX_BUILD)
    osx_open_file(pdfname);
#else
    gretl_fork("viewpdf", pdfname);
#endif
}

/* dump_plot_buffer: this is used when we're taking the material from
   an editor window containing gnuplot commands, and either (a)
   sending it to gnuplot for execution, or (b) saving it to "user
   file".  There's a question over what we should do with non-ascii
   strings in the plot file.  These will be in UTF-8 in the GTK editor
   window.  It seems that the best thing is to determine the character
   set for the current locale (using g_get_charset) and if it is not
   UTF-8, recode to the locale.  This won't be right in all cases, but
   I'm not sure how we could do better.

   It might perhaps be worth offering a dialog box with a choice of
   encodings, but the user would have to be quite knowledgeable
   to make sense of this.  AC, 2008-01-10.
*/

int dump_plot_buffer (const char *buf, const char *fname,
		      int addpause)
{
    FILE *fp;
    int gotpause = 0;
    int done, recode = 0;
    char bufline[512];
#ifdef ENABLE_NLS
    const gchar *cset;
    gchar *trbuf;
#endif

    fp = gretl_fopen(fname, "w");
    if (fp == NULL) {
	file_write_errbox(fname);
	return E_FOPEN;
    }

#ifdef ENABLE_NLS
    recode = !g_get_charset(&cset);
#endif

    bufgets_init(buf);

    while (bufgets(bufline, sizeof bufline, buf)) {
	done = 0;
#ifdef ENABLE_NLS
	if (recode) {
	    trbuf = gp_locale_from_utf8(bufline);
	    if (trbuf != NULL) {
		fputs(trbuf, fp);
		g_free(trbuf);
		done = 1;
	    }
	}
#endif
	if (!done) {
	    fputs(bufline, fp);
	}
	if (addpause && strstr(bufline, "pause -1")) {
	    gotpause = 1;
	}
    }

    bufgets_finalize(buf);

#ifdef G_OS_WIN32
    /* sending directly to gnuplot on MS Windows */
    if (addpause && !gotpause) {
        fprintf(stderr, "adding 'pause -1'\n");
	fputs("pause -1\n", fp);
    }
#endif

    fclose(fp);

    return 0;
}

#ifdef G_OS_WIN32

static void real_send_to_gp (const char *tmpfile)
{
    gchar *cmd;
    int err;

    cmd = g_strdup_printf("\"%s\" \"%s\"", paths.gnuplot, tmpfile);
    err = (WinExec(cmd, SW_SHOWNORMAL) < 32);
    g_free(cmd);

    if (err) {
	win_show_last_error();
    }
}

#else

#include <sys/types.h>
#include <sys/wait.h>

static void real_send_to_gp (const char *tmpfile)
{
    GError *error = NULL;
    gchar *argv[4];
    GPid pid = 0;
    gint fd = -1;
    gboolean run;

    argv[0] = g_strdup(paths.gnuplot);
    argv[1] = g_strdup("-persist");
    argv[2] = g_strdup(tmpfile);
    argv[3] = NULL;

    run = g_spawn_async_with_pipes(NULL, argv, NULL, 
				   G_SPAWN_SEARCH_PATH | 
				   G_SPAWN_DO_NOT_REAP_CHILD,
				   NULL, NULL, &pid, NULL, NULL,
				   &fd, &error);

    if (error != NULL) {
	errbox(error->message);
	g_error_free(error);
    } else if (!run) {
	errbox(_("gnuplot command failed"));
    } else if (pid > 0) {
	int status = 0, err = 0;

	/* bodge below: try to give gnuplot time to bomb
	   out, if it's going to -- but we don't want to
	   hold things up if we're doing OK */
	
	sleep(1);
	waitpid(pid, &status, WNOHANG);
	if (WIFEXITED(status)) {
	    err = WEXITSTATUS(status);
	} 

	if (err && fd > 0) {
	    char buf[128] = {0};

	    if (read(fd, buf, 127) > 0) {
		errbox(buf);
	    }
	}
    }

    if (fd > 0) {
	close(fd);
    }

    g_spawn_close_pid(pid);

    g_free(argv[0]);
    g_free(argv[1]);
    g_free(argv[2]);
}

#endif

/* callback for execute icon in window editing gnuplot
   commands */

void gp_send_callback (GtkWidget *w, windata_t *vwin)
{
#ifdef G_OS_WIN32
    int addpause = 1;
#else
    int addpause = 0;
#endif
    gchar *tmpfile;
    char *buf;
    int err = 0;

    buf = textview_get_text(vwin->w);
    if (buf == NULL) {
	return;
    }

    tmpfile = g_strdup_printf("%showtmp.gp", paths.dotdir);
    err = dump_plot_buffer(buf, tmpfile, addpause);
    g_free(buf);

    if (!err) {
	real_send_to_gp(tmpfile);
    }   

    remove(tmpfile);
    g_free(tmpfile);
}

#ifdef G_OS_WIN32

/* common code for sending an EMF file to the clipboard,
   or printing an EMF, on MS Windows */

static void win32_process_graph (GPT_SPEC *spec, int color, int dest)
{
    char emfname[FILENAME_MAX];
    char plttmp[FILENAME_MAX];
    gchar *plotcmd;
    const char *term;
    int err = 0;

    build_path(plttmp, paths.dotdir, "gptout.tmp", NULL);
    build_path(emfname, paths.dotdir, "gpttmp.emf", NULL);

    term = get_gretl_emf_term_line(spec->code, color);
    if (!strncmp(term, "set term ", 9)) {
	term += 9;
    }

    err = revise_plot_file(spec->fname, plttmp, emfname, term);
    if (err) {
	return;
    }

    plotcmd = g_strdup_printf("\"%s\" \"%s\"", paths.gnuplot, plttmp);
    err = winfork(plotcmd, NULL, SW_SHOWMINIMIZED, 0);
    g_free(plotcmd);
    remove(plttmp);
    
    if (err) {
        errbox(_("Gnuplot error creating graph"));
    } else if (dest == WIN32_TO_CLIPBOARD) {
	err = emf_to_clipboard(emfname);
    } else if (dest == WIN32_TO_PRINTER) {
	err = winprint_graph(emfname);
    }

    remove(emfname);
}

#endif

/* chop trailing comma, if present; return 1 if comma chopped,
   zero otherwise */

static int chop_comma (char *str)
{
    size_t i, n = strlen(str);

    for (i=n-1; i>0; i--) {
	if (isspace((unsigned char) str[i])) {
	    continue;
	}
	if (str[i] == ',') {
	    str[i] = 0;
	    return 1;
	} else {
	    break;
	}
    }
		
    return 0;
}

static int get_gpt_marker (const char *line, char *label)
{
    const char *p = strchr(line, '#');
    char format[6];

    if (p != NULL) {
	sprintf(format, "%%%ds", OBSLEN - 1);
	sscanf(p + 1, format, label);
#if GPDEBUG > 1
	fprintf(stderr, "read marker: '%s'\n", label);
#endif
	return 0;
    }

    return 1;
}

/* special graphs for which editing via GUI is not supported */

#define cant_edit(p) (p == PLOT_CORRELOGRAM || \
                      p == PLOT_LEVERAGE || \
                      p == PLOT_MULTI_IRF || \
                      p == PLOT_MULTI_SCATTER || \
                      p == PLOT_PANEL || \
                      p == PLOT_TRI_GRAPH || \
                      p == PLOT_BI_GRAPH || \
                      p == PLOT_VAR_ROOTS || \
		      p == PLOT_ELLIPSE)

/* graphs where we don't attempt to find data coordinates */

#define no_readback(p) (p == PLOT_CORRELOGRAM || \
                        p == PLOT_LEVERAGE || \
                        p == PLOT_MULTI_IRF || \
                        p == PLOT_MULTI_SCATTER || \
                        p == PLOT_PANEL || \
                        p == PLOT_TRI_GRAPH || \
                        p == PLOT_BI_GRAPH)

static int get_gpt_data (GPT_SPEC *spec, int do_markers, FILE *fp)
{
    char s[MAXLEN];
    char *got;
    double *x[4] = { NULL };
    char test[4][32];
    int started_data_lines = 0;
    int i, j, t;
    int err = 0;

    spec->okobs = spec->nobs;

    gretl_push_c_numeric_locale();

    for (i=0; i<spec->n_lines && !err; i++) {
	int okobs = spec->nobs;
	int offset = 1;

	if (spec->lines[i].ncols == 0) {
	    continue;
	}

	if (!started_data_lines) {
	    offset = 0;
	    x[0] = spec->data;
	    x[1] = x[0] + spec->nobs;
	    started_data_lines = 1;
	} 

	x[2] = x[1] + spec->nobs;
	x[3] = x[2] + spec->nobs;	

	for (t=0; t<spec->nobs; t++) {
	    int missing = 0;
	    int nf = 0;

	    got = fgets(s, sizeof s, fp);
	    if (got == NULL) {
		err = 1;
		break;
	    }

	    nf = 0;
	    if (spec->lines[i].ncols == 4) {
		nf = sscanf(s, "%31s %31s %31s %31s", test[0], test[1], test[2], test[3]);
	    } else if (spec->lines[i].ncols == 3) {
		nf = sscanf(s, "%31s %31s %31s", test[0], test[1], test[2]);
	    } else if (spec->lines[i].ncols == 2) {
		nf = sscanf(s, "%31s %31s", test[0], test[1]);
	    }

	    if (nf != spec->lines[i].ncols) {
		err = 1;
	    }

	    for (j=offset; j<nf; j++) {
		if (test[j][0] == '?') {
		    x[j][t] = NADBL;
		    missing++;
		} else {
		    x[j][t] = atof(test[j]);
		}
	    }

	    if (missing) {
		okobs--;
	    }

	    if (i == 0 && do_markers) {
		get_gpt_marker(s, spec->markers[t]);
	    }
	}

	if (okobs < spec->okobs) {
	    spec->okobs = okobs;
	} 

	/* trailer line for data block */
	fgets(s, sizeof s, fp);

	/* shift 'y' writing location */
	x[1] += (spec->lines[i].ncols - 1) * spec->nobs;
    }

    gretl_pop_c_numeric_locale();

    return err;
}

/* read a gnuplot source line specifying a text label */

static int parse_label_line (GPT_SPEC *spec, const char *line, int i)
{
    const char *p, *s;
    double x, y;
    int nc, q2 = 0;
    int textread = 0;

    /* set label "this is a label" at 1998.26,937.557 left front */
    /* set label 'foobar' at 1500,350 left */

    if (i >= MAX_PLOT_LABELS) {
	return 1;
    }

    plotspec_label_init(&(spec->labels[i]));

    /* find first single or double quote */
    p = strchr(line, '\'');
    if (p == NULL) {
	p = strchr(line, '"');
	if (p == NULL) {
	    return 1;
	}
	q2 = 1;
    }

    p++;
    s = p;

    /* get the label text */
    while (*s) {
	if (q2) {
	    if (*s == '"' && *(s-1) != '\\') {
		textread = 1;
	    }
	} else if (*s == '\'') {
	    textread = 1;
	}

	if (textread) {
	    int len = s - p;

	    if (len > PLOT_LABEL_TEXT_LEN) {
		len = PLOT_LABEL_TEXT_LEN;
	    }
	    strncat(spec->labels[i].text, p, len);
	    break;
	}

	s++;
    }

    if (!textread) {
	return 1;
    }

    /* get the position */
    p = strstr(s, "at");
    if (p == NULL) {
	spec->labels[i].text[0] = '\0';
	return 1;
    }

    p += 2;

    gretl_push_c_numeric_locale();
    nc = sscanf(p, "%lf,%lf", &x, &y);
    gretl_pop_c_numeric_locale();

    if (nc != 2) {
	spec->labels[i].text[0] = '\0';
	return 1;
    }

    spec->labels[i].pos[0] = x;
    spec->labels[i].pos[1] = y;

    /* justification */
    if (strstr(p, "right")) {
	spec->labels[i].just = GP_JUST_RIGHT;
    } else if (strstr(p, "center")) {
	spec->labels[i].just = GP_JUST_CENTER;
    } 

    return 0;
}

static int 
read_plotspec_range (const char *obj, const char *s, GPT_SPEC *spec)
{
    double r0, r1;
    int i = 0, err = 0;

    if (!strcmp(obj, "xrange")) {
	i = 0;
    } else if (!strcmp(obj, "yrange")) {
	i = 1;
    } else if (!strcmp(obj, "y2range")) {
	i = 2;
    } else if (!strcmp(obj, "trange")) {
	i = 3;
    } else {
	err = 1;
    }

    if (!strcmp(s, "[*:*]")) {
	r0 = r1 = NADBL;
    } else {
	gretl_push_c_numeric_locale();
	if (!err && sscanf(s, "[%lf:%lf]", &r0, &r1) != 2) {
	    err = 1;
	}
	gretl_pop_c_numeric_locale();
    }

    if (!err) {
	spec->range[i][0] = r0;
	spec->range[i][1] = r1;
    }

    return err;
}

static int read_plot_logscale (const char *s, GPT_SPEC *spec)
{
    char axis[3] = {0};
    double base = 0;
    int i, n, err = 0;

    n = sscanf(s, "%2s %lf", axis, &base);

    if (n < 1 || (n == 2 && base < 1.1)) {
	err = 1;
    } else {
	if (n == 1) {
	    base = 10.0;
	}
	if (!strcmp(axis, "x")) {
	    i = 0;
	} else if (!strcmp(axis, "y")) {
	    i = 1;
	} else if (!strcmp(axis, "y2")) {
	    i = 2;
	} else {
	    err = 1;
	}
    }
    
    if (!err) {
	spec->logbase[i] = base;
    }

    return err;
}

static int catch_value (char *targ, const char *src, int maxlen)
{
    int i, n;

    src += strspn(src, " \t\r\n");
    if (*src == '\'' || *src == '"') {
	src++;
    }

    *targ = '\0';

    if (*src != '\0') {
	strncat(targ, src, maxlen - 1);
	n = strlen(targ);

	for (i=n-1; i>=0; i--) {
	    if (isspace((unsigned char) targ[i])) {
		targ[i] = '\0';
	    } else {
		break;
	    }
	}  
	if (targ[i] == '\'' || targ[i] == '"') {
	    targ[i] = '\0';
	}
    }

    return (*targ != '\0');
}

static int parse_gp_set_line (GPT_SPEC *spec, const char *s, int *labelno)
{
    char variable[16] = {0};
    char value[MAXLEN] = {0};

    if (strstr(s, "encoding") != NULL) {
#if 0
	fprintf(stderr, "Got encoding: '%s'\n", s);
#endif
	return 0;
    }

    if (sscanf(s + 4, "%11s", variable) != 1) {
	errbox(_("Failed to parse gnuplot file"));
	fprintf(stderr, "parse_gp_set_line: bad line '%s'\n", s);
	return 1;
    }

    if (!strcmp(variable, "y2tics")) {
	spec->flags |= GPT_Y2AXIS;
	return 0;
    } else if (!strcmp(variable, "border 3")) {
	spec->flags |= GPT_MINIMAL_BORDER;
	return 0;
    } else if (!strcmp(variable, "parametric")) {
	spec->flags |= GPT_PARAMETRIC;
	return 0;
    } else if (!strcmp(variable, "xzeroaxis")) {
	spec->flags |= GPT_XZEROAXIS;
	return 0;
    } else if (!strcmp(variable, "yzeroaxis")) {
	spec->flags |= GPT_YZEROAXIS;
	return 0;
    } else if (!strcmp(variable, "nokey")) {
	strcpy(spec->keyspec, "none");
	return 0;
    } else if (!strcmp(variable, "label")) {
	parse_label_line(spec, s, *labelno);
	*labelno += 1;
	return 0;
    }

    if (!catch_value(value, s + 4 + strlen(variable), MAXLEN)) {
	return 0;
    }

    if (strstr(variable, "range")) {
	if (read_plotspec_range(variable, value, spec)) {
	    errbox(_("Failed to parse gnuplot file"));
	    fprintf(stderr, "parse_gp_set_line: bad line '%s'\n", s);
	    return 1;
	}
    } else if (!strcmp(variable, "logscale")) {
	if (read_plot_logscale(value, spec)) {
	    errbox(_("Failed to parse gnuplot file"));
	    fprintf(stderr, "parse_gp_set_line: bad line '%s'\n", s);
	    return 1;
	}	
    } else if (!strcmp(variable, "title")) {
	strcpy(spec->titles[0], value);
    } else if (!strcmp(variable, "xlabel")) {
	strcpy(spec->titles[1], value);
	*spec->xvarname = '\0';
	strncat(spec->xvarname, value, MAXDISP-1);
    } else if (!strcmp(variable, "ylabel")) {
	strcpy(spec->titles[2], value);
	*spec->yvarname = '\0';
	strncat(spec->yvarname, value, MAXDISP-1);
    } else if (!strcmp(variable, "y2label")) {
	strcpy(spec->titles[3], value);
    } else if (!strcmp(variable, "key")) {
	strcpy(spec->keyspec, value);
    } else if (!strcmp(variable, "xtics")) { 
	safecpy(spec->xtics, value, 15);
    } else if (!strcmp(variable, "mxtics")) { 
	safecpy(spec->mxtics, value, 3);
    } else if (!strcmp(variable, "boxwidth")) {
	spec->boxwidth = (float) atof(value);
    } else if (!strcmp(variable, "samples")) {
	spec->samples = atoi(value);
    } 

    return 0;
}

/* allocate markers for identifying particular data points */

static int allocate_plotspec_markers (GPT_SPEC *spec)
{
    int i, j;

    spec->markers = mymalloc(spec->nobs * sizeof *spec->markers);
    if (spec->markers == NULL) {
	return 1;
    }

    for (i=0; i<spec->nobs; i++) {
	spec->markers[i] = malloc(OBSLEN);
	if (spec->markers[i] == NULL) {
	    for (j=0; j<i; j++) {
		free(spec->markers[j]);
	    }
	    free(spec->markers);
	    spec->n_markers = 0;
	    return 1;
	}
	spec->markers[i][0] = 0;
    }

    spec->n_markers = spec->nobs;

    return 0;
}

/* Determine the number of data points in a plot.  While we're at it,
   determine the type of plot, and check whether there are any
   data-point markers along with the data.
*/

static int get_plot_nobs (FILE *fp, PlotType *ptype, int *do_markers)
{
    int n = 0, started = -1;
    char line[MAXLEN], label[9];
    char *p;

    *ptype = PLOT_REGULAR;
    *do_markers = 0;

    while (fgets(line, MAXLEN - 1, fp)) {

	if (*line == '#' && *ptype == PLOT_REGULAR) {
	    tailstrip(line);
	    *ptype = plot_type_from_string(line);
	}

	if (!strncmp(line, "plot", 4)) {
	    started = 0;
	}

	if (started == 0 && strchr(line, '\\') == NULL) {
	    started = 1;
	    continue;
	}

	if (started == 1) {
	    if (*do_markers == 0 && (p = strchr(line, '#')) != NULL) {
		if (sscanf(p + 1, "%8s", label) == 1) {
		    *do_markers = 1;
		}
	    }
	    if (*line == 'e') {
		break;
	    }
	    n++;
	}
    }

    return n;
}

static int grab_fit_coeffs (GPT_SPEC *spec, const char *s)
{
    int n, err = 0;

    if (spec->fit == PLOT_FIT_OLS) {
	spec->b_ols = gretl_column_vector_alloc(2);
	if (spec->b_ols == NULL) {
	    err = E_ALLOC;
	} else {
	    gretl_push_c_numeric_locale();
	    n = sscanf(s, "%lf + %lf", &spec->b_ols->val[0],
		       &spec->b_ols->val[1]);
	    gretl_pop_c_numeric_locale();
	    if (n != 2) {
		gretl_matrix_free(spec->b_ols);
		spec->b_ols = NULL;
		err = E_DATA;
	    }
	}
    } else if (spec->fit == PLOT_FIT_INVERSE) {
	spec->b_inv = gretl_column_vector_alloc(2);
	if (spec->b_inv == NULL) {
	    err = E_ALLOC;
	} else {
	    gretl_push_c_numeric_locale();
	    n = sscanf(s, "%lf + %lf", &spec->b_inv->val[0],
		       &spec->b_inv->val[1]);
	    gretl_pop_c_numeric_locale();
	    if (n != 2) {
		gretl_matrix_free(spec->b_inv);
		spec->b_inv = NULL;
		err = E_DATA;
	    }
	}
    } else if (spec->fit == PLOT_FIT_QUADRATIC) {
	spec->b_quad = gretl_column_vector_alloc(3);
	if (spec->b_quad == NULL) {
	    err = E_ALLOC;
	} else {
	    gretl_push_c_numeric_locale();
	    n = sscanf(s, "%lf + %lf*X + %lf", &spec->b_quad->val[0],
		       &spec->b_quad->val[1], &spec->b_quad->val[2]);
	    gretl_pop_c_numeric_locale();
	    if (n != 3) {
		gretl_matrix_free(spec->b_quad);
		spec->b_quad = NULL;
		err = E_DATA;
	    }
	}
    }	

    if (err) {
	spec->flags &= ~GPT_AUTO_FIT;
    }

    return err;
}

/* scan the stuff after "title '" or 'title "' */

static void grab_line_title (char *targ, const char *src)
{
    char *fmt;

    if (*src == '\'') {
	fmt = "%79[^']'";
    } else {
	fmt = "%79[^\"]\"";
    }

    sscanf(src + 1, fmt, targ);
}

/* parse the "using..." portion of plot specification for a
   given plot line: full form is like:
  
     using XX axes XX title XX w XX lt XX lw XX
*/

static int parse_gp_line_line (const char *s, GPT_SPEC *spec)
{
    const char *p;
    int i, err;

    err = plotspec_add_line(spec);
    if (err) {
	return err;
    }

    i = spec->n_lines - 1;

    if ((p = strstr(s, " using "))) {
	/* data column spec */
	p += 7;
	if (strstr(p, "1:2:3:4")) {
	    spec->lines[i].ncols = 4;
	} else if (strstr(p, "1:2:3")) {
	    spec->lines[i].ncols = 3;
	} else if ((p = strstr(s, "($2*"))) {
	    sscanf(p + 4, "%7[^)]", spec->lines[i].scale);
	    spec->lines[i].ncols = 2;
	} else {
	    spec->lines[i].ncols = 2;
	}
	if (spec->lines[i].scale[0] == '\0') {
	    strcpy(spec->lines[i].scale, "1.0");
	}
    } else {
	/* absence of "using" means the line plots a formula, not a
	   set of data columns */
	strcpy(spec->lines[i].scale, "NA");
	/* get the formula: it runs up to "title" or "notitle" */
	p = strstr(s, " title");
	if (p == NULL) {
	    p = strstr(s, " notitle");
	}
	if (p != NULL) {
	    strncat(spec->lines[i].formula, s, p - s);
	    if (i == 1 && spec->flags & GPT_AUTO_FIT) {
		grab_fit_coeffs(spec, spec->lines[i].formula);
	    }
	}
    }

    if (strstr(s, "axes x1y2")) {
	spec->lines[i].yaxis = 2;
    } 

    if ((p = strstr(s, " title "))) {
	grab_line_title(spec->lines[i].title, p + 7);
    }

    if ((p = strstr(s, " w "))) {
	sscanf(p + 3, "%15[^, ]", spec->lines[i].style);
    } 

    if ((p = strstr(s, " lt "))) {
	sscanf(p + 4, "%d", &spec->lines[i].type);
    } 

    if ((p = strstr(s, " lw "))) {
	sscanf(p + 4, "%d", &spec->lines[i].width);
    } 

    if (spec->lines[i].ncols == 0 && spec->lines[i].formula[0] == '\0') {
	/* got neither data column spec nor formula */
	err = 1;
    }

    return err;
}

static int plot_ols_var_ok (const char *vname, int v)
{
    int vi = varindex(datainfo, vname);

    if (vi <= datainfo->v && !strcmp(datainfo->varname[vi], vname)) {
	return 1;
    }

    return 0;
}

static void maybe_set_all_markers_ok (GPT_SPEC *spec)
{
    if (spec->n_lines <= 2 &&
	spec->lines[0].ncols == 2 &&
	(spec->n_lines == 1 || spec->lines[1].ncols == 0) &&
	spec->n_markers > 0) {
	spec->flags |= GPT_ALL_MARKERS_OK;
    } else {
	spec->flags &= ~GPT_ALL_MARKERS_OK;
    }
}

static void maybe_set_add_fit_ok (GPT_SPEC *spec)
{
    if (spec->n_lines == 2 && spec->fit != PLOT_FIT_NONE) {
	; /* already OK */
    } else if (spec->data != NULL &&
	spec->code == PLOT_REGULAR &&
	spec->n_lines == 1 &&
	spec->lines[0].ncols == 2 &&
	!(spec->flags & (GPT_IMPULSES|GPT_LINES|GPT_RESIDS|GPT_TS))) {
	spec->fit = PLOT_FIT_NONE;
    } else {
	spec->fit = PLOT_FIT_NA;
    }
}

static int 
plot_get_data_and_markers (GPT_SPEC *spec, FILE *fp, int datacols, 
			   int do_markers)
{
    int err = 0;

#if GPDEBUG
    fprintf(stderr, "allocating: nobs=%d, datacols=%d, size=%d\n", 
	    spec->nobs, datacols, spec->nobs * datacols * sizeof *spec->data);
#endif  

    /* allocate for the plot data... */
    spec->data = mymalloc(spec->nobs * datacols * sizeof *spec->data);
    if (spec->data == NULL) {
	err = 1;
    }

    /* and markers if any */
    if (!err && do_markers) {
	if (allocate_plotspec_markers(spec)) {
	    free(spec->data);
	    spec->data = NULL;
	    err = 1;
	}
    }

    /* Read the data (and perhaps markers) from the plot file */
    if (!err) {
	err = get_gpt_data(spec, do_markers, fp);
    }

#if GPDEBUG
    fprintf(stderr, "plot_get_data_and_markers:\n"
	    " spec->data = %p, spec->markers = %p, spec->n_markers = %d, err = %d\n",
	    spec->data, (void *) spec->markers, spec->n_markers, err);
#endif

    return err;
}

static int uneditable_get_markers (GPT_SPEC *spec, FILE *fp, int *polar)
{
    char line[256];
    long offset = 0;
    int gotit = 0;
    int err = 0;

    rewind(fp);

    /* advance to the right line (with data plus markers) */
    while (fgets(line, sizeof line, fp)) {
	if ((isdigit(*line) || *line == '-') && strchr(line, '#')) {
	    gotit = 1;
	    break;
	} else if (strstr(line, "set polar")) {
	    *polar = 1;
	}
	offset = ftell(fp);
    }

    if (!gotit) {
	return 1;
    } else {
	fseek(fp, offset, SEEK_SET);
    }

    err = plotspec_add_line(spec);
    
    if (!err) {
	spec->lines[0].ncols = 2;
	err = plot_get_data_and_markers(spec, fp, 2, 1);
    }

    if (!err) {
	maybe_set_all_markers_ok(spec);
    }

    return err;
}

static FitType recognize_fit_string (const char *s)
{
    if (strstr(s, "OLS")) {
	return PLOT_FIT_OLS;
    } else if (strstr(s, "quadratic")) {
	return PLOT_FIT_QUADRATIC;
    } else if (strstr(s, "inverse")) {
	return PLOT_FIT_INVERSE;
    } else if (strstr(s, "loess")) {
	return PLOT_FIT_LOESS;
    } else {
	return PLOT_FIT_NONE;
    }
}

#define plot_needs_obs(c) (c != PLOT_ELLIPSE && c != PLOT_PROB_DIST)

/* Read plotspec struct from gnuplot command file.  This is _not_ a
   general parser for gnuplot files; it is designed specifically for
   files auto-generated by gretl.
*/

static int read_plotspec_from_file (GPT_SPEC *spec, int *plot_pd, int *polar)
{
    int i, done, labelno;
    int do_markers = 0;
    int datacols = 0;
    int reglist[4] = {0};
    char gpline[MAXLEN];
    char *got = NULL;
    FILE *fp;
    int err = 0;

#if GPDEBUG
    fprintf(stderr, "read_plotspec_from_file: spec=%p\n", 
	    (void *) spec);
#endif

    /* check: are we already done? */
    if (PLOTSPEC_DETAILS_IN_MEMORY(spec)) {
#if GPDEBUG
	fprintf(stderr, " info already in memory, returning 0\n");
#endif
	return 0;
    }

    /* open the plot file */
    fp = gretl_fopen(spec->fname, "r");
    if (fp == NULL) {
	file_read_errbox(spec->fname);
	return 1;
    }

    /* get the number of data-points, plot type, and check for markers */
    spec->nobs = get_plot_nobs(fp, &spec->code, &do_markers);
    if (spec->nobs == 0 && plot_needs_obs(spec->code)) {
	/* failed reading plot data */
#if GPDEBUG
	fprintf(stderr, " got spec->nobs = 0\n");
#endif
	fclose(fp);
	return 1;
    }

    if (spec->nobs > MAX_MARKERS && do_markers) {
	do_markers = 0;
    }

    if (cant_edit(spec->code)) {
	fprintf(stderr, "read_plotspec_from_file: plot is not editable\n");
	if (do_markers) {
	    uneditable_get_markers(spec, fp, polar);
	}
	fclose(fp);
	return 0;
    }

    rewind(fp);

    /* get the preamble and "set" lines */
    labelno = 0;
    while ((got = fgets(gpline, sizeof gpline, fp))) {
	char vname[VNAMELEN];
	int v;

	if (!strncmp(gpline, "# timeseries", 12)) {
	    if (sscanf(gpline, "# timeseries %d", &spec->pd)) {
		*plot_pd = spec->pd;
	    }
	    spec->flags |= GPT_TS;
	    if (strstr(gpline, "letterbox")) {
		spec->flags |= GPT_LETTERBOX;
	    }
	    continue;
	} else if (!strncmp(gpline, "# multiple timeseries", 21)) {
	    if (sscanf(gpline, "# multiple timeseries %d", &spec->pd)) {
		*plot_pd = spec->pd;
	    }
	    spec->flags |= GPT_TS;
	    continue;
	}	    

	if (sscanf(gpline, "# X = '%15[^\']' (%d)", vname, &v) == 2) {
	    if (plot_ols_var_ok(vname, v)) {
		reglist[2] = v;
	    }
	    continue;
	} else if (sscanf(gpline, "# Y = '%15[^\']' (%d)", vname, &v) == 2) {
	    if (reglist[2] > 0 && plot_ols_var_ok(vname, v)) {
		reglist[0] = 3;
		reglist[1] = v;
	    }
	    continue;
	}
	
	if (sscanf(gpline, "# literal lines = %d", &spec->n_literal)) {
	    spec->literal = strings_array_new(spec->n_literal);
	    if (spec->literal == NULL) {
		err = E_ALLOC;
		goto plot_bailout;
	    }
	    for (i=0; i<spec->n_literal; i++) {
		if (!fgets(gpline, MAXLEN - 1, fp)) {
		    errbox(_("Plot file is corrupted"));
		} else {
		    top_n_tail(gpline);
		    spec->literal[i] = g_strdup(gpline);
		}
	    }
	    continue;
	}

	if (strstr(gpline, "automatic fit")) {
	    spec->flags |= GPT_AUTO_FIT;
	    spec->fit = recognize_fit_string(gpline);
	    continue;
	}

	if (strstr(gpline, "printing data labels")) {
	    spec->flags |= GPT_ALL_MARKERS;
	    continue;
	}	

	if (!strncmp(gpline, "# ", 2)) {
	    /* ignore unknown comment lines */
	    continue;
	}

	if (strncmp(gpline, "set ", 4)) {
	    /* done reading "set" lines */
	    break;
	}

	if (parse_gp_set_line(spec, gpline, &labelno)) {
	    err = 1;
	    goto plot_bailout;
	}
    }

    if (got == NULL) {
	err = 1;
	goto plot_bailout;
    }

    for (i=0; i<4; i++) {
	if (spec->titles[i][0] != '\0') {
	    delchar('"', spec->titles[i]);
	}
    }

    if (*spec->keyspec == '\0') {
	strcpy(spec->keyspec, "none");
    }

    /* then get the "plot" lines */
    if (strncmp(gpline, "plot ", 5) ||
	(strlen(gpline) < 10 && fgets(gpline, MAXLEN - 1, fp) == NULL)) {	
	errbox(_("Failed to parse gnuplot file"));
	fprintf(stderr, "bad plotfile line: '%s'\n", gpline);
	err = 1;
	goto plot_bailout;
    }

    done = 0;

    while (!err) {
	top_n_tail(gpline);

	if (!chop_comma(gpline)) {
	    /* line did not end with comma -> no continuation of
	       the plot command */
	    done = 1;
	} 

	err = parse_gp_line_line(gpline, spec);

	if (err || done || (got = fgets(gpline, MAXLEN - 1, fp)) == NULL) {
	    break;
	}
    }

    if (err || got == NULL) {
	err = 1;
	goto plot_bailout;
    }

    /* determine total number of required data columns */
    for (i=0; i<spec->n_lines; i++) {
	if (spec->lines[i].ncols == 0) {
	    continue;
	}
	if (datacols == 0) {
	    datacols = spec->lines[i].ncols;
	} else {
	    datacols += spec->lines[i].ncols - 1;
	}
    }

    err = plot_get_data_and_markers(spec, fp, datacols, do_markers);

    if (!err && reglist != NULL && reglist[0] > 0) {
	spec->reglist = gretl_list_copy(reglist);
    }

    if (!err && spec->markers != NULL) {
	maybe_set_all_markers_ok(spec);
    }

    if (!err && spec->fit == PLOT_FIT_NONE) {
	maybe_set_add_fit_ok(spec);
    }

 plot_bailout:

    fclose(fp);

    return err;
}

#define has_log_axis(s) (s->logbase[0] != 0 || s->logbase[1] != 0)

static int get_data_xy (png_plot *plot, int x, int y, 
			double *data_x, double *data_y)
{
    double xmin, xmax;
    double ymin, ymax;
    double dx = NADBL;
    double dy = NADBL;
    int ok = 1;

    if (plot_is_zoomed(plot)) {
	xmin = plot->zoom_xmin;
	xmax = plot->zoom_xmax;
	ymin = plot->zoom_ymin;
	ymax = plot->zoom_ymax;
    } else {
	xmin = plot->xmin;
	xmax = plot->xmax;
	ymin = plot->ymin;
	ymax = plot->ymax;
    }

#if POINTS_DEBUG
    if (plot_doing_position(plot)) {
	fprintf(stderr, "get_data_xy:\n"
		" plot->xmin=%g, plot->xmax=%g, plot->ymin=%g, plot->ymax=%g\n",
		plot->xmin, plot->xmax, plot->ymin, plot->ymax);
    }
#endif

    if (xmin == 0.0 && xmax == 0.0) { 
	fprintf(stderr, "get_data_xy: unknown x range\n");
    } else {
	dx = xmin + ((double) x - plot->pixel_xmin) / 
	    (plot->pixel_xmax - plot->pixel_xmin) * (xmax - xmin);
    }

    if (!na(dx)) {
	if (ymin == 0.0 && ymax == 0.0) { 
	    fprintf(stderr, "get_data_xy: unknown y range\n");
	} else {
	    dy = ymax - ((double) y - plot->pixel_ymin) / 
		(plot->pixel_ymax - plot->pixel_ymin) * (ymax - ymin);
	}
    }

    if (na(dx) || na(dx)) {
	ok = 0;
    } else if (has_log_axis(plot->spec)) {
	double base, dprop, lr;

	base = plot->spec->logbase[0];
	if (base != 0) {
	    if (xmin > 0) {
		dprop = (dx - xmin) / (xmax - xmin);
		lr = log(xmax / xmin) / log(base);
		dx = pow(base, dprop * lr);
	    } else {
		dx = NADBL;
		ok = 0;
	    }
	}
	base = plot->spec->logbase[1];
	if (base != 0) {
	    if (ymin > 0) {
		dprop = (dy - ymin) / (ymax - ymin);
		lr = log(ymax / ymin) / log(base);
		dy = pow(base, dprop * lr);
	    } else {
		dy = NADBL;
		ok = 0;
	    }
	}
    } else if (plot_is_polar(plot)) {
	double px = atan2(dy, dx);
	double py = sqrt(dx * dx + dy * dy);

	dx = px;
	dy = py;
    }

    *data_x = dx;
    *data_y = dy;

    return ok;
}

static void x_to_date (double x, int pd, char *str)
{
    int yr = (int) x;
    double t, frac = 1.0 / pd;
    int subper = (int) ((x - yr + frac) * pd);
    static int decpoint;

    if (decpoint == 0) {
	decpoint = get_local_decpoint();
    }

    t = yr + subper / ((pd < 10)? 10.0 : 100.0);
    sprintf(str, "%.*f", (pd < 10)? 1 : 2, t);
    charsub(str, decpoint, ':');
}

static void create_selection_gc (png_plot *plot)
{
    if (plot->invert_gc == NULL) {
	plot->invert_gc = gdk_gc_new(plot->canvas->window);
	gdk_gc_set_function(plot->invert_gc, GDK_INVERT);
    }
}

static void draw_selection_rectangle (png_plot *plot,
				      int x, int y)
{
    int rx, ry, rw, rh;

    rx = (plot->screen_xmin < x)? plot->screen_xmin : x;
    ry = (plot->screen_ymin < y)? plot->screen_ymin : y;
    rw = x - plot->screen_xmin;
    rh = y - plot->screen_ymin;
    if (rw < 0) rw = -rw;
    if (rh < 0) rh = -rh;    

    /* draw one time to make the rectangle appear */
    gdk_draw_rectangle(plot->pixmap,
		       plot->invert_gc,
		       FALSE,
		       rx, ry, rw, rh);
    /* show the modified pixmap */
    gdk_window_copy_area(plot->canvas->window,
			 plot->canvas->style->fg_gc[GTK_STATE_NORMAL],
			 0, 0,
			 plot->pixmap,
			 0, 0,
			 plot->pixel_width, plot->pixel_height);
    /* draw (invert) again to erase the rectangle */
    gdk_draw_rectangle(plot->pixmap,
		       plot->invert_gc,
		       FALSE,
		       rx, ry, rw, rh);
}

static int make_alt_label (gchar *alt, const gchar *label)
{
    double x, y;
    int err = 0;

    gretl_push_c_numeric_locale();

    if (sscanf(label, "%lf,%lf", &x, &y) != 2) {
	err = 1;
    }

    gretl_pop_c_numeric_locale();

    if (!err) {
	if (get_local_decpoint() != '.') {
	    sprintf(alt, "%.2f %.2f", x, y);
	} else {
	    sprintf(alt, "%.2f,%.2f", x, y);
	}
    }

    return err;
}

static void
write_label_to_plot (png_plot *plot, int i, gint x, gint y)
{
    const gchar *label = plot->spec->markers[i];
    PangoContext *context;
    PangoLayout *pl;

    if (plot_is_roots(plot)) {
	gchar alt_label[12];
	
	if (make_alt_label(alt_label, label)) {
	    return;
	}

	label = alt_label;
    }

    if (plot->invert_gc == NULL) {
	create_selection_gc(plot);
    }

    context = gtk_widget_get_pango_context(plot->shell);
    pl = pango_layout_new(context);
    pango_layout_set_text(pl, label, -1);

    /* draw the label */
    gdk_draw_layout(plot->pixmap, plot->invert_gc, x, y, pl);

    /* show the modified pixmap */
    gdk_window_copy_area(plot->canvas->window,
			 plot->canvas->style->fg_gc[GTK_STATE_NORMAL],
			 0, 0,
			 plot->pixmap,
			 0, 0,
			 plot->pixel_width, plot->pixel_height);

    /* trash the pango layout */
    g_object_unref(G_OBJECT(pl));

    /* record that a label is shown */
    plot->format |= PLOT_MARKERS_UP;
}

#define TOLDIST 0.01

static gint
identify_point (png_plot *plot, int pixel_x, int pixel_y,
		double x, double y) 
{
    const double *data_x = NULL;
    const double *data_y = NULL;
    double xrange, yrange;
    double xdiff, ydiff;
    double min_xdist, min_ydist;
    int best_match = -1;
    int t;

#if GPDEBUG > 2
    fprintf(stderr, "identify_point: pixel_x = %d (x=%g), pixel_y = %d (y=%g)\n",
	    pixel_x, x, pixel_y, y);
#endif

    if (plot->err) {
	return TRUE;
    }

    /* no markers to show */
    if (plot->spec->markers == NULL) {
	plot->status |= PLOT_NO_MARKERS;	
	return TRUE;
    }

    /* need array to keep track of which points are labeled */
    if (plot->spec->labeled == NULL) {
	plot->spec->labeled = calloc(plot->spec->nobs, 1);
	if (plot->spec->labeled == NULL) {
	    return TRUE;
	}
    }

    if (plot_is_zoomed(plot)) {
	min_xdist = xrange = plot->zoom_xmax - plot->zoom_xmin;
	min_ydist = yrange = plot->zoom_ymax - plot->zoom_ymin;
    } else {
	min_xdist = xrange = plot->xmax - plot->xmin;
	min_ydist = yrange = plot->ymax - plot->ymin;
    }

    data_x = plot->spec->data;
    data_y = data_x + plot->spec->nobs;

    if (plot_has_y2axis(plot)) {
	/* use first y-var that's on y1 axis, if any */
	int i, got_y = 0;

	for (i=0; i<plot->spec->n_lines; i++) {
	    if (plot->spec->lines[i].yaxis == 1) {
		got_y = 1;
		break;
	    }
	    if (plot->spec->lines[i].ncols > 0) {
		data_y += (plot->spec->lines[i].ncols - 1) * plot->spec->nobs;
	    }
	}
	if (!got_y) {
	    data_y = NULL;
	    plot->status |= PLOT_NO_MARKERS;	
	    return TRUE;
	}
    } 

    /* try to find the best-matching data point */
    for (t=0; t<plot->spec->nobs; t++) {
	if (na(data_x[t]) || na(data_y[t])) {
	    continue;
	}
#if GPDEBUG > 2
	fprintf(stderr, "considering t=%d: x=%g, y=%g\n", t, data_x[t], data_y[t]);
#endif
	xdiff = fabs(data_x[t] - x);
	ydiff = fabs(data_y[t] - y);
	if (xdiff <= min_xdist && ydiff <= min_ydist) {
	    min_xdist = xdiff;
	    min_ydist = ydiff;
	    best_match = t;
	}
    }

    /* if the point is already labeled, skip */
    if (plot->spec->labeled[best_match]) {
	return TRUE;
    }

#if GPDEBUG > 2
    fprintf(stderr, " best_match=%d, with data_x[%d]=%g, data_y[%d]=%g\n", 
	    best_match, best_match, data_x[best_match], 
	    best_match, data_y[best_match]);
#endif

    /* if the match is good enough, show the label */
    if (best_match >= 0 && 
	min_xdist < TOLDIST * xrange &&
	min_ydist < TOLDIST * yrange) {
	write_label_to_plot(plot, best_match, pixel_x, pixel_y);
	/* flag the point as labeled already */
	plot->spec->labeled[best_match] = 1;
    }

    return TRUE;
}

#define float_fmt(i,x) ((i) && fabs(x) < 1.0e7)

static gint
motion_notify_event (GtkWidget *widget, GdkEventMotion *event, png_plot *plot)
{
    GdkModifierType state;
    gchar label[32], label_y[16];
    int x, y;

    if (plot->err) {
	return TRUE;
    }

    if (event->is_hint) {
        gdk_window_get_pointer(event->window, &x, &y, &state);
    } else {
        x = event->x;
        y = event->y;
        state = event->state;
    }

    *label = 0;

    if (x > plot->pixel_xmin && x < plot->pixel_xmax && 
	y > plot->pixel_ymin && y < plot->pixel_ymax) {
	double data_x, data_y;

	get_data_xy(plot, x, y, &data_x, &data_y);
	if (na(data_x)) {
	    return TRUE;
	}

	if (!plot_has_no_markers(plot) && !plot_show_all_markers(plot) &&
	    !plot_is_zooming(plot) &&
	    !na(data_y)) {
	    identify_point(plot, x, y, data_x, data_y);
	}

	if (plot->pd == 4 || plot->pd == 12) {
	    x_to_date(data_x, plot->pd, label);
	} else {
	    sprintf(label, (float_fmt(plot->xint, data_x))? "%7.0f" : 
		    "%7.4g", data_x);
	}

	if (!na(data_y)) {
	    if (plot_has_png_coords(plot)) {
		sprintf(label_y, (float_fmt(plot->yint, data_y))? " %-7.0f" : 
			" %-7.4g", data_y);
	    } else {
		/* pretty much guessing at y coordinate here */
		sprintf(label_y, (float_fmt(plot->yint, data_y))? " %-7.0f" : 
				  " %-6.3g", data_y);
	    }
	    strcat(label, label_y);
	}

	if (plot_is_zooming(plot) && (state & GDK_BUTTON1_MASK)) {
	    draw_selection_rectangle(plot, x, y);
	}
    } 

    gtk_label_set_text(GTK_LABEL(plot->cursor_label), label);
  
    return TRUE;
}

static void set_plot_format_flags (png_plot *plot)
{
    plot->format = 0;

    if (!string_is_blank(plot->spec->titles[0])) {
	plot->format |= PLOT_TITLE;
    }
    if (!string_is_blank(plot->spec->titles[1])) {
	plot->format |= PLOT_XLABEL;
    }
    if (!string_is_blank(plot->spec->titles[2])) {
	plot->format |= PLOT_YLABEL;
    }
    if (!string_is_blank(plot->spec->titles[3])) {
	plot->format |= PLOT_Y2LABEL;
    }
    if (plot->spec->flags & GPT_Y2AXIS) {
	plot->format |= PLOT_Y2AXIS;
    }
}

/* called from png plot popup menu */

static void start_editing_png_plot (png_plot *plot)
{
#if GPDEBUG
    fprintf(stderr, "start_editing_png_plot: plot = %p\n", (void *) plot);
#endif

    if (!PLOTSPEC_DETAILS_IN_MEMORY(plot->spec)) {
	errbox(_("Couldn't access graph info"));
	plot->err = 1;
	return;
    }

    if (show_gnuplot_dialog(plot->spec) == 0) { /* OK */
	plot->status |= PLOT_HAS_CONTROLLER;
    }
}

#ifdef HAVE_AUDIO
static void audio_render_plot (png_plot *plot)
{
    void *handle;
    int (*midi_play_graph) (const char *, const char *, const char *);

    if (plot_not_editable(plot)) {
	return;
    }

    midi_play_graph = gui_get_plugin_function("midi_play_graph", 
					      &handle);
    if (midi_play_graph == NULL) {
        return;
    }

# ifdef G_OS_WIN32
    (*midi_play_graph) (plot->spec->fname, paths.dotdir, NULL);
# else
    (*midi_play_graph) (plot->spec->fname, paths.dotdir, midiplayer);
# endif

    close_plugin(handle);
}
#endif

static gint color_popup_activated (GtkWidget *w, gpointer data)
{
    gchar *item = (gchar *) data;
    gpointer ptr = g_object_get_data(G_OBJECT(w), "plot");
    png_plot *plot = (png_plot *) ptr;
    gint color = strcmp(item, _("monochrome"));
    GtkWidget *parent = (GTK_MENU(w->parent))->parent_menu_item;
    gchar *parent_item = g_object_get_data(G_OBJECT(parent), "string");

    if (!strcmp(parent_item, _("Save as postscript (EPS)..."))) {
	strcpy(plot->spec->termtype, "postscript");
	if (color) {
	    strcat(plot->spec->termtype, " color");
	} 
	file_selector(_("Save gnuplot graph"), SAVE_GNUPLOT, 
		      FSEL_DATA_MISC, plot->spec);
    } else if (!strcmp(parent_item, _("Save as Windows metafile (EMF)..."))) {
	strcpy(plot->spec->termtype, "emf");
	if (color) {
	    strcat(plot->spec->termtype, " color");
	} else {
	    strcat(plot->spec->termtype, " mono");
	}
	file_selector(_("Save gnuplot graph"), SAVE_GNUPLOT, 
		      FSEL_DATA_MISC, plot->spec);
    } 
#ifdef G_OS_WIN32
    else if (!strcmp(parent_item, _("Copy to clipboard"))) {
	win32_process_graph(plot->spec, color, WIN32_TO_CLIPBOARD);
    } else if (!strcmp(parent_item, _("Print"))) {
	win32_process_graph(plot->spec, color, WIN32_TO_PRINTER);
    }    
#endif   

    return TRUE;
}

static void show_numbers_from_markers (GPT_SPEC *spec)
{
    PRN *prn;
    double x, y;
    double mod, freq;
    int i, err = 0;

    if (bufopen(&prn)) {
	return;
    } 

    pputs(prn, _("VAR roots (real, imaginary, modulus, frequency)"));
    pputs(prn, "\n\n");

    if (get_local_decpoint() != '.') {
	gretl_push_c_numeric_locale();
	for (i=0; i<spec->n_markers; i++) {
	    if (sscanf(spec->markers[i], "%lf,%lf", &x, &y) == 2) {
		freq = spec->data[i] / (2.0 * M_PI);
		mod = spec->data[spec->nobs + i];
		gretl_pop_c_numeric_locale();
		pprintf(prn, "%2d: (%7.4f  %7.4f  %7.4f  %7.4f)\n", i+1, 
			x, y, mod, freq);
		gretl_push_c_numeric_locale();
	    } else {
		err = E_DATA;
		break;
	    }
	}
	gretl_pop_c_numeric_locale();
    } else {
	for (i=0; i<spec->n_markers; i++) {
	    if (sscanf(spec->markers[i], "%lf,%lf", &x, &y) == 2) {
		freq = spec->data[i] / (2.0 * M_PI);
		mod = spec->data[spec->nobs + i];
		pprintf(prn, "%2d: (%7.4f, %7.4f, %7.4f, %7.4f)\n", i+1, 
			x, y, mod, freq);
	    } else {
		err = E_DATA;
		break;
	    }
	}
    }

    if (err) {
	gui_errmsg(err);
	gretl_print_destroy(prn);
    } else {
	gchar *title = g_strdup_printf("gretl: %s", _("VAR roots"));

	view_buffer(prn, 72, 340, title, PRINT, NULL);
	g_free(title);	
    }
}

static void add_to_session_callback (GPT_SPEC *spec)
{
    char fullname[MAXLEN] = {0};
    int err;

    err = add_graph_to_session(spec->fname, fullname);

    if (!err) {
	remove_png_term_from_plotfile(fullname, spec);
	mark_plot_as_saved(spec);
    }
}

static gint plot_popup_activated (GtkWidget *w, gpointer data)
{
    gchar *item = (gchar *) data;
    gpointer ptr = g_object_get_data(G_OBJECT(w), "plot");
    png_plot *plot = (png_plot *) ptr;
    int killplot = 0;

    gtk_widget_destroy(plot->popup);
    plot->popup = NULL;

    if (!strcmp(item, _("Add another curve..."))) {
	stats_calculator(plot, CALC_GRAPH_ADD, NULL);
    } else if (!strcmp(item, _("Save as PNG..."))) {
	if (gnuplot_png_terminal() == GP_PNG_CAIRO) {
	    strcpy(plot->spec->termtype, "pngcairo");
	} else {
	    strcpy(plot->spec->termtype, "png");
	}
        file_selector(_("Save gnuplot graph"), SAVE_GNUPLOT, 
		      FSEL_DATA_MISC, plot->spec);
    } else if (!strcmp(item, _("Save as PDF..."))) {
	if (gnuplot_pdf_terminal() == GP_PDF_CAIRO) {
	    strcpy(plot->spec->termtype, PDF_CAIRO_STRING);
	} else {
	    strcpy(plot->spec->termtype, "pdf");
	}
        file_selector(_("Save gnuplot graph"), SAVE_GNUPLOT, 
		      FSEL_DATA_MISC, plot->spec);
    } else if (!strcmp(item, _("Save to session as icon"))) { 
	add_to_session_callback(plot->spec);
    } else if (plot_is_range_mean(plot) && !strcmp(item, _("Help"))) { 
	context_help(NULL, GINT_TO_POINTER(RMPLOT));
    } else if (plot_is_hurst(plot) && !strcmp(item, _("Help"))) { 
	context_help(NULL, GINT_TO_POINTER(HURST));
    } else if (!strcmp(item, _("Freeze data labels"))) {
	plot->spec->flags |= GPT_ALL_MARKERS;
	redisplay_edited_plot(plot);
    } else if (!strcmp(item, _("Clear data labels"))) { 
	zoom_unzoom_png(plot, PNG_REDISPLAY);
    } else if (!strcmp(item, _("Zoom..."))) { 
	GdkCursor* cursor;

	cursor = gdk_cursor_new(GDK_CROSSHAIR);
	gdk_window_set_cursor(plot->canvas->window, cursor);
	gdk_cursor_destroy(cursor);
	plot->status |= PLOT_ZOOMING;
	gtk_statusbar_push(GTK_STATUSBAR(plot->statusbar), plot->cid, 
			   _(" Drag to define zoom rectangle"));
	create_selection_gc(plot);
    } else if (!strcmp(item, _("Restore full view"))) { 
	zoom_unzoom_png(plot, PNG_UNZOOM);
    }
#if defined(USE_GNOME) || defined(GTK_PRINTING)
    else if (!strcmp(item, _("Print..."))) { 
	gtk_print_graph(plot->spec->fname);
    }
#endif 
    else if (!strcmp(item, _("Display PDF"))) { 
	graph_display_pdf(plot->spec);
    } else if (!strcmp(item, _("OLS estimates"))) { 
	if (plot->spec != NULL) {
	    do_graph_model(plot->spec->reglist, plot->spec->fit);
	}
    } else if (!strcmp(item, _("Numerical values"))) {
	show_numbers_from_markers(plot->spec);
    } else if (!strcmp(item, _("Edit"))) { 
	start_editing_png_plot(plot);
    } else if (!strcmp(item, _("Close"))) { 
        killplot = 1;
    } 

    if (killplot) {
	gtk_widget_destroy(plot->shell);
    }

    return TRUE;
}

static void attach_color_popup (GtkWidget *w, png_plot *plot)
{
    GtkWidget *item, *cpopup;
    const char *color_items[] = {
	N_("color"),
	N_("monochrome")
    };
    int i;

    cpopup = gtk_menu_new();

    for (i=0; i<2; i++) {
	item = gtk_menu_item_new_with_label(_(color_items[i]));
	g_signal_connect(G_OBJECT(item), "activate",
			 G_CALLBACK(color_popup_activated),
			 _(color_items[i]));
	g_object_set_data(G_OBJECT(item), "plot", plot);
	gtk_widget_show(item);
	gtk_menu_shell_append(GTK_MENU_SHELL(cpopup), item);
    } 

    gtk_menu_item_set_submenu(GTK_MENU_ITEM(w), cpopup);
}

#define graph_model_ok(f) (f == PLOT_FIT_OLS || \
                           f == PLOT_FIT_QUADRATIC || \
                           f == PLOT_FIT_INVERSE)

static void build_plot_menu (png_plot *plot)
{
    GtkWidget *item;    
    const char *regular_items[] = {
	N_("Add another curve..."),
#ifdef G_OS_WIN32
	N_("Save as Windows metafile (EMF)..."),
#endif
	N_("Save as PNG..."),
        N_("Save as postscript (EPS)..."),
	N_("Save as PDF..."),
#ifndef G_OS_WIN32
	N_("Save as Windows metafile (EMF)..."),
#endif
#ifdef G_OS_WIN32
	N_("Copy to clipboard"),
#endif
	N_("Save to session as icon"),
	N_("Freeze data labels"),
	N_("Clear data labels"),
	N_("Zoom..."),
#if defined(USE_GNOME) || defined(GTK_PRINTING)
	N_("Print..."),
#endif
#ifdef G_OS_WIN32
	N_("Print"),
#endif
	N_("Display PDF"),
	N_("OLS estimates"),
	N_("Numerical values"),
	N_("Edit"),
	N_("Help"),
        N_("Close"),
        NULL
    };
    const char *zoomed_items[] = {
	N_("Restore full view"),
	N_("Close"),
	NULL
    };
    const char **plot_items;
    static int pdf_ok = -1;
    int i;

    if (pdf_ok == -1) {
	pdf_ok = gnuplot_pdf_terminal();
    }

    plot->popup = gtk_menu_new();

    if (plot_is_zoomed(plot)) {
	plot_items = zoomed_items;
    } else {
	plot_items = regular_items;
    }

    i = 0;
    while (plot_items[i]) {
	if (plot->spec->code != PLOT_PROB_DIST &&
	    !strcmp(plot_items[i], "Add another curve...")) {
	    i++;
	    continue;
	}
	if (plot_not_zoomable(plot) &&
	    !strcmp(plot_items[i], "Zoom...")) {
	    i++;
	    continue;
	}
	if (!(plot_is_range_mean(plot) || plot_is_hurst(plot)) &&
	    !strcmp(plot_items[i], "Help")) {
	    i++;
	    continue;
	}
	if (plot_is_saved(plot) &&
	    !strcmp(plot_items[i], "Save to session as icon")) {
	    i++;
	    continue;
	}
	if ((plot_has_controller(plot) || plot_not_editable(plot)) &&
	    !strcmp(plot_items[i], "Edit")) {
	    i++;
	    continue;
	}
	if (!pdf_ok && (!strcmp(plot_items[i], "Save as PDF...") ||
			!strcmp(plot_items[i], "Display PDF"))) {
	    i++;
	    continue;
	}
	if (pdf_ok && !strcmp(plot_items[i], "Print...")) {
	    /* Print... is currently very funky for graphs.  If
	       we're able to display PDF, bypass this option */
	    i++;
	    continue;
	}	    
	if (!plot_has_data_markers(plot) &&
	    (!strcmp(plot_items[i], "Freeze data labels") ||
	     !strcmp(plot_items[i], "Clear data labels"))) {
	    i++;
	    continue;
	}
	if ((!plot_has_regression_list(plot) || 
	     !graph_model_ok(plot->spec->fit)) && 
	    !strcmp(plot_items[i], "OLS estimates")) {
	    i++;
	    continue;
	}
	if (!plot_is_roots(plot) && 
	    !strcmp(plot_items[i], "Numerical values")) {
	    i++;
	    continue;
	}	

        item = gtk_menu_item_new_with_label(_(plot_items[i]));
        g_object_set_data(G_OBJECT(item), "plot", plot);
        gtk_widget_show(item);
        gtk_menu_shell_append(GTK_MENU_SHELL(plot->popup), item);

	/* items with color sub-menu */
	if (!strcmp(plot_items[i], "Save as Windows metafile (EMF)...") ||
	    !strcmp(plot_items[i], "Save as postscript (EPS)...") ||
	    !strcmp(plot_items[i], "Copy to clipboard") ||
	    !strcmp(plot_items[i], "Print")) {
	    attach_color_popup(item, plot);
	    g_object_set_data(G_OBJECT(item), "string", _(plot_items[i]));
	} else {
	    g_signal_connect(G_OBJECT(item), "activate",
			     G_CALLBACK(plot_popup_activated),
			     _(plot_items[i]));
	}
        i++;
    }

    g_signal_connect(G_OBJECT(plot->popup), "destroy",
		     G_CALLBACK(gtk_widget_destroyed), 
		     &plot->popup);
}

int redisplay_edited_plot (png_plot *plot)
{
    gchar *plotcmd;
    FILE *fp;
    int err = 0;

#if GPDEBUG
    fprintf(stderr, "redisplay_edited_plot: plot = %p\n", (void *) plot);
#endif

    /* open file in which to dump plot specification */
    gnuplot_png_init(plot->spec, &fp);
    if (fp == NULL) {
	return 1;
    }

    /* dump the edited plot details to file */
    set_png_output(plot->spec);
    plotspec_print(plot->spec, fp);
    fclose(fp);

    /* get gnuplot to create a new PNG graph */
    plotcmd = g_strdup_printf("\"%s\" \"%s\"", paths.gnuplot, 
			      plot->spec->fname);
    err = gretl_spawn(plotcmd);
    g_free(plotcmd);

    if (err) {
	gui_errmsg(err);
	return err;
    }

    /* reset format flags */
    set_plot_format_flags(plot);

    /* grab (possibly modified) data ranges */
    get_plot_ranges(plot);

    /* put the newly created PNG onto the plot canvas */
    return render_pngfile(plot, PNG_REDISPLAY);
}

static int zoom_unzoom_png (png_plot *plot, int view)
{
    int err = 0;
    char zoomname[MAXLEN];
    gchar *plotcmd = NULL;

    if (view == PNG_ZOOM) {
	FILE *fpin, *fpout;
	char line[MAXLEN];

	fpin = gretl_fopen(plot->spec->fname, "r");
	if (fpin == NULL) {
	    return 1;
	}

	build_path(zoomname, paths.dotdir, "zoomplot.gp", NULL);
	fpout = gretl_fopen(zoomname, "w");
	if (fpout == NULL) {
	    fclose(fpin);
	    return 1;
	}

	/* write zoomed range into auxiliary gnuplot source file */

	gretl_push_c_numeric_locale();
	fprintf(fpout, "set xrange [%g:%g]\n", plot->zoom_xmin,
		plot->zoom_xmax);
	fprintf(fpout, "set yrange [%g:%g]\n", plot->zoom_ymin,
		plot->zoom_ymax);
	gretl_pop_c_numeric_locale();

	while (fgets(line, MAXLEN-1, fpin)) {
	    if (strncmp(line, "set xrange", 10) &&
		strncmp(line, "set yrange", 10))
		fputs(line, fpout);
	}

	fclose(fpout);
	fclose(fpin);

	plotcmd = g_strdup_printf("\"%s\" \"%s\"", paths.gnuplot, 
				  zoomname);
    } else { 
	/* PNG_UNZOOM or PNG_START */
	plotcmd = g_strdup_printf("\"%s\" \"%s\"", paths.gnuplot, 
				  plot->spec->fname);
    }

    err = gretl_spawn(plotcmd);
    g_free(plotcmd);  

    if (view == PNG_ZOOM) {
	remove(zoomname);
    }

    if (err) {
	gui_errmsg(err);
	return err;
    }

    return render_pngfile(plot, view);
}

static gint plot_button_release (GtkWidget *widget, GdkEventButton *event, 
				 png_plot *plot)
{
    if (plot_is_zooming(plot)) {
	double z;

	if (!get_data_xy(plot, event->x, event->y, 
			 &plot->zoom_xmax, &plot->zoom_ymax)) {
	    return TRUE;
	}

	/* flip the selected rectangle if required */
	if (plot->zoom_xmin > plot->zoom_xmax) {
	    z = plot->zoom_xmax;
	    plot->zoom_xmax = plot->zoom_xmin;
	    plot->zoom_xmin = z;
	}

	if (plot->zoom_ymin > plot->zoom_ymax) {
	    z = plot->zoom_ymax;
	    plot->zoom_ymax = plot->zoom_ymin;
	    plot->zoom_ymin = z;
	}

	if (plot->zoom_xmin != plot->zoom_xmax &&
	    plot->zoom_ymin != plot->zoom_ymax) {
	    zoom_unzoom_png(plot, PNG_ZOOM);
	}

	plot->status ^= PLOT_ZOOMING;
	gdk_window_set_cursor(plot->canvas->window, NULL);
	gtk_statusbar_pop(GTK_STATUSBAR(plot->statusbar), plot->cid);
    }

    return TRUE;
}

static gint plot_button_press (GtkWidget *widget, GdkEventButton *event, 
			       png_plot *plot)
{
    if (plot_is_zooming(plot)) {
	/* think about this */
	if (get_data_xy(plot, event->x, event->y, 
			&plot->zoom_xmin, &plot->zoom_ymin)) {
	    plot->screen_xmin = event->x;
	    plot->screen_ymin = event->y;
	}
	return TRUE;
    }

    if (plot_doing_position(plot)) {
	if (plot->labelpos_entry != NULL) {
	    double dx, dy;
	    
	    if (get_data_xy(plot, event->x, event->y, &dx, &dy)) {
		gchar *posstr;

		posstr = g_strdup_printf("%g %g", dx, dy);
		gtk_entry_set_text(GTK_ENTRY(plot->labelpos_entry), posstr);
		g_free(posstr);
	    }
	} 
	terminate_plot_positioning(plot);
	return TRUE;
    }

    if (plot->popup != NULL) {
	gtk_widget_destroy(plot->popup);
	plot->popup = NULL;
    }

    if (!plot->err) {
	build_plot_menu(plot);
	gtk_menu_popup(GTK_MENU(plot->popup), NULL, NULL, NULL, NULL,
		       event->button, event->time);
    }

    return TRUE;
}

static gboolean 
plot_key_handler (GtkWidget *w, GdkEventKey *key, png_plot *plot)
{
    switch (key->keyval) {
    case GDK_q:
    case GDK_Q:
	gtk_widget_destroy(w);
	break;
    case GDK_s:
    case GDK_S:
	add_to_session_callback(plot->spec);
	break;
#ifdef G_OS_WIN32
    case GDK_c:
	win32_process_graph(plot->spec, 1, WIN32_TO_CLIPBOARD);
	break;
#endif
#ifdef HAVE_AUDIO
    case GDK_a:
    case GDK_A:
	audio_render_plot(plot);
	break;
#endif
    default:
	break;
    }

    return TRUE;
}

static 
void plot_expose (GtkWidget *widget, GdkEventExpose *event,
		  GdkPixmap *dbuf_pixmap)
{
    /* Don't repaint entire window on each exposure */
    gdk_window_set_back_pixmap(widget->window, NULL, FALSE);

    /* Refresh double buffer, then copy the "dirtied" area to
       the on-screen GdkWindow */
    gdk_window_copy_area(widget->window,
			 widget->style->fg_gc[GTK_STATE_NORMAL],
			 event->area.x, event->area.y,
			 dbuf_pixmap,
			 event->area.x, event->area.y,
			 event->area.width, event->area.height);
}

static int render_pngfile (png_plot *plot, int view)
{
    gint width;
    gint height;
    GdkPixbuf *pbuf;
    char pngname[MAXLEN];
    GError *error = NULL;

    build_path(pngname, paths.dotdir, "gretltmp.png", NULL);
#if 1
    fprintf(stderr, "pngname: '%s'\n", pngname);
#endif

    pbuf = gdk_pixbuf_new_from_file(pngname, &error);
    if (pbuf == NULL) {
	fprintf(stderr, "error from gdk_pixbuf_new_from_file\n");
        errbox(error->message);
        g_error_free(error);
	remove(pngname);
	return 1;
    }

    width = gdk_pixbuf_get_width(pbuf);
    height = gdk_pixbuf_get_height(pbuf);

    if (width == 0 || height == 0) {
	errbox(_("Malformed PNG file for graph"));
	g_object_unref(pbuf);
	remove(pngname);
	return 1;
    }

    /* scrap any old record of which points are labeled */
    if (plot->spec->labeled != NULL) {
	free(plot->spec->labeled);
	plot->spec->labeled = NULL;
	plot->format &= ~PLOT_MARKERS_UP;
    }

    gdk_pixbuf_render_to_drawable(pbuf, plot->pixmap, 
				  plot->canvas->style->fg_gc[GTK_STATE_NORMAL],
				  0, 0, 0, 0, width, height,
				  GDK_RGB_DITHER_NONE, 0, 0);

    g_object_unref(pbuf);
    remove(pngname);
    
    if (view != PNG_START) { 
	/* we're changing the view, so refresh the whole canvas */
	gdk_window_copy_area(plot->canvas->window,
			     plot->canvas->style->fg_gc[GTK_STATE_NORMAL],
			     0, 0,
			     plot->pixmap,
			     0, 0,
			     plot->pixel_width, plot->pixel_height);
	if (view == PNG_ZOOM) {
	    plot->status |= PLOT_ZOOMED;
	} else if (view == PNG_UNZOOM) {
	    plot->status ^= PLOT_ZOOMED;
	}
    }

    return 0;
}

static void destroy_png_plot (GtkWidget *w, png_plot *plot)
{
    /* delete temporary plot source file? */
    if (!plot_is_saved(plot)) {
	remove(plot->spec->fname);
    }

#if GPDEBUG
    fprintf(stderr, "destroy_png_plot: plot = %p, spec = %p\n",
	    (void *) plot, (void *) plot->spec);
#endif

    if (plot_has_controller(plot)) {
	/* if the png plot has a controller, destroy it too */
	plot->spec->ptr = NULL;
	destroy_gpt_control_window();
    } else {
	/* no controller: take responsibility for freeing the
	   plot specification */
	plotspec_destroy(plot->spec);
    }

    if (plot->invert_gc != NULL) {
	gdk_gc_destroy(plot->invert_gc);
    }

    gtk_widget_unref(plot->shell);

    free(plot);
}

static void set_approx_pixel_bounds (png_plot *plot, 
				     int max_num_width,
				     int max_num2_width)
{
    if (plot_has_xlabel(plot)) {
	plot->pixel_ymax = plot->pixel_height - 36;
    } else {
	plot->pixel_ymax = plot->pixel_height - 24;
    }

    if (plot_has_title(plot)) {
	plot->pixel_ymin = 36;
    } else {
	plot->pixel_ymin = 14;
    }

    plot->pixel_xmin = 27 + 7 * max_num_width;
    if (plot_has_ylabel(plot)) {
	plot->pixel_xmin += 12;
    }

    plot->pixel_xmax = plot->pixel_width - 20; 
    if (plot_has_y2axis(plot)) {
	plot->pixel_xmax -= 7 * (max_num2_width + 1);
    }
    if (plot_has_y2label(plot)) {
	plot->pixel_xmax -= 11;
    }

#if POINTS_DEBUG
    fprintf(stderr, "set_approx_pixel_bounds():\n"
	    " xmin=%d xmax=%d ymin=%d ymax=%d\n", 
	    plot->pixel_xmin, plot->pixel_xmax,
	    plot->pixel_ymin, plot->pixel_ymax);
    fprintf(stderr, "set_approx_pixel_bounds():\n"
	    " max_num_width=%d max_num2_width=%d\n", 
	    max_num_width, max_num2_width);
#endif
}

int ok_dumb_line (const char *s)
{
    if (strstr(s, "x2tics")) return 0;
    if (strstr(s, "set style line")) return 0;
    if (strstr(s, "set style inc")) return 0;
    return 1;
}

/* Attempt to read y-range info from the ascii representation
   of a gnuplot graph (the "dumb" terminal): return 0 on
   success, non-zero on failure.
*/

static int get_dumb_plot_yrange (png_plot *plot)
{
    FILE *fpin, *fpout;
    char line[MAXLEN], dumbgp[MAXLEN], dumbtxt[MAXLEN];
    gchar *plotcmd = NULL;
    int err = 0, x2axis = 0;
    int max_ywidth = 0;
    int max_y2width = 0;

    fpin = gretl_fopen(plot->spec->fname, "r");
    if (fpin == NULL) {
	return 1;
    }

    build_path(dumbgp, paths.dotdir, "dumbplot.gp", NULL);
    build_path(dumbtxt, paths.dotdir, "gptdumb.txt", NULL);
    fpout = gretl_fopen(dumbgp, "w");
    if (fpout == NULL) {
	fclose(fpin);
	return 1;
    }

    /* switch to the "dumb" (ascii) terminal in gnuplot */
    while (fgets(line, MAXLEN-1, fpin)) {
	if (strstr(line, "set term")) {
	    fputs("set term dumb\n", fpout);
	} else if (strstr(line, "set output")) { 
	    fprintf(fpout, "set output '%s'\n", dumbtxt);
	} else if (ok_dumb_line(line)) {
	    fputs(line, fpout);
	}
	if (strstr(line, "x2range")) {
	    x2axis = 1;
	}
    }

    fclose(fpin);
    fclose(fpout);

    plotcmd = g_strdup_printf("\"%s\" \"%s\"", paths.gnuplot, dumbgp);
    err = gretl_spawn(plotcmd);
    g_free(plotcmd);

    remove(dumbgp);

    if (err) {
#if POINTS_DEBUG
	fputs("get_dumb_plot_yrange(): plot command failed\n", stderr);
#endif
	gretl_error_clear();
	return 1;
    } else {
	double y[16] = {0};
	int y_numwidth[16] = {0};
	int y2_numwidth[16] = {0};
	char numstr[32];
	int i, j, k, imin;

	fpin = gretl_fopen(dumbtxt, "r");
	if (fpin == NULL) {
	    return 1;
	}

	/* read the y-axis min and max from the ascii graph */

	gretl_push_c_numeric_locale();

	i = j = 0;
	while (i < 16 && fgets(line, MAXLEN-1, fpin)) {
	    const char *s = line;
	    int nsp = 0;

	    while (isspace((unsigned char) *s)) {
	        nsp++;
	        s++;
            }
	    if (nsp > 5) {
		/* not a y-axis number */
		continue; 
	    }
	    if (sscanf(s, "%lf", &y[i]) == 1) {
#if POINTS_DEBUG
		fprintf(stderr, "from text plot: read y[%d]=%g\n",
			i, y[i]);
#endif
		sscanf(s, "%31s", numstr);
		y_numwidth[i++] = strlen(numstr);
	    }
	    if (plot_has_y2axis(plot) && j < 16) {
		double y2;

		s = strrchr(s, ' ');
		if (s != NULL && sscanf(s, "%lf", &y2) == 1) {
		    sscanf(s, "%31s", numstr);
		    y2_numwidth[j++] = strlen(numstr);
		}
	    }
	}

	gretl_pop_c_numeric_locale();

	fclose(fpin);
#if (POINTS_DEBUG == 0)
	remove(dumbtxt);
#endif

	imin = (x2axis)? 1 : 0;

	if (i > (imin + 2) && y[imin] > y[i-1]) {
	    plot->ymin = y[i-1];
	    plot->ymax = y[imin];
	    for (k=imin; k<i-1; k++) {
		if (y_numwidth[k] > max_ywidth) {
		    max_ywidth = y_numwidth[k];
		}
	    }
	}	    

#if POINTS_DEBUG
	fprintf(stderr, "Reading y range from text plot: plot->ymin=%g, "
		"plot->ymax=%g\n", plot->ymin, plot->ymax);
#endif

	if (plot_has_y2axis(plot)) {
	    for (k=imin; k<j-2; k++) {
		if (y2_numwidth[k] > max_y2width) {
		    max_y2width = y2_numwidth[k];
		}
	    }
	}
    }

    if (plot->ymax <= plot->ymin) {
	err = 1;
    }

    if (!err) {
	set_approx_pixel_bounds(plot, max_ywidth, max_y2width);
    }
    
    return err;
}

/* Do a partial parse of the gnuplot source file: enough to determine
   the data ranges so we can read back the mouse pointer coordinates
   when the user moves the pointer over the graph.
*/

static int get_plot_ranges (png_plot *plot)
{
    FILE *fp;
    char line[MAXLEN];
    int got_x = 0;
    int got_y = 0;
    png_bounds b;
    int err = 0;

#if GPDEBUG
    fprintf(stderr, "get_plot_ranges: plot=%p, plot->spec=%p\n", 
	    (void *) plot, (void *) plot->spec);
#endif    

    plot->xmin = plot->xmax = 0.0;
    plot->ymin = plot->ymax = 0.0;   
    plot->xint = plot->yint = 0;
    plot->pd = 0;

    if (no_readback(plot->spec->code)) {
	plot->status |= (PLOT_DONT_ZOOM | PLOT_DONT_MOUSE);
	return 1;
    }

    fp = gretl_fopen(plot->spec->fname, "r");
    if (fp == NULL) {
	plot->status |= (PLOT_DONT_ZOOM | PLOT_DONT_MOUSE);
	return 1;
    }

    gretl_push_c_numeric_locale();

    while (fgets(line, MAXLEN-1, fp) && strncmp(line, "plot ", 5)) {
	if (sscanf(line, "set xrange [%lf:%lf]", 
		   &plot->xmin, &plot->xmax) == 2) { 
	    got_x = 1;
	} 
    }

    gretl_pop_c_numeric_locale();

    fclose(fp);

    /* now try getting accurate coordinate info from 
       auxiliary file (or maybe PNG file)
    */
    if (get_png_bounds_info(&b) == GRETL_PNG_OK) {
	plot->status |= PLOT_PNG_COORDS;
	got_x = got_y = 1;
	plot->pixel_xmin = b.xleft;
	plot->pixel_xmax = b.xright;
	plot->pixel_ymin = plot->pixel_height - b.ytop;
	plot->pixel_ymax = plot->pixel_height - b.ybot;
	plot->xmin = b.xmin;
	plot->xmax = b.xmax;
	plot->ymin = b.ymin;
	plot->ymax = b.ymax;
# if POINTS_DEBUG
	fprintf(stderr, "get_png_bounds_info():\n"
		" xmin=%d xmax=%d ymin=%d ymax=%d\n", 
		plot->pixel_xmin, plot->pixel_xmax,
		plot->pixel_ymin, plot->pixel_ymax);
	fprintf(stderr, "using px_height %d, px_width %d\n",
		plot->pixel_height, plot->pixel_width);
# endif
	fprintf(stderr, "get_png_bounds_info(): OK\n");
    } else {
	fprintf(stderr, "get_png_bounds_info(): failed\n");
    }

    /* If got_x = 0 at this point, we didn't get an x-range out of 
       gnuplot, so we might as well give up.
    */

    if (got_x) {
	plot->status |= PLOT_HAS_XRANGE;
    } else {
	plot->status |= (PLOT_DONT_ZOOM | PLOT_DONT_MOUSE);
	return 1;
    }    

    /* get the "dumb" y coordinates only if we haven't got
       more accurate ones already */
    if (!plot_has_png_coords(plot)) { 
	err = get_dumb_plot_yrange(plot);
    }

    if (!err) {
	plot->status |= PLOT_HAS_YRANGE;
	if ((plot->xmax - plot->xmin) / 
	    (plot->pixel_xmax - plot->pixel_xmin) >= 1.0) {
	    plot->xint = 1;
	}
	if ((plot->ymax - plot->ymin) / 
	    (plot->pixel_ymax - plot->pixel_ymin) >= 1.0) {
	    plot->yint = 1;
	}
    } else {
	plot->status |= (PLOT_DONT_ZOOM | PLOT_DONT_MOUSE);
#if POINTS_DEBUG 
	fputs("get_plot_ranges: setting PLOT_DONT_ZOOM, PLOT_DONT_MOUSE\n", 
	      stderr);
#endif
    }

    return err;
}

static png_plot *png_plot_new (void)
{
    png_plot *plot = mymalloc(sizeof *plot);

    if (plot == NULL) {
	return NULL;
    }

    plot->shell = NULL;
    plot->canvas = NULL;
    plot->popup = NULL;
    plot->statusarea = NULL;    
    plot->statusbar = NULL;
    plot->cursor_label = NULL;
    plot->pixmap = NULL;
    plot->invert_gc = NULL;
    plot->spec = NULL;

    plot->pixel_width = 640;
    plot->pixel_height = 480;

    plot->xmin = plot->xmax = 0.0;
    plot->ymin = plot->ymax = 0.0;
    plot->xint = plot->yint = 0;

    plot->zoom_xmin = plot->zoom_xmax = 0.0;
    plot->zoom_ymin = plot->zoom_ymax = 0.0;
    plot->screen_xmin = plot->screen_ymin = 0;

    plot->pd = 0;
    plot->err = 0;
    plot->cid = 0;
    plot->status = 0;
    plot->format = 0;

    return plot;
}

static png_plot *
gnuplot_show_png (const char *plotfile, GPT_SPEC *spec, int saved)
{
    GtkWidget *vbox;
    GtkWidget *canvas_hbox;
    GtkWidget *label_frame = NULL;
    GtkWidget *status_hbox = NULL;
    png_plot *plot;
    int polar = 0;
    int err = 0;

#if GPDEBUG
    fprintf(stderr, "gnuplot_show_png:\n plotfile='%s', spec=%p, saved=%d\n",
	    plotfile, (void *) spec, saved);
#endif

    plot = png_plot_new();
    if (plot == NULL) {
	return NULL;
    }

    if (spec != NULL) {
	plot->spec = spec;
    } else {
	plot->spec = plotspec_new();
	if (plot->spec == NULL) {
	    free(plot);
	    return NULL;
	}
	strcpy(plot->spec->fname, plotfile);
    }

    if (saved) {
	plot->status |= PLOT_SAVED;
    }

    /* make png plot struct accessible via spec */
    plot->spec->ptr = plot;

    /* Parse the gnuplot source file.  If we hit errors here,
       flag this, but it's not necessarily a show-stopper in
       terms of simply displaying the graph. 
    */
    err = read_plotspec_from_file(plot->spec, &plot->pd, &polar);

#if 1
    fprintf(stderr, "read_plotspec_from_file: err = %d\n", err);
#endif

    if (err) {
	plot->err = 1;
	plot->status |= (PLOT_DONT_EDIT | PLOT_DONT_ZOOM | PLOT_DONT_MOUSE);
    } else if (cant_edit(plot->spec->code)) {
	if (plot->spec->n_markers > 0) {
	    plot->status |= (PLOT_DONT_EDIT | PLOT_DONT_ZOOM);
	    if (polar) {
		plot->format |= PLOT_POLAR;
	    }
	} else {
	    plot->status |= (PLOT_DONT_EDIT | PLOT_DONT_ZOOM | PLOT_DONT_MOUSE);
	}
    } else {
	set_plot_format_flags(plot);
    } 

    if (plot->spec->code == PLOT_VAR_ROOTS) {
	plot->pixel_width = plot->pixel_height;
    }

    if (plot->spec->flags & GPT_LETTERBOX) {
	plot->pixel_width = 680;
	plot->pixel_height = 400;
    }

    if (!err) {
	get_plot_ranges(plot);
    }

    plot->shell = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    /* note need for corresponding unref */
    gtk_widget_ref(plot->shell);

#if 1
    fprintf(stderr, "setting window title\n");
#endif
    gtk_window_set_title(GTK_WINDOW(plot->shell), _("gretl: gnuplot graph")); 
    gtk_window_set_resizable(GTK_WINDOW(plot->shell), FALSE);

    vbox = gtk_vbox_new(FALSE, 2);
    gtk_container_add(GTK_CONTAINER(plot->shell), vbox);

    g_signal_connect(G_OBJECT(plot->shell), "destroy",
		     G_CALLBACK(destroy_png_plot), plot);
    g_signal_connect(G_OBJECT(plot->shell), "key_press_event", 
		     G_CALLBACK(plot_key_handler), plot);

    /* box to hold canvas */
    canvas_hbox = gtk_hbox_new(FALSE, 1);
    gtk_box_pack_start(GTK_BOX(vbox), canvas_hbox, TRUE, TRUE, 0);
    gtk_widget_show(canvas_hbox);

    /* eventbox and hbox for status area  */
    plot->statusarea = gtk_event_box_new();
    gtk_box_pack_start(GTK_BOX(vbox), plot->statusarea, FALSE, FALSE, 0);

    status_hbox = gtk_hbox_new (FALSE, 2);
    gtk_container_add(GTK_CONTAINER(plot->statusarea), status_hbox);
    gtk_widget_show (status_hbox);
    gtk_container_set_resize_mode (GTK_CONTAINER (status_hbox),
				   GTK_RESIZE_QUEUE);

    /* Create drawing-area widget */
    plot->canvas = gtk_drawing_area_new();
    gtk_widget_set_size_request(GTK_WIDGET(plot->canvas), 
				plot->pixel_width, plot->pixel_height);
    gtk_widget_set_events (plot->canvas, GDK_EXPOSURE_MASK
                           | GDK_LEAVE_NOTIFY_MASK
                           | GDK_BUTTON_PRESS_MASK
                           | GDK_BUTTON_RELEASE_MASK
                           | GDK_POINTER_MOTION_MASK
                           | GDK_POINTER_MOTION_HINT_MASK);

    GTK_WIDGET_SET_FLAGS(plot->canvas, GTK_CAN_FOCUS);

    g_signal_connect(G_OBJECT(plot->canvas), "button_press_event", 
		     G_CALLBACK(plot_button_press), plot);
    g_signal_connect(G_OBJECT(plot->canvas), "button_release_event", 
		     G_CALLBACK(plot_button_release), plot);

    /* create the contents of the status area */
    if (plot_has_xrange(plot)) {
	/* cursor label (graph position indicator) */
	label_frame = gtk_frame_new(NULL);
	gtk_frame_set_shadow_type(GTK_FRAME(label_frame), GTK_SHADOW_IN);

	plot->cursor_label = gtk_label_new(" ");
	gtk_container_add(GTK_CONTAINER(label_frame), plot->cursor_label);
	gtk_widget_show(plot->cursor_label);
    }

    /* the statusbar */
    plot->statusbar = gtk_statusbar_new();

    gtk_widget_set_size_request(plot->statusbar, 1, -1);
    gtk_statusbar_set_has_resize_grip(GTK_STATUSBAR(plot->statusbar), FALSE);

    gtk_container_set_resize_mode(GTK_CONTAINER (plot->statusbar),
				  GTK_RESIZE_QUEUE);
    plot->cid = gtk_statusbar_get_context_id(GTK_STATUSBAR(plot->statusbar),
					     "plot_message");

    if (!plot->err) {
	gtk_statusbar_push(GTK_STATUSBAR(plot->statusbar),
			   plot->cid, _(" Click on graph for pop-up menu"));
    }
    
    if (plot_has_xrange(plot)) {
	g_signal_connect(G_OBJECT(plot->canvas), "motion_notify_event",
			 G_CALLBACK(motion_notify_event), plot);
    }

    /* pack the widgets */
    gtk_box_pack_start(GTK_BOX(canvas_hbox), plot->canvas, FALSE, FALSE, 0);

    /* fill the status area */
    if (plot_has_xrange(plot)) {
	gtk_box_pack_start(GTK_BOX(status_hbox), label_frame, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(status_hbox), plot->statusbar, TRUE, TRUE, 0); 

    /* show stuff */
    gtk_widget_show(plot->canvas);

    if (plot_has_xrange(plot)) {
	gtk_widget_show(label_frame);
    }

    gtk_widget_show(plot->statusbar);
    gtk_widget_show(plot->statusarea);

    gtk_widget_realize(plot->canvas);
    gdk_window_set_back_pixmap(plot->canvas->window, NULL, FALSE);

    if (plot_has_xrange(plot)) {
	gtk_widget_realize(plot->cursor_label);
	gtk_widget_set_size_request(plot->cursor_label, 140, -1);
    }

    gtk_widget_show(vbox);
#if 1
    fprintf(stderr, "calling gtk_widget_show on plot->shell\n");
#endif
    gtk_widget_show(plot->shell);       

    /* set the focus to the canvas area */
    gtk_widget_grab_focus(plot->canvas);  

    plot->pixmap = gdk_pixmap_new(plot->shell->window, 
				  plot->pixel_width, plot->pixel_height, 
				  -1);
    g_signal_connect(G_OBJECT(plot->canvas), "expose_event",
		     G_CALLBACK(plot_expose), plot->pixmap);

    err = render_pngfile(plot, PNG_START);
    if (err) {
	gtk_widget_destroy(plot->shell);
	plot = NULL;
    }

    return plot;
}

void gnuplot_show_png_by_name (const char *fname)
{
    gnuplot_show_png(fname, NULL, 0);
}

void display_session_graph_png (const char *fname) 
{
    char fullname[MAXLEN];
    gchar *plotcmd;
    int err = 0;

    if (g_path_is_absolute(fname)) {
	strcpy(fullname, fname);
    } else {
	sprintf(fullname, "%s%s", paths.dotdir, fname);
    }

    if (add_png_term_to_plotfile(fullname)) {
	return;
    }

    plotcmd = g_strdup_printf("\"%s\" \"%s\"", paths.gnuplot, fullname);
    err = gretl_spawn(plotcmd);
    g_free(plotcmd);

    if (err) {
	gui_errmsg(err);
    } else {
	gnuplot_show_png(fullname, NULL, 1);
    }
}

static int get_png_plot_bounds (const char *str, png_bounds *bounds)
{
    int ret = GRETL_PNG_OK;

    bounds->xleft = bounds->xright = 0;
    bounds->ybot = bounds->ytop = 0;

    if (sscanf(str, "pixel_bounds: %d %d %d %d",
	       &bounds->xleft, &bounds->xright,
	       &bounds->ybot, &bounds->ytop) != 4) {
	ret = GRETL_PNG_BAD_COMMENTS;
    } 

    if (ret == GRETL_PNG_OK && bounds->xleft == 0 && 
	bounds->xright == 0 && bounds->ybot == 0 && 
	bounds->ytop == 0) {
	ret = GRETL_PNG_NO_COORDS;
    }

#if POINTS_DEBUG
    fprintf(stderr, "Got: xleft=%d, xright=%d, ybot=%d, ytop=%d\n",
	    bounds->xleft, bounds->xright, bounds->ybot, bounds->ytop);
#endif

    return ret;
}

static int get_png_data_bounds (char *str, png_bounds *bounds)
{
    char *p = str;
    int ret = GRETL_PNG_OK;

    while (*p) {
	if (*p == ',') *p = '.';
	p++;
    }

    bounds->xmin = bounds->xmax = 0.0;
    bounds->ymin = bounds->ymax = 0.0;

    gretl_push_c_numeric_locale();

    if (sscanf(str, "data_bounds: %lf %lf %lf %lf",
	       &bounds->xmin, &bounds->xmax,
	       &bounds->ymin, &bounds->ymax) != 4) {
	ret = GRETL_PNG_BAD_COMMENTS;
    } 

    if (ret == GRETL_PNG_OK && bounds->xmin == 0.0 && 
	bounds->xmax == 0.0 && bounds->ymin == 0.0 && 
	bounds->ymax == 0.0) {
	ret = GRETL_PNG_NO_COORDS;
    } 

#if POINTS_DEBUG
    fprintf(stderr, "Got: xmin=%g, xmax=%g, ymin=%g, ymax=%g\n",
	    bounds->xmin, bounds->xmax, bounds->ymin, bounds->ymax);
#endif

    gretl_pop_c_numeric_locale();

    return ret;
}

static int get_png_bounds_info (png_bounds *bounds)
{
    char bbname[MAXLEN];
    FILE *fp;
    char line[128];
    int plot_ret = -1, data_ret = -1;
    int ret = GRETL_PNG_OK;

    build_path(bbname, paths.dotdir, "gretltmp.png", ".bounds"); 
    fp = gretl_fopen(bbname, "r");

    if (fp == NULL) {
	return GRETL_PNG_NO_COMMENTS;
    }

    if (fgets(line, sizeof line, fp) == NULL) {
	plot_ret = GRETL_PNG_NO_COMMENTS;
    } else {
	plot_ret = get_png_plot_bounds(line, bounds);
    }

    if (fgets(line, sizeof line, fp) == NULL) {
	data_ret = GRETL_PNG_NO_COMMENTS;
    } else {
	data_ret = get_png_data_bounds(line, bounds);
    }

    if (plot_ret == GRETL_PNG_NO_COORDS && data_ret == GRETL_PNG_NO_COORDS) {
	/* comments were present and correct, but all zero */
	ret = GRETL_PNG_NO_COORDS;
    } else if (plot_ret != GRETL_PNG_OK || data_ret != GRETL_PNG_OK) {
	/* one or both set of coordinates bad or missing */
	if (plot_ret >= 0 || data_ret >= 0) {
	    ret = GRETL_PNG_BAD_COMMENTS;
	} else {
	    ret = GRETL_PNG_NO_COMMENTS;
	}
    }

    fclose(fp);
    remove(bbname);

    return ret;
}


