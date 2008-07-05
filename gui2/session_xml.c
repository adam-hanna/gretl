
/* addendum to session.c, for handling the saving of session info to
   an XML file, and the re-building of a session from same.
*/

#include "usermat.h"

static int check_graph_file (const char *fname)
{
    char fullname[MAXLEN];
    FILE *fp;
    int err = 0;

    session_file_make_path(fullname, fname);
    fp = gretl_fopen(fullname, "r");

    if (fp == NULL) {
	file_read_errbox(fname);
	err = 1;
    } else {
	fclose(fp);
	err = maybe_recode_gp_file_to_utf8(fullname);
    }

    return err;
}

static int restore_session_graphs (xmlNodePtr node)
{
    xmlNodePtr cur;
    int i = 0;
    int inpage = 0;
    int err = 0;

    /* reset prior to parsing */
    session.ngraphs = 0;

    cur = node->xmlChildrenNode;

    while (cur != NULL && !err) {
	xmlChar *name = NULL;
	xmlChar *fname = NULL;
	int type, ID;

	name = xmlGetProp(cur, (XUC) "name");
	if (name == NULL) {
	    err = 1;
	}

	if (!err) {
	    fname = xmlGetProp(cur, (XUC) "fname");
	    if (fname == NULL) {
		err = 1;
	    } else {
		err = check_graph_file((const char *) fname);
	    } 
	}

	if (!err && (!gretl_xml_get_prop_as_int(cur, "ID", &ID) ||
	    !gretl_xml_get_prop_as_int(cur, "type", &type))) {
	    err = 1;
	}

	if (!err) {
	    SESSION_GRAPH *sg;

	    sg = session_append_graph((const char *) name,
				      (const char *) fname,
				      type);
	    err = (sg == NULL);
	}

	if (!err) {
	    if (gretl_xml_get_prop_as_int(cur, "inpage", &inpage)) {
		graph_page_add_file((const char *) fname); /* FIXME path? */
	    }
	}

	free(name);
	free(fname);

	cur = cur->next;
	i++;
    }

    return err;
}

static int restore_session_texts (xmlNodePtr node, xmlDocPtr doc)
{
    xmlNodePtr cur;
    int i = 0;
    int err = 0;

    session.ntexts = 0;

    cur = node->xmlChildrenNode;

    while (cur != NULL && !err) {
	xmlChar *name = NULL;
	xmlChar *buf = NULL;

	name = xmlGetProp(cur, (XUC) "name");
	if (name == NULL) {
	    err = 1;
	} else {
	    buf = xmlNodeListGetString(doc, cur->xmlChildrenNode, 1);
	    if (buf == NULL) {
		err = 1;
	    } else {
		err = session_append_text((const char *) name,
					  (char *) buf);
	    }
	}

	free(name);

	cur = cur->next;
	i++;
    }

    return err;
}

static int data_submask_from_xml (xmlNodePtr node, xmlDocPtr doc,
				  struct sample_info *sinfo)
{
    char *mask;
    int err;

    err = gretl_xml_get_submask(node, doc, &mask);
    if (!err) {
	sinfo->mask = mask;
    }

    return err;
}

static gpointer rebuild_session_model (const char *fname, 
				       GretlObjType type,
				       SavedObjectFlags *flags,
				       int *err)
{
    gpointer ptr = NULL;
    xmlDocPtr doc;
    xmlNodePtr node;
    char *name;

    *err = gretl_xml_open_doc_root(fname, 
				   (type == GRETL_OBJ_EQN)? "gretl-model" :
				   (type == GRETL_OBJ_VAR)? "gretl-VAR" :
				   "gretl-equation-system", &doc, &node);
    if (*err) {
	fprintf(stderr, "Failed on gretl_xml_open_doc_root\n");
	return NULL;
    }

    if (!gretl_xml_get_prop_as_int(node, "saveflags", (int *) flags)) {
	*flags = IN_GUI_SESSION;
    }    

    if (type == GRETL_OBJ_EQN) {
	ptr = gretl_model_from_XML(node, doc, err);
    } else if (type == GRETL_OBJ_VAR) {
	ptr = gretl_VAR_from_XML(node, doc, err);
    } else {
	ptr = equation_system_from_XML(node, doc, err);
    }

    xmlFreeDoc(doc);

    if (ptr != NULL) {
	/* FIXME: division of labour with 'reattach_model' below?? */
	name = gretl_object_get_name(ptr, type);
	gretl_stack_object_as(ptr, type, name);
    }

    /* need to clean up on error here (also: clean up XML parser?) */

    return ptr;
}

