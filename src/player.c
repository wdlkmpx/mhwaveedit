/*
 * Copyright (C) 2003 2004 2005, Magnus Hjorth
 *
 * This file is part of mhWaveEdit.
 *
 * mhWaveEdit is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by        
 * the Free Software Foundation; either version 2 of the License, or  
 * (at your option) any later version.
 *
 * mhWaveEdit is distributed in the hope that it will be useful,   
 * but WITHOUT ANY WARRANTY; without even the implied warranty of  
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mhWaveEdit; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 */


#include <config.h>

#include <glib.h>
#include "inifile.h"
#include "sound.h"
#include "player.h"
#include "main.h"
#include "um.h"
#include "rateest.h"
#include "rateconv.h"
#include "gettext.h"

Dataformat player_fallback_format = { DATAFORMAT_PCM, 44100, 2, 0, 0, TRUE, 
				      IS_BIGENDIAN };

gboolean varispeed_smooth_flag = FALSE;

/* Info about currently playing sound. */
static ChunkHandle *ch=NULL; /* NULL == stopped */
static rateconv *varispeed_conv = NULL;
static gfloat file_speed = 1.0; /* Speed relative to file's sample rate */
static gfloat true_speed = 1.0; /* Speed relative to playback sample rate */
static off_t loopstart, loopend, curpos;
static off_t realpos_offset;
static gboolean loop, small_loop;

/* After changing speed with the "smooth" method, the data that has already 
 * been sent to the driver will be played with the old speed. So we need to 
 * keep track of how much data that is. */
static off_t oldspeed_samples = 0;
static gfloat oldspeed_speed = 1.0;


/* Info after stopping */
static off_t realpos_atstop = 0;

/* Output buffer */
static gchar varispeed_buf[8192];
static gint varispeed_bufsize,varispeed_bufpos;
static gchar player_buf[4096],small_loop_buf[4096];
static gint player_bufsize,player_bufpos,small_loop_bufsize;


static int get_frames(void *buffer, int maxsize)
{
     off_t frames,sz;
     int read;
     if (curpos < loopstart) 
	  frames=loopstart-curpos;
     else if (curpos == loopstart && small_loop) {
	  memcpy(buffer, small_loop_buf, small_loop_bufsize);
	  return small_loop_bufsize;
     } else if (curpos < loopend)
	  frames = loopend-curpos;
     else if (!loop && curpos == loopend)
	  frames = 0;
     else
	  frames = ch->length - curpos;

     if (frames == 0) return 0;

     sz = frames * ch->format.samplebytes;
     if (sz > maxsize) sz = maxsize;
     read = chunk_read_array(ch,curpos,sz,buffer,dither_playback);
     g_assert(read <= sz);
     curpos += read / ch->format.samplebytes;
     if (loop && curpos == loopend) curpos = loopstart;
     return read;
}

static void get_new_data(void)
{

     if (varispeed_conv != NULL) {
	  if (varispeed_bufsize != 0) 
	       do {	       
		    if (varispeed_bufpos == varispeed_bufsize) {
			 varispeed_bufsize = get_frames(varispeed_buf,
							sizeof(varispeed_buf));
			 varispeed_bufpos = 0;
			 if (varispeed_bufsize == 0) {
			      rateconv_write(varispeed_conv,NULL,0);
			      return;
			 }
		    }
		    if (varispeed_bufpos < varispeed_bufsize)
			 varispeed_bufpos += 
			      rateconv_write(varispeed_conv,
					     varispeed_buf+varispeed_bufpos,
					     varispeed_bufsize-
					     varispeed_bufpos);
	       } while (varispeed_bufpos == varispeed_bufsize && 
			varispeed_bufsize > 0);
	  player_bufsize = rateconv_read(varispeed_conv,player_buf,
					 sizeof(player_buf));	  
     } else {
	  player_bufsize = get_frames(player_buf,sizeof(player_buf));
     }
     player_bufpos = 0;
}

/* Check if the sound driver wants more data and send it.
 * Returns FALSE if it had nothing to do. */

