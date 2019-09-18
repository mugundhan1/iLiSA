/*

gcc -Wall -O -o dump_udp_ow_11 dump_udp_ow_11.c -lpthread

or more pedantic:

gcc -Wall -std=c11 -pedantic -Werror -O -o dump_udp_ow_11 dump_udp_ow_11.c -lpthread

even more (too much for the current code):

gcc -Wall -ansi -pedantic -Werror -O -o dump_udp_ow_11 dump_udp_ow_11.c -lpthread





static (may not work):
gcc -static -Wall -O -o dump_udp_ow_11 dump_udp_ow_11.c -lpthread


maintained by Olaf Wucknitz <wucknitz@mpifr-bonn.mpg.de>


apologies for the chaotic structure, the code should be cleaned up
at some point






for tests on instantmix.mpifr-bonn.mpg.de:

dd if=/media/storage_1/wucknitz/TEMP/B1133+16_udp/B1133+16_band110_190_lanes4_sb12-499_lofarc4.16033.start.2018-11-28T06:00:31.000 bs=7824 | ~/astro_mpi/SOFT/MIRACULIX2/bin/throttle -w 1 -M 100 -s 7824 | ~/astro_mpi/SOFT/MIRACULIX2/bin/socat -b 7824 -u STDIN UDP-DATAGRAM:localhost:16011

./dump_udp_ow_11 --ports 16011 --out /media/storage_1/wucknitz/TEST/test --duration 1 --check



*/


/* not really sure about these features... */
#if 0
#define _XOPEN_SOURCE   600
#define _BSD_SOURCE
#else
#define  _GNU_SOURCE
#endif




#include <pthread.h> 

#include  <time.h>

#include  <sys/types.h>
#include  <sys/socket.h>
#include  <assert.h>
#include  <string.h>
#include  <stdio.h>
#include  <stdlib.h>
#include  <math.h>

#include  <signal.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include  <sys/stat.h>

#include  <sys/select.h>


#include  <getopt.h>

#include <sys/time.h>
#include <errno.h>


/* for dirname: */
#include  <libgen.h>

#include  <sys/mman.h>



#define  MAXNSOCK  12

/* this is only used for single packet buffers and thus a bit more
   generous */
#define  MMAXLEN  10000



/* additional debugging, e.g. for "stopped" ? */
#define  MYDEBUG  1




pthread_mutex_t region_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stopped_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t space_available = PTHREAD_COND_INITIALIZER;
pthread_cond_t data_available = PTHREAD_COND_INITIALIZER;
/* the last one is also used to stop recording! It is used to tell consumer()
to act one way or the other. */

int  producer_running= 0;


#if  MYDEBUG
pthread_mutex_t mydebug_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif



/* MAP_ANONYMOUS not defined at KAIRA, but MAP_ANON is */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif



/* virtual ring buffer adapted from this:
   https://en.wikipedia.org/w/index.php?title=Circular_buffer&oldid=588930355#Optimization
   virtual ringbuffer with two virtual copies so that all access
   can be done without wrapping
*/
struct vrb {
  long  fillsize,  /* bytes in buffer */
    front,  /* older end (take away data from here) */
    rear,  /* newer end (add new data here) */
    totsize;  /* total size of buffer */
  char  *buff;  /* address of first copy, allowed range is 2*totsize */
  char  *filename;  /* or NULL */
};



