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
 *   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111, USA.
 *
 */

/*  guiprint.h for gretl */ 

#ifndef GUIPRINT_H
#define GUIPRINT_H

#ifdef G_OS_WIN32
int win_copy_buf (char *buf, int format, size_t len);
#endif

#if defined(G_OS_WIN32) || defined (USE_GNOME)
void winprint (char *fullbuf, char *selbuf);
#endif

void texprint_summary (GRETLSUMMARY *summ,
		       const DATAINFO *pdinfo,
		       PRN *prn);

void rtfprint_summary (GRETLSUMMARY *summ,
		       const DATAINFO *pdinfo,
		       PRN *prn);

void texprint_corrmat (CORRMAT *corr,
		       const DATAINFO *pdinfo, 
		       PRN *prn);

void rtfprint_corrmat (CORRMAT *corr,
		       const DATAINFO *pdinfo, 
		       PRN *prn);

void texprint_fit_resid (const FITRESID *fr, 
			 const DATAINFO *pdinfo, 
			 PRN *prn);

void rtfprint_fit_resid (const FITRESID *fr, 
			 const DATAINFO *pdinfo, 
			 PRN *prn);

void texprint_fcast_with_errs (const FITRESID *fr, 
			       const DATAINFO *pdinfo, 
			       PRN *prn);

void rtfprint_fcast_with_errs (const FITRESID *fr, 
			       const DATAINFO *pdinfo, 
			       PRN *prn);

void texprint_confints (const CONFINT *cf, 
			const DATAINFO *pdinfo, 
			PRN *prn);

void rtfprint_confints (const CONFINT *cf, 
			const DATAINFO *pdinfo, 
			PRN *prn);

void texprint_vcv (const VCV *vcv, 
		   const DATAINFO *pdinfo, 
		   PRN *prn);

void rtfprint_vcv (const VCV *vcv, 
		   const DATAINFO *pdinfo, 
		   PRN *prn);

void augment_copy_menu (windata_t *vwin);

int csv_to_clipboard (void);

#endif /* GUIPRINT_H */
