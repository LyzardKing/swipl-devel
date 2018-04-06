/*  Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@vu.nl
    WWW:           http://www.swi-prolog.org
    Copyright (c)  2018, University of Amsterdam
                         VU University Amsterdam
		         CWI, Amsterdam
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    1. Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in
       the documentation and/or other materials provided with the
       distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
    FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
    INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
    BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
    LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
    ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

#include "minizip/zip.h"
#include "minizip/unzip.h"
#include "pl-incl.h"


		 /*******************************
		 *  ACCESS ARCHIVES AS STREAMS  *
		 *******************************/

static voidpf
zopen64_file(voidpf opaque, const char* filename, int mode)
{ char modes[4];
  char *m = modes;

  if ( (mode&ZLIB_FILEFUNC_MODE_CREATE) )
    *m++ = 'w';
  else
    *m++ = 'r';
  *m++ = 'b';
  *m = EOS;

  return Sopen_file(filename, modes);
}

static uLong
zread_file(voidpf opaque, voidpf stream, void* buf, uLong size)
{ return Sfread(buf, 1, size, stream);
}

static uLong
zwrite_file(voidpf opaque, voidpf stream, void* buf, uLong size)
{ return Sfwrite(buf, 1, size, stream);
}

static ZPOS64_T
ztell64_file(voidpf opaque, voidpf stream)
{ return Stell64(stream);
}

static long
zseek64_file(voidpf opaque, voidpf stream, ZPOS64_T offset, int origin)
{ return Sseek64(stream, offset, origin);
}

static int
zclose_file(voidpf opaque, voidpf stream)
{ return Sclose(stream);
}

static int
zerror_file(voidpf opaque, voidpf stream)
{ return Sferror(stream);
}

static zlib_filefunc64_def zfile_functions =
{ zopen64_file,
  zread_file,
  zwrite_file,
  ztell64_file,
  zseek64_file,
  zclose_file,
  zerror_file,
  NULL						/* opaque */
};

		 /*******************************
		 *	   ARCHIVE BLOB		*
		 *******************************/

typedef struct zipper
{ zipFile writer;
} zipper;

static int
write_zipper(IOSTREAM *s, atom_t aref, int flags)
{ zipper *ref = PL_blob_data(aref, NULL, NULL);

  Sfprintf(s, "<zipper>(%p)", ref);
  return TRUE;
}

static void
acquire_zipper(atom_t aref)
{ zipper *ref = PL_blob_data(aref, NULL, NULL);
  (void)ref;
}

static int
release_zipper(atom_t aref)
{ zipper *ref = PL_blob_data(aref, NULL, NULL);
  zipFile zf;

  if ( (zf=ref->writer) )
  { ref->writer = NULL;
    zipClose(zf, NULL);
  }
  free(ref);

  return TRUE;
}

static int
save_zipper(atom_t aref, IOSTREAM *fd)
{ zipper *ref = PL_blob_data(aref, NULL, NULL);
  (void)fd;

  return PL_warning("Cannot save reference to <zipper>(%p)",
		    ref);
}

static atom_t
load_zipper(IOSTREAM *fd)
{ (void)fd;

  return PL_new_atom("<zipper>");
}

static PL_blob_t zipper_blob =
{ PL_BLOB_MAGIC,
  PL_BLOB_NOCOPY,
  "zipper",
  release_zipper,
  NULL,
  write_zipper,
  acquire_zipper,
  save_zipper,
  load_zipper
};

static int
unify_zipper(term_t t, zipper *zipper)
{ return PL_unify_blob(t, zipper, sizeof(*zipper), &zipper_blob);
}

static int
get_zipper(term_t t, zipper **zipper)
{ void *p;
  size_t len;
  PL_blob_t *type;

  if ( PL_get_blob(t, &p, &len, &type) && type == &zipper_blob )
  { *zipper = p;
    return TRUE;
  }

  PL_type_error("zipper", t);
  return FALSE;
}

		 /*******************************
		 *     OPEN CLOSE ARCHIVES	*
		 *******************************/

/** zip_open(+File, +Mode, -Zip, +Options)
*/

static
PRED_IMPL("zip_open", 4, zip_open, 0)
{ PRED_LD
  char *fname;
  atom_t mode;
  int fflags = PL_FILE_OSPATH;
  zipper *z;

  if ( !PL_get_atom_ex(A2, &mode) )
    return FALSE;
  if ( mode == ATOM_read )
    fflags |= PL_FILE_EXIST;
  else if ( mode == ATOM_write || mode == ATOM_append )
    fflags |= PL_FILE_WRITE;
  else
    return PL_domain_error("file_mode", A2);

  if ( !PL_get_file_name(A1, &fname, fflags) )
    return FALSE;

  if ( !(z=malloc(sizeof(*z))) )
    return PL_resource_error("memory");

  if ( mode == ATOM_write || mode == ATOM_append )
  { if ( (z->writer=zipOpen2_64(fname, mode == ATOM_append,
				NULL,
				&zfile_functions)) )
    { return unify_zipper(A3, z);
    } else
    { goto error;
    }
  } else
  { assert(0);
  }

error:
  return PL_warning("zip_open/4 failed");
}

/** zip_close(+Zipper, +Comment)
*/

static
PRED_IMPL("zip_close", 2, zip_close, 0)
{ PRED_LD
  char *comment = NULL;
  zipper *z;
  int flags = (CVT_ATOM|CVT_STRING|CVT_EXCEPTION|REP_UTF8);

  if ( get_zipper(A1, &z) &&
       (PL_is_variable(A2) || PL_get_chars(A2, &comment, flags)) )
  { zipFile zf;

    if ( (zf=z->writer) )
    { z->writer = NULL;

      if ( zipClose(zf, comment) == 0 )
	return TRUE;
      else
	return PL_warning("zip_close/2 failed");
    }
  }

  return FALSE;
}

		 /*******************************
		 *      PUBLISH PREDICATES	*
		 *******************************/

BeginPredDefs(zip)
  PRED_DEF("zip_open",  4, zip_open,  0)
  PRED_DEF("zip_close", 2, zip_close, 0)
EndPredDefs