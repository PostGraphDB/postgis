/**********************************************************************
 *
 * PostGIS - Spatial Types for PostgreSQL
 * http://postgis.net
 *
 * Copyright (C) 2008 OpenGeo.org
 * Copyright (C) 2009 Mark Cave-Ayland <mark.cave-ayland@siriusit.co.uk>
 *
 * This is free software; you can redistribute and/or modify it under
 * the terms of the GNU General Public Licence. See the COPYING file.
 *
 * Maintainer: Paul Ramsey <pramsey@cleverelephant.ca>
 *
 **********************************************************************/

#include "../postgis_config.h"

#include <math.h> /* for isnan */

#include "shp2pgsql-core.h"
#include "../liblwgeom/liblwgeom.h"
#include "../liblwgeom/lwgeom_log.h" /* for LWDEBUG macros */



/* Internal ring/point structures */
typedef struct struct_point
{
	double x, y, z, m;
} Point;

typedef struct struct_ring
{
	Point *list;		/* list of points */
	struct struct_ring *next;
	int n;			/* number of points in list */
	unsigned int linked; 	/* number of "next" rings */
} Ring;


/*
 * Internal functions
 */

#define UTF8_GOOD_RESULT 0
#define UTF8_BAD_RESULT 1
#define UTF8_NO_RESULT 2

char *escape_copy_string(char *str);
char *escape_insert_string(char *str);

int GeneratePointGeometry(SHPLOADERSTATE *state, SHPObject *obj, char **geometry, int force_multi);
int GenerateLineStringGeometry(SHPLOADERSTATE *state, SHPObject *obj, char **geometry);
int PIP(Point P, Point *V, int n);
int FindPolygons(SHPObject *obj, Ring ***Out);
void ReleasePolygons(Ring **polys, int npolys);
int GeneratePolygonGeometry(SHPLOADERSTATE *state, SHPObject *obj, char **geometry);


/* Return allocated string containing UTF8 string converted from encoding fromcode */
static int
utf8(const char *fromcode, char *inputbuf, char **outputbuf)
{
	iconv_t cd;
	char *outputptr;
	size_t outbytesleft;
	size_t inbytesleft;

	inbytesleft = strlen(inputbuf);

	cd = iconv_open("UTF-8", fromcode);
	if ( cd == ((iconv_t)(-1)) )
		return UTF8_NO_RESULT;

	outbytesleft = inbytesleft * 3 + 1; /* UTF8 string can be 3 times larger */
	/* then local string */
	*outputbuf = (char *)malloc(outbytesleft);
	if (!*outputbuf)
		return UTF8_NO_RESULT;

	memset(*outputbuf, 0, outbytesleft);
	outputptr = *outputbuf;

	/* Does this string convert cleanly? */
	if ( iconv(cd, &inputbuf, &inbytesleft, &outputptr, &outbytesleft) == (size_t)-1 )
	{
#ifdef HAVE_ICONVCTL
		int on = 1;
		/* No. Try to convert it while transliterating. */
		iconvctl(cd, ICONV_SET_TRANSLITERATE, &on);
		if ( iconv(cd, &inputbuf, &inbytesleft, &outputptr, &outbytesleft) == -1 )
		{
			/* No. Try to convert it while discarding errors. */
			iconvctl(cd, ICONV_SET_DISCARD_ILSEQ, &on);
			if ( iconv(cd, &inputbuf, &inbytesleft, &outputptr, &outbytesleft) == -1 )
			{
				/* Still no. Throw away the buffer and return. */
				free(*outputbuf);
				iconv_close(cd);
				return UTF8_NO_RESULT;
			}
		}
		iconv_close(cd);
		return UTF8_BAD_RESULT;
#else
		free(*outputbuf);
		iconv_close(cd);
		return UTF8_NO_RESULT;
#endif
	}
	/* Return a good result, converted string is in buffer. */
	iconv_close(cd);
	return UTF8_GOOD_RESULT;
}

/**
 * Escape input string suitable for COPY. If no characters require escaping, simply return
 * the input pointer. Otherwise return a new allocated string.
 */
char *
escape_copy_string(char *str)
{
	/*
	 * Escape the following characters by adding a preceding backslash
	 *      tab, backslash, cr, lf
	 *
	 * 1. find # of escaped characters
	 * 2. make new string
	 *
	 */

	char *result;
	char *ptr, *optr;
	int toescape = 0;
	size_t size;

	ptr = str;

	/* Count how many characters we need to escape so we know the size of the string we need to return */
	while (*ptr)
	{
		if (*ptr == '\t' || *ptr == '\\' || *ptr == '\n' || *ptr == '\r')
			toescape++;

		ptr++;
	}

	/* If we don't have to escape anything, simply return the input pointer */
	if (toescape == 0)
		return str;

	size = ptr - str + toescape + 1;
	result = calloc(1, size);
	optr = result;
	ptr = str;

	while (*ptr)
	{
		if ( *ptr == '\t' || *ptr == '\\' || *ptr == '\n' || *ptr == '\r' )
			*optr++ = '\\';

		*optr++ = *ptr++;
	}

	*optr = '\0';

	return result;
}


/**
 * Escape input string suitable for INSERT. If no characters require escaping, simply return
 * the input pointer. Otherwise return a new allocated string.
 */
char *
escape_insert_string(char *str)
{
	/*
	 * Escape single quotes by adding a preceding single quote
	 *
	 * 1. find # of characters
	 * 2. make new string
	 */

	char *result;
	char *ptr, *optr;
	int toescape = 0;
	size_t size;

	ptr = str;

	/* Count how many characters we need to escape so we know the size of the string we need to return */
	while (*ptr)
	{
		if (*ptr == '\'')
			toescape++;

		ptr++;
	}

	/* If we don't have to escape anything, simply return the input pointer */
	if (toescape == 0)
		return str;

	size = ptr - str + toescape + 1;
	result = calloc(1, size);
	optr = result;
	ptr = str;

	while (*ptr)
	{
		if (*ptr == '\'')
			*optr++='\'';

		*optr++ = *ptr++;
	}

	*optr='\0';

	return result;
}


/**
 * @brief Generate an allocated geometry string for shapefile object obj using the state parameters
 * if "force_multi" is true, single points will instead be created as multipoints with a single vertice.
 */
int
GeneratePointGeometry(SHPLOADERSTATE *state, SHPObject *obj, char **geometry, int force_multi)
{
	LWGEOM **lwmultipoints;
	LWGEOM *lwgeom = NULL;

	POINT4D point4d;

	int dims = 0;
	int u;

	char *mem;
	size_t mem_length;

	FLAGS_SET_Z(dims, state->has_z);
	FLAGS_SET_M(dims, state->has_m);

	/* POINT EMPTY encoded as POINT(NaN NaN) */
	if (obj->nVertices == 1 && isnan(obj->padfX[0]) && isnan(obj->padfY[0]))
	{
		lwgeom = lwpoint_as_lwgeom(lwpoint_construct_empty(state->from_srid, state->has_z, state->has_m));
	}
	/* Not empty */
	else
	{
		/* Allocate memory for our array of LWPOINTs and our dynptarrays */
		lwmultipoints = malloc(sizeof(LWPOINT *) * obj->nVertices);

		/* We need an array of pointers to each of our sub-geometries */
		for (u = 0; u < obj->nVertices; u++)
		{
			/* Create a ptarray containing a single point */
			POINTARRAY *pa = ptarray_construct_empty(state->has_z, state->has_m, 1);

			/* Generate the point */
			point4d.x = obj->padfX[u];
			point4d.y = obj->padfY[u];

			if (state->has_z)
				point4d.z = obj->padfZ[u];
			if (state->has_m)
				point4d.m = obj->padfM[u];

			/* Add in the point! */
			ptarray_append_point(pa, &point4d, LW_TRUE);

			/* Generate the LWPOINT */
			lwmultipoints[u] = lwpoint_as_lwgeom(lwpoint_construct(state->from_srid, NULL, pa));
		}

		/* If we have more than 1 vertex then we are working on a MULTIPOINT and so generate a MULTIPOINT
		rather than a POINT */
		if ((obj->nVertices > 1) || force_multi)
		{
			lwgeom = lwcollection_as_lwgeom(lwcollection_construct(MULTIPOINTTYPE, state->from_srid, NULL, obj->nVertices, lwmultipoints));
		}
		else
		{
			lwgeom = lwmultipoints[0];
			lwfree(lwmultipoints);
		}
	}

	if (state->config->use_wkt)
	{
		mem = lwgeom_to_wkt(lwgeom, WKT_EXTENDED, WKT_PRECISION, &mem_length);
	}
	else
	{
		mem = lwgeom_to_hexwkb_buffer(lwgeom, WKB_EXTENDED);
	}

	if ( !mem )
	{
		snprintf(state->message, SHPLOADERMSGLEN, "unable to write geometry");
		return SHPLOADERERR;
	}

	/* Free all of the allocated items */
	lwgeom_free(lwgeom);

	/* Return the string - everything ok */
	*geometry = mem;

	return SHPLOADEROK;
}