static int 
reattach_model (void *ptr, GretlObjType type, const char *name, 
		SavedObjectFlags flags)
{
    int err = 0;

#if 1
    fprintf(stderr, "reattach_model: IN_GUI_SESSION = %s, IN_MODEL_TABLE= %s\n"
	    "IN_NAMED_STACK = %s, IS_LAST_MODEL = %s\n",
	    (flags & IN_GUI_SESSION)? "yes" : "no",
	    (flags & IN_MODEL_TABLE)? "yes" : "no",
	    (flags & IN_NAMED_STACK)? "yes" : "no",
	    (flags & IS_LAST_MODEL)? "yes" : "no");
#endif

    if (flags & IN_GUI_SESSION) {
	SESSION_MODEL *smod;

	smod = session_model_new(ptr, name, type);
	if (smod == NULL) {
	    fprintf(stderr, "error %d from session_model_new\n", err);
	    err = E_ALLOC;
	} else {
	    err = session_append_model(smod);
	    fprintf(stderr, "error %d from session_append_model\n", err);
	}
    }

    if (!err && (flags & IN_MODEL_TABLE)) {
	add_to_model_table(ptr, MODEL_ADD_BY_CMD, NULL);
    }

    if (!err && (flags & IN_NAMED_STACK)) {
	err = gretl_stack_object(ptr, type);
    } 

    if (!err && (flags & IS_LAST_MODEL)) {
	set_as_last_model(ptr, type);
    }     

    return err;
}

static int restore_session_models (xmlNodePtr node, xmlDocPtr doc)
{
    char fullname[MAXLEN];
    xmlNodePtr cur;
    int i, err = 0;

    /* reset prior to parsing */
    session.nmodels = 0;

    cur = node->xmlChildrenNode;
    i = 0;

    while (cur != NULL && !err) {
	SavedObjectFlags flags;
	xmlChar *fname = NULL;
	xmlChar *name = NULL;
	gpointer ptr = NULL;
	int type = GRETL_OBJ_EQN;

	fname = xmlGetProp(cur, (XUC) "fname");
	if (fname == NULL) {
	    err = 1;
	} else {
	    name = xmlGetProp(cur, (XUC) "name");
	    if (name == NULL) {
		err = 1;
	    }
	}

	if (!err) {
	    session_file_make_path(fullname, (const char *) fname);
	    gretl_xml_get_prop_as_int(cur, "type", &type);
	    fprintf(stderr, "model file: fname='%s', type=%d\n", fullname, type);
	    ptr = rebuild_session_model(fullname, type, &flags, &err);
	}

	if (!err) {
	    fprintf(stderr, "reattaching model to session\n");
	    err = reattach_model(ptr, type, (const char *) name, flags);
	    if (!err) {
		model_count_plus();
	    } else {
		fprintf(stderr, "reattach_model: failed on %s (err = %d)\n",
			name, err);
	    }
	} else {
	    fprintf(stderr, "rebuild_session_model: failed on %s (err = %d)\n",
		    fullname, err);
	}

	free(fname);
	free(name);

	cur = cur->next;
	i++;
    }

#if SESSION_DEBUG
    fprintf(stderr, "restore_session_models: returning %d\n", err);
#endif

    return err;
}

