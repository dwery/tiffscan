/*
 * tiffscan -- command line scanning utility
 * Copyright (C) 2007-12 by Alessandro Zummo <a.zummo@towertech.it>
 *
 * Loosely based on scanimage,
 *  Copyright (C) 1996, 1997, 1998 Andreas Beck and David Mosberger
 *  
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include <sane/sane.h>

#if defined(SANE_HAS_EVOLVED)
#define SANE_HAS_WARMING_UP
#endif

#ifndef SANE_FRAME_IR
#define SANE_FRAME_IR 0x0F
#endif

#ifndef SANE_FRAME_RGBI
#define SANE_FRAME_RGBI 0x10
#endif

#define SANE_HAS_INFRARED

#include <sane/saneopts.h>

#ifdef SANE_HAS_EVOLVED
#include <sane/helper.h>
#endif

#include <tiff.h>
#include <tiffio.h>
#include <popt.h>
#include <paper.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* XXX move to hacks.h */

enum optio
{
	OPT_HELP = 1, OPT_LIST_DEVS, OPT_VERSION, OPT_SCAN,
	OPT_VERBOSE, OPT_DEVICE
};

#define BATCH_COUNT_UNLIMITED -1

static void tiffscan_exit(void);

/*
static SANE_Word tl_x = 0;
static SANE_Word tl_y = 0;
static SANE_Word br_x = 0;
static SANE_Word br_y = 0;
static SANE_Word w_x = 0;
static SANE_Word h_y = 0;
*/

/* main options */
static char *devname = NULL;
static int verbose = 0;
static int progress = 0;
static int scanlines = 50;

/* tiff tags */
static const char *tiff_artist = NULL;
static const char *tiff_copyright = NULL;
static const char *tiff_documentname = NULL;
static const char *tiff_imagedesc = NULL;
static const char *tiff_orientation = NULL;

/* batch options */
static int batch = 0;
static int batch_prompt = 0;
static int batch_amount = BATCH_COUNT_UNLIMITED;
static int batch_start_at = 1;
static int batch_increment = 1;

/* output options */
static char *output_file = NULL;
static const char *icc_profile = NULL;
static int compress = 1;
static int multi = 1;

/* misc options */
static const char *paper = NULL;

/* globals */
static SANE_Handle handle;
static int batch_count = 0;
static int resolution_optind = -1;
static int corners[4];
static char output_file_buf[80];
#ifdef SANE_HAS_EVOLVED
static SANE_Scanner_Info si;
#endif

static struct poptOption options[] = {
	{"device", 'd', POPT_ARG_STRING, NULL, OPT_DEVICE, "device name", NULL},
	{"scan", 's', POPT_ARG_NONE, NULL, OPT_SCAN,
	 "this should be obvious :)", NULL},
	{"list-devices", 'L', POPT_ARG_NONE, NULL, OPT_LIST_DEVS,
	 "list known devices", NULL},

	/* informational */
	{"version", 0, POPT_ARG_NONE, NULL, OPT_VERSION,
	 "show program version", NULL},
	{"progress", 'p', POPT_ARG_NONE, &progress, 0,
	 "show progress information", NULL},
	{"verbose", 'v', POPT_ARG_NONE, NULL, OPT_VERBOSE,
	 "gives detailed status messages", NULL},
	{"help", 'h', POPT_ARG_NONE, NULL, OPT_HELP, "this help message",
	 NULL},

	/* output options */
	{"output-file", 'o', POPT_ARG_STRING, &output_file, 0,
	 "output file name, use %d to insert page number", "FILE"},
	{"multi-page", 0, POPT_BIT_SET | POPT_ARGFLAG_TOGGLE, &multi, 1,
	 "create a multi-page TIFF file", NULL},
	{"compress", 0, POPT_BIT_SET | POPT_ARGFLAG_TOGGLE, &compress, 1,
	 "use TIFF lossless compression", NULL},
	{"icc-profile", 0, POPT_ARG_STRING, &icc_profile, 0,
	 "embed an ICC profile in the TIFF file", "FILE"},

	/* tiff tags */
	{"artist", 0, POPT_ARG_STRING, &tiff_artist, 0,
	 "TIFF tag: Artist", NULL},
	{"copyright", 0, POPT_ARG_STRING, &tiff_copyright, 0,
	 "TIFF tag: Copyright", NULL},
	{"document-name", 0, POPT_ARG_STRING, &tiff_documentname, 0,
	 "TIFF tag: DocumentName", NULL},
	{"image-description", 0, POPT_ARG_STRING, &tiff_imagedesc, 0,
	 "TIFF tag: ImageDescription", NULL},
	{"orientation", 0, POPT_ARG_STRING, &tiff_orientation, 0,
	 "TIFF tag: Orientation", NULL},

	/* batch options */

	{"batch", 0, POPT_ARG_NONE, &batch, 0, "batch mode", NULL},
	{"batch-count", 0, POPT_ARG_INT, &batch_amount, 0,
	 "number of pages to scan", NULL},
	{"batch-start", 0, POPT_ARG_INT, &batch_start_at, 0,
	 "number of the first page", NULL},
	{"batch-increment", 0, POPT_ARG_INT, &batch_increment, 0,
	 "page number increment amount", NULL},
	{"batch-prompt", 0, POPT_ARG_NONE, &batch_prompt, 0,
	 "manual prompt before scanning", NULL},

	/* other options */
	{"paper", 0, POPT_ARG_STRING, &paper, 0,
	 "scanning area as paper name (A4, Letter, ...)", NULL},

	POPT_TABLEEND,		/* this entry will be used for device options */
	POPT_TABLEEND,
};

static void
sighandler(int signum)
{
	static SANE_Bool first_time = SANE_TRUE;

	if (handle) {
		printf("\nreceived signal %d\n", signum);
		if (first_time) {
			first_time = SANE_FALSE;
			printf("trying to stop the scanner, one more CTRL-C will exit tiffscan.\n");
			sane_cancel(handle);
		} else {
			printf("aborting\n");
			_exit(0);
		}
	}
}

/* A scalar has the following syntax:

     V [ U ]

   V is the value of the scalar.  It is either an integer or a
   floating point number, depending on the option type.

   U is an optional unit.  If not specified, the default unit is used.
   The following table lists which units are supported depending on
   what the option's default unit is:

     Option's unit:	Allowed units:

     SANE_UNIT_NONE:
     SANE_UNIT_PIXEL:	pel
     SANE_UNIT_BIT:	b (bit), B (byte)
     SANE_UNIT_MM:	mm (millimeter), cm (centimeter), in or " (inches),
     SANE_UNIT_DPI:	dpi
     SANE_UNIT_PERCENT:	%
     SANE_UNIT_PERCENT:	us
 */