/**
 * @brief Generate an allocated geometry string for shapefile object obj using the state parameters
 */
int
GenerateLineStringGeometry(SHPLOADERSTATE *state, SHPObject *obj, char **geometry)
{

	LWGEOM **lwmultilinestrings;
	LWGEOM *lwgeom = NULL;
	POINT4D point4d;
	int dims = 0;
	int u, v, start_vertex, end_vertex;
	char *mem;
	size_t mem_length;


	FLAGS_SET_Z(dims, state->has_z);
	FLAGS_SET_M(dims, state->has_m);

	if (state->config->simple_geometries == 1 && obj->nParts > 1)
	{
		snprintf(state->message, SHPLOADERMSGLEN, _("We have a Multilinestring with %d parts, can't use -S switch!"), obj->nParts);

		return SHPLOADERERR;
	}

	/* Allocate memory for our array of LWLINEs and our dynptarrays */
	lwmultilinestrings = malloc(sizeof(LWPOINT *) * obj->nParts);

	/* We need an array of pointers to each of our sub-geometries */
	for (u = 0; u < obj->nParts; u++)
	{
		/* Create a ptarray containing the line points */
		POINTARRAY *pa = ptarray_construct_empty(state->has_z, state->has_m, obj->nParts);

		/* Set the start/end vertices depending upon whether this is
		a MULTILINESTRING or not */
		if ( u == obj->nParts-1 )
			end_vertex = obj->nVertices;
		else
			end_vertex = obj->panPartStart[u + 1];

		start_vertex = obj->panPartStart[u];

		for (v = start_vertex; v < end_vertex; v++)
		{
			/* Generate the point */
			point4d.x = obj->padfX[v];
			point4d.y = obj->padfY[v];

			if (state->has_z)
				point4d.z = obj->padfZ[v];
			if (state->has_m)
				point4d.m = obj->padfM[v];

			ptarray_append_point(pa, &point4d, LW_FALSE);
		}

		/* Generate the LWLINE */
		lwmultilinestrings[u] = lwline_as_lwgeom(lwline_construct(state->from_srid, NULL, pa));
	}

	/* If using MULTILINESTRINGs then generate the serialized collection, otherwise just a single LINESTRING */
	if (state->config->simple_geometries == 0)
	{
		lwgeom = lwcollection_as_lwgeom(lwcollection_construct(MULTILINETYPE, state->from_srid, NULL, obj->nParts, lwmultilinestrings));
	}
	else
	{
		lwgeom = lwmultilinestrings[0];
		lwfree(lwmultilinestrings);
	}

	if (!state->config->use_wkt)
		mem = lwgeom_to_hexwkb_buffer(lwgeom, WKB_EXTENDED);
	else
		mem = lwgeom_to_wkt(lwgeom, WKT_EXTENDED, WKT_PRECISION, &mem_length);

	if ( !mem )
	{
		snprintf(state->message, SHPLOADERMSGLEN, "unable to write geometry");
		return SHPLOADERERR;
	}

	/* Free all of the allocated items */
	lwgeom_free(lwgeom);

	/* Return the string - everything ok */
	*geometry = mem;

	return SHPLOADEROK;
}


/**
 * @brief PIP(): crossing number test for a point in a polygon
 *      input:   P = a point,
 *               V[] = vertex points of a polygon V[n+1] with V[n]=V[0]
 * @return   0 = outside, 1 = inside
 */
int
PIP(Point P, Point *V, int n)
{
	int cn = 0;    /* the crossing number counter */
	int i;

	/* loop through all edges of the polygon */
	for (i = 0; i < n-1; i++)      /* edge from V[i] to V[i+1] */
	{
		if (((V[i].y <= P.y) && (V[i + 1].y > P.y))    /* an upward crossing */
		        || ((V[i].y > P.y) && (V[i + 1].y <= P.y)))   /* a downward crossing */
		{
			double vt = (float)(P.y - V[i].y) / (V[i + 1].y - V[i].y);
			if (P.x < V[i].x + vt * (V[i + 1].x - V[i].x)) /* P.x < intersect */
				++cn;   /* a valid crossing of y=P.y right of P.x */
		}
	}

	return (cn&1);    /* 0 if even (out), and 1 if odd (in) */
}


int
FindPolygons(SHPObject *obj, Ring ***Out)
{
	Ring **Outer;    /* Pointers to Outer rings */
	int out_index=0; /* Count of Outer rings */
	Ring **Inner;    /* Pointers to Inner rings */
	int in_index=0;  /* Count of Inner rings */
	int pi; /* part index */

#if POSTGIS_DEBUG_LEVEL > 0
	static int call = -1;
	call++;
#endif

	LWDEBUGF(4, "FindPolygons[%d]: allocated space for %d rings\n", call, obj->nParts);

	/* Allocate initial memory */
	Outer = (Ring **)malloc(sizeof(Ring *) * obj->nParts);
	Inner = (Ring **)malloc(sizeof(Ring *) * obj->nParts);

	/* Iterate over rings dividing in Outers and Inners */
	for (pi=0; pi < obj->nParts; pi++)
	{
		int vi; /* vertex index */
		int vs; /* start index */
		int ve; /* end index */
		int nv; /* number of vertex */
		double area = 0.0;
		Ring *ring;

		/* Set start and end vertexes */
		if (pi == obj->nParts - 1)
			ve = obj->nVertices;
		else
			ve = obj->panPartStart[pi + 1];

		vs = obj->panPartStart[pi];

		/* Compute number of vertexes */
		nv = ve - vs;

		/* Allocate memory for a ring */
		ring = (Ring *)malloc(sizeof(Ring));
		ring->list = (Point *)malloc(sizeof(Point) * nv);
		ring->n = nv;
		ring->next = NULL;
		ring->linked = 0;

		/* Iterate over ring vertexes */
		for (vi = vs; vi < ve; vi++)
		{
			int vn = vi+1; /* next vertex for area */
			if (vn == ve)
				vn = vs;

			ring->list[vi - vs].x = obj->padfX[vi];
			ring->list[vi - vs].y = obj->padfY[vi];
			ring->list[vi - vs].z = obj->padfZ[vi];
			ring->list[vi - vs].m = obj->padfM[vi];

			area += (obj->padfX[vi] * obj->padfY[vn]) -
			        (obj->padfY[vi] * obj->padfX[vn]);
		}

		/* Close the ring with first vertex  */
		/*ring->list[vi].x = obj->padfX[vs]; */
		/*ring->list[vi].y = obj->padfY[vs]; */
		/*ring->list[vi].z = obj->padfZ[vs]; */
		/*ring->list[vi].m = obj->padfM[vs]; */

		/* Clockwise (or single-part). It's an Outer Ring ! */
		if (area < 0.0 || obj->nParts == 1)
		{
			Outer[out_index] = ring;
			out_index++;
		}
		else
		{
			/* Counterclockwise. It's an Inner Ring ! */
			Inner[in_index] = ring;
			in_index++;
		}
	}

	LWDEBUGF(4, "FindPolygons[%d]: found %d Outer, %d Inners\n", call, out_index, in_index);

	/* Put the inner rings into the list of the outer rings */
	/* of which they are within */
	for (pi = 0; pi < in_index; pi++)
	{
		Point pt, pt2;
		int i;
		Ring *inner = Inner[pi], *outer = NULL;

		pt.x = inner->list[0].x;
		pt.y = inner->list[0].y;

		pt2.x = inner->list[1].x;
		pt2.y = inner->list[1].y;

		/*
		* If we assume that the case of the "big polygon w/o hole
		* containing little polygon w/ hold" is ordered so that the
		* big polygon comes first, then checking the list in reverse
		* will assign the little polygon's hole to the little polygon
		* w/o a lot of extra fancy containment logic here
		*/
		for (i = out_index - 1; i >= 0; i--)
		{
			int in;

			in = PIP(pt, Outer[i]->list, Outer[i]->n);
			if ( in || PIP(pt2, Outer[i]->list, Outer[i]->n) )
			{
				outer = Outer[i];
				break;
			}
		}

		if (outer)
		{
			outer->linked++;
			while (outer->next)
				outer = outer->next;

			outer->next = inner;
		}
		else
		{
			/* The ring wasn't within any outer rings, */
			/* assume it is a new outer ring. */
			LWDEBUGF(4, "FindPolygons[%d]: hole %d is orphan\n", call, pi);

			Outer[out_index] = inner;
			out_index++;
		}
	}

	*Out = Outer;
	/*
	* Only free the containing Inner array, not the ring elements, because
	* the rings are now owned by the linked lists in the Outer array elements.
	*/
	free(Inner);

	return out_index;
}


