/*
 * ss_spice2.c: routines for SpiceStream that handle the output
 * 	format from Berkeley Spice2G6
 * 
 * include LICENSE
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
// #include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <float.h>

#include <spicestream.h>
 
#ifdef TRACE_MEM
#include <tracemem.h>
#endif
        

/* header file for spice2g6 raw file structures */

typedef struct {
   char title[80];
   char date[8];
   char time[8];
   short mode:16;
   short nvars:16;
   short const4:16;
} spice_hdr_t;

#define S2_NAME_SIZE 8
#define S2_TITLE_SIZE 24

#define SPICE_MAGIC "rawfile1"

typedef union {
   double val;
   struct {
      float r;
      float j;
   } cval;
   char magic[8];
} spice_var_t;

typedef short spice_var_type_t;
typedef short spice_var_loc_t;

void sf_s2clean_name(char *line, int len)
{
   char *cp;
   
   line[len - 1] = 0;
   if ((cp = strchr( line, ' ')) ){
      *cp = 0;
   }
}

/*
 * Read spice-type file header - Berkeley Spice2G6 "raw" format
 *
 * return  1 ok
 *         0 EOF
 *        -1 Error
 */
int sf_rdhdr_s2raw(  SpiceStream *ss )
{
   int i;
   spice_hdr_t *s2hdr;
   spice_var_t *s2var;
   spice_var_type_t *s2vtype;
   char *line;

   if ( ! (ss->flags & SSF_PHDR) ) {
      if ( fdbuf_read(ss->linebuf, sizeof(spice_var_t) ) != sizeof(spice_var_t) ) {
	 return 0;
      }
   }
   line = ((DBuf *) ss->linebuf)->s;
   s2var = (spice_var_t *) line;
   if (memcmp(s2var, SPICE_MAGIC, 8)) {
      msg_dbg("not a spice2 rawfile (bad magic number)");
      return -1;
   }
   if ( fdbuf_read(ss->linebuf, sizeof(spice_hdr_t) ) != sizeof(spice_hdr_t) ) {
      return 0;
   }
   line = ((DBuf *) ss->linebuf)->s;
   s2hdr = (spice_hdr_t *) line;
   ss->ndv = s2hdr->nvars - 1;
   msg_dbg("nvars=%d const=%d analysis mode %d",
	       s2hdr->nvars, s2hdr->const4, s2hdr->mode);

   /* independent variable name */
   if ( fdbuf_read(ss->linebuf, S2_NAME_SIZE ) != S2_NAME_SIZE ) {
      return 0;
   }
   line = ((DBuf *) ss->linebuf)->s;
   sf_s2clean_name(line, S2_NAME_SIZE);
   spicestream_var_add ( ss, line, TIME, 1 );
   ss->ncols = 1;

   for (i = 0; i < ss->ndv; i++) {
      if ( fdbuf_read(ss->linebuf, S2_NAME_SIZE ) != S2_NAME_SIZE ) {
	 return 0;
      }
      line = ((DBuf *) ss->linebuf)->s;
      sf_s2clean_name(line, S2_NAME_SIZE);

      spicestream_var_add ( ss, line, VOLTAGE, 1 );  /* FIXME:sgt: get correct type */
      ss->ncols++;
   }

   /* ivar type */
   if ( fdbuf_read(ss->linebuf, sizeof(spice_var_type_t) ) != sizeof(spice_var_type_t) ) {
      return 0;
   }
   line = ((DBuf *) ss->linebuf)->s;
   s2vtype = (spice_var_type_t *) line;
   dataset_set_wavevar_type(ss->wds, 0, *s2vtype );

   /* dvar type */
   for (i = 0; i < ss->ndv; i++) {
      if ( fdbuf_read(ss->linebuf, sizeof(spice_var_type_t) ) != sizeof(spice_var_type_t) ) {
	 return 0;
      }
      line = ((DBuf *) ss->linebuf)->s;
      s2vtype = (spice_var_type_t *) line;
      dataset_set_wavevar_type(ss->wds,  1 + i, *s2vtype );
   }

   /* vloc */
   for (i = 0; i < ss->ndv + 1; i++) {
      if ( fdbuf_read(ss->linebuf, sizeof(spice_var_loc_t) ) != sizeof(spice_var_loc_t) ) {
	 return 0;
      }
   }
   if ( fdbuf_read(ss->linebuf, S2_TITLE_SIZE ) != S2_TITLE_SIZE ) {
      return 0;
   }
   line = ((DBuf *) ss->linebuf)->s;
   sf_s2clean_name(line, S2_TITLE_SIZE);
   dataset_dup_setname(ss->wds, line);
   msg_dbg("title=\"%s\" - done with header at offset=0x%lx",
	    line, (long) fdbuf_tello(ss->linebuf) );
   ss->flags = SSF_PHDR ;
   return 1;
}