static const char *
parse_scalar(const SANE_Option_Descriptor * opt,
	     const char *str, SANE_Word * value)
{
	char *end;
	double v;

	if (opt->type == SANE_TYPE_FIXED)
		v = strtod(str, &end) * (1 << SANE_FIXED_SCALE_SHIFT);
	else
		v = strtol(str, &end, 10);

	if (str == end) {
		printf("option --%s: bad option value (rest of option: %s)\n",
		       opt->name, str);
		exit(1);
	}
	str = end;

	switch (opt->unit) {
	case SANE_UNIT_NONE:
	case SANE_UNIT_PIXEL:
		break;

	case SANE_UNIT_BIT:
		if (*str == 'b' || *str == 'B') {
			if (*str++ == 'B')
				v *= 8;
		}
		break;

	case SANE_UNIT_MM:
		if (str[0] == '\0')
			v *= 1.0;	/* default to mm */
		else if (strcmp(str, "mm") == 0)
			str += sizeof("mm") - 1;
		else if (strcmp(str, "cm") == 0) {
			str += sizeof("cm") - 1;
			v *= 10.0;
		} else if (strcmp(str, "in") == 0 || *str == '"') {
			if (*str++ != '"')
				++str;
			v *= 25.4;	/* 25.4 mm/inch */
		} else {
			printf("option --%s: illegal unit (rest of option: %s)\n", opt->name, str);
			return 0;
		}
		break;

	case SANE_UNIT_DPI:
		if (strcmp(str, "dpi") == 0)
			str += sizeof("dpi") - 1;
		break;

	case SANE_UNIT_PERCENT:
		if (*str == '%')
			++str;
		break;

	case SANE_UNIT_MICROSECOND:
		if (strcmp(str, "us") == 0)
			str += sizeof("us") - 1;
		break;
	}

	*value = v + 0.5;
	return str;
}

/* A vector has the following syntax:
 *
 *  [ '[' I ']' ] S { [','|'-'] [ '[' I ']' S }
 *
 * The number in brackets (I), if present, determines the index of the
 * vector element to be set next.  If I is not present, the value of
 * last index used plus 1 is used.  The first index value used is 0
 * unless I is present.
 *
 * S is a scalar value as defined by parse_scalar().
 *
 * If two consecutive value specs are separated by a comma (,) their
 * values are set independently.  If they are separated by a dash (-),
 * they define the endpoints of a line and all vector values between
 * the two endpoints are set according to the value of the
 * interpolated line.  For example, [0]15-[255]15 defines a vector of
 * 256 elements whose value is 15.  Similarly, [0]0-[255]255 defines a
 * vector of 256 elements whose value starts at 0 and increases to
 * 255.
 *
 */

/* XXX exit is not appropriate in this func */
static void
parse_vector(const SANE_Option_Descriptor * opt,
	     const char *str, SANE_Word * vector, size_t vector_length)
{
	SANE_Word value, prev_value = 0;
	int index = -1, prev_index = 0;
	char *end, separator = '\0';

	/* initialize vector to all zeroes: */
	memset(vector, 0, vector_length * sizeof(SANE_Word));

	do {
		if (*str == '[') {
			/* read index */
			index = strtol(++str, &end, 10);
			if (str == end || *end != ']') {
				printf("option --%s: closing bracket missing "
				       "(rest of option: %s)\n", opt->name,
				       str);
				exit(1);
			}
			str = end + 1;
		} else
			++index;

		if (index < 0 || index >= (int) vector_length) {
			printf("option --%s: index %d out of range [0..%ld]\n", opt->name, index, (long) vector_length - 1);
			exit(1);
		}

		/* read value */
		str = parse_scalar(opt, str, &value);
		if (!str)
			exit(1);

		if (*str && *str != '-' && *str != ',') {
			printf("option --%s: illegal separator (rest of option: %s)\n", opt->name, str);
			exit(1);
		}

		/* store value: */
		vector[index] = value;
		if (separator == '-') {
			/* interpolate */
			double v, slope;
			int i;

			v = (double) prev_value;
			slope = ((double) value - v) / (index - prev_index);

			for (i = prev_index + 1; i < index; ++i) {
				v += slope;
				vector[i] = (SANE_Word) v;
			}
		}

		prev_index = index;
		prev_value = value;
		separator = *str++;
	}
	while (separator == ',' || separator == '-');

	if (verbose > 1) {
		int i;

		printf("value for --%s is: ", opt->name);
		for (i = 0; i < (int) vector_length; ++i)
			if (opt->type == SANE_TYPE_FIXED)
				printf("%g ", SANE_UNFIX(vector[i]));
			else
				printf("%d ", vector[i]);
		fputc('\n', stdout);
	}
}

/* XXX move to strings.c */
static void
strext(char **dst, const char *src)
{
	int len = strlen(src);

	if (dst == NULL)
		return;

	if (*dst) {
		len += strlen(*dst);
		*dst = realloc(*dst, len + 1);
		strcat(*dst, src);
	} else {
		*dst = malloc(len + 1);
		strcpy(*dst, src);
	}
}

static char *
strjoin(char **src, char sep)
{
	char *dst;
	int i = 0;
	int len = 0;

	/* calc len */
	for (i = 0; src[i]; i++)
		len += strlen(src[i]);

	/* account for separators and '\0' */
	len += i + 1;

	dst = realloc(NULL, len);
	if (dst == NULL)
		return NULL;

	dst[0] = '\0';

	for (i = 0; src[i]; i++) {
		strcat(dst, src[i]);
		strncat(dst, &sep, 1);
	}

	/* remove last sep */
	dst[len - 2] = '\0';

	return dst;
}

static char *
itoa(int v)
{
	char *s;
	int l = log10(abs(v) + 1) + 3;
	/* + 3 included sign and '\0' */

	s = malloc(l);
	if (s == NULL)
		return NULL;

	snprintf(s, l, "%d", v);
	return s;
}

static char *
dtoa(double v)
{
	char *s;
	int l = log10(v + 1) + 1 + 1 + 3;

	s = malloc(l);
	if (s == NULL)
		return NULL;

	snprintf(s, l, "%.02f", v);
	return s;
}