void
ReleasePolygons(Ring **polys, int npolys)
{
	int pi;

	/* Release all memory */
	for (pi = 0; pi < npolys; pi++)
	{
		Ring *Poly, *temp;
		Poly = polys[pi];
		while (Poly != NULL)
		{
			temp = Poly;
			Poly = Poly->next;
			free(temp->list);
			free(temp);
		}
	}

	free(polys);
}


/**
 * @brief Generate an allocated geometry string for shapefile object obj using the state parameters
 *
 * This function basically deals with the polygon case. It sorts the polys in order of outer,
 * inner,inner, so that inners always come after outers they are within.
 *
 */
int
GeneratePolygonGeometry(SHPLOADERSTATE *state, SHPObject *obj, char **geometry)
{
	Ring **Outer;
	int polygon_total, ring_total;
	int pi, vi; /* part index and vertex index */

	LWGEOM **lwpolygons;
	LWGEOM *lwgeom;

	POINT4D point4d;

	int dims = 0;

	char *mem;
	size_t mem_length;

	FLAGS_SET_Z(dims, state->has_z);
	FLAGS_SET_M(dims, state->has_m);

	polygon_total = FindPolygons(obj, &Outer);

	if (state->config->simple_geometries == 1 && polygon_total != 1) /* We write Non-MULTI geometries, but have several parts: */
	{
		snprintf(state->message, SHPLOADERMSGLEN, _("We have a Multipolygon with %d parts, can't use -S switch!"), polygon_total);

		return SHPLOADERERR;
	}

	/* Allocate memory for our array of LWPOLYs */
	lwpolygons = malloc(sizeof(LWPOLY *) * polygon_total);

	/* Cycle through each individual polygon */
	for (pi = 0; pi < polygon_total; pi++)
	{
		LWPOLY *lwpoly = lwpoly_construct_empty(state->from_srid, state->has_z, state->has_m);

		Ring *polyring;
		int ring_index = 0;

		/* Firstly count through the total number of rings in this polygon */
		ring_total = 0;
		polyring = Outer[pi];
		while (polyring)
		{
			ring_total = ring_total + 1;
			polyring = polyring->next;
		}

		/* Cycle through each ring within the polygon, starting with the outer */
		polyring = Outer[pi];

		while (polyring)
		{
			/* Create a POINTARRAY containing the points making up the ring */
			POINTARRAY *pa = ptarray_construct_empty(state->has_z, state->has_m, polyring->n);

			for (vi = 0; vi < polyring->n; vi++)
			{
				/* Build up a point array of all the points in this ring */
				point4d.x = polyring->list[vi].x;
				point4d.y = polyring->list[vi].y;

				if (state->has_z)
					point4d.z = polyring->list[vi].z;
				if (state->has_m)
					point4d.m = polyring->list[vi].m;

				ptarray_append_point(pa, &point4d, LW_TRUE);
			}

			/* Copy the POINTARRAY pointer so we can use the LWPOLY constructor */
			lwpoly_add_ring(lwpoly, pa);

			polyring = polyring->next;
			ring_index = ring_index + 1;
		}

		/* Generate the LWGEOM */
		lwpolygons[pi] = lwpoly_as_lwgeom(lwpoly);
	}

	/* If using MULTIPOLYGONS then generate the serialized collection, otherwise just a single POLYGON */
	if (state->config->simple_geometries == 0)
	{
		lwgeom = lwcollection_as_lwgeom(lwcollection_construct(MULTIPOLYGONTYPE, state->from_srid, NULL, polygon_total, lwpolygons));
	}
	else
	{
		lwgeom = lwpolygons[0];
		lwfree(lwpolygons);
	}

	if (!state->config->use_wkt)
		mem = lwgeom_to_hexwkb_buffer(lwgeom, WKB_EXTENDED);
	else
		mem = lwgeom_to_wkt(lwgeom, WKT_EXTENDED, WKT_PRECISION, &mem_length);

	if ( !mem )
	{
		/* Free the linked list of rings */
		ReleasePolygons(Outer, polygon_total);

		snprintf(state->message, SHPLOADERMSGLEN, "unable to write geometry");
		return SHPLOADERERR;
	}

	/* Free all of the allocated items */
	lwgeom_free(lwgeom);

	/* Free the linked list of rings */
	ReleasePolygons(Outer, polygon_total);

	/* Return the string - everything ok */
	*geometry = mem;

	return SHPLOADEROK;
}


/*
 * External functions (defined in shp2pgsql-core.h)
 */


/* Convert the string to lower case */
void
strtolower(char *s)
{
	size_t j;

	for (j = 0; j < strlen(s); j++)
		s[j] = tolower(s[j]);
}


/* Default configuration settings */
void
set_loader_config_defaults(SHPLOADERCONFIG *config)
{
	config->opt = 'c';
	config->table = NULL;
	config->schema = NULL;
	config->geo_col = NULL;
	config->shp_file = NULL;
	config->dump_format = 0;
	config->simple_geometries = 0;
	config->geography = 0;
	config->quoteidentifiers = 0;
	config->forceint4 = 0;
	config->createindex = 0;
	config->analyze = 1;
	config->readshape = 1;
	config->force_output = FORCE_OUTPUT_DISABLE;
	config->encoding = strdup(ENCODING_DEFAULT);
	config->null_policy = POLICY_NULL_INSERT;
	config->sr_id = SRID_UNKNOWN;
	config->shp_sr_id = SRID_UNKNOWN;
	config->use_wkt = 0;
	config->tablespace = NULL;
	config->idxtablespace = NULL;
	config->usetransaction = 1;
	config->column_map_filename = NULL;
}

