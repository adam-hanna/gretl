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

#ifndef COMPAT_H
#define COMPAT_H

void graphyzx (const int *list, const double *y1, const double *y2, 
	       const double *x, int n, const char *yname, 
	       const char *xname, const DATAINFO *pdinfo, 
	       gretlopt oflag, PRN *prn);

int ascii_graph (const int *list, const double **Z, const DATAINFO *pdinfo, 
		 gretlopt opt, PRN *prn);

int rhodiff (char *param, const int *list, double ***pZ, DATAINFO *pdinfo);

int gretl_multiply (char *s, int *list, char *sfx, double ***pZ,
		    DATAINFO *pdinfo);

#endif /* COMPAT_H */
