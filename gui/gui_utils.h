#ifndef GUI_UTILS_H
#define GUI_UTILS_H

/* functions follow */

#ifdef G_OS_WIN32
void read_rc (void);
#endif

#if defined(G_OS_WIN32) || defined (USE_GNOME)
void window_print (gpointer data, guint u, GtkWidget *widget);
#endif

void load_fixed_font (void);

void write_rc (void);

int getbufline (char *buf, char *line, int init);

void flip (GtkItemFactory *ifac, char *path, gboolean s);

void mkfilelist (int filetype, const char *newfile);

void delete_from_filelist (int filetype, const char *fname);

void add_files_to_menu (int filetype);

int copyfile (const char *src, const char *dest);

void prn_to_clipboard (PRN *prn);

int isdir (const char *path);

void append_dir (char *fname, const char *dir);

char *endbit (char *dest, char *src, int addscore);
 
void set_rcfile (void);

void delete_model (GtkWidget *widget, gpointer data);

void delete_widget (GtkWidget *widget, gpointer data);

void catch_key (GtkWidget *w, GdkEventKey *key);

void *mymalloc (size_t size); 

void *myrealloc (void *ptr, size_t size);

void clear_data (int full);

void register_data (const char *fname, int record);

void verify_open_data (gpointer userdata);

void datafile_find (GtkWidget *widget, gpointer data);

void verify_open_session (gpointer userdata);

void save_session (char *fname);

void helpfile_init (void);

void menu_find (gpointer data, guint dbfind, GtkWidget *widget);

void close_window (gpointer data, guint win_code, GtkWidget *widget);

void context_help (GtkWidget *widget, gpointer data);

void do_gui_help (gpointer data, guint pos, GtkWidget *widget);

void do_script_help (gpointer data, guint pos, GtkWidget *widget);

void windata_init (windata_t *mydata);

void free_windata (GtkWidget *w, gpointer data);

windata_t *view_buffer (PRN *prn, int hsize, int vsize, 
			char *title, int role,
			GtkItemFactoryEntry menu_items[]);

windata_t *view_file (char *filename, int editable, int del_file, 
		      int hsize, int vsize, int role, 
		      GtkItemFactoryEntry menu_items[]);

windata_t *edit_buffer (char **pbuf, int hsize, int vsize, 
			char *title, int role);

int view_model (PRN *prn, MODEL *pmod, int hsize, int vsize, 
		char *title);

void setup_column (GtkWidget *listbox, int column, int width);

void errbox (const char *msg);

void infobox (const char *msg);

int validate_varname (const char *varname);

void options_dialog (gpointer data);

void font_selector (void);

void text_copy (gpointer data, guint how, GtkWidget *widget);

void text_paste (windata_t *mydata, guint u, GtkWidget *widget);

void text_undo (windata_t *mydata, guint u, GtkWidget *widget);

void make_menu_item (gchar *label, GtkWidget *menu,
		     GtkSignalFunc func, gpointer data);

void get_stats_table (void);

int gui_open_plugin (const char *plugin, void **handle);

void get_default_dir (char *s);

#endif /* GUI_UTILS_H */