/* Create a new shapefile state object */
SHPLOADERSTATE *
ShpLoaderCreate(SHPLOADERCONFIG *config)
{
	SHPLOADERSTATE *state;

	/* Create a new state object and assign the config to it */
	state = malloc(sizeof(SHPLOADERSTATE));
	state->config = config;

	/* Set any state defaults */
	state->hSHPHandle = NULL;
	state->hDBFHandle = NULL;
	state->has_z = 0;
	state->has_m = 0;
	state->types = NULL;
	state->widths = NULL;
	state->precisions = NULL;
	state->col_names = NULL;
	state->field_names = NULL;
	state->num_fields = 0;
	state->pgfieldtypes = NULL;

	state->from_srid = config->shp_sr_id;
	state->to_srid = config->sr_id;

	/* If only one has a valid SRID, use it for both. */
	if (state->to_srid == SRID_UNKNOWN)
	{
		if (config->geography)
		{
			state->to_srid = 4326;
		}
		else
		{
			state->to_srid = state->from_srid;
		}
	}

	if (state->from_srid == SRID_UNKNOWN)
	{
		state->from_srid = state->to_srid;
	}

	/* If the geo col name is not set, use one of the defaults. */
	state->geo_col = config->geo_col;

	if (!state->geo_col)
	{
		state->geo_col = config->geography ? GEOGRAPHY_DEFAULT : GEOMETRY_DEFAULT;
	}

	colmap_init(&state->column_map);

	return state;
}


/* Open the shapefile and extract the relevant field information */
int
ShpLoaderOpenShape(SHPLOADERSTATE *state)
{
	SHPObject *obj = NULL;
	int ret = SHPLOADEROK;
	char name[MAXFIELDNAMELEN];
	char name2[MAXFIELDNAMELEN];
	DBFFieldType type = FTInvalid;
	char *utf8str;

	/* If we are reading the entire shapefile, open it */
	if (state->config->readshape == 1)
	{
		state->hSHPHandle = SHPOpen(state->config->shp_file, "rb");

		if (state->hSHPHandle == NULL)
		{
			snprintf(state->message, SHPLOADERMSGLEN, _("%s: shape (.shp) or index files (.shx) can not be opened, will just import attribute data."), state->config->shp_file);
			state->config->readshape = 0;

			ret = SHPLOADERWARN;
		}
	}

	/* Open the DBF (attributes) file */
	state->hDBFHandle = DBFOpen(state->config->shp_file, "rb");
	if ((state->hSHPHandle == NULL && state->config->readshape == 1) || state->hDBFHandle == NULL)
	{
		snprintf(state->message, SHPLOADERMSGLEN, _("%s: dbf file (.dbf) can not be opened."), state->config->shp_file);

		return SHPLOADERERR;
	}


	/* Open the column map if one was specified */
	if (state->config->column_map_filename)
	{
		ret = colmap_read(state->config->column_map_filename,
		                  &state->column_map, state->message, SHPLOADERMSGLEN);
		if (!ret) return SHPLOADERERR;
	}

	/* User hasn't altered the default encoding preference... */
	if ( strcmp(state->config->encoding, ENCODING_DEFAULT) == 0 )
	{
	    /* But the file has a code page entry... */
	    if ( state->hDBFHandle->pszCodePage )
	    {
	        /* And we figured out what iconv encoding it maps to, so use it! */
            char *newencoding = NULL;
	        if ( (newencoding = codepage2encoding(state->hDBFHandle->pszCodePage)) )
	        {
                lwfree(state->config->encoding);
                state->config->encoding = newencoding;
            }
        }
	}

	/* If reading the whole shapefile (not just attributes)... */
	if (state->config->readshape == 1)
	{
		SHPGetInfo(state->hSHPHandle, &state->num_entities, &state->shpfiletype, NULL, NULL);

		/* If null_policy is set to abort, check for NULLs */
		if (state->config->null_policy == POLICY_NULL_ABORT)
		{
			/* If we abort on null items, scan the entire file for NULLs */
			for (int j = 0; j < state->num_entities; j++)
			{
				obj = SHPReadObject(state->hSHPHandle, j);

				if (!obj)
				{
					snprintf(state->message, SHPLOADERMSGLEN, _("Error reading shape object %d"), j);
					return SHPLOADERERR;
				}

				if (obj->nVertices == 0)
				{
					snprintf(state->message, SHPLOADERMSGLEN, _("Empty geometries found, aborted.)"));
					return SHPLOADERERR;
				}

				SHPDestroyObject(obj);
			}
		}

		/* Check the shapefile type */
		int geomtype = 0;
		switch (state->shpfiletype)
		{
		case SHPT_POINT:
			/* Point */
			state->pgtype = "POINT";
			geomtype = POINTTYPE;
			state->pgdims = 2;
			break;

		case SHPT_ARC:
			/* PolyLine */
			state->pgtype = "MULTILINESTRING";
			geomtype = MULTILINETYPE ;
			state->pgdims = 2;
			break;

		case SHPT_POLYGON:
			/* Polygon */
			state->pgtype = "MULTIPOLYGON";
			geomtype = MULTIPOLYGONTYPE;
			state->pgdims = 2;
			break;

		case SHPT_MULTIPOINT:
			/* MultiPoint */
			state->pgtype = "MULTIPOINT";
			geomtype = MULTIPOINTTYPE;
			state->pgdims = 2;
			break;

		case SHPT_POINTM:
			/* PointM */
			geomtype = POINTTYPE;
			state->has_m = 1;
			state->pgtype = "POINTM";
			state->pgdims = 3;
			break;

		case SHPT_ARCM:
			/* PolyLineM */
			geomtype = MULTILINETYPE;
			state->has_m = 1;
			state->pgtype = "MULTILINESTRINGM";
			state->pgdims = 3;
			break;

		case SHPT_POLYGONM:
			/* PolygonM */
			geomtype = MULTIPOLYGONTYPE;
			state->has_m = 1;
			state->pgtype = "MULTIPOLYGONM";
			state->pgdims = 3;
			break;

		case SHPT_MULTIPOINTM:
			/* MultiPointM */
			geomtype = MULTIPOINTTYPE;
			state->has_m = 1;
			state->pgtype = "MULTIPOINTM";
			state->pgdims = 3;
			break;

		case SHPT_POINTZ:
			/* PointZ */
			geomtype = POINTTYPE;
			state->has_m = 1;
			state->has_z = 1;
			state->pgtype = "POINT";
			state->pgdims = 4;
			break;

		case SHPT_ARCZ:
			/* PolyLineZ */
			state->pgtype = "MULTILINESTRING";
			geomtype = MULTILINETYPE;
			state->has_z = 1;
			state->has_m = 1;
			state->pgdims = 4;
			break;

		case SHPT_POLYGONZ:
			/* MultiPolygonZ */
			state->pgtype = "MULTIPOLYGON";
			geomtype = MULTIPOLYGONTYPE;
			state->has_z = 1;
			state->has_m = 1;
			state->pgdims = 4;
			break;

		case SHPT_MULTIPOINTZ:
			/* MultiPointZ */
			state->pgtype = "MULTIPOINT";
			geomtype = MULTIPOINTTYPE;
			state->has_z = 1;
			state->has_m = 1;
			state->pgdims = 4;
			break;

		default:
			state->pgtype = "GEOMETRY";
			geomtype = COLLECTIONTYPE;
			state->has_z = 1;
			state->has_m = 1;
			state->pgdims = 4;

			snprintf(state->message, SHPLOADERMSGLEN, _("Unknown geometry type: %d\n"), state->shpfiletype);
			return SHPLOADERERR;

			break;
		}

		/* Force Z/M-handling if configured to do so */
		switch(state->config->force_output)
		{
		case FORCE_OUTPUT_2D:
			state->has_z = 0;
			state->has_m = 0;
			state->pgdims = 2;
			break;

		case FORCE_OUTPUT_3DZ:
			state->has_z = 1;
			state->has_m = 0;
			state->pgdims = 3;
			break;

		case FORCE_OUTPUT_3DM:
			state->has_z = 0;
			state->has_m = 1;
			state->pgdims = 3;
			break;

		case FORCE_OUTPUT_4D:
			state->has_z = 1;
			state->has_m = 1;
			state->pgdims = 4;
			break;
		default:
			/* Simply use the auto-detected values above */
			break;
		}

		/* If in simple geometry mode, alter names for CREATE TABLE by skipping MULTI */
		if (state->config->simple_geometries)
		{
			if ((geomtype == MULTIPOLYGONTYPE) || (geomtype == MULTILINETYPE) || (geomtype == MULTIPOINTTYPE))
			{
				/* Chop off the "MULTI" from the string. */
				state->pgtype += 5;
			}
		}

	}
	else
	{
		/* Otherwise just count the number of records in the DBF */
		state->num_entities = DBFGetRecordCount(state->hDBFHandle);
	}


	/* Get the field information from the DBF */
	state->num_fields = DBFGetFieldCount(state->hDBFHandle);

	state->num_records = DBFGetRecordCount(state->hDBFHandle);

	/* Allocate storage for field information */
	state->field_names = malloc(state->num_fields * sizeof(char*));
	state->types = (DBFFieldType *)malloc(state->num_fields * sizeof(int));
	state->widths = malloc(state->num_fields * sizeof(int));
	state->precisions = malloc(state->num_fields * sizeof(int));
	state->pgfieldtypes = malloc(state->num_fields * sizeof(char *));
	state->col_names = malloc((state->num_fields + 2) * sizeof(char) * MAXFIELDNAMELEN);

	strcpy(state->col_names, "" );
	/* Generate a string of comma separated column names of the form "col1, col2 ... colN" for the SQL
	   insertion string */

	for (int j = 0; j < state->num_fields; j++)
	{
		int field_precision = 0, field_width = 0;
		type = DBFGetFieldInfo(state->hDBFHandle, j, name, &field_width, &field_precision);

		state->types[j] = type;
		state->widths[j] = field_width;
		state->precisions[j] = field_precision;
/*		fprintf(stderr, "XXX %s width:%d prec:%d\n", name, field_width, field_precision); */

		if (state->config->encoding)
		{
			char *encoding_msg = _("Try \"LATIN1\" (Western European), or one of the values described at http://www.gnu.org/software/libiconv/.");

			int rv = utf8(state->config->encoding, name, &utf8str);

			if (rv != UTF8_GOOD_RESULT)
			{
				if ( rv == UTF8_BAD_RESULT )
					snprintf(state->message, SHPLOADERMSGLEN, _("Unable to convert field name \"%s\" to UTF-8 (iconv reports \"%s\"). Current encoding is \"%s\". %s"), utf8str, strerror(errno), state->config->encoding, encoding_msg);
				else if ( rv == UTF8_NO_RESULT )
					snprintf(state->message, SHPLOADERMSGLEN, _("Unable to convert field name to UTF-8 (iconv reports \"%s\"). Current encoding is \"%s\". %s"), strerror(errno), state->config->encoding, encoding_msg);
				else
					snprintf(state->message, SHPLOADERMSGLEN, _("Unexpected return value from utf8()"));

				if ( rv == UTF8_BAD_RESULT )
					free(utf8str);

				return SHPLOADERERR;
			}

			strncpy(name, utf8str, MAXFIELDNAMELEN);
			name[MAXFIELDNAMELEN-1] = '\0';
			free(utf8str);
		}

		/* If a column map file has been passed in, use this to create the postgresql field name from
		   the dbf column name */
		{
		  const char *mapped = colmap_pg_by_dbf(&state->column_map, name);
		  if (mapped)
		  {
			  strncpy(name, mapped, MAXFIELDNAMELEN);
			  name[MAXFIELDNAMELEN-1] = '\0';
			}
		}

		/*
		 * Make field names lowercase unless asked to
		 * keep identifiers case.
		 */
		if (!state->config->quoteidentifiers)
			strtolower(name);

		/*
		 * Escape names starting with the
		 * escape char (_), those named 'gid'
		 * or after pgsql reserved attribute names
		 */
		if (name[0] == '_' ||
		        ! strcmp(name, "gid") || ! strcmp(name, "tableoid") ||
		        ! strcmp(name, "cmin") ||
		        ! strcmp(name, "cmax") ||
		        ! strcmp(name, "xmin") ||
		        ! strcmp(name, "xmax") ||
		        ! strcmp(name, "primary") ||
		        ! strcmp(name, "oid") || ! strcmp(name, "ctid"))
		{
			size_t len = strlen(name);
			if (len > (MAXFIELDNAMELEN - 2))
				len = MAXFIELDNAMELEN - 2;
			strncpy(name2 + 2, name, len);
			name2[MAXFIELDNAMELEN-1] = '\0';
			name2[len + 2] = '\0';
			name2[0] = '_';
			name2[1] = '_';
			strcpy(name, name2);
		}

		/* Avoid duplicating field names */
		for (int z = 0; z < j; z++)
		{
			if (strcmp(state->field_names[z], name) == 0)
			{
				strncat(name, "__", MAXFIELDNAMELEN - 1);
				snprintf(name + strlen(name),
					 MAXFIELDNAMELEN - 1 - strlen(name),
					 "%i",
					 j);
				break;
			}
		}

		state->field_names[j] = strdup(name);

		/* Now generate the PostgreSQL type name string and width based upon the shapefile type */
		switch (state->types[j])
		{
		case FTString:
			state->pgfieldtypes[j] = strdup("varchar");
			break;

		case FTDate:
			state->pgfieldtypes[j] = strdup("date");
			break;

		case FTInteger:
			/* Determine exact type based upon field width */
			if (state->config->forceint4 || (state->widths[j] >=5 && state->widths[j] < 10))
			{
				state->pgfieldtypes[j] = strdup("int4");
			}
			else if (state->widths[j] >=10 && state->widths[j] < 19)
			{
				state->pgfieldtypes[j] = strdup("int8");
			}
			else if (state->widths[j] < 5)
			{
				state->pgfieldtypes[j] = strdup("int2");
			}
			else
			{
				state->pgfieldtypes[j] = strdup("numeric");
			}
			break;

		case FTDouble:
			/* Determine exact type based upon field width */
			fprintf(stderr, "Field %s is an FTDouble with width %d and precision %d\n",
					state->field_names[j], state->widths[j], state->precisions[j]);
			if (state->widths[j] > 18)
			{
				state->pgfieldtypes[j] = strdup("numeric");
			}
			else
			{
				state->pgfieldtypes[j] = strdup("float8");
			}
			break;

		case FTLogical:
			state->pgfieldtypes[j] = strdup("boolean");
			break;

		default:
			snprintf(state->message, SHPLOADERMSGLEN, _("Invalid type %x in DBF file"), state->types[j]);
			return SHPLOADERERR;
		}

		strcat(state->col_names, "\"");
		strcat(state->col_names, name);

		if (state->config->readshape == 1 || j < (state->num_fields - 1))
		{
			/* Don't include last comma if its the last field and no geometry field will follow */
			strcat(state->col_names, "\",");
		}
		else
		{
			strcat(state->col_names, "\"");
		}
	}

	/* Append the geometry column if required */
	if (state->config->readshape == 1)
		strcat(state->col_names, state->geo_col);

	/* Return status */
	return ret;
}