/* XXX move to options.c */
static void
add_default_option(char **dst, SANE_Handle handle,
		   const SANE_Option_Descriptor * opt, int opt_num)
{
	void *val;

	if (!SANE_OPTION_IS_ACTIVE(opt->cap)) {
		strext(dst, " [inactive]");
		return;
	}

	val = malloc(opt->size);
	if (val == NULL)
		return;

	sane_control_option(handle, opt_num, SANE_ACTION_GET_VALUE, val, 0);

	strext(dst, " [");

	switch (opt->type) {
	case SANE_TYPE_BOOL:
		strext(dst, *(SANE_Bool *) val ? "yes" : "no");
		break;

	case SANE_TYPE_STRING:
		strext(dst, (char *) val);
		break;

	case SANE_TYPE_INT:
	{
		char *s = itoa(*(SANE_Int *) val);
		strext(dst, s);
		free(s);
		break;
	}

	case SANE_TYPE_FIXED:
	{
		char *s = dtoa(SANE_UNFIX(*(SANE_Fixed *) val));
		strext(dst, s);
		free(s);
		break;
	}

	default:
		break;
	}

	strext(dst, "]");

	free(val);
}

static void
track_corners(const int index, const SANE_Option_Descriptor * opt)
{
	/* XXX only SANE_TYPE_FIXED right now */
/*	if (!(opt->type == SANE_TYPE_FIXED || opt->type == SANE_TYPE_INT)) */
	if (opt->type != SANE_TYPE_FIXED)
		return;

	/* SANE_Int == SANE_Word == SANE_Fixed */
	if (opt->size != sizeof(SANE_Word))
		return;

	/* XXX we handle only mm right now */
	if (opt->unit != SANE_UNIT_MM)
		return;

	if (strcmp(opt->name, SANE_NAME_SCAN_TL_X) == 0)
		corners[0] = index;
	else if (strcmp(opt->name, SANE_NAME_SCAN_TL_Y) == 0)
		corners[1] = index;
	else if (strcmp(opt->name, SANE_NAME_SCAN_BR_X) == 0)
		corners[2] = index;
	else if (strcmp(opt->name, SANE_NAME_SCAN_BR_Y) == 0)
		corners[3] = index;
}

static struct poptOption *
fetch_options(SANE_Handle handle)
{
	SANE_Status status;
	struct poptOption *options;

	const SANE_Option_Descriptor *opt;
	SANE_Int num_dev_options;
	int i, count;

	/* init corners tracking */
	memset(corners, -1, sizeof(corners));

	/* query number of options */
	status = sane_control_option(handle, 0, SANE_ACTION_GET_VALUE,
				     &num_dev_options, 0);
	if (status != SANE_STATUS_GOOD)
		return NULL;

	options = malloc(sizeof(struct poptOption) * (num_dev_options + 1));
	if (options == NULL)
		return NULL;

	/* clear. last entry will be all zero */
	memset(options, 0x00,
	       sizeof(struct poptOption) * (num_dev_options + 1));

	/* build backend options table */
	for (count = i = 0; i < num_dev_options; i++) {
		struct poptOption *thisopt = &options[count];

		opt = sane_get_option_descriptor(handle, i);

		if (!SANE_OPTION_IS_SETTABLE(opt->cap))
			continue;

                if (opt->type == SANE_TYPE_GROUP)
                        continue;

		/* search and save resolution */
		if ((opt->type == SANE_TYPE_FIXED
		     || opt->type == SANE_TYPE_INT)
		    && opt->size == sizeof(SANE_Int)
		    && (opt->unit == SANE_UNIT_DPI)
		    && (strcmp(opt->name, SANE_NAME_SCAN_RESOLUTION) == 0))
			resolution_optind = i;

		thisopt->longName = opt->name ? opt->name : "unknown";
		thisopt->shortName = 0;
		thisopt->arg = NULL;
		thisopt->val = 1000 + i;
		thisopt->descrip = (opt->desc
			&& strlen(opt->desc)) ? opt->desc : " ";
		thisopt->argDescrip = NULL;

		switch (opt->type) {
		case SANE_TYPE_BOOL:
			thisopt->argInfo = POPT_ARG_INT;
			if (opt->cap & SANE_CAP_AUTOMATIC)
				thisopt->argDescrip = strdup("yes|no|auto");
			else
				thisopt->argDescrip = strdup("yes|no");

			break;

		case SANE_TYPE_BUTTON:
			thisopt->argInfo = POPT_ARG_NONE;
			break;

		default:
			thisopt->argInfo = POPT_ARG_STRING;

			switch (opt->constraint_type) {
			case SANE_CONSTRAINT_NONE:
				switch (opt->type) {
				case SANE_TYPE_INT:
//                                            thisopt->argDescrip = strdup("<int>");
					break;	/* FIXED STRING */

				default:
					break;
				}
				break;
			case SANE_CONSTRAINT_RANGE:
				/*%d .. %d opt->constraint.rang->min max */
/* XXX must handle int and float
	if (opt->constraint.range->quant)
		printf (" (in steps of %d)", opt->constraint.range->quant);
*/
				break;
			case SANE_CONSTRAINT_WORD_LIST:
// for (i = 0; i < opt->constraint.word_list[0]; ++i)
				break;

			case SANE_CONSTRAINT_STRING_LIST:
				thisopt->argDescrip =
					strjoin((char **) opt->constraint.
						string_list, '|');
				break;

			}
			break;
		}

		add_default_option((char **) &thisopt->argDescrip, handle,
				   opt, i);
/*
		switch (opt->unit) {
		case SANE_UNIT_PIXEL:
			thisopt->argDescrip = "PIXEL";
			break;

		case SANE_UNIT_BIT:
			thisopt->argDescrip = "BIT";
			break;

		case SANE_UNIT_MM:
			thisopt->argDescrip = "MM";
			break;

		case SANE_UNIT_DPI:
			thisopt->argDescrip = "DPI";
			break;

		case SANE_UNIT_PERCENT:
			thisopt->argDescrip = "PERCENT";
			break;

		case SANE_UNIT_MICROSECOND:
			thisopt->argDescrip = "MICROSECOND";
			break;

		case SANE_UNIT_NONE:
			break;
		}
*/
		count++;

		/* Keep track of corner options */
		track_corners(i, opt);
	}

	return options;
}