gboolean player_work(void)
{
     guint32 i;
     if (!ch || !output_want_data()) return FALSE;
     if (player_bufpos == player_bufsize) {
	  /* puts("Hey 1"); */
	  get_new_data();
	  /* Still no new data ? Then we should stop.. */
	  if (player_bufpos == player_bufsize) {
	       /* puts("Calling output_play(NULL,0)"); */
	       i = output_play(NULL,0);
	       /* puts("output_play done"); */
	       if (i > 0 || rateest_frames_played()<rateest_frames_written()) 
		    return FALSE;
	       /* puts("Calling output_stop"); */
	       output_stop(TRUE);
	       chunk_close(ch);
	       gtk_object_unref(GTK_OBJECT(ch));
	       ch=NULL;
	       return FALSE;
	  }
     }
     /* printf("Calling output_play with %d bytes\n",
	player_bufsize-player_bufpos); */
     i = output_play(player_buf+player_bufpos,player_bufsize-player_bufpos);
     player_bufpos += i;
     rateest_log_data(i/ch->format.samplebytes);
     return (i>0);
}


static void check_small_loop(void)
{
     off_t o;
     guint32 u,v;
     small_loop = FALSE;
     if (loop) {
	  o = (loopend-loopstart)*ch->format.samplebytes;
	  if (o <= sizeof(small_loop_buf)) {
	       u = (guint32) o;
	       small_loop = TRUE;
	       chunk_read_array(ch,loopstart,u,small_loop_buf,dither_playback);
	       for (v=1; v < sizeof(player_buf)/u; v++) 
		    memcpy(small_loop_buf+u*v,small_loop_buf,u); 
	       /* printf("check_small_loop: u=%d, v=%d\n",u,v); */
	       small_loop_bufsize = v*u;
	  }
     }
}

static void restart_converter(void)
{
     guint c,i;
     if (varispeed_conv != NULL)
	  rateconv_destroy(varispeed_conv);

     if (inifile_get_gboolean("varispeed",TRUE)) {
	  c = rateconv_driver_count(TRUE);
	  i = inifile_get_guint32("varispeedConv",c-1);
	  if (i >= c) i = c-1;
	  varispeed_conv = rateconv_new(TRUE,rateconv_driver_id(TRUE,i),
					&(ch->format),
					(gfloat)ch->format.samplerate / 
					true_speed, dither_playback);
     } else
	  varispeed_conv = NULL;
}

static gboolean player_play_main(Chunk *chk, off_t spos, off_t epos, 
				 gboolean lp, gboolean silent)
{
     gint i;
     gboolean b;
     Dataformat fmt;
     player_stop();
     if (spos == epos) return TRUE;
     true_speed = file_speed;
     memcpy(&fmt,&(chk->format),sizeof(Dataformat));
     i = output_select_format(&(chk->format),silent);
     if (i != 0) {	  
	  if (silent) {
	       if (!output_suggest_format(&(chk->format),&fmt))
		    memcpy(&fmt,&player_fallback_format,sizeof(Dataformat));
	       
	       b = !dataformat_samples_equal(&(chk->format),&fmt); 
	       if (b) {
		    chk = chunk_convert_sampletype(chk,&fmt);
		    b = player_play_main(chk,spos,epos,lp,TRUE);
		    gtk_object_sink(GTK_OBJECT(chk));
		    return b;

	       } else if (chk->format.samplerate != fmt.samplerate) {

		    if (!inifile_get_gboolean("varispeed",TRUE)) {
			 /* If we don't have varispeed, still play the sound
			  * with the wrong speed */

			 true_speed = 1.0;/* the only possible w/o varispeed */
			 file_speed = ((float)fmt.samplerate) / 
			      ((float)chk->format.samplerate);
		    } else {
			 true_speed = file_speed * 
			      ((float)chk->format.samplerate) /
			      ((float)fmt.samplerate);
		    }
		    /* Select the format with different sample rate */
		    i = output_select_format(&fmt,FALSE);
		    if (i < 0) 
			 return player_play_main(chk,spos,epos,lp,FALSE);
		    else if (i > 0)
			 return TRUE;
		    /* Fall out to other init stuff below */

	       } else {
		    return player_play_main(chk,spos,epos,lp,FALSE);	    
	       }
	  } else {
	       if (i < 0)
		    user_error(_("The sound format of this file is not "
			       "supported for playing."));
	       return TRUE;
	  }
     }
     
     /* printf("true_speed: %f\n",(float)true_speed); */

     ch = chunk_open(chk);
     gtk_object_ref(GTK_OBJECT(chk));
     rateest_init(fmt.samplerate);

     restart_converter();

     loopstart = spos;
     loopend = epos;
     loop = lp;
     player_bufsize = 1024; /* Anything >0 */
     player_bufpos = player_bufsize;
     varispeed_bufsize = 1024;
     varispeed_bufpos = varispeed_bufsize;
     check_small_loop();
     curpos = spos;
     realpos_offset = spos;
     oldspeed_samples = 0;
     oldspeed_speed = 1.0;
     player_work();
     return FALSE;
}