/* Return a pointer to an allocated string containing the header for the specified loader state */
int
ShpLoaderGetSQLHeader(SHPLOADERSTATE *state, char **strheader)
{
	stringbuffer_t *sb;
	char *ret;
	int j;

	/* Create the stringbuffer containing the header; we use this API as it's easier
	   for handling string resizing during append */
	sb = stringbuffer_create();
	stringbuffer_clear(sb);

	/* Set the client encoding if required */
	if (state->config->encoding)
	{
		stringbuffer_aprintf(sb, "SET CLIENT_ENCODING TO UTF8;\n");
	}

	/* Use SQL-standard string escaping rather than PostgreSQL standard */
	stringbuffer_aprintf(sb, "SET STANDARD_CONFORMING_STRINGS TO ON;\n");

	/* Drop table if requested */
	if (state->config->opt == 'd')
	{
		/**
		 * TODO: if the table has more then one geometry column
		 * 	the DROP TABLE call will leave spurious records in
		 * 	geometry_columns.
		 *
		 * If the geometry column in the table being dropped
		 * does not match 'the_geom' or the name specified with
		 * -g an error is returned by DropGeometryColumn.
		 *
		 * The table to be dropped might not exist.
		 */
		if (state->config->schema)
		{
			if (state->config->readshape == 1 && (! state->config->geography) )
			{
				stringbuffer_aprintf(sb, "SELECT DropGeometryColumn('%s','%s','%s');\n",
				                     state->config->schema, state->config->table, state->geo_col);
			}

			stringbuffer_aprintf(sb, "DROP TABLE IF EXISTS \"%s\".\"%s\";\n", state->config->schema,
			                     state->config->table);
		}
		else
		{
			if (state->config->readshape == 1  && (! state->config->geography) )
			{
				stringbuffer_aprintf(sb, "SELECT DropGeometryColumn('','%s','%s');\n",
				                     state->config->table, state->geo_col);
			}

			stringbuffer_aprintf(sb, "DROP TABLE IF EXISTS \"%s\";\n", state->config->table);
		}
	}

	/* Start of transaction if we are using one */
	if (state->config->usetransaction)
	{
		stringbuffer_aprintf(sb, "BEGIN;\n");
	}

	/* If not in 'append' mode create the spatial table */
	if (state->config->opt != 'a')
	{
		/*
		* Create a table for inserting the shapes into with appropriate
		* columns and types
		*/
		if (state->config->schema)
		{
			stringbuffer_aprintf(sb, "CREATE TABLE \"%s\".\"%s\" (gid serial",
			                     state->config->schema, state->config->table);
		}
		else
		{
			stringbuffer_aprintf(sb, "CREATE TABLE \"%s\" (gid serial", state->config->table);
		}

		/* Generate the field types based upon the shapefile information */
		for (j = 0; j < state->num_fields; j++)
		{
			stringbuffer_aprintf(sb, ",\n\"%s\" ", state->field_names[j]);

			/* First output the raw field type string */
			stringbuffer_aprintf(sb, "%s", state->pgfieldtypes[j]);

			/* Some types do have typmods */
			/* Apply width typmod for varchar if there is positive width **/
			if (!strcmp("varchar", state->pgfieldtypes[j]) && state->widths[j] > 0)
					stringbuffer_aprintf(sb, "(%d)", state->widths[j]);

			if (!strcmp("numeric", state->pgfieldtypes[j]))
			{
				/* Doubles we just allow PostgreSQL to auto-detect the size */
				if (state->types[j] != FTDouble)
					stringbuffer_aprintf(sb, "(%d,0)", state->widths[j]);
			}
		}

		/* Add the geography column directly to the table definition, we don't
		   need to do an AddGeometryColumn() call. */
		if (state->config->readshape == 1 && state->config->geography)
		{
			char *dimschar;

			if (state->pgdims == 4)
				dimschar = "ZM";
			else
				dimschar = "";

			if (state->to_srid == SRID_UNKNOWN ){
				state->to_srid = 4326;
			}

			stringbuffer_aprintf(sb, ",\n\"%s\" geography(%s%s,%d)", state->geo_col, state->pgtype, dimschar, state->to_srid);
		}
		stringbuffer_aprintf(sb, ")");

		/* Tablespace is optional. */
		if (state->config->tablespace != NULL)
		{
			stringbuffer_aprintf(sb, " TABLESPACE \"%s\"", state->config->tablespace);
		}
		stringbuffer_aprintf(sb, ";\n");

		/* Create the primary key.  This is done separately because the index for the PK needs
                 * to be in the correct tablespace. */

		/* TODO: Currently PostgreSQL does not allow specifying an index to use for a PK (so you get
                 *       a default one called table_pkey) and it does not provide a way to create a PK index
                 *       in a specific tablespace.  So as a hacky solution we create the PK, then move the
                 *       index to the correct tablespace.  Eventually this should be:
		 *           CREATE INDEX table_pkey on table(gid) TABLESPACE tblspc;
                 *           ALTER TABLE table ADD PRIMARY KEY (gid) USING INDEX table_pkey;
		 *       A patch has apparently been submitted to PostgreSQL to enable this syntax, see this thread:
		 *           http://archives.postgresql.org/pgsql-hackers/2011-01/msg01405.php */
		stringbuffer_aprintf(sb, "ALTER TABLE ");

		/* Schema is optional, include if present. */
		if (state->config->schema)
		{
			stringbuffer_aprintf(sb, "\"%s\".",state->config->schema);
		}
		stringbuffer_aprintf(sb, "\"%s\" ADD PRIMARY KEY (gid);\n", state->config->table);

		/* Tablespace is optional for the index. */
		if (state->config->idxtablespace != NULL)
		{
			stringbuffer_aprintf(sb, "ALTER INDEX ");
			if (state->config->schema)
			{
				stringbuffer_aprintf(sb, "\"%s\".",state->config->schema);
			}

			/* WARNING: We're assuming the default "table_pkey" name for the primary
			 *          key index.  PostgreSQL may use "table_pkey1" or similar in the
			 *          case of a name conflict, so you may need to edit the produced
			 *          SQL in this rare case. */
			stringbuffer_aprintf(sb, "\"%s_pkey\" SET TABLESPACE \"%s\";\n",
						state->config->table, state->config->idxtablespace);
		}

		/* Create the geometry column with an addgeometry call */
		if (state->config->readshape == 1 && (!state->config->geography))
		{
			/* If they didn't specify a target SRID, see if they specified a source SRID. */
			int32_t srid = state->to_srid;
			if (state->config->schema)
			{
				stringbuffer_aprintf(sb, "SELECT AddGeometryColumn('%s','%s','%s','%d',",
				                     state->config->schema, state->config->table, state->geo_col, srid);
			}
			else
			{
				stringbuffer_aprintf(sb, "SELECT AddGeometryColumn('','%s','%s','%d',",
				                     state->config->table, state->geo_col, srid);
			}

			stringbuffer_aprintf(sb, "'%s',%d);\n", state->pgtype, state->pgdims);
		}
	}

	/**If we are in dump mode and a transform was asked for need to create a temp table to store original data
	 You may ask, why don't we go straight into the main table and then do an alter table alter column afterwards
	 Main reason is so we don't incur the penalty of WAL logging when we change the typmod in final run. **/
	if (state->config->dump_format && state->to_srid != state->from_srid){
		/** create a temp table with same structure as main except for no restriction on geometry type */
		stringbuffer_aprintf(sb, "CREATE TEMP TABLE \"pgis_tmp_%s\" AS SELECT * FROM ", state->config->table);
		/* Schema is optional, include if present. */
		if (state->config->schema)
		{
			stringbuffer_aprintf(sb, "\"%s\".",state->config->schema);
		}
		stringbuffer_aprintf(sb, "\"%s\" WHERE false;\n", state->config->table, state->geo_col);
		/**out input data is going to be in different srid from target, so need to remove type constraint **/
		stringbuffer_aprintf(sb, "ALTER TABLE \"pgis_tmp_%s\" ALTER COLUMN \"%s\" TYPE geometry USING ( (\"%s\"::geometry) ); \n", state->config->table,  state->geo_col, state->geo_col);
	}

	/* Copy the string buffer into a new string, destroying the string buffer */
	ret = (char *)malloc(strlen((char *)stringbuffer_getstring(sb)) + 1);
	strcpy(ret, (char *)stringbuffer_getstring(sb));
	stringbuffer_destroy(sb);

	*strheader = ret;

	return SHPLOADEROK;
}