static int 
read_session_xml (const char *fname, struct sample_info *sinfo) 
{
    xmlDocPtr doc = NULL;
    xmlNodePtr cur = NULL;
    xmlChar *tmp;
    int err = 0;

    LIBXML_TEST_VERSION
	xmlKeepBlanksDefault(0);

    err = gretl_xml_open_doc_root(fname, "gretl-session", &doc, &cur);

    if (err) {
	gui_errmsg(err);
	return 1;
    }

    /* read datafile attribute, if present */
    tmp = xmlGetProp(cur, (XUC) "datafile");
    if (tmp != NULL) {
	strcpy(sinfo->datafile, (char *) tmp);
	my_filename_from_utf8(sinfo->datafile);
	free(tmp);
    }

    /* Now walk the tree */
    cur = cur->xmlChildrenNode;
    while (cur != NULL && !err) {
        if (!xmlStrcmp(cur->name, (XUC) "sample")) {
	    tmp = xmlGetProp(cur, (XUC) "t1");
	    if (tmp != NULL) {
		sinfo->t1 = atoi((const char *) tmp);
		free(tmp);
	    } else {
		err = 1;
	    }
	    tmp = xmlGetProp(cur, (XUC) "t2");
	    if (tmp != NULL) {
		sinfo->t2 = atoi((const char *) tmp);
		free(tmp);
	    } else {
		err = 1;
	    }
	} else if (!xmlStrcmp(cur->name, (XUC) "submask")) {
	    err = data_submask_from_xml(cur, doc, sinfo);
	} else if (!xmlStrcmp(cur->name, (XUC) "resample")) {
	    tmp = xmlGetProp(cur, (XUC) "seed");
	    if (tmp != NULL) {
		sinfo->seed = (unsigned) atoi((const char *) tmp);
		free(tmp);
	    }
	    tmp = xmlGetProp(cur, (XUC) "n");
	    if (tmp != NULL) {
		sinfo->resample_n = atoi((const char *) tmp);
		free(tmp);
	    }
	    if (sinfo->resample_n <= 0) {
		sinfo->seed = 0;
		sinfo->resample_n = 0;
	    }
	} else if (!xmlStrcmp(cur->name, (XUC) "models")) {
	    tmp = xmlGetProp(cur, (XUC) "count");
	    if (tmp != NULL) {
		session.nmodels = atoi((const char *) tmp);
		free(tmp);
		if (session.nmodels > 0) {
		    err = restore_session_models(cur, doc);
		}		
	    }
        } else if (!xmlStrcmp(cur->name, (XUC) "graphs")) {
	    tmp = xmlGetProp(cur, (XUC) "count");
	    if (tmp != NULL) {
		session.ngraphs = atoi((const char *) tmp);
		free(tmp);
		if (session.ngraphs > 0) {
		    err = restore_session_graphs(cur);
		}
	    }	    
	} else if (!xmlStrcmp(cur->name, (XUC) "texts")) {
	    tmp = xmlGetProp(cur, (XUC) "count");
	    if (tmp != NULL) {
		session.ntexts = atoi((const char *) tmp);
		free(tmp);
		if (session.ntexts > 0) {
		    err = restore_session_texts(cur, doc);
		}		
	    }
	} else if (!xmlStrcmp(cur->name, (XUC) "notes")) {
	    session.notes = 
		(char *) xmlNodeListGetString(doc, cur->xmlChildrenNode, 1);
	    if (session.notes == NULL) {
		err = 1;
	    }
	}
	if (!err) {
	    cur = cur->next;
	}
    }

    if (doc != NULL) {
	xmlFreeDoc(doc);
	xmlCleanupParser();
    }

    return err;
}

static int maybe_read_matrix_file (const char *fname) 
{
    FILE *fp;

    fp = gretl_fopen(fname, "r");
    if (fp == NULL) {
	/* nothing to be read */
	return 0;
    }

    fclose(fp);

    return load_user_matrix_file(fname);
}

static int maybe_read_functions_file (const char *fname) 
{
    FILE *fp;

    fp = gretl_fopen(fname, "r");
    if (fp == NULL) {
	/* nothing to be read */
	return 0;
    }

    fclose(fp);

    return read_session_functions_file(fname);
}

static int maybe_read_lists_file (const char *fname) 
{
    FILE *fp;

    fp = gretl_fopen(fname, "r");
    if (fp == NULL) {
	/* nothing to be read */
	return 0;
    }

    fclose(fp);

    return load_user_lists_file(fname);
}

static int model_in_session (const void *ptr)
{
    int i;

    for (i=0; i<session.nmodels; i++) {
	if (session.models[i]->ptr == ptr) {
	    return 1;
	}
    }

    return 0;
}

static SavedObjectFlags model_save_flags (const void *ptr, 
					  GretlObjType type)
{
    SavedObjectFlags flags = 0;

    if (model_in_session(ptr)) {
	flags |= IN_GUI_SESSION;
    }

    if (object_is_on_stack(ptr)) {
	flags |= IN_NAMED_STACK;
    }

    if (get_last_model(NULL) == ptr) {
	flags |= IS_LAST_MODEL;
    }

    if (type == GRETL_OBJ_EQN && in_model_table(ptr)) {
	flags |= IN_MODEL_TABLE;
    }

    return flags;
}

static int maybe_write_matrix_file (void)
{
    char fullname[MAXLEN];
    FILE *fp;

    if (n_user_matrices() == 0) {
	return 0;
    }

    session_file_make_path(fullname, "matrices.xml");
    fp = gretl_fopen(fullname, "w");
    if (fp == NULL) {
	return E_FOPEN;
    }

    write_matrices_to_file(fp);
    fclose(fp);

    return 0;
}

static int maybe_write_function_file (void)
{
    char fullname[MAXLEN];

    session_file_make_path(fullname, "functions.xml");
    return write_user_function_file(fullname);
}

static int maybe_write_lists_file (void)
{
    char fullname[MAXLEN];

    session_file_make_path(fullname, "lists.xml");
    return gretl_serialize_lists(fullname);
}