gboolean player_play(Chunk *chk, off_t spos, off_t epos, gboolean lp)
{
     return player_play_main(chk,spos,epos,lp,TRUE);
}

off_t player_get_buffer_pos(void)
{
     return curpos;
}

static off_t get_realpos_main(off_t frames_played)
{
     gdouble fp;     
     off_t o,x;
     if (frames_played < oldspeed_samples) {
	  fp = (gdouble)frames_played * oldspeed_speed;
     } else {
	  fp = (gdouble)oldspeed_samples*oldspeed_speed + 
	       (gdouble)(frames_played-oldspeed_samples) * true_speed;
     }
     o = (off_t)(fp) + realpos_offset;
     /* printf("fp = %f, realpos_offset = %d -> o = %d\n",fp,
	(int)realpos_offset,
	(int)o);  */
     if (loop && curpos < loopend && o >= loopend) {
	  /* printf("o f�re: %Ld",o); */
	  x = o-loopstart;
	  o = loopstart + x%(loopend-loopstart);
	  /* printf(", efter: %Ld\n",o); */
     }
     if (o >= ch->length) o = ch->length;
     return o;     
}

/* This function calculates at what sample the listener is.
 * Since for most drivers the amount of buffering is unknown, I use clock time
 * to find out where we should be. After some time has passed, we should be
 * able to estimate the buffer amount by comparing the "clock" position and
 * the buffer position. Also, we check for overruns by checking for 
 * unreasonable values */
off_t player_get_real_pos(void)
{
     off_t o;
     if (ch == NULL) return realpos_atstop;
     o = get_realpos_main(rateest_frames_played());
     /* printf("get_realpos_main returned: %Ld\n",o); */
     return o;
}

void player_get_range(off_t *startpos, off_t *endpos)
{
     *startpos = loopstart;
     *endpos = loopend;
}

void player_change_range(off_t start, off_t end)
{
     off_t rp;

     /* Special case - we're playing a loop and the sound has looped, but 
      * because of the output delay the cursor and the audible sound hasn't 
      * come there yet. In that case, we're forced to flush buffers etc 
      * to avoid sending out the old loop to the listener. */
     if (loop) {
	  rp = player_get_real_pos();
	  if (rp > curpos) {
	       /* Fixme: Should handle error return (although unlikely) */
	       player_play(ch,rp,end,TRUE);
	       loopstart = start;
	       return;
	  }
     }
     
     loopstart = start;
     loopend = end;
     check_small_loop();
     
}

void player_set_buffer_pos(off_t pos)
{
     if (pos != curpos) {
	  if (curpos == loopend && !loop && varispeed_conv != NULL)
	       restart_converter();
	  curpos = pos;
	  output_clear_buffers();
	  rateest_init(rateest_real_samplerate());
	  oldspeed_samples = 0;
	  realpos_offset = pos;
	  player_bufsize = player_bufpos = 1;
	  player_work();
     }
}