/* Return an allocated string containing the copy statement for this state */
int
ShpLoaderGetSQLCopyStatement(SHPLOADERSTATE *state, char **strheader)
{
	//char *copystr;
	stringbuffer_t *sb;
	char *ret;
	sb = stringbuffer_create();
	stringbuffer_clear(sb);


	/* Allocate the string for the COPY statement */
	if (state->config->dump_format)
	{
		stringbuffer_aprintf(sb, "COPY ");

		if (state->to_srid != state->from_srid){
			/** if we need to transform we copy into temp table instead of main table first */
			stringbuffer_aprintf(sb, " \"pgis_tmp_%s\" (%s) FROM stdin;\n", state->config->table, state->col_names);
		}
		else {
			if (state->config->schema)
			{
				stringbuffer_aprintf(sb, " \"%s\".", state->config->schema);
			}

			stringbuffer_aprintf(sb, "\"%s\" (%s) FROM stdin;\n", state->config->table, state->col_names);
		}

		/* Copy the string buffer into a new string, destroying the string buffer */
		ret = (char *)malloc(strlen((char *)stringbuffer_getstring(sb)) + 1);
		strcpy(ret, (char *)stringbuffer_getstring(sb));
		stringbuffer_destroy(sb);

		*strheader = ret;
		return SHPLOADEROK;
	}
	else
	{
		/* Flag an error as something has gone horribly wrong */
		snprintf(state->message, SHPLOADERMSGLEN, _("Internal error: attempt to generate a COPY statement for data that hasn't been requested in COPY format"));

		return SHPLOADERERR;
	}
}