/* XXX options.c */
static SANE_Status
sane_set_opt_word(SANE_Handle handle, SANE_Word index, double v)
{
	SANE_Word info, *p, value, orig;
	SANE_Status status;
	const SANE_Option_Descriptor *opt;

	opt = sane_get_option_descriptor(handle, index);
	if (opt == NULL) {
		printf("Couldn't get option descriptor for option %d\n",
		       index);
		return SANE_STATUS_INVAL;
	}

	if (opt->type == SANE_TYPE_FIXED)
		orig = value = SANE_FIX(v);
	else
		orig = value = (SANE_Word)v;

	p = &value;
	status = sane_control_option(handle, index,
				     SANE_ACTION_SET_VALUE, p, &info);
	if (status != SANE_STATUS_GOOD)
	        return status;

	if (info & SANE_INFO_INEXACT) {
		if (opt->type == SANE_TYPE_INT)
			printf("Rounded value of %s from %d to %d\n",
			       opt->name, orig, *(SANE_Word *) p);
		else if (opt->type == SANE_TYPE_FIXED)
			printf("Rounded value of %s from %g to %g\n",
			       opt->name, SANE_UNFIX(orig),
			       SANE_UNFIX(*(SANE_Word *) p));
	}

	return status;
}

#define ADF_STR "Automatic Document Feeder"

static SANE_Status
set_option(SANE_Handle handle, int optnum, void *valuep)
{
	const SANE_Option_Descriptor *opt;
	SANE_Status status;
	SANE_Int info;

	opt = sane_get_option_descriptor(handle, optnum);
	if (opt == NULL)
		return SANE_STATUS_INVAL;

	// auto batch mode
	if (strncmp(opt->name, SANE_NAME_SCAN_SOURCE, strlen(SANE_NAME_SCAN_SOURCE)) == 0) {
		if (strncmp(valuep, ADF_STR, strlen(ADF_STR)) == 0) {
			batch = 1;
		}
	}


	if (opt->type == SANE_TYPE_INT && opt->size == sizeof(SANE_Word))
		status = sane_set_opt_word(handle, optnum,
					 *(SANE_Word *) valuep);

	else if (opt->type == SANE_TYPE_FIXED && opt->size == sizeof(SANE_Word))
		status = sane_set_opt_word(handle, optnum,
					 SANE_UNFIX(*(SANE_Word *) valuep));

        else {
        	status = sane_control_option(handle, optnum,
				     SANE_ACTION_SET_VALUE, valuep, &info);

		if (status != SANE_STATUS_GOOD && strcmp(opt->name, "mode") == 0
			&& strcmp(valuep, "binary") == 0) {

			strcpy(valuep, "lineart");

	        	status = sane_control_option(handle, optnum,
					     SANE_ACTION_SET_VALUE, valuep, &info);
		}
	}

	if (status != SANE_STATUS_GOOD)
		printf("setting of option --%s failed (%s)\n",
		       opt->name, sane_strstatus(status));

	return status;
}

static SANE_Status
process_backend_option(SANE_Handle handle, int optnum, const char *optarg)
{
	static SANE_Word *vector = 0;
	static size_t vector_size = 0;
	const SANE_Option_Descriptor *opt;
	size_t vector_length;
	SANE_Status status;
	SANE_Word value;
	void *valuep;

	opt = sane_get_option_descriptor(handle, optnum);
	if (!opt)
		return SANE_STATUS_INVAL;

	if (!SANE_OPTION_IS_ACTIVE(opt->cap)) {
		printf("attempted to set inactive option %s, ignoring\n",
		       opt->name);
		return SANE_STATUS_GOOD;
	}

	if ((opt->cap & SANE_CAP_AUTOMATIC) && optarg &&
	    strncasecmp(optarg, "auto", 4) == 0) {
		status = sane_control_option(handle, optnum,
					     SANE_ACTION_SET_AUTO, 0, 0);
		if (status != SANE_STATUS_GOOD)
			printf("failed to set option --%s to automatic (%s)\n", opt->name, sane_strstatus(status));

		return status;
	}

	valuep = &value;
	switch (opt->type) {
	case SANE_TYPE_BOOL:
		value = 1;	/* no argument means option is set */
		if (optarg) {
			if (strncasecmp(optarg, "yes", strlen(optarg))
			    == 0)
				value = 1;
			else if (strncasecmp
				 (optarg, "no", strlen(optarg)) == 0)
				value = 0;
			else {
				printf("option --%s: bad option value `%s'\n",
				       opt->name, optarg);
				return SANE_STATUS_INVAL;
			}
		}
		break;

	case SANE_TYPE_INT:
	case SANE_TYPE_FIXED:
		/* ensure vector is long enough: */
		vector_length = opt->size / sizeof(SANE_Word);
		if (vector_size < vector_length) {
			vector_size = vector_length;
			vector = realloc(vector,
					 vector_length * sizeof(SANE_Word));
			if (!vector) {
				printf("out of memory\n");
				return SANE_STATUS_NO_MEM;
			}
		}
		parse_vector(opt, optarg, vector, vector_length);
		valuep = vector;
		break;

	case SANE_TYPE_STRING:
		valuep = malloc(opt->size);
		if (valuep == NULL) {
			printf("out of memory\n");
			return SANE_STATUS_NO_MEM;
		}
		strncpy(valuep, optarg, opt->size);
		((char *) valuep)[opt->size - 1] = 0;
		break;

	case SANE_TYPE_BUTTON:
		value = 0;	/* value doesn't matter */
		break;

	default:
		printf("got unknown option type %d\n", opt->type);
		return SANE_STATUS_INVAL;
	}

	status = set_option(handle, optnum, valuep);

	if (opt->type == SANE_TYPE_STRING)
		free(valuep);

	return status;
}

/* XXX tiff.c */
static void
embed_icc_profile(TIFF * image, const char *file)
{
	FILE *fd;
	char *buf;
	struct stat info;

	if (stat(file, &info) != 0)
		return;

	buf = malloc(info.st_size);
	if (buf == NULL)
		return;

	if (verbose)
		printf("using ICC profile '%s', %d bytes\n", file,
		       (int) info.st_size);

	if (info.st_size < 44) {
		printf("ICC profile is too short (%d)\n", (int) info.st_size);
		return;
	}

	fd = fopen(file, "r");

	if (fd) {

		if (fread(buf, 1, info.st_size, fd) == info.st_size) {

			if (strncmp(&buf[36], "acsp", 4) == 0) {
				TIFFSetField(image, TIFFTAG_ICCPROFILE,
					     (uint32) info.st_size, buf);
			} else {
				printf("%s is not a valid ICC profile\n",
				       file);
			}
		}

		fclose(fd);
	}

	free(buf);
}