void player_switch(Chunk *chunk, off_t movestart, off_t movedist)
{
     ChunkHandle *c;
     off_t oldpos = curpos;
     off_t newpos, newstart, newend;
     Chunk *x;

     if (ch == NULL) return;

     g_assert(chunk->format.samplerate == ch->format.samplerate && 
	      chunk->format.channels == ch->format.channels);
     if (!dataformat_samples_equal(&(chunk->format),&(ch->format))) {
	  x = chunk_convert_sampletype(chunk, &(ch->format));
	  player_switch(x,movestart,movedist);
	  gtk_object_sink(GTK_OBJECT(x));
	  return;
     } 

     newpos = curpos;
     if (newpos >= movestart) {
	  newpos += movedist;
	  if (newpos < movestart) {
	       player_stop();
	       curpos = movestart;
	       return;
	  }
	  if (newpos > chunk->length) newpos = chunk->length;
     }
     
     newstart = loopstart;
     if (newstart >= movestart) {
	  newstart += movedist;
	  if (newstart < movestart) newstart = movestart;
	  if (newstart >= chunk->length) newstart = chunk->length;
     }
     newend = loopend;
     if (newend >= movestart) {
	  newend += movedist;
	  if (newend < movestart) newend = movestart;
	  if (newend >= chunk->length) newend = chunk->length;
     }
     
     c = chunk_open(chunk);
     if (c == NULL) return;
     chunk_close(ch);
     gtk_object_unref(GTK_OBJECT(ch));
     ch = c;
     gtk_object_ref(GTK_OBJECT(ch));

     loopstart = newstart;
     loopend = newend;
     curpos = newpos;
     if (newstart == newend) loop = FALSE;
     
     check_small_loop();

     realpos_offset += newpos - oldpos;
}

gboolean player_playing(void)
{
     return (ch != NULL);
}



void player_stop(void)
{
     gboolean b;
     if (ch == NULL) return;
     chunk_close(ch);
     b = output_stop(FALSE);
     realpos_atstop = get_realpos_main(b ? rateest_frames_written() : 
				       rateest_frames_played());
     gtk_object_unref(GTK_OBJECT(ch));
     ch = NULL;
}

gboolean player_looping(void)
{
     return loop;
}

void player_nudge(gfloat seconds)
{
     off_t samps,newpos;
     samps = seconds * ch->format.samplerate;
     newpos = player_get_real_pos() + samps;
     if (newpos < 0) newpos = 0;
     player_set_buffer_pos(newpos);
}

void player_set_speed(gfloat s)
{
     off_t wr,pl,rp;
     gfloat os;
     
     if (file_speed == s) return;
     
     if (ch == NULL || varispeed_conv==NULL) {
	  file_speed = true_speed = s;
	  return;
     }

     /* We must calculate these before changing the speed variable */
     wr = rateest_frames_written();
     pl = rateest_frames_played();
     rp = get_realpos_main(pl);

     os = true_speed;
     true_speed *= (s / file_speed);
     file_speed = s;

     if (varispeed_smooth_flag) {
	  /* Low latency version - just change the converter */
	  
	  rateconv_set_outrate(varispeed_conv,
			       (gfloat)ch->format.samplerate / true_speed);

	  /* Update oldspeed_speed and oldspeed_samples. */	  
	  if (pl < oldspeed_samples) {
	       /* This if case is just to avoid getting Inf in speed,
		* not strictly necessary */
	       if (wr != pl)
		    oldspeed_speed = (oldspeed_speed*
				      (gfloat)(oldspeed_samples-pl) + 
				      os*(gfloat)(wr-oldspeed_samples)) / 
		    (gfloat)(wr-pl);
	       oldspeed_samples = wr-pl;
	  } else {
	       oldspeed_speed = os;
	       oldspeed_samples = wr-pl;
	  }	
	  
	  rateest_init(rateest_real_samplerate());
	  rateest_prelog_data(wr-pl);
	  realpos_offset = rp;

	  /* printf("After rateest_init: rateest_frames_played=%Ld\n",
	     rateest_frames_played()); */
     } else {
	  /* High latency version - clears buffers, restarts converter */
	  output_clear_buffers();
	  restart_converter();     
	  curpos = rp;
	  rateest_init(rateest_real_samplerate());
	  realpos_offset = curpos;
	  oldspeed_samples = 0;
	  player_bufsize = player_bufpos = 1;
	  player_work();
     }
     
}

gfloat player_get_speed(void)
{
     return file_speed;
}