/* Return a count of the number of entities in this shapefile */
int
ShpLoaderGetRecordCount(SHPLOADERSTATE *state)
{
	return state->num_entities;
}


/* Return an allocated string representation of a specified record item */
int
ShpLoaderGenerateSQLRowStatement(SHPLOADERSTATE *state, int item, char **strrecord)
{
	SHPObject *obj = NULL;
	stringbuffer_t *sb;
	stringbuffer_t *sbwarn;
	char val[MAXVALUELEN];
	char *escval;
	char *geometry=NULL, *ret;
	char *utf8str;
	int res, i;
	int rv;

	/* Clear the stringbuffers */
	sbwarn = stringbuffer_create();
	stringbuffer_clear(sbwarn);
	sb = stringbuffer_create();
	stringbuffer_clear(sb);

	/* Skip deleted records */
	if (state->hDBFHandle && DBFIsRecordDeleted(state->hDBFHandle, item))
	{
		*strrecord = NULL;
		return SHPLOADERRECDELETED;
	}

	/* If we are reading the shapefile, open the specified record */
	if (state->config->readshape == 1)
	{
		obj = SHPReadObject(state->hSHPHandle, item);
		if (!obj)
		{
			snprintf(state->message, SHPLOADERMSGLEN, _("Error reading shape object %d"), item);
			return SHPLOADERERR;
		}

		/* If we are set to skip NULLs, return a NULL record status */
		if (state->config->null_policy == POLICY_NULL_SKIP && obj->nVertices == 0 )
		{
			SHPDestroyObject(obj);

			*strrecord = NULL;
			return SHPLOADERRECISNULL;
		}
	}

	/* If not in dump format, generate the INSERT string */
	if (!state->config->dump_format)
	{
		if (state->config->schema)
		{
			stringbuffer_aprintf(sb, "INSERT INTO \"%s\".\"%s\" (%s) VALUES (", state->config->schema,
			                     state->config->table, state->col_names);
		}
		else
		{
			stringbuffer_aprintf(sb, "INSERT INTO \"%s\" (%s) VALUES (", state->config->table,
			                     state->col_names);
		}
	}


	/* Read all of the attributes from the DBF file for this item */
	for (i = 0; i < DBFGetFieldCount(state->hDBFHandle); i++)
	{
		/* Special case for NULL attributes */
		if (DBFIsAttributeNULL(state->hDBFHandle, item, i))
		{
			if (state->config->dump_format)
				stringbuffer_aprintf(sb, "\\N");
			else
				stringbuffer_aprintf(sb, "NULL");
		}
		else
		{
			/* Attribute NOT NULL */
			switch (state->types[i])
			{
			case FTInteger:
			case FTDouble:
				rv = snprintf(val, MAXVALUELEN, "%s", DBFReadStringAttribute(state->hDBFHandle, item, i));
				if (rv >= MAXVALUELEN || rv == -1)
				{
					stringbuffer_aprintf(sbwarn, "Warning: field %d name truncated\n", i);
					val[MAXVALUELEN - 1] = '\0';
				}

				/* If the value is an empty string, change to 0 */
				if (val[0] == '\0')
				{
					val[0] = '0';
					val[1] = '\0';
				}

				/* If the value ends with just ".", remove the dot */
				if (val[strlen(val) - 1] == '.')
					val[strlen(val) - 1] = '\0';
				break;

			case FTString:
			case FTLogical:
				rv = snprintf(val, MAXVALUELEN, "%s", DBFReadStringAttribute(state->hDBFHandle, item, i));
				if (rv >= MAXVALUELEN || rv == -1)
				{
					stringbuffer_aprintf(sbwarn, "Warning: field %d name truncated\n", i);
					val[MAXVALUELEN - 1] = '\0';
				}
				break;

			case FTDate:
				rv = snprintf(val, MAXVALUELEN, "%s", DBFReadStringAttribute(state->hDBFHandle, item, i));
				if (rv >= MAXVALUELEN || rv == -1)
				{
					stringbuffer_aprintf(sbwarn, "Warning: field %d name truncated\n", i);
					val[MAXVALUELEN - 1] = '\0';
				}
				if (strlen(val) == 0)
				{
					if (state->config->dump_format)
						stringbuffer_aprintf(sb, "\\N");
					else
						stringbuffer_aprintf(sb, "NULL");
					goto done_cell;
				}
				break;

			default:
				snprintf(state->message, SHPLOADERMSGLEN, _("Error: field %d has invalid or unknown field type (%d)"), i, state->types[i]);

				/* clean up and return err */
				SHPDestroyObject(obj);
				stringbuffer_destroy(sbwarn);
				stringbuffer_destroy(sb);
				return SHPLOADERERR;
			}

			if (state->config->encoding)
			{
				char *encoding_msg = _("Try \"LATIN1\" (Western European), or one of the values described at http://www.postgresql.org/docs/current/static/multibyte.html.");

				rv = utf8(state->config->encoding, val, &utf8str);

				if (rv != UTF8_GOOD_RESULT)
				{
					if ( rv == UTF8_BAD_RESULT )
						snprintf(state->message, SHPLOADERMSGLEN, _("Unable to convert data value \"%s\" to UTF-8 (iconv reports \"%s\"). Current encoding is \"%s\". %s"), utf8str, strerror(errno), state->config->encoding, encoding_msg);
					else if ( rv == UTF8_NO_RESULT )
						snprintf(state->message, SHPLOADERMSGLEN, _("Unable to convert data value to UTF-8 (iconv reports \"%s\"). Current encoding is \"%s\". %s"), strerror(errno), state->config->encoding, encoding_msg);
					else
						snprintf(state->message, SHPLOADERMSGLEN, _("Unexpected return value from utf8()"));

					if ( rv == UTF8_BAD_RESULT )
						free(utf8str);

					/* clean up and return err */
					SHPDestroyObject(obj);
					stringbuffer_destroy(sbwarn);
					stringbuffer_destroy(sb);
					return SHPLOADERERR;
				}
				strncpy(val, utf8str, MAXVALUELEN);
				val[MAXVALUELEN-1] = '\0';
				free(utf8str);

			}

			/* Escape attribute correctly according to dump format */
			if (state->config->dump_format)
			{
				escval = escape_copy_string(val);
				stringbuffer_aprintf(sb, "%s", escval);
			}
			else
			{
				escval = escape_insert_string(val);
				stringbuffer_aprintf(sb, "'%s'", escval);
			}

			/* Free the escaped version if required */
			if (val != escval)
				free(escval);
		}

done_cell:

		/* Only put in delimeter if not last field or a shape will follow */
		if (state->config->readshape == 1 || i < DBFGetFieldCount(state->hDBFHandle) - 1)
		{
			if (state->config->dump_format)
				stringbuffer_aprintf(sb, "\t");
			else
				stringbuffer_aprintf(sb, ",");
		}

		/* End of DBF attribute loop */
	}


	/* Add the shape attribute if we are reading it */
	if (state->config->readshape == 1)
	{
		/* Force the locale to C */
		char *oldlocale = setlocale(LC_NUMERIC, "C");

		/* Handle the case of a NULL shape */
		if (obj->nVertices == 0)
		{
			if (state->config->dump_format)
				stringbuffer_aprintf(sb, "\\N");
			else
				stringbuffer_aprintf(sb, "NULL");
		}
		else
		{
			/* Handle all other shape attributes */
			switch (obj->nSHPType)
			{
			case SHPT_POLYGON:
			case SHPT_POLYGONM:
			case SHPT_POLYGONZ:
				res = GeneratePolygonGeometry(state, obj, &geometry);
				break;

			case SHPT_POINT:
			case SHPT_POINTM:
			case SHPT_POINTZ:
				res = GeneratePointGeometry(state, obj, &geometry, 0);
				break;

			case SHPT_MULTIPOINT:
			case SHPT_MULTIPOINTM:
			case SHPT_MULTIPOINTZ:
				/* Force it to multi unless using -S */
				res = GeneratePointGeometry(state, obj, &geometry,
					state->config->simple_geometries ? 0 : 1);
				break;

			case SHPT_ARC:
			case SHPT_ARCM:
			case SHPT_ARCZ:
				res = GenerateLineStringGeometry(state, obj, &geometry);
				break;

			default:
				snprintf(state->message, SHPLOADERMSGLEN, _("Shape type is not supported, type id = %d"), obj->nSHPType);
				SHPDestroyObject(obj);
				stringbuffer_destroy(sbwarn);
				stringbuffer_destroy(sb);

				return SHPLOADERERR;
			}
			/* The default returns out of the function, so res will always have been set. */
			if (res != SHPLOADEROK)
			{
				/* Error message has already been set */
				SHPDestroyObject(obj);
				stringbuffer_destroy(sbwarn);
				stringbuffer_destroy(sb);

				return SHPLOADERERR;
			}

			/* Now generate the geometry string according to the current configuration */
			if (!state->config->dump_format)
			{
				if (state->to_srid != state->from_srid)
				{
					stringbuffer_aprintf(sb, "ST_Transform(");
				}
				stringbuffer_aprintf(sb, "'");
			}

			stringbuffer_aprintf(sb, "%s", geometry);

			if (!state->config->dump_format)
			{
				stringbuffer_aprintf(sb, "'");

				/* Close the ST_Transform if reprojecting. */
				if (state->to_srid != state->from_srid)
				{
					/* We need to add an explicit cast to geography/geometry to ensure that
					   PostgreSQL doesn't get confused with the ST_Transform() raster
					   function. */
					if (state->config->geography)
						stringbuffer_aprintf(sb, "::geometry, %d)::geography", state->to_srid);
					else
						stringbuffer_aprintf(sb, "::geometry, %d)", state->to_srid);
				}
			}

			//			free(geometry);
		}

		/* Tidy up everything */
		SHPDestroyObject(obj);

		setlocale(LC_NUMERIC, oldlocale);
	}

	/* Close the line correctly for dump/insert format */
	if (!state->config->dump_format)
		stringbuffer_aprintf(sb, ");");


	/* Copy the string buffer into a new string, destroying the string buffer */
	ret = (char *)malloc(strlen((char *)stringbuffer_getstring(sb)) + 1);
	strcpy(ret, (char *)stringbuffer_getstring(sb));
	stringbuffer_destroy(sb);

	*strrecord = ret;

	/* If any warnings occurred, set the returned message string and warning status */
	if (strlen((char *)stringbuffer_getstring(sbwarn)) > 0)
	{
		snprintf(state->message, SHPLOADERMSGLEN, "%s", stringbuffer_getstring(sbwarn));
		stringbuffer_destroy(sbwarn);

		return SHPLOADERWARN;
	}
	else
	{
		/* Everything went okay */
		stringbuffer_destroy(sbwarn);

		return SHPLOADEROK;
	}
}