static int write_session_xml (const char *datname)
{
    MODEL *pmod;
    char fname[MAXLEN];
    char tmpname[MAXLEN];
    FILE *fp, *fq;
    int nmodels;
    int tabmodels;
    int i, modnum;
    int err = 0;

    chdir(paths.dotdir);

    sprintf(fname, "%s%csession.xml", session.dirname, SLASH);
    fp = gretl_fopen(fname, "w");

    if (fp == NULL) {
	file_write_errbox(fname);
	return E_FOPEN;
    }

    gretl_xml_header(fp);

    if (*datname != '\0') {
	/* ensure UTF-8 inside XML file */
	gchar *trname = my_filename_to_utf8(datname);

	fprintf(fp, "<gretl-session datafile=\"%s\">\n", trname);
	g_free(trname);
    } else {
	fputs("<gretl-session>\n", fp);
    }

    fprintf(fp, " <sample t1=\"%d\" t2=\"%d\"/>\n", datainfo->t1, datainfo->t2);
    write_datainfo_submask(datainfo, fp);

    nmodels = session.nmodels;
    tabmodels = model_table_n_models();

    for (i=0; i<tabmodels; i++) {
	pmod = model_table_model_by_index(i);
	if (!model_in_session(pmod)) {
	    nmodels++;
	}
    }

    fprintf(fp, " <models count=\"%d\">\n", nmodels);

    modnum = 1;

    for (i=0; i<session.nmodels && !err; i++) {
	int type = session.models[i]->type;
	void *ptr = session.models[i]->ptr;
	SavedObjectFlags sflags;

	sprintf(tmpname, "%s%cmodel.%d", session.dirname, SLASH, modnum);
	fq = gretl_fopen(tmpname, "w");

	if (fq == NULL) {
	    file_write_errbox(tmpname);
	    err = E_FOPEN;
	} else {
	    sprintf(tmpname, "model.%d", modnum++);
	    fprintf(fp, "  <session-model name=\"%s\" fname=\"%s\" type=\"%d\"/>\n", 
		    session.models[i]->name, tmpname, type);
	    gretl_xml_header(fq);
	    sflags = model_save_flags(ptr, type);
	    if (type == GRETL_OBJ_EQN) {
		gretl_model_serialize(ptr, sflags, fq);
	    } else if (type == GRETL_OBJ_VAR) {
		gretl_VAR_serialize(ptr, sflags, fq);
	    } else if (type == GRETL_OBJ_SYS) {
		equation_system_serialize(ptr, sflags, fq);
	    }
	    fclose(fq);
	}
    }

    for (i=0; i<tabmodels && !err; i++) {
	pmod = model_table_model_by_index(i);
	if (!model_in_session(pmod)) {
	    sprintf(tmpname, "%s%cmodel.%d", session.dirname, SLASH, modnum);
	    fq = gretl_fopen(tmpname, "w");
	    if (fq == NULL) {
		file_write_errbox(tmpname);
		err = E_FOPEN;
	    } else {
		sprintf(tmpname, "model.%d", modnum++);
		fprintf(fp, "  <session-model name=\"%s\" fname=\"%s\" type=\"%d\"/>\n", 
			(pmod->name != NULL)? pmod->name : "none", tmpname, 
			GRETL_OBJ_EQN);
		gretl_xml_header(fq);
		gretl_model_serialize(pmod, model_save_flags(pmod, GRETL_OBJ_EQN), 
				      fq);
		fclose(fq);
	    }
	}
    }

    if (err) {
	fclose(fp);
	remove(fname);
	return err;
    }

    fputs(" </models>\n", fp);

    fprintf(fp, " <graphs count=\"%d\">\n", session.ngraphs);
    for (i=0; i<session.ngraphs; i++) {
	fprintf(fp, "  <session-graph name=\"%s\" fname=\"%s\" "
		"ID=\"%d\" type=\"%d\"", 
		session.graphs[i]->name, session.graphs[i]->fname,
		session.graphs[i]->ID, session.graphs[i]->type);
	if (in_graph_page(session.graphs[i]->fname)) {
	    fputs(" inpage=\"1\"/>\n", fp);
	} else {
	    fputs("/>\n", fp);
	}
    } 
    fputs(" </graphs>\n", fp);

    fprintf(fp, " <texts count=\"%d\">\n", session.ntexts);
    for (i=0; i<session.ntexts; i++) {
	fprintf(fp, "  <session-text name=\"%s\">", session.texts[i]->name);
	gretl_xml_put_raw_string(session.texts[i]->buf, fp);
	fputs("</session-text>\n", fp);
    }    
    fputs(" </texts>\n", fp);

    if (session.notes != NULL) {
	fputs("<notes>", fp);
	gretl_xml_put_raw_string(session.notes, fp);
	fputs("</notes>\n", fp);
    } 

    fputs("</gretl-session>\n", fp);

    fclose(fp);

    maybe_write_matrix_file();
    maybe_write_function_file();
    maybe_write_lists_file();

    return 0;
}