static int
check_sane_format(SANE_Parameters * parm)
{
	switch (parm->format) {
	case SANE_FRAME_RED:
	case SANE_FRAME_GREEN:
	case SANE_FRAME_BLUE:
		if (parm->depth == 8)
			return 1;
		break;

	case SANE_FRAME_RGBI:
	case SANE_FRAME_RGB:
		if (parm->depth == 16 || parm->depth == 8)
			return 1;

	case SANE_FRAME_IR:
	case SANE_FRAME_GRAY:
		if (parm->depth == 16 || parm->depth == 8 || parm->depth == 1)
			return 1;
	default:
		break;
	}
	return 0;
}

static void
tiff_set_user_fields(TIFF * image)
{
	if (tiff_artist)
		TIFFSetField(image, TIFFTAG_ARTIST, tiff_artist);

	if (tiff_copyright)
		TIFFSetField(image, TIFFTAG_COPYRIGHT, tiff_copyright);

	if (tiff_documentname)
		TIFFSetField(image, TIFFTAG_DOCUMENTNAME, tiff_documentname);

	if (tiff_imagedesc)
		TIFFSetField(image, TIFFTAG_IMAGEDESCRIPTION, tiff_imagedesc);
}

static void
tiff_set_hostcomputer(TIFF * image)
{
	struct utsname u;
	char *host;
	char *uts[6];

	uname(&u);

	uts[0] = u.sysname;
	uts[1] = u.nodename;
	uts[2] = u.release;
	uts[3] = u.version;
	uts[4] = u.machine;
	uts[5] = NULL;

	host = strjoin(uts, ' ');
	if (host) {
		TIFFSetField(image, TIFFTAG_HOSTCOMPUTER, host);
		free(host);
	}
}