/* Return a pointer to an allocated string containing the header for the specified loader state */
int
ShpLoaderGetSQLFooter(SHPLOADERSTATE *state, char **strfooter)
{
	stringbuffer_t *sb;
	char *ret;

	/* Create the stringbuffer containing the header; we use this API as it's easier
	   for handling string resizing during append */
	sb = stringbuffer_create();
	stringbuffer_clear(sb);

	if ( state->config->dump_format && state->to_srid != state->from_srid){
		/** We need to copy from the temp table to the real table, transforming to to_srid **/
		stringbuffer_aprintf(sb, "ALTER TABLE  \"pgis_tmp_%s\" ALTER COLUMN \"%s\" TYPE ", state->config->table, state->geo_col);
		if (state->config->geography){
			stringbuffer_aprintf(sb, "geography USING (ST_Transform(\"%s\", %d)::geography );\n", state->geo_col, state->to_srid);
		}
		else {
			stringbuffer_aprintf(sb, "geometry USING (ST_Transform(\"%s\", %d)::geometry );\n", state->geo_col, state->to_srid);
		}
		stringbuffer_aprintf(sb, "INSERT INTO ");
		// /* Schema is optional, include if present. */
		if (state->config->schema)
		{
			stringbuffer_aprintf(sb, "\"%s\".", state->config->schema);
		}
		stringbuffer_aprintf(sb, "\"%s\" (%s) ", state->config->table, state->col_names);
		stringbuffer_aprintf(sb, "SELECT %s FROM \"pgis_tmp_%s\";\n", state->col_names, state->config->table);
	}

	/* Create gist index if specified and not in "prepare" mode */
	if (state->config->readshape && state->config->createindex)
	{
		stringbuffer_aprintf(sb, "CREATE INDEX ON ");
		/* Schema is optional, include if present. */
		if (state->config->schema)
		{
			stringbuffer_aprintf(sb, "\"%s\".",state->config->schema);
		}
		stringbuffer_aprintf(sb, "\"%s\" USING GIST (\"%s\")", state->config->table, state->geo_col);
		/* Tablespace is also optional. */
		if (state->config->idxtablespace != NULL)
		{
			stringbuffer_aprintf(sb, " TABLESPACE \"%s\"", state->config->idxtablespace);
		}
		stringbuffer_aprintf(sb, ";\n");
	}

	/* End the transaction if there is one. */
	if (state->config->usetransaction)
	{
		stringbuffer_aprintf(sb, "COMMIT;\n");
	}


	if(state->config->analyze)
	{
		/* Always ANALYZE the resulting table, for better stats */
		stringbuffer_aprintf(sb, "ANALYZE ");
		if (state->config->schema)
		{
			stringbuffer_aprintf(sb, "\"%s\".", state->config->schema);
		}
		stringbuffer_aprintf(sb, "\"%s\";\n", state->config->table);
	}

	/* Copy the string buffer into a new string, destroying the string buffer */
	ret = (char *)malloc(strlen((char *)stringbuffer_getstring(sb)) + 1);
	strcpy(ret, (char *)stringbuffer_getstring(sb));
	stringbuffer_destroy(sb);

	*strfooter = ret;

	return SHPLOADEROK;
}


void
ShpLoaderDestroy(SHPLOADERSTATE *state)
{
	/* Destroy a state object created with ShpLoaderOpenShape */
	int i;
	if (state != NULL)
	{
		if (state->hSHPHandle)
			SHPClose(state->hSHPHandle);
		if (state->hDBFHandle)
			DBFClose(state->hDBFHandle);
		if (state->field_names)
		{
			for (i = 0; i < state->num_fields; i++)
				free(state->field_names[i]);

			free(state->field_names);
		}
		if (state->pgfieldtypes)
		{
			for (i = 0; i < state->num_fields; i++)
				free(state->pgfieldtypes[i]);

			free(state->pgfieldtypes);
		}
		if (state->types)
			free(state->types);
		if (state->widths)
			free(state->widths);
		if (state->precisions)
			free(state->precisions);
		if (state->col_names)
			free(state->col_names);

		/* Free any column map fieldnames if specified */
		colmap_clean(&state->column_map);

		/* Free the state itself */
		free(state);
	}
}