/*
 * Read row of values from a spice2 rawfile
 */
int sf_readrow_s2raw(SpiceStream *ss )
{
   int i;
   int ret;
   spice_var_t *val;
   char *line;

   /* independent var */
   if ( ( ret = fdbuf_read(ss->linebuf, sizeof(spice_var_t) )) <= 0 ) {
      return ret;
   }
   line = ((DBuf *) ss->linebuf)->s;
   val = (spice_var_t *) line;

   if (memcmp( val, SPICE_MAGIC, 8) == 0){ /* another analysis */
      return -2;
   }
   dataset_val_add( ss->wds, val->val );
	
   /* dependent vars */
   for (i = 0; i < ss->ndv; i++) {
      if ( ( ret = fdbuf_read(ss->linebuf, sizeof(spice_var_t) )) != sizeof(spice_var_t) ) {
	 msg_error(_("unexpected EOF at dvar %d"), i);
	 return -1;
      }
      line = ((DBuf *) ss->linebuf)->s;
      val = (spice_var_t *) line;
      dataset_val_add( ss->wds, val->val );
   }
   return 1;
}


void sf_write_file_s2raw( FILE *fd, WaveTable *wt, char *fmt)
{
   int i;
   int j;
   int k = 0;
   WDataSet *wds;
   WaveVar *var ;
   spice_var_t s2var;
   spice_hdr_t s2hdr;
   char name[S2_NAME_SIZE];
   char title[S2_TITLE_SIZE];
   spice_var_type_t s2vtype;
   spice_var_loc_t s2vloc;
   
   while ( (wds = wavetable_get_dataset( wt, k++)) ){
      memcpy(s2var.magic, SPICE_MAGIC, 8);
      fwrite( &s2var, sizeof(spice_var_t), 1, fd);
   
      memset(&s2hdr, 0, sizeof(spice_hdr_t) );
      app_strncpy(s2hdr.title, "Generated by Gaw", sizeof(spice_hdr_t) ); 
      s2hdr.nvars = wds->numVars ;
      s2hdr.const4 = 0;
      s2hdr.mode = 0;
      fwrite( &s2hdr, sizeof(spice_hdr_t), 1, fd);
   
      for ( i = 0 ; i < wds->numVars ; i++ ){
	 var = g_ptr_array_index( wds->vars, i);
	 app_strncpy(name, var->varName, S2_NAME_SIZE);
	 fwrite( name, sizeof(name), 1, fd);
      }

      for ( i = 0 ; i < wds->numVars ; i++ ){
	 var = g_ptr_array_index( wds->vars, i);
	 s2vtype = var->type;
	 fwrite( &s2vtype, sizeof(spice_var_type_t), 1, fd);
      }
      
      for ( i = 0 ; i < wds->numVars ; i++ ){
	 var = g_ptr_array_index( wds->vars, i);
	 s2vloc = 0;
	 fwrite( &s2vloc, sizeof(spice_var_loc_t), 1, fd);
      }
      
      app_strncpy(title, wds->setname, S2_TITLE_SIZE);
      fwrite( title, sizeof(title), 1, fd);

      msg_dbg("writing data at offset 0x%lx\n", (long) ftello(fd) );
   
      for ( i = 0 ; i < wds->nrows ; i++ ){
	 for ( j = 0 ; j < wds->ncols ; j++ ){
	    double val = dataset_val_get( wds, i, j );
	    s2var.val = val;
	    fwrite( &s2var, sizeof(spice_var_t), 1, fd);
	 }
      }
   }
}