/* allocate and init */
void  init_vrb (struct vrb  *vrb, long  minsize)
{
  int  i, fd;
  static char  path1[]= "/dev/shm/dump_udp_ow_11_vrb-XXXXXX";
  static char  path2[]= "/tmp/dump_udp_ow_11_vrb-XXXXXX";
  char  *addr, *path;
  
  /* promote to the next full page size: */
  i= getpagesize ();
  vrb->totsize= (minsize+i-1)/i * i;
#if 0
  printf ("minsize %ld  totsize %ld  pages %ld\n", minsize, vrb->totsize,
	  vrb->totsize/i);
#endif
  /* set up the buffer */

  path= path1;
  fd= mkstemp (path);
  if (fd<0)  /* this failed for some reason: try /tmp */
    {
      path= path2;
      fd= mkstemp (path);
      if (fd<0)
	{
	  perror ("mkstemp() in init_vrb()");
	  exit (1);
	}
    }

#if 1
  i= unlink (path);
  if (i)
    {
      perror ("unlink() in init_vrb()");
      exit (1);
    }
#endif

  i= ftruncate (fd, vrb->totsize);
  if (i)
    {
      perror ("ftruncate() in init_vrb()");
      exit (1);
    }


  addr= mmap (NULL, 2*vrb->totsize,
	      PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (addr==MAP_FAILED)
    {
      perror ("first mmap() in init_vrb()");
      exit (1);
    }

  vrb->buff= mmap (addr, vrb->totsize, PROT_READ | PROT_WRITE,
		   MAP_FIXED | MAP_SHARED, fd, 0);
  if (vrb->buff!=addr)
    {
      perror ("second mmap() in init_vrb()");
      exit (1);
    }

  addr= mmap (vrb->buff + vrb->totsize, vrb->totsize, PROT_READ | PROT_WRITE,
		   MAP_FIXED | MAP_SHARED, fd, 0);
  /*  printf ("%p %p %p\n", addr, vrb->buff, vrb->buff + vrb->totsize); */
  if (addr != vrb->buff + vrb->totsize)
    {
      perror ("third mmap() in init_vrb()");
      exit (1);
    }

  i= close (fd);
  if (i)
    {
      perror ("close() buffer in init_vrb()");
      exit (1);
    }

  vrb->front= vrb->rear= vrb->fillsize= 0;


#if 0
  for (i= 0; i<255; i++)
    {
  vrb->buff[0]= i;
  printf ("%3d  %3d  %3d\n", i, vrb->buff[0], vrb->buff[vrb->totsize]);
}
#endif
}



void  free_vrb (struct vrb  *vrb)
{
  int  i;

  i= munmap (vrb->buff, 2*vrb->totsize);
  if (i)
    {
      perror ("munmap() buffer in free_vrb()");
      exit (1);
    }
}



char  *vrb_poi_new (struct vrb  *vrb, long  thissize)
{
  /*  fprintf (stdout, "vrb_poi_new %4ld  rear %8ld  front %8ld  fillsize %8ld\n", thissize, vrb->rear, vrb->front, vrb->fillsize); */
  if (vrb->fillsize + thissize > vrb->totsize)  /* not enough space */
    return NULL;
  else
    return vrb->buff + vrb->rear;
}


void  vrb_advance_new (struct vrb  *vrb, long  thissize)
{
  vrb->rear= (vrb->rear+thissize)%vrb->totsize;
  vrb->fillsize+= thissize;
  /*  fprintf (stdout, "vrb_advance_new %4ld  rear %8ld  front %8ld  fillsize %8ld\n", thissize, vrb->rear, vrb->front, vrb->fillsize); */
}


char  *vrb_poi_old (struct vrb  *vrb)
{
  /*  fprintf (stdout, "vrb_poi_old  rear %8ld  front %8ld  fillsize %8ld\n", vrb->rear, vrb->front, vrb->fillsize); */
  if (vrb->fillsize==0) /* nothing in buffer */
    return NULL;
  else
    return vrb->buff + vrb->front;
}


void  vrb_advance_old (struct vrb  *vrb, long  thissize)
{
  vrb->front= (vrb->front+thissize)%vrb->totsize;
  vrb->fillsize-= thissize;
  /*  fprintf (stdout, "vrb_advance_old %4ld  rear %8ld  front %8ld  fillsize %8ld\n", thissize, vrb->rear, vrb->front, vrb->fillsize); */
}



struct vrb  ringbuffer;



/*
  rear is only used in producer, front only in consumer
  size in both
  allbuffs also in both, but there are no conflicts

  region_mutex protects size and the buffer (?)
 */

int  maxsock;
int  sock[MAXNSOCK];

int  packlen;

int  do_blocklen= 0;
int  verbose= 0;

int  beamformed_check= 0;


/* recording stopped? 
   0: no, 1: for this file, 2: forever, -1: stop this file (split)
*/
int  stopped= 0;
/* is set by signal_handler and read by producer() and consumer() */


/*long bufsize; */
long maxsize, bytes_written_thisfile;

double  sum_filllevel;
long  n_filllevel;


/* write data in chunks so that the buffer is already partly free'd: */
long  maxwrite= 1024*1024;
/*long  maxwrite= 1024; */


FILE  *outf;
long  totlen, lasttotlen;
long  packs_seen[MAXNSOCK], packs_dropped[MAXNSOCK], bytes_written[MAXNSOCK],
  beamformed_good_packs[MAXNSOCK];
int  portnos[MAXNSOCK],nsock;
long  beamformed_first_packno[MAXNSOCK], beamformed_last_packno[MAXNSOCK];

long  last_packs_dropped[MAXNSOCK], last_packs_expected[MAXNSOCK], last_packs_seen[MAXNSOCK], last_good_packs[MAXNSOCK];



struct timespec  timeout;


fd_set  allsocks;

char  thisfilename[1000], filename[500], hostname[100];

char  *portlist;



int  compress;
char  *compcommand;


double  maxfilesize;
int  filenumber, stat_per_splitfile;


/* return timestamp for either timestamp as string or yyyy-mm-ddThh:mm:ss 
   not yet implemented:  ss.ssss
   <0 for error
*/
double  time_to_timestamp (char  *time)
{
  char  *p;
  double  res;
  struct tm  tm;

  if (strchr (time, 'T')==0)   /* then it is a timestamp already */
    {
      res= strtod (time, &p);
      if (p[0]!=0)   /* still stuff left after conversion */
	res= -1;
    }
  else
    {
      memset (&tm, 0, sizeof (tm));
      p= strptime (time, "%Y-%m-%dT%T", &tm);
      if (p==NULL || p[0]!=0)  /* not all converted */
	res= -1;
      else
	res= timegm (&tm);
    }

  return  res;
}



/* currently:  ss.sss */
void  timestamp_to_str (double  timestamp, char  *buff, int  len)
{
  int  j;
  time_t  t;
  struct tm  tm;
  
  assert (len>4);
  t= (time_t)timestamp;
  gmtime_r (&t, &tm);
  j= strftime (buff, len-4, "%FT%T", &tm);
  if (j==0)
    {
      fprintf (stderr,
	       "error in strftime() in timestamp_to_str with %e "
	       "for len=%d\n", timestamp, len);
      exit (1);
    }
  sprintf (buff+strlen (buff), ".%03d", (int)((timestamp-t)*1e3));
}


double  realtime ()
{
  struct timeval  tv;
  int  i;

  i= gettimeofday (&tv, NULL);
  if (i!=0)
    {
      perror ("gettimeofday() in realtime()");
      exit (1);
    }

  return tv.tv_sec+1e-6*tv.tv_usec;
}



void  final_statistics ()
{
  long  ntot;
  int  i;


  if (totlen==0)
    return;
  
  printf ("\ntotal per socket:  (with%s checks for beamformed data)\n",
	  beamformed_check?"":"out");
  for (i= 0; i<nsock; i++)
    {
      if (beamformed_check)
	{
	  ntot= beamformed_last_packno[i]-beamformed_first_packno[i]+1;
	  printf (  "port %5d :  expected packets %9ld\n"
		    "                missed packets %9ld   %10.6f %% of exp\n"
		    "                  seen packets %9ld   %10.6f %% of exp\n"
		    "                  good packets %9ld   %10.6f %% of seen\n"
		    "               dropped packets %9ld   %10.6f %% of seen\n"
		    "               written packets %9ld   %10.6f %% of seen\n"
		    "                                           "
		    "%10.6f %% of exp\n"
		    "                       volume    %7.3f GB\n",
		    portnos[i], ntot,
		    ntot-packs_seen[i], (ntot-packs_seen[i])*100./ntot,
		    packs_seen[i], packs_seen[i]*100./ntot,
		    beamformed_good_packs[i],
		    beamformed_good_packs[i]*100./packs_seen[i],
		    packs_dropped[i], packs_dropped[i]*100./packs_seen[i],
		    packs_seen[i]-packs_dropped[i],
		    (packs_seen[i]-packs_dropped[i])*100./packs_seen[i],
		    (packs_seen[i]-packs_dropped[i])*100./ntot,
		    bytes_written[i]/pow (1024,3));
	}
      else
	{
	  ntot= packs_seen[i];
	  printf (  "port %5d :  seen packets %9ld\n"
		    "           dropped packets %9ld   %10.6f %% of seen\n"
		    "           written packets %9ld   %10.6f %% of seen\n"
		    "                   volume    %7.3f GB\n",
		    portnos[i], ntot,
		    packs_dropped[i], packs_dropped[i]*100./ntot,
		    packs_seen[i]-packs_dropped[i],
		    (packs_seen[i]-packs_dropped[i])*100./ntot,
		    bytes_written[i]/pow (1024,3));
	}
    }

  printf ("\ntotal %7.3f GB  max buff %ld/%ld (%.1f %% full)  mean frac %.3e\n",
	  totlen/pow(1024,3), maxsize, ringbuffer.totsize, /* bufsize, */
	  maxsize/(double)ringbuffer.totsize*100.,
	  sum_filllevel/n_filllevel);

}



/* <=0 for no signal 
   -1 for timeout
   0 for regular
*/
void  signal_handler(int signum)
{
  int  i;


  if (signum>0)
    printf ("caught signal %d%s\n", signum,
	    signum==1 ? "  (HUP)" : signum==2 ? "  (INT)" :
	    signum==14 ? "  (ALRM)   end_time reached" : signum==15 ? "  (TERM)" : "");
  if (signum<0 && outf==NULL)  /* timeout, but no file open */
    {
      if (nsock==1 && portnos[0]==0)   /* then read stdin */
	{
	  printf ("no data on stdin\n");
	  pthread_mutex_lock(&stopped_mutex);
	  stopped= 2;
	  pthread_mutex_unlock(&stopped_mutex);
	  pthread_cond_signal(&data_available);  /* pretend that data is available */
	}
      return;
    }

  if (totlen)
    {
      printf ("total %7.3f GB  max buff %ld/%ld (%.1f %% full)  mean frac %.3e\n\n",
	      totlen/pow(1024,3), maxsize, ringbuffer.totsize, /* bufsize, */
	    maxsize/(double)ringbuffer.totsize*100.,
	    sum_filllevel/n_filllevel);
    }
  
  
  lasttotlen= totlen;
  
  
  for (i= 0; i<nsock; i++)
    {
      if (totlen)
	{
	if (beamformed_check)
	{
	    printf ("port %5d : %8ld exp  %10.6f %% missed  %10.6f %% dropped  "
		    "%7.3f GB\n",
		    portnos[i],
		    beamformed_last_packno[i]-beamformed_first_packno[i]+1,
		    100-packs_seen[i]*100./(
			beamformed_last_packno[i]-beamformed_first_packno[i]+1),
		    packs_dropped[i]*100./packs_seen[i],
		    bytes_written[i]/pow (1024,3));
	    printf ("                           %10.6f %% good\n",
		    beamformed_good_packs[i]*100./packs_seen[i]);

	    printf ("      block: %8ld exp  %10.6f %% missed  "
		    "%10.6f %% dropped\n",
		    beamformed_last_packno[i]-beamformed_first_packno[i]+1 -
		    last_packs_expected[i],
		    100-(packs_seen[i]-last_packs_seen[i])*100. /
		    (beamformed_last_packno[i]-beamformed_first_packno[i]+1
		     -last_packs_expected[i]),
		    (packs_dropped[i]-last_packs_dropped[i])*100./(
			packs_seen[i]-last_packs_seen[i]));
	    printf ("                           %10.6f %% good\n",
		    (beamformed_good_packs[i]-last_good_packs[i])*100./
		    (packs_seen[i]-last_packs_seen[i]));
	    
	    
	    last_packs_expected[i]= beamformed_last_packno[i]-
		beamformed_first_packno[i]+1;
	    last_good_packs[i]= beamformed_good_packs[i];
	}
	else
	{
	    printf ("port %5d : %8ld seen  %10.6f %% dropped  "
		    "%7.3f GB\n",
		    portnos[i], packs_seen[i], /* packs_dropped[i], */
		    packs_dropped[i]*100./packs_seen[i],
		    bytes_written[i]/pow (1024,3));
	    printf ("      block: %8ld seen  %10.6f %% dropped\n",
		    packs_seen[i]-last_packs_seen[i],
		    /*packs_dropped[i]-last_packs_dropped[i], */
		    (packs_dropped[i]-last_packs_dropped[i])*100./(
			packs_seen[i]-last_packs_seen[i]));
	}
	}
    last_packs_dropped[i]= packs_dropped[i];
    last_packs_seen[i]= packs_seen[i];
    }

  if (signum==SIGINT || signum==SIGTERM || signum==SIGALRM)
    {
      printf ("stopping\n");

      pthread_mutex_lock(&stopped_mutex);
      stopped= 2;
      pthread_mutex_unlock(&stopped_mutex);

      pthread_cond_signal(&data_available);  /* pretend that data is available */

    }
  else if (signum==-1 || signum==SIGHUP)
    {
      if (outf)  
        {
          if (signum<0)
	    {
	      if (nsock==1 && portnos[0]==0)   /* then read stdin */
		{
		  printf ("no more data on stdin\n");
		  pthread_mutex_lock(&stopped_mutex);
		  stopped= 2;
		  pthread_mutex_unlock(&stopped_mutex);
		}
	      else
		{
		  printf ("timeout\n");
		  if (stopped==0)  /* may already be 2 */
		    {
		      pthread_mutex_lock(&stopped_mutex);
		      stopped= 1;
		      pthread_mutex_unlock(&stopped_mutex);
		    }

		}
	    }
	  else  /* SIGHUP */
	    if (stopped==0)  /* may already be 2 */
		{
		  pthread_mutex_lock(&stopped_mutex);
		  stopped= 1;
		  pthread_mutex_unlock(&stopped_mutex);
		}
	  pthread_cond_signal(&data_available);/* pretend that data is available */

        }
    }


}


/* header_lofar of each packet: 
   now corrected!!  03.01.2019   source_int has 16 bit, not 8
*/
struct __attribute__((__packed__))  header_lofar
{
  uint8_t   version;
  union __attribute__ ((__packed__)) {
    struct __attribute__ ((__packed__)) {
      unsigned int  rsp_id   : 5;
      unsigned int  unused1  : 1;
      unsigned int  error    : 1;
      unsigned int  is200mhz : 1;
      unsigned int  bm       : 2;
      unsigned int  unused2  : 6;
    } source;
    /*    uint8_t  source_int; */
    uint16_t  source_int;
  };
  uint8_t   config;
  uint16_t  station;
  uint8_t   num_beamlets, num_slices;
  /*  uint32_t  timestamp, sequence; */
  int32_t  timestamp, sequence;

  /*  int8_t   data[BEAMLETS][SLICES][4];  // 4 : X/Y  R/I */
};



long  beamformed_packno (struct header_lofar  *header)
{
  return ((header->timestamp*1000000l*(160+40*header->source.is200mhz)+512)/1024+header->sequence)/16;
}


int  beamformed_checkpack (struct header_lofar  *header)
{
  return header->source.error==0 && header->timestamp!=-1;
}


void *producer ()
{
  char  buff[MMAXLEN+2];
  socklen_t  slen;
  struct sockaddr_in  addr_src;
  fd_set  myallsocks;
  int  i, thissize; 

  char  *newpoi;
  char  *buff2;




  producer_running= 1;
  
  if (do_blocklen)
    buff2= buff+2;  /* we need two bytes for size */
  else
      buff2= buff;


  slen= sizeof (addr_src);
  while (1)
    {


      if (totlen-lasttotlen>1e9)
	signal_handler (0);  /* regular printouts */

      thissize= -999;  /* prevent compiler warning on some systems */

      if (nsock==1 && portnos[0]==0)   /* then read stdin */
	{
	  if (stopped)
	    thissize= 0;
	  else
	    {
	      
	      /* wait till space available in buffer 

	      (for stdin we can wait with reading, don't drop packets)
	      */

	      pthread_mutex_lock(&region_mutex);
	      while ( vrb_poi_new (&ringbuffer, packlen) == NULL )
		/* not enough space */
		  pthread_cond_wait(&space_available,&region_mutex); 
	      pthread_mutex_unlock(&region_mutex);
	      /* printf ("after    available: %ld\n", ringbuffer.totsize-ringbuffer.fillsize); */
	      
	      
	      thissize= fread (buff2, 1, packlen, stdin);
	      if (ferror (stdin))
		perror ("reading from stdin in producer()");
	      
	      if (thissize==0)  /* treat as timeout */
		{
		  signal_handler (-1);
		  /*stopped= 2; */
		}
	    }
	  /* here we already have read the packet (or thissize==0) */
	}
      else  /* read from socket */
	{
	  if (stopped==2)
	    {
#if  MYDEBUG
	      pthread_mutex_lock (&mydebug_mutex);
	      printf ("MYDEBUG producer(), line %d  stopped==2: "
		      "closing sockets\n", __LINE__);
	      pthread_mutex_unlock (&mydebug_mutex);
#endif
	      /* close all sockets and return */
	      assert (nsock!=1 || portnos[0]!=0);  /* not reading from stdin */
	      for (i= 0; i<nsock; i++)
		{
		  int  j;
		  
		  j= close (sock[i]);
		  if (j)
		    {
		      perror ("closing socket");
		      producer_running= 0;
		      exit (1);
		    }
		}
	      /*return NULL;*/
#if  MYDEBUG
	      pthread_mutex_lock (&mydebug_mutex);
	      printf ("MYDEBUG producer(), line %d  calling pthread_exit()\n",
		      __LINE__);
	      pthread_mutex_unlock (&mydebug_mutex);
#endif
	      /*exit (0);*/
	      producer_running= 0;
	      pthread_exit (NULL);
	      
	    }
	  
	  
	  myallsocks= allsocks;  /* we have to reset this every call */
	  
	  i= pselect (maxsock+1, &myallsocks, NULL, NULL,
		      &timeout,
		      NULL);
	  if (i==-1)
	    {
	      perror ("pselect in producer()");
	      producer_running= 0;
	      exit (1);
	    }
	  if (i==0)  /* timeout */
	    {
	      signal_handler (-1);
	    }
	}
      
      for (i= 0; i<nsock; i++)
	{
	  if (nsock!=1 || portnos[0]!=0)  /* not reading from stdin */
	    {
	      if (FD_ISSET (sock[i], &myallsocks))
		{
		  thissize= recvfrom (sock[i], buff2, MMAXLEN-1, /* play safe */
				      0,
				      (struct sockaddr *)&addr_src,
				      &slen);
		  if (thissize==-1)
		    {
		      perror ("recvfrom() in producer()");
		      producer_running= 0;
		      exit (1);
		    }
		  if (thissize>=MMAXLEN)  /* this should not happen */
		    {
		      fprintf (stderr, "producer(): recvfrom() result %d >= %d"
			       " (should not happen)\n", thissize, MMAXLEN);
		      producer_running= 0;
		      exit (1);
		    }
		  
		}
	      else
		thissize= 0;
	    }
	  if (thissize)
	  {
	      if (stopped

		  ==2  /* discard only of we really want to stop */


		  )
		{
		  if (verbose)
		    printf ("discarding packet\n");
		}
	      else
	      {
		/* now we have a packet either from socket or from stdin */
		  if (packlen==0 || thissize==packlen)
		    
		    {

		      if (do_blocklen)/*add the blocklen if wanted (two bytes) */
			/* not tested */
			{
			    *(uint16_t*)buff= (uint16_t)thissize;
			    /*thissize+= 2;  see below */
			}

		      
		      if (do_blocklen)/* add the blocklen if wanted (two bytes) */
			thissize+= 2;
		      
		      
		      if (beamformed_check)
			{
			  beamformed_last_packno[i]= beamformed_packno (
					(struct header_lofar*)buff2);
			  if (beamformed_first_packno[i]==-1)
			    beamformed_first_packno[i]= 
			      beamformed_last_packno[i];
			  if (beamformed_checkpack ((struct 
						     header_lofar*)buff2))
			    beamformed_good_packs[i]++;
			}
		      
		      packs_seen[i]++;
		      
		      /* (no conflicts for rear and allbuffs) */

		      /* locking is probably not necessary here, but anyway */
		      pthread_mutex_lock(&region_mutex);
		      newpoi= vrb_poi_new (&ringbuffer, thissize);
		      sum_filllevel+= ringbuffer.fillsize
			        /(double)ringbuffer.totsize;
		      n_filllevel++;
		      pthread_mutex_unlock(&region_mutex);

		      
		      if (newpoi==NULL)   /* not enough space */
			/* this should not happen for read from stdin */
			{
			  /* simply drop this packet */
			  packs_dropped[i]++;
			}
		      else /* enough space */
			
			{
			  memcpy (newpoi, buff, thissize);

			  /* ... and then lock again here  */
			  pthread_mutex_lock(&region_mutex);

			  vrb_advance_new (&ringbuffer, thissize);

			  if (ringbuffer.fillsize>maxsize)
			      maxsize= ringbuffer.fillsize;

			  pthread_cond_signal(&data_available);
			  pthread_mutex_unlock(&region_mutex);
			  
			  totlen+= thissize;
			  
			  bytes_written[i]+= thissize;
			}
		      
		    }  /* packlen==0 || ... */
		  else
		    printf ("received %5d bytes, wrong length in sock %d, "
			    "should be %d\n", thissize, i, packlen);
	      }   /* not stopped */
	  }  /* thissize!=0 */
	}  /* for (i= 0; i<nsock .... */
    } /* while (1) */
}



void  init_thisfilestat ()
{
  int  j;

  for (j= 0; j<nsock; j++)
    {
      beamformed_first_packno[j]= -1;
      bytes_written[j]= packs_seen[j]= packs_dropped[j]= 
	beamformed_good_packs[j]= 0;

      last_packs_dropped[j]= last_packs_expected[j]= last_packs_seen[j]= 
	  last_good_packs[j]= 0;
    }

  lasttotlen= totlen= 0;
  maxsize= 0;
  sum_filllevel= 0;
  n_filllevel= 0;

}


void  start_file (double  timestamp)
{
  char  buff[1000];
  static double  timestamp_last= 0;

  if (timestamp)
    timestamp_last= timestamp;  /* we may need this for the next part */
  else
    timestamp= timestamp_last;  /* then we are re-using the last */
  
  if (compress)
    printf ("start compression pipe\n");
  else
    printf ("start file\n");
  timestamp_to_str (timestamp, buff, sizeof (buff));
  if (strcmp (filename, "/dev/null")==0)
    {
      strcpy (thisfilename, filename);
      printf ("\nopening %s\n", thisfilename);
    }
  else
    {
      if (filenumber>=0)
	/*	sprintf (thisfilename, "%s_%s.%s.%s.%s%s", filename, portlist, */
	/*hostname, comment, buff, compress ? ".zst" : ""); */
	{
	  sprintf (thisfilename, "%s_%s.%s.%s_%04d%s", filename, portlist,
		   hostname, buff, filenumber, compress ? ".zst" : "");
	  filenumber++;
	}
      else
	sprintf (thisfilename, "%s_%s.%s.%s%s", filename, portlist, hostname,
		 buff, compress ? ".zst" : "");
      printf ("\ncreating %s\n", thisfilename);
    }

  bytes_written_thisfile= 0;
  if (compress)
    {
      sprintf (buff, compcommand, thisfilename);
      outf= popen (buff, "w");
      if (outf==NULL)
	{
	  perror ("opening output compression pipe in start_file()");
	  exit (1);
	}
    }
  else
    {
      outf= fopen (thisfilename, "w");
      if (outf==NULL)
	{
	  perror ("opening output file in start_file()");
	  exit (1);
	}
    }
}


void *consumer ()
{
  long  i, thissize;
  char  *oldpoi;
  int  my_stopped, old_stopped;  /* to buffer stopped internally and avoid race conditions with signal_handler()  (and don't want to lock it for too long) */
  
  while (1)
    {

      pthread_mutex_lock(&region_mutex);
      /*      if (size == 0 && ! stopped)  // no data available */

      while ((oldpoi=vrb_poi_old (&ringbuffer)) == NULL
	     && ! stopped)  /* no data available */
	pthread_cond_wait(&data_available,&region_mutex); 
      pthread_mutex_unlock(&region_mutex);

      /* now we have oldpoi!=NULL or stopped 

	 global stopped may change in the process, simply take value NOW:
	 locking is probably not necessary here:
      */
      pthread_mutex_lock(&stopped_mutex);
      my_stopped= old_stopped= stopped;
      pthread_mutex_unlock(&stopped_mutex);



#if  MYDEBUG
      if (my_stopped)
	{
	  pthread_mutex_lock (&mydebug_mutex);
	  printf ("MYDEBUG consumer(), line %d  detected stopped==%d  "
		  "oldpoi=%p\n", __LINE__, my_stopped, oldpoi);
	  pthread_mutex_unlock (&mydebug_mutex);
	}
#endif

      
      /* also stop for filesize */
      if (my_stopped==0 && (maxfilesize>0 && bytes_written_thisfile>maxfilesize))
	my_stopped= -1;

      /* end file if wanted  */
      if (( (my_stopped==2 && oldpoi==NULL)|| /* we want to end and buffer is empty */
	    abs (my_stopped)==1)  /* or timeout or HUP or split file, then we do */
	  /* not need empty buffer */
	  && outf)  /* either this file or total:  close this file */
	{
	  int  j;
	      
#if  MYDEBUG
	  pthread_mutex_lock (&mydebug_mutex);
	  printf ("MYDEBUG consumer(), line %d  my_stopped==%d\n", __LINE__,
		  my_stopped);
	  pthread_mutex_unlock (&mydebug_mutex);
#endif


	  if (my_stopped!=-1 || stat_per_splitfile)
	    {
	      final_statistics ();
	      init_thisfilestat ();
	    }
	  printf ("closing %s%s\n", thisfilename,
		  my_stopped==-1 ? "  (split file)" : "" );
	  if (compress)
	    {
	      struct stat  stats;
	      long  len;

	      /* not entirely sure if pclose is flushing */
	      j= fflush (outf);
	      if (j)
		perror ("fflush() output pipe");
	      /* but don't exit */
	      
	      j= pclose (outf);
	      if (j!=0)
		{
		  perror ("closing output compression pipe in consumer()");
		  exit (1);
		}
	      i= stat (thisfilename, &stats);
	      if (i<0)
		{
		  perror ("checking filesize with stat() in consumer()");
		  len= 0;
		}
	      else
		len= stats.st_size;
	      printf ("compression: %ld -> %ld  reduced to %.3f %%\n",
		      /*totlen, */
		      bytes_written_thisfile,
		      len, len/(double)bytes_written_thisfile*100);
	    }
	  else
	    {
	      j= fclose (outf);
	      if (j!=0)
		{
		  perror ("closing file in consumer()");
		  exit (1);
		}
	    }
	  outf= NULL;
	  if (my_stopped==-1)
	    /* then we stopped to start a new split-file */
	    {
	      assert (filenumber>=0);  /* otherwise bug */
	      start_file (0); /* with same name, new number */
	    }
	}  /* close file */
      if (my_stopped==2 && oldpoi==NULL)   /* end program (even if no file) */
	{
#if  MYDEBUG
	  pthread_mutex_lock (&mydebug_mutex);
	  printf ("MYDEBUG consumer(), line %d  calling pthread_exit()\n",
		  __LINE__);
	  pthread_mutex_unlock (&mydebug_mutex);
#endif
	  /*exit (0);*/
	  pthread_exit (NULL);
	}
      

      pthread_mutex_lock(&stopped_mutex);
      if (stopped==old_stopped)
	{
	  if (stopped!=2)
	    {
#if  MYDEBUG
	      if (stopped)
		{
		  pthread_mutex_lock (&mydebug_mutex);
		  printf ("MYDEBUG consumer(), line %d  clearing stopped "
			  "flag\n", __LINE__);
		  pthread_mutex_unlock (&mydebug_mutex);
		}
#endif
	      stopped= 0;  /* then we can clear the stop flag */
	    }
	}
      else /* we got another signal in the meantime to change the stop status */
	{
	  /* then keep the new one, but write message to diagnose potential problems: */
	  fprintf (stderr, "stopped status changed from %d to %d while setting my_stopped to %d\n", old_stopped, stopped, my_stopped);
	}
      pthread_mutex_unlock(&stopped_mutex);

      /* my_stopped= 0;   this is not needed */
      if (oldpoi==NULL)  /* then no data available, wait for next */
	  continue;


      
      assert (oldpoi);
      
      pthread_mutex_lock(&region_mutex);
      thissize= ringbuffer.fillsize;
      assert (thissize);
      pthread_mutex_unlock(&region_mutex);
      
      if (outf==NULL)  /* no file open, open it now */
        /* producer can produce in parallel */
        {
	  if (filenumber>0)
	    filenumber= 0;  /* start with number 0 */
	  start_file (realtime ());
        }


      /* now write to disk */

      if (thissize>maxwrite)
	  thissize= maxwrite;

      if (packlen)  /* only write full packages (if length known) */
	thissize= (thissize/packlen)*packlen;

#if 0
      /* some delay for tests */
      {
	struct timespec  timespec;
	
	timespec.tv_sec= 0;
	timespec.tv_nsec= (long) (1e-3  *1e9);
	nanosleep (&timespec, NULL);
      }
#endif

      /*oldpoi= vrb_poi_old (&ringbuffer); */
      i= fwrite (oldpoi, 1, thissize, outf);
      if (i!=thissize)
	{
	  perror ("writing file in consumer()");
	  exit (1);
	}
      bytes_written_thisfile+= thissize;
      
      /* ... and then lock again here (for advance) */
      pthread_mutex_lock(&region_mutex);
      vrb_advance_old (&ringbuffer, thissize);
      pthread_cond_signal(&space_available);
      pthread_mutex_unlock(&region_mutex);
    }
}

 




int  main (int  argc, char  **argv)
{
  struct sockaddr_in  addr[MAXNSOCK];

  pthread_t producer_thread; 
  pthread_t consumer_thread;

  struct option long_options[] =
    {
      {"verbose",  no_argument, &verbose, 1},
      {"len",      required_argument, 0, 'l'},
      {"ports",    required_argument, 0, 'p'},
      {"out",      required_argument, 0, 'o'},
      {"help",     no_argument,       0, 'h'},
      {"Help",     no_argument,       0, 'H'},
      {"sizehead", no_argument,       0, 's'},
      {"timeout",  required_argument, 0, 't'},
      {"Start",    required_argument, 0, 'S'},
      {"End",      required_argument, 0, 'E'},
      {"duration", required_argument, 0, 'd'},
      {"Maxfilesize", required_argument, 0, 'M'},
      {"check",    no_argument,       0, 'c'},
      {"bufsize",  required_argument, 0, 'b'},
      {"maxwrite", required_argument, 0, 'm'},
      {"compress", no_argument,       0, 'z'},
      {"compcommand", required_argument, 0, 'Z'},
      {"path", required_argument, 0, 'P'},
      {0, 0, 0, 0}
    };
  char  *short_options= "hHvl:p:o:sb:m:t:S:E:d:M:czZ:P:", *cp, *cp2, *cp3, *cp4,
    stdportlist[]= "4346", *start_time= NULL, *end_time= NULL,
    *compcommand_std= "zstd -1 --zstd='strategy=0,wlog=13,hlog=7,slog=1,slen=7' -q -f -T2 -o %s";
  int  i, j, c, option_index= 0;
  double  start_timestamp= 0, end_timestamp= 0, duration= 0,  timeout_sec= 10.0;
  long bufsize;  


  
  maxfilesize= 0.;
  compress= 0;

  compcommand= compcommand_std;

  portlist= stdportlist;


  strcpy (filename, "udp");

  i= gethostname (hostname, sizeof (hostname));
  if (i!=0)
    {
      strcpy (hostname, "unknown");
      fprintf (stderr, "cannot determine hostname, using %s", hostname);
      perror ("gethostname");
    }

  outf= NULL;



  bufsize= 104857600;
  packlen= 0;  /* arbitrary */

  stat_per_splitfile= 1;
  
  while (1)
    {
      c = getopt_long (argc, argv, short_options,
		       long_options, &option_index);

      if (c==-1)  /* no more options */
        {
          /* check that there are no other arguments */
          if (argc>optind)
            {
              fprintf (stderr, "no other arguments allowed\n");
              c= '?'; /* error */
            }
          else
            break;
        }
      
      switch (c)
        {
	case 0:  /* for the flag */
            break;
          case 'v':
            verbose= 1;
            break;
          case 'l':
            if (sscanf (optarg, "%d", &packlen)!=1 ||
                packlen<=0 || packlen>=MMAXLEN)
              {
                fprintf (stderr, "problem with packet length\n");
                c= '?';
              }
	    if (beamformed_check && packlen!=7824)
	      {
		fprintf (stderr, "--check implies --len 7824, cannot use "
			 "other value\n");
		c= '?';
	      }
            break;
          case 'b':
	    {
	      double  bufsize_float;
	      
	      if (sscanf (optarg, "%lf", &bufsize_float)!=1 ||
		  bufsize_float<=1e4 || bufsize_float>16e9)
		{
		  fprintf (stderr,
			   "problem with bufsize\n");
		  c= '?';
		}
	      else
		bufsize= (long)bufsize_float;
	    }
            break;
          case 'm':
            if (sscanf (optarg, "%ld", &maxwrite)!=1 ||
                maxwrite<=1024)
              {
                fprintf (stderr,
                         "problem with maxwrite\n");
                c= '?';
              }
            break;
          case 't':
            if (sscanf (optarg, "%lf", &timeout_sec)!=1 ||
                timeout_sec<1e-3)
              {
                fprintf (stderr, "problem with timeout\n");
                c= '?';
              }
            break;
          case 'o':
            strncpy (filename, optarg, sizeof (filename)-1);
            filename[sizeof (filename)-1]= 0;
            break;
          case 'p':
            portlist= optarg;
            break;
          case 's':
            do_blocklen= 1;
            break;
          case 'S':
            start_time= optarg;
	    start_timestamp= time_to_timestamp (start_time);
	    if (start_timestamp<0)
              {
                fprintf (stderr, "problem with start time\n");
                c= '?';
              }
            break;
          case 'E':
            end_time= optarg;
	    end_timestamp= time_to_timestamp (end_time);
	    if (end_timestamp<0)
              {
                fprintf (stderr, "problem with end time\n");
                c= '?';
              }
	    if (duration)
	      {
		fprintf (stderr, "cannot use --End and --duration together\n");
		c= '?';
	      }
            break;
	  case 'd':
            if (sscanf (optarg, "%lf", &duration)!=1 ||
                duration<=0)
              {
                fprintf (stderr,
                         "problem with duration\n");
                c= '?';
              }
	    if (end_time)
	      {
		fprintf (stderr, "cannot use --End and --duration together\n");
		c= '?';
	      }
            break;
	  case 'M':
            if (sscanf (optarg, "%lf", &maxfilesize)!=1 ||
                maxfilesize==0)
              {
                fprintf (stderr,
                         "problem with Maxfilesize\n");
                c= '?';
              }
	    /* standard: stats per file, otherwise for all split-files comb */
	    stat_per_splitfile= maxfilesize>0;
	    maxfilesize= abs (maxfilesize);
	    break;
	  case 'c':
	    if (packlen && packlen!=7824)
	      {
		fprintf (stderr, "--check implies --len 7824, "
			 "cannot use other value\n");
		c= '?';
		break;
	      }
	    packlen= 7824;
	    beamformed_check= 1;
	    break;
	  case 'z':
	    compress= 1;
	    break;
	  case 'Z':
	    compcommand= optarg;
	    if (strstr (compcommand, "%s")==NULL)
	      {
		fprintf (stderr, "Compression command must include '%%s' for the filename.\n");
		c= '?';
	      }
	    break;
	case 'P':
	  if (setenv ("PATH", optarg, 1)!=0)
	    {
	      perror ("setenv");
	      exit (1);
	    }
	  break;
	default:  /* incl '?' */
            c= '?';
        }
      if (c=='?' || c=='h' || c=='H')  /* some error or help */
        {
          fprintf (stderr, "\n%s  options\n"
                   "    [--ports/-p  portlist]   current: %s\n"
                   "                             e.g.  31664,31665 or 31664x2\n"
		   "                             or 0 for stdin read\n"
                   "    [--out/-o filename]      current: %s\n"
                   "    [--verbose/-v] \n"
                   "    [--len/-l packet_len]    current: %d, 0=arbitrary\n"
                   "    [--sizehead/-s]          write packet lengths as headers\n"
		   "                             (not well tested)\n"
                   "    [--timeout/-t sec]       current: %f\n"
                   /*		   "    [--out/-o filename]\n" */
		   "    [--Start/-S time]        default: now\n"
		   "    [--End/-E time]          default: never\n"
		   "                             time: unix-timestamp or yyyy-mm-ddThh:mm:ss\n"
		   /* not yet implemented:  ss.ssss */
		   "    [--duration/-d sec]      default: infinity\n"
		   "                             (from start time or first packet)\n"
		   "    [--check/-c]             packet statistics for beamformed data\n"
		   "                             implies --len 7824\n"
		   "    [--compress/-z]          compress with zstd\n"
		   "    [--compcommand/-Z]       compression command, current: %s\n"
		   "    [--path/-P]              PATH to be used, e.g. compcommand\n"
		   "    [--Maxfilesize/-M float] split files to this maximum size\n"
		   "                             (bytes before compression), default: no limit\n"
		   "                             pos: stats per file, neg: stats combined\n"
                   "    [--bufsize/-b size]      current: %ld  (float will be converted)\n"
		   "    [--maxwrite/-m size]     max. write block, current: %ld\n"
                   "    [--help/-h]              brief help\n"
                   "    [--Help/-H]              extended help\n",
		   argv[0], portlist, filename, packlen,
		   timeout_sec, compcommand, bufsize, maxwrite);
	  if (c=='H')
	    fprintf (stderr,
"\nWe can work in different modes. If --Start is given, start at that time,\n"
"otherwise with first arriving packet. If --End is given, stop at that time.\n"
"If --duration is given, run for that long. This duration either starts at\n"
"--Start or with first packet. --timeout stops recording after that time\n"
"with no packets. If --Start used, timeout can also happen before first\n"
"packet, otherwise only once data have arrived. After timeout the programme\n"
"stops this recording but then waits for next packet and potentially starts\n"
"new file(s). After --duration or at --End, the programme stops.\n"
"We can listen to several ports, but all data will go to one file.\n"
"--ports 0 reads from stdin. It requires --len but cannot use --Start, --End\n"
"or --duration. End of file is treated as timeout.\n"
"Filename is built from --out parameter plus portlist plus\n"
"plus hostname '%s' plus 'start' or 'packet' (depending on whether we start at\n"
"certain time or with first packet) plus UTC timestamp.\n"
"Filename '/dev/null' (this exact spelling) is used directly.\n"
"Packets can be any length, unless --len is given, then only that length is\n"
"accepted (others discarded). For variable packet length we can write the\n"
"lengths as headers (--sizehead). The internal ring buffer size can be set\n"
"with --bufsize. --verbose produces more output.\n"
"Reading and writing have their own threads, data are written in maximum\n"
"blocks given by --maxwrite. (Should be << bufsize, because each block\n"
"is only released after complete write.)\n"
"With --check we compare the number of packets (received and written) with\n"
"the number expected from the packet numbers and determine a completeness.\n"
"With --compress we compress on the fly, using zstd (must be in PATH).\n"
"The compression command must include a %%s that will be replaced by the output filename.\n",
hostname
);

          exit (c=='?');
        }

    }  /* while (1) */



#if 0
  /* create directory if necessary */
  if (strcmp (filename, "/dev/null")!=0)  /* that is a special filename */
    {
      char  *dir;
      
      strcpy (thisfilename, filename);  /* because dirname may modify it */
      dir= dirname (thisfilename);
      printf ("dirname is '%s'\n", dir);
      if (strcmp (dir, ".")!=0)  /* current dir does exist */
	{
	  /* ist mkdir always in /bin/ ? */
	  execl ("/bin/mkdir", "/bin/mkdir", "-p", thisfilename, NULL)
	}
    }
#endif


  if (maxfilesize>0)
    filenumber= 0;  /* start with number 0 */
  else
    filenumber= -1;  /* means no number */
  
  timeout.tv_sec= timeout_sec;
  timeout.tv_nsec= (int)((timeout_sec-timeout.tv_sec)*1e9+0.5);

#if  MYDEBUG
  printf ("starting %s with MYDEBUG\n", __FILE__);
#else
  printf ("starting %s\n", __FILE__);
#endif

  if (verbose)
    {
      char  buff[100];

      printf ("packlen %d\n", packlen);
      printf ("filename %s\n", filename);
      printf ("portlist %s\n", portlist);
      printf ("timeout %.6f sec\n", timeout_sec);
      if (start_time)
	{
	  timestamp_to_str (start_timestamp, buff, sizeof (buff));
	  printf ("start time %.3f = %s\n", start_timestamp, buff);
	}
      if (end_time)
	{
	  timestamp_to_str (end_timestamp, buff, sizeof (buff));
	  printf ("end time   %.3f = %s\n", end_timestamp, buff);
	}
      if (duration)
	printf ("duration %.3f sec\n", duration);

      if (beamformed_check)
	printf ("check%s beamformed statistics\n",
		beamformed_check==2 ? "extended" : "");
    }

  init_vrb (&ringbuffer, bufsize);

  lasttotlen= 0;

  maxsize= 0;
  sum_filllevel= 0;
  n_filllevel= 0;



  cp2= NULL;  /* prevent warnings */

  nsock= 0;
  cp= strtok_r (portlist, ",", &cp2);
  while (cp!=NULL)
  {
      cp3= strchr (cp, 'x');
      if (cp3==0)
      {
	if (nsock>=MAXNSOCK)
	  {
	    fprintf (stderr,
		     "number of sockets too large (>%d, allowed max. %d)\n",
		     nsock, MAXNSOCK);
	    exit (1);
	  }
	assert (cp[0]!=0);
	i= strtol (cp, &cp4, 10);
	assert (cp4[0]==0);
	portnos[nsock]= i;
	nsock++;
      }
      else
      {
	  assert (cp[0]!=0);
	  i= strtol (cp, &cp4, 10);
	  assert (cp4[0]=='x');
	  assert (cp3[1]!=0);
	  j= strtol (cp3+1, &cp4, 10);
	  assert (cp4[0]==0);
	  while (j)
	  {
	    if (nsock>=MAXNSOCK)
	      {
		fprintf (stderr,
			 "number of sockets too large (>%d, allowed max. %d)\n",
			 nsock, MAXNSOCK);
		exit (1);
	      }
	    portnos[nsock]= i;
	    i++;
	    nsock++;
	    j--;
	  }
	      
      }
      cp= strtok_r (NULL, ",", &cp2);
  }



  if (verbose)
      for (i= 0; i<nsock; i++)
        printf ("port %d  %d\n", i, portnos[i]);

  if (nsock==1 && portnos[0]==0)   /* then read stdin */
    {
      if (packlen==0)
	{
	  fprintf (stderr, "Reading from stdin requires --len.\n");
	  exit (1);
	}
      if (start_timestamp || end_timestamp || duration)
	{
	  fprintf (stderr, "Reading from stdin is not compatible with "
		   "--Start, --End, --duration.\n");
	  exit (1);
	}
    }


  if (start_timestamp)
    {
      double  wait_time;
      struct timespec  timespec;

      /* filenumber is either 0 or -1 */
      start_file (start_timestamp);

      wait_time= start_timestamp-realtime ();

      printf ("waiting for %.3f sec...\n", wait_time);

      if (wait_time<0)
	{
	  printf ("negative wait, starting now!\n");
	  if (duration)
	    end_timestamp= realtime ()+duration;
	}
      else
	{
	  if (duration)
	    end_timestamp= start_timestamp+duration;
	  while (wait_time>0)
	    {
	      if (wait_time>=1)
		sleep ((unsigned int)wait_time);
	      else
		{
		  timespec.tv_sec= (time_t)wait_time;
		  timespec.tv_nsec= (long)((wait_time-timespec.tv_sec)*1e9+0.5);
		  nanosleep (&timespec, NULL);
		}
	      wait_time= start_timestamp-realtime ();
	    }
	  if (verbose)
	    printf ("remaining wait_time = %.6f sec\n", wait_time);
	}
      
    }
  else
    if (duration)
      end_timestamp= realtime ()+duration;

  if (end_timestamp)
    {
      double  wait_time;
      struct itimerval  itimer;

      wait_time= end_timestamp-realtime ();
      printf ("running for max %.3f sec...\n", wait_time);
      if (wait_time<0.1)
      {
	  printf ("time is%s negative, do not record at all\n",
		  wait_time>=0 ? " almost" : "");
	  exit (1);
      }
      else
	{
	  itimer.it_value.tv_sec= (long)wait_time;
	  itimer.it_value.tv_usec= (long)((wait_time-
					   itimer.it_value.tv_sec)*1e6+0.5);
	  itimer.it_interval.tv_sec= itimer.it_interval.tv_usec= 0;

	  j= setitimer (ITIMER_REAL, &itimer, NULL);
	  if (j!=0)
	    {
	      perror ("setitimer()");
	      exit (1);
	    }
	  signal (SIGALRM, signal_handler);
	}
    }



  maxsock= -1;
  FD_ZERO (&allsocks);

  if (nsock==1 && portnos[0]==0)   /* then read stdin */
    printf ("reading from stdin\n");
  else
    {
      /*printf ("listening to %s\n", portlist); */
      printf ("listening to ");
      for (i= 0; i<nsock; i++)
	printf ("%d%c", portnos[i], i==nsock-1 ? '\n' : ',');
      for (i= 0; i<nsock; i++)
	{
	  sock[i]= socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	  if (sock[i]==-1)
	    {
	      perror ("socket()");
	      exit (1);
	    }
	  /*      printf ("sock %d %d\n", i, sock[i]); */
	  if (sock[i]>maxsock)
	    maxsock= sock[i];
	  
	  memset(&addr[i], 0, sizeof(addr[i]));
	  addr[i].sin_family= AF_INET;
	  addr[i].sin_port= htons (portnos[i]);
	  addr[i].sin_addr.s_addr= htonl (INADDR_ANY);
	  
	  j= bind (sock[i], (struct sockaddr *)&addr[i], sizeof (addr[i]));
	  if (j==-1)
	    {
	      perror ("bind()");
	      exit (1);
	    }
	  
	  FD_SET (sock[i], &allsocks);
	}
    }



  init_thisfilestat ();


  signal (SIGINT, signal_handler);
  signal (SIGTERM, signal_handler);
  signal (SIGHUP, signal_handler);

  totlen= 0;

  j= pthread_create(&consumer_thread,NULL,consumer,NULL);
  if (j)
  {
      perror ("pthread_create for consumer");
      exit (1);
  }
  j= pthread_create(&producer_thread,NULL,producer,NULL);
  if (j)
  {
      perror ("pthread_create for producer");
      exit (1);
  }

  j= pthread_join(consumer_thread,NULL);
  if (j)
  {
      perror ("pthread_join");
      exit (1);
  }


  /* if producer is still running, give it some time, then cancel it */
  if (producer_running)
    {
      pthread_mutex_lock (&mydebug_mutex);
      printf ("MYDEBUG line %d  producer still running, give it one second\n",
	      __LINE__);
      pthread_mutex_unlock (&mydebug_mutex);
      sleep (1);
      if (producer_running)
	{
	  pthread_mutex_lock (&mydebug_mutex);
	  printf ("MYDEBUG line %d  ask producer to cancel\n", __LINE__);
	  pthread_mutex_unlock (&mydebug_mutex);
      
	  j= pthread_cancel (producer_thread);
	  if (j)
	    perror ("cancel producer");
	}
      else
	{
	  pthread_mutex_lock (&mydebug_mutex);
	  printf ("MYDEBUG line %d  producer exited in the meantime\n",
		  __LINE__);
	  pthread_mutex_unlock (&mydebug_mutex);
	}
    }
	  

  /* wait until it really has finished */
  j= pthread_join(producer_thread,NULL);
  if (j)
  {
      perror ("pthread_join");
      exit (1);
  }


  
  free_vrb (&ringbuffer);

#if  MYDEBUG
  printf ("regular exit of %s with MYDEBUG\n", __FILE__);
#else
  printf ("regular exit of %s\n", __FILE__);
#endif

  return 0;
}