static void
tiff_set_fields(TIFF * image, SANE_Parameters * parm, int resolution)
{
	char buf[20];
	time_t now = time(NULL);

	strftime((char *) buf, 20, "%Y:%m:%d %H:%M:%S", localtime(&now));

	TIFFSetField(image, TIFFTAG_DATETIME, buf);

	/* setup header. height will be dynamically incremented */

	TIFFSetField(image, TIFFTAG_IMAGEWIDTH, parm->pixels_per_line);
	TIFFSetField(image, TIFFTAG_BITSPERSAMPLE, parm->depth);
	TIFFSetField(image, TIFFTAG_ROWSPERSTRIP, 1);

	if (parm->depth == 1) {
		TIFFSetField(image, TIFFTAG_SAMPLESPERPIXEL, 1);
		TIFFSetField(image, TIFFTAG_PHOTOMETRIC,
			     PHOTOMETRIC_MINISWHITE);
		TIFFSetField(image, TIFFTAG_THRESHHOLDING,
			THRESHHOLD_BILEVEL);
	} else {
		TIFFSetField(image, TIFFTAG_SAMPLESPERPIXEL,
			     ((8 * parm->bytes_per_line) /
			      parm->pixels_per_line) / parm->depth);

		if (parm->format == SANE_FRAME_GRAY) {
			TIFFSetField(image, TIFFTAG_PHOTOMETRIC,
				     PHOTOMETRIC_MINISBLACK);
			TIFFSetField(image, TIFFTAG_THRESHHOLDING,
				THRESHHOLD_HALFTONE);
#ifdef SANE_HAS_INFRARED
		} else if (parm->format == SANE_FRAME_IR) {
			TIFFSetField(image, TIFFTAG_PHOTOMETRIC,
				     PHOTOMETRIC_MINISBLACK);
			TIFFSetField(image, TIFFTAG_THRESHHOLDING,
				THRESHHOLD_HALFTONE);
		} else if (parm->format == SANE_FRAME_RGBI) {
			TIFFSetField(image, TIFFTAG_EXTRASAMPLES,
				     EXTRASAMPLE_UNSPECIFIED);
#endif
		} else {
			TIFFSetField(image, TIFFTAG_PHOTOMETRIC,
				     PHOTOMETRIC_RGB);
		}
		

	}

	if (compress) {
		if (parm->depth == 1)
			TIFFSetField(image, TIFFTAG_COMPRESSION,
				     COMPRESSION_CCITTFAX4);
		else
			TIFFSetField(image, TIFFTAG_COMPRESSION,
				     COMPRESSION_DEFLATE);
	}

	TIFFSetField(image, TIFFTAG_FILLORDER, FILLORDER_MSB2LSB);
	TIFFSetField(image, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);

	TIFFSetField(image, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
	TIFFSetField(image, TIFFTAG_XRESOLUTION, (float) resolution);
	TIFFSetField(image, TIFFTAG_YRESOLUTION, (float) resolution);

	/* XXX build add date */
	TIFFSetField(image, TIFFTAG_SOFTWARE, __RELEASE, __DATE__);

	if (tiff_orientation) {
		if (strcmp(tiff_orientation, "topleft") == 0)
			TIFFSetField(image, TIFFTAG_ORIENTATION,
				ORIENTATION_TOPLEFT);
		else if (strcmp(tiff_orientation, "topright") == 0)
			TIFFSetField(image, TIFFTAG_ORIENTATION,
				ORIENTATION_TOPRIGHT);
		else if (strcmp(tiff_orientation, "botright") == 0)
			TIFFSetField(image, TIFFTAG_ORIENTATION,
				ORIENTATION_BOTRIGHT);
		else if (strcmp(tiff_orientation, "botleft") == 0)
			TIFFSetField(image, TIFFTAG_ORIENTATION,
				ORIENTATION_BOTLEFT);
		else if (strcmp(tiff_orientation, "lefttop") == 0)
			TIFFSetField(image, TIFFTAG_ORIENTATION,
				ORIENTATION_LEFTTOP);
		else if (strcmp(tiff_orientation, "righttop") == 0)
			TIFFSetField(image, TIFFTAG_ORIENTATION,
				ORIENTATION_RIGHTTOP);
		else if (strcmp(tiff_orientation, "rightbot") == 0)
			TIFFSetField(image, TIFFTAG_ORIENTATION,
				ORIENTATION_RIGHTBOT);
		else if (strcmp(tiff_orientation, "leftbot") == 0)
			TIFFSetField(image, TIFFTAG_ORIENTATION,
				ORIENTATION_LEFTBOT);
		else
			printf("unkown orientation: %s\n", tiff_orientation);
	}

#ifdef SANE_HAS_EVOLVED
	if (strlen(si.vendor))
		TIFFSetField(image, TIFFTAG_MAKE, si.vendor);

	if (strlen(si.model))
		TIFFSetField(image, TIFFTAG_MODEL, si.model);
#endif
}

static char *
format2name(SANE_Frame format)
{
	switch (format) {
	case SANE_FRAME_RGB:
		return "RGB";
	case SANE_FRAME_GRAY:
		return "gray";
#ifdef SANE_HAS_INFRARED
	case SANE_FRAME_IR:
		return "infrared";
	case SANE_FRAME_RGBI:
		return "RGBI";
#endif
	case SANE_FRAME_RED:
		return "red";
	case SANE_FRAME_GREEN:
		return "green";
	case SANE_FRAME_BLUE:
		return "blue";
	default:
		return "unknown";
	}
}


static SANE_Status
scan_to_tiff(TIFF * image, int pageno, int pages, int resolution)
{
	int rows = 0;
	int tries = 4;
	int len, hundred_percent;

/* XXX	SANE_Byte min = 0xff, max = 0; */
	SANE_Parameters parm;
	SANE_Status status;
	SANE_Word total_bytes = 0, expected_bytes;

	SANE_Byte *buffer;
	size_t buffer_size;

#ifdef SANE_HAS_WARMING_UP
scan:
#endif
	if (tries == 0) {
		printf("Your scanner must be frozen, will not try again :)\n");
		return SANE_STATUS_IO_ERROR;
	}

	status = sane_start(handle);

	/* return immediately when no docs are available */
	if (status == SANE_STATUS_NO_DOCS)
		return status;

#ifdef SANE_HAS_WARMING_UP
	if (status == SANE_STATUS_WARMING_UP) {
		tries--;
		printf("The scanner is warming up, will retry in 15 seconds...\n");
		sleep(15);
		goto scan;
	}
#endif
	if (status != SANE_STATUS_GOOD) {
		printf("sane_start: %s\n", sane_strstatus(status));
		return status;
	}

	status = sane_get_parameters(handle, &parm);
	if (status != SANE_STATUS_GOOD) {
		printf("sane_get_parameters: %s\n", sane_strstatus(status));
		return status;
	}

	if (verbose) {
		if (parm.lines >= 0) {
			printf("scanning %s image of size %dx%d pixels at "
			       "%d bits/pixel\n",
			       format2name(parm.format),
			       parm.pixels_per_line, parm.lines,
			       8 * parm.bytes_per_line /
			       parm.pixels_per_line);
		} else {
			printf("scanning %s image %d pixels wide and "
			       "variable height at %d bits/pixel\n",
			       format2name(parm.format),
			       parm.pixels_per_line,
			       8 * parm.bytes_per_line /
			       parm.pixels_per_line);
		}
	}

	/* check format */
	if (!check_sane_format(&parm))
		return SANE_STATUS_INVAL;

	/* set tiff fields */
	tiff_set_fields(image, &parm, resolution);
	tiff_set_user_fields(image);
	tiff_set_hostcomputer(image);

	if (pageno && batch)
		TIFFSetField(image, TIFFTAG_PAGENUMBER, pageno, pages);

	hundred_percent = parm.bytes_per_line * parm.lines;
	/* XXX
	   switch (parm.format) {
	   case SANE_FRAME_RGB:
	   case SANE_FRAME_GRAY:
	   break;


	   }

	   *
	   ((parm.format == SANE_FRAME_RGB
	   || parm.format == SANE_FRAME_GRAY) ? 1 : 3);
	 */

	buffer_size = scanlines * parm.bytes_per_line;

	if (verbose > 1) {
		printf("working on a %d Kb buffer that holds %d scanlines\n",
			buffer_size / 1024,
			scanlines);
	}		


	buffer = malloc(buffer_size);
	if (buffer == NULL)
		return SANE_STATUS_NO_MEM;

	while (1) {
		double progr;

		/* read from SANE */
		status = sane_read(handle, buffer, buffer_size, &len);
		if (status == SANE_STATUS_EOF)
			break;

		if (status != SANE_STATUS_GOOD) {
			printf("sane_read: %s\n", sane_strstatus(status));
			break;
		}

		total_bytes += (SANE_Word) len;
		progr = ((total_bytes * 100.) / (double) hundred_percent);
		if (progr > 100.)
			progr = 100.;
		if (progress)
			printf("progress: %3.1f%%\r", progr);

		/* write to file */
		{
			int i;
			unsigned char *p = buffer;
			int lines = len / parm.bytes_per_line;

			/* Write each scanline */
			for (i = 0; i < lines; i++) {
				TIFFWriteScanline(image, p, rows++, 0);
				p += parm.bytes_per_line;
			}
		}
	}

	expected_bytes = parm.bytes_per_line * parm.lines;
	/* *
	   ((parm.format == SANE_FRAME_RGB
	   || parm.format == SANE_FRAME_GRAY) ? 1 : 3);
	 */

	if (parm.lines < 0)
		expected_bytes = 0;

	if (total_bytes > expected_bytes && expected_bytes != 0) {
		printf("WARNING: read more data than announced by backend "
		       "(%u/%u)\n", total_bytes, expected_bytes);
	} else if (verbose)
		printf("read %u bytes in total\n", total_bytes);

	return status;
}

static int
get_resolution(SANE_Handle handle)
{
	const SANE_Option_Descriptor *resopt;
	int resol = 0;
	void *val;

	if (resolution_optind < 0)
		return 0;

	resopt = sane_get_option_descriptor(handle, resolution_optind);
	if (!resopt)
		return 0;

	val = malloc(resopt->size);
	if (val == NULL)
		return 0;

	sane_control_option(handle, resolution_optind,
			    SANE_ACTION_GET_VALUE, val, 0);
	if (resopt->type == SANE_TYPE_INT)
		resol = *(SANE_Int *) val;
	else
		resol = (int) (SANE_UNFIX(*(SANE_Fixed *) val) + 0.5);

	free(val);

	return resol;
}

static void
tiffscan_exit(void)
{
	if (handle) {
		if (verbose > 1)
			printf("closing device\n");
		sane_close(handle);
	}

	sane_exit();

	paperdone();

	if (verbose)
		printf("done.\n");
}

static void
list_devices(void)
{
	int i;
	const SANE_Device **device_list;
	SANE_Status status;

	status = sane_get_devices(&device_list, SANE_FALSE);
	if (status != SANE_STATUS_GOOD) {
		printf("sane_get_devices() failed: %s\n",
		       sane_strstatus(status));
		return;
	}

	for (i = 0; device_list[i]; i++) {
		printf("device '%s' is a %s %s %s\n",
		       device_list[i]->name,
		       device_list[i]->vendor,
		       device_list[i]->model, device_list[i]->type);
	}

	if (i == 0) {
		printf("\nNo scanners were identified. If you were expecting "
		       "something different,\ncheck that the scanner is plugged "
		       "in, turned on and detected by the\nsane-find-scanner tool "
		       "(if appropriate). Please read the documentation\nwhich came "
		       "SANE (README, FAQ, manpages).\n");
	}
}

/* caller must free the returned string */

static char *
find_suitable_device(void)
{
	const SANE_Device **device_list;
	SANE_Status status;

	char *s = getenv("SANE_DEFAULT_DEVICE");
	if (s)
		return strdup(s);

	status = sane_get_devices(&device_list, SANE_FALSE);
	if (status != SANE_STATUS_GOOD) {
		printf("sane_get_devices() failed: %s\n",
		       sane_strstatus(status));
		return NULL;
	}

	if (!device_list[0]) {
		printf("no SANE devices found\n");
		return NULL;
	}

	return strdup(device_list[0]->name);
}

static TIFF *
tiff_open(const char *file, const char *icc, int pageno)
{
	TIFF *image;
	char *f;
	int len;

	/* alloc space for file name + formatting */
	len = strlen(file) * 2;

	f = malloc(len);
	if (f == NULL)
		return NULL;

	/* add formatting to the file name */
	snprintf(f, len, file, pageno);

	image = TIFFOpen(f, "w");

	free(f);

	/* embed ICC profile */
	if (icc)
		embed_icc_profile(image, icc);

	return image;
}

static SANE_Status
scan(SANE_Handle handle)
{
	char readbuf[2];
	char *readbuf2;

	TIFF *image = NULL;

	int n = batch_start_at;
	int count = batch_amount;

	SANE_Status status = SANE_STATUS_GOOD;

	int resolution = get_resolution(handle);	/* XXX */

	signal(SIGHUP, sighandler);
	signal(SIGINT, sighandler);
	signal(SIGPIPE, sighandler);
	signal(SIGTERM, sighandler);

	if (output_file == NULL) {
		/* choice an appropriate file name */
		time_t now = time(NULL);

		strftime((char *) output_file_buf, 16,
			 "%Y%m%d%H%M%S", localtime(&now));

		if (batch && !multi)
			strcat((char *) output_file_buf, "-%04d");

		strcat((char *) output_file_buf, ".tif");

		output_file = output_file_buf;
	}


	printf("Scanning to %s at %d dpi\n", output_file, resolution);

	if (resolution < 100)
		printf("WARNING: you are scanning at a low dpi value, please check your parameters\n");

	if (batch) {

		if (batch_amount != BATCH_COUNT_UNLIMITED)
			printf(" %d", batch_amount);
		else
			printf(" an unknown amount of");

		printf(" pages, incrementing by %d, numbering from %d\n",
		       batch_increment, batch_start_at);
	}

	do {
		/* open file if necessary */
		if (image == NULL)
			image = tiff_open(output_file, icc_profile, n);

		if (image == NULL) {
			printf("cannot open file\n");
			break;
		}

		/* XXX check return code */

		if (batch_prompt) {
			printf("Place page no. %d on the scanner.\n", n);
			printf("Press <RETURN> to continue.\n");
			printf("Press Ctrl + D to terminate.\n");
			readbuf2 = fgets(readbuf, 2, stdin);

			if (readbuf2 == NULL) {
				printf("Batch terminated, %d pages scanned\n",
				       (n - batch_increment));
				break;	/* get out of this loop */
			}
		}

		if (batch)
			printf("Scanning page %d\n", n);

		status = scan_to_tiff(image, n,
				      (batch_amount > 0) ? batch_amount : 0,
				      resolution);

		/* no more docs, stop here */
		if (status == SANE_STATUS_NO_DOCS) {
			printf("No (more) documents in the scanner\n");
			break;
		}

		if (batch || verbose > 1)
			printf("Scanned page %d to %s .\n", n,
			       TIFFFileName(image));
			       

		/* continue reading when EOF */
		if (status == SANE_STATUS_EOF)
			status = SANE_STATUS_GOOD;

		/* any error? */
		if (status != SANE_STATUS_GOOD)
			break;

		/* continuing... */

		/* write current image and prepare for next one */
		TIFFWriteDirectory(image);

		/* close if appropriate */
		if (batch && !multi) {
			TIFFClose(image);
			image = NULL;
		}

		n += batch_increment;
		count--;
		batch_count++;
	}
	while ((batch && (batch_amount == BATCH_COUNT_UNLIMITED || count)));


	if (batch)
		printf("Scanned %d pages\n", batch_count);

	if (image) {
		/* If there are no more docs, we should delete the
		 * otherwise empty file.
		 */
		if (status == SANE_STATUS_NO_DOCS)
			if (!multi || batch_count == 0)
				unlink(TIFFFileName(image));

		TIFFClose(image);
	}

	return status;
}

/* input is in postscript points, 1/72 inch */
static void
set_scanning_area(SANE_Handle handle, double width, double height)
{
	SANE_Word w = width / 72.0 * 10 * 2.54;	/* convert to mm */
	SANE_Word h = height / 72.0 * 10 * 2.54;

	if (verbose)
		printf("Setting scanning area to %dx%d mm\n", w, h);

	sane_set_opt_word(handle, corners[2], w);
	sane_set_opt_word(handle, corners[3], h);
}


enum modes
{ MODE_NONE, MODE_SCAN, MODE_LIST, MODE_ERROR, MODE_STOP,
	MODE_VERSION
};

static int
process_cmd_line(int argc, const char **argv)
{
	int optrc;
	int mode = MODE_NONE;

	poptContext optc = poptGetContext("tiffscan", argc, argv, options,
					  POPT_CONTEXT_POSIXMEHARDER);

	/* parse arguments */
	for (optrc = 0; optrc != -1;) {

		optrc = poptGetNextOpt(optc);

		switch (optrc) {
		case -1:
			break;

		case POPT_ERROR_BADOPT:
        	        /* maybe a backend option */
        	        break;

		case OPT_VERBOSE:
			verbose++;
			break;

		case OPT_LIST_DEVS:
			mode = MODE_STOP;
			list_devices();
			break;

		case OPT_SCAN:
			if (mode != MODE_NONE)
				printf("BUG: mode is %d\n", mode);
			else
				mode = MODE_SCAN;
			break;

		case OPT_VERSION:
			mode = MODE_VERSION;
			break;

		case OPT_HELP:
			if (devname == NULL) {
				mode = MODE_STOP;
				poptPrintHelp(optc, stdout, 0);
			}
			break;

                /* this is saved here, do not use libopt automatic
                 * assignment to a variable.
                 */
                case OPT_DEVICE:
                        devname = poptGetOptArg(optc);
                        break;

		default:
			mode = MODE_STOP;
			printf("%s: %s\n",
			       poptBadOption(optc, POPT_BADOPTION_NOALIAS),
			       poptStrerror(optrc));
			/* immediately stop parsing options */
			optrc = -1;
			break;
		}
	}

	poptFreeContext(optc);

	return mode;
}

static int
process_backend_options(SANE_Handle handle, int argc, const char **argv,
			int mode)
{
	SANE_Status status;

	int optrc;
	poptContext optc;
	struct poptOption *dev_options;

	dev_options = fetch_options(handle);
	if (dev_options == NULL)
		return MODE_STOP;

	/* include the backend options into the main table */
	options[ARRAY_SIZE(options) - 2].argInfo = POPT_ARG_INCLUDE_TABLE;
	options[ARRAY_SIZE(options) - 2].arg = dev_options;
	options[ARRAY_SIZE(options) - 2].descrip = "Backend options";

	optc = poptGetContext("tiffscan", argc, argv, options, 0);

	while ((optrc = poptGetNextOpt(optc)) > 0) {
		if (optrc >= 1000) {	/* backend option */
			status = process_backend_option(handle,
							optrc - 1000,
							poptGetOptArg(optc));

			if (status != SANE_STATUS_GOOD) {
				mode = MODE_STOP;
				break;
			}
		}

		if (optrc == OPT_HELP) {
			mode = MODE_STOP;
			poptPrintHelp(optc, stdout, 0);
                        break;
		}

		if (optrc == OPT_SCAN)
			mode = MODE_SCAN;

                if (optrc == OPT_DEVICE) {
                        const char *d = poptGetOptArg(optc);

                        if (d && strcmp(d, devname) != 0) {
                                printf("WARNING: device name must be given before backend options\n");
                                mode = MODE_STOP;
                                break;
                        }
                }
	}

	if (optrc < -1) {
		mode = MODE_STOP;
		printf("%s: %s\n",
		       poptBadOption(optc, POPT_BADOPTION_NOALIAS),
		       poptStrerror(optrc));
	}

	poptFreeContext(optc);
	free(dev_options);

	return mode;
}

int
main(int argc, const char **argv)
{
	int rc = 0, mode = MODE_NONE;

	SANE_Int version;
	SANE_Status status;

	atexit(tiffscan_exit);

	paperinit();

	sane_init(&version, NULL);

	mode = process_cmd_line(argc, argv);

	if (mode == MODE_VERSION || verbose > 0) {
		printf("tiffscan %d.%d (%s); libsane version %d.%d.%d\n",
		       __VERSION, __REVISION, __DATE__,
		       SANE_VERSION_MAJOR(version),
		       SANE_VERSION_MINOR(version),
		       SANE_VERSION_BUILD(version));
	}

	if (mode == MODE_VERSION)
	        goto end;

	if (mode == MODE_STOP)
		goto end;

	/* find a scanner */
	if (devname == NULL)
                devname = find_suitable_device();

	if (devname == NULL)
		goto end;

	/* open */
	if (devname[0] == '/') {
		printf("\nYou seem to have specified a UNIX device name, "
		       "or filename instead of selecting\nthe SANE scanner or "
		       "image acquisition device you want to use. As an example,\n"
		       "you might want 'epson2:/dev/sg0' or "
		       "'hp:/dev/usbscanner0'. If any supported\ndevices are "
		       "installed in your system, you should be able to see a "
		       "list with\ntiffscan --list-devices.\n");
	}

	status = sane_open(devname, &handle);
	if (status != SANE_STATUS_GOOD) {
		printf("failed to open device %s: %s\n",
		       devname, sane_strstatus(status));
		goto end;
	}

	
	printf("Using %s\n", devname);

#ifdef SANE_HAS_EVOLVED
	if (sane_has_evolved(handle, version)) {
		printf("... with SANE Evolution extensions!\n");

		sane_tell_api_level(handle, SANE_API(1, 1, 0));
		
		memset(&si, 0x00, sizeof(si));

		status = sane_get_scanner_info(handle, &si);
		if (status == SANE_STATUS_GOOD) {
			printf("%s %s (revision: %s, serial: %s)\n",
				si.vendor, si.model,
				strlen(si.revision) ? si.revision : "n/a",
				strlen(si.serial) ? si.serial : "n/a");
		}
	} else {
/*		printf("SANE Evolution NOT detected. You will miss some features.\n"
			"Please check http://code.google.com/p/sane-evolution/\n");*/
	}
#else
/*	printf("tiffscan has been compiled with the old SANE, you might\n"
		"want to evolve to SANE Evolution or you will miss some features.\n"
		"Please check http://code.google.com/p/sane-evolution/\n");*/

#endif
	/* handle backend options */
	if (verbose)
		printf("Setting backend parameters\n");

	mode = process_backend_options(handle, argc, argv, mode);
	if (mode == MODE_STOP)
		goto end;

	/* shall we dance? */
	if (mode != MODE_SCAN) {
		printf("Use --scan to begin scanning, --help for details.\n");
		goto end;
	}

	/* set scanning area size */
	if (paper) {
		const struct paper *pi = paperinfo(paper);

		if (pi == NULL) {
			printf("Unknown paper name: %s\n", paper);
			goto end;
		}

		if (corners[2] == -1 || corners[3] == -1) {
			printf("Setting scanning area size is not supported on this scanner.\n");
			goto end;
		}

		set_scanning_area(handle, paperpswidth(pi),
				  paperpsheight(pi));
	}

	/* scan */
	status = scan(handle);

	if (batch && batch_count == 0 && status == SANE_STATUS_NO_DOCS)
		rc = 2;

	if (status != SANE_STATUS_GOOD && status != SANE_STATUS_NO_DOCS
		&& status != SANE_STATUS_CANCELLED)
		printf("SANE error: %s\n", sane_strstatus(status));

end:

        free(devname);          

	exit(rc);
}
