/* Copyright (C) 2000-2003 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <my_global.h>
#include <my_sys.h>
#include <mysys_err.h>
#include <m_string.h>
#include <m_ctype.h>
#include "mysql.h"
#include "mysql_version.h"
#include "mysqld_error.h"
#include "errmsg.h"
#include <violite.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <assert.h> /* for DBUG_ASSERT() */
#ifdef	 HAVE_PWD_H
#include <pwd.h>
#endif
#if !defined(MSDOS) && !defined(__WIN__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifdef HAVE_SELECT_H
#include <select.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#endif /* !defined(MSDOS) && !defined(__WIN__) */
#ifdef HAVE_POLL
#include <sys/poll.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#if defined(THREAD) && !defined(__WIN__)
#include <my_pthread.h>				/* because of signal()	*/
#endif
#ifndef INADDR_NONE
#define INADDR_NONE	-1
#endif

#include <sql_common.h>
#include "client_settings.h"

ulong 		net_buffer_length=8192;
ulong		max_allowed_packet= 1024L*1024L*1024L;
ulong		net_read_timeout=  CLIENT_NET_READ_TIMEOUT;
ulong		net_write_timeout= CLIENT_NET_WRITE_TIMEOUT;


#ifdef EMBEDDED_LIBRARY
#undef net_flush
my_bool	net_flush(NET *net);
#endif

#if defined(MSDOS) || defined(__WIN__)
/* socket_errno is defined in my_global.h for all platforms */
#define perror(A)
#else
#include <errno.h>
#define SOCKET_ERROR -1
#endif /* __WIN__ */

/*
  If allowed through some configuration, then this needs to
  be changed
*/
#define MAX_LONG_DATA_LENGTH 8192
#define unsigned_field(A) ((A)->flags & UNSIGNED_FLAG)

static void stmt_update_metadata(MYSQL_STMT *stmt, MYSQL_ROWS *data);
static void append_wild(char *to,char *end,const char *wild);
sig_handler pipe_sig_handler(int sig);

static my_bool mysql_client_init= 0;
static my_bool org_my_init_done= 0;


/*
  Initialize the MySQL client library

  SYNOPSIS
    mysql_server_init()

  NOTES
    Should be called before doing any other calls to the MySQL
    client library to initialize thread specific variables etc.
    It's called by mysql_init() to ensure that things will work for
    old not threaded applications that doesn't call mysql_server_init()
    directly.

  RETURN
    0  ok
    1  could not initialize environment (out of memory or thread keys)
*/

int STDCALL mysql_server_init(int argc __attribute__((unused)),
			      char **argv __attribute__((unused)),
			      char **groups __attribute__((unused)))
{
  int result= 0;
  if (!mysql_client_init)
  {
    mysql_client_init=1;
    org_my_init_done=my_init_done;
    if (my_init())				/* Will init threads */
      return 1;
    init_client_errs();
    if (!mysql_port)
    {
      mysql_port = MYSQL_PORT;
#ifndef MSDOS
      {
	struct servent *serv_ptr;
	char	*env;
	if ((serv_ptr = getservbyname("mysql", "tcp")))
	  mysql_port = (uint) ntohs((ushort) serv_ptr->s_port);
	if ((env = getenv("MYSQL_TCP_PORT")))
	  mysql_port =(uint) atoi(env);
      }
#endif
    }
    if (!mysql_unix_port)
    {
      char *env;
#ifdef __WIN__
      mysql_unix_port = (char*) MYSQL_NAMEDPIPE;
#else
      mysql_unix_port = (char*) MYSQL_UNIX_ADDR;
#endif
      if ((env = getenv("MYSQL_UNIX_PORT")))
	mysql_unix_port = env;
    }
    mysql_debug(NullS);
#if defined(SIGPIPE) && !defined(__WIN__) && !defined(__NETWARE__)
    (void) signal(SIGPIPE, SIG_IGN);
#endif
#ifdef EMBEDDED_LIBRARY
    result= init_embedded_server(argc, argv, groups);
#endif
  }
#ifdef THREAD
  else
    result= (int)my_thread_init();         /* Init if new thread */
#endif
  return result;
}


void STDCALL mysql_server_end()
{
#ifdef EMBEDDED_LIBRARY
  end_embedded_server();
#endif
  /* If library called my_init(), free memory allocated by it */
  if (!org_my_init_done)
  {
    my_end(0);
#ifndef THREAD
  /* Remove TRACING, if enabled by mysql_debug() */
    DBUG_POP();
#endif
  }
  else
    mysql_thread_end();
  mysql_client_init= org_my_init_done= 0;
}

static MYSQL_PARAMETERS mysql_internal_parameters=
{&max_allowed_packet, &net_buffer_length};

MYSQL_PARAMETERS *STDCALL mysql_get_parameters(void)
{
  return &mysql_internal_parameters;
}

my_bool STDCALL mysql_thread_init()
{
#ifdef THREAD
  return my_thread_init();
#else
  return 0;
#endif
}

void STDCALL mysql_thread_end()
{
#ifdef THREAD
  my_thread_end();
#endif
}

/*
  Let the user specify that we don't want SIGPIPE;  This doesn't however work
  with threaded applications as we can have multiple read in progress.
*/
static MYSQL* spawn_init(MYSQL* parent, const char* host,
			 unsigned int port,
			 const char* user,
			 const char* passwd);



/*
  Expand wildcard to a sql string
*/

static void
append_wild(char *to, char *end, const char *wild)
{
  end-=5;					/* Some extra */
  if (wild && wild[0])
  {
    to=strmov(to," like '");
    while (*wild && to < end)
    {
      if (*wild == '\\' || *wild == '\'')
	*to++='\\';
      *to++= *wild++;
    }
    if (*wild)					/* Too small buffer */
      *to++='%';				/* Nicer this way */
    to[0]='\'';
    to[1]=0;
  }
}


/**************************************************************************
  Init debugging if MYSQL_DEBUG environment variable is found
**************************************************************************/

void STDCALL
mysql_debug(const char *debug __attribute__((unused)))
{
#ifndef DBUG_OFF
  char	*env;
  if (_db_on_)
    return;					/* Already using debugging */
  if (debug)
  {
    DEBUGGER_ON;
    DBUG_PUSH(debug);
  }
  else if ((env = getenv("MYSQL_DEBUG")))
  {
    DEBUGGER_ON;
    DBUG_PUSH(env);
#if !defined(_WINVER) && !defined(WINVER)
    puts("\n-------------------------------------------------------");
    puts("MYSQL_DEBUG found. libmysql started with the following:");
    puts(env);
    puts("-------------------------------------------------------\n");
#else
    {
      char buff[80];
      buff[sizeof(buff)-1]= 0;
      strxnmov(buff,sizeof(buff)-1,"libmysql: ", env, NullS);
      MessageBox((HWND) 0,"Debugging variable MYSQL_DEBUG used",buff,MB_OK);
    }
#endif
  }
#endif
}


/**************************************************************************
  Close the server connection if we get a SIGPIPE
   ARGSUSED
**************************************************************************/

sig_handler
pipe_sig_handler(int sig __attribute__((unused)))
{
  DBUG_PRINT("info",("Hit by signal %d",sig));
#ifdef DONT_REMEMBER_SIGNAL
  (void) signal(SIGPIPE,pipe_sig_handler);
#endif
}

/* perform query on master */
my_bool STDCALL mysql_master_query(MYSQL *mysql, const char *q,
				   unsigned long length)
{
  DBUG_ENTER("mysql_master_query");
  if (mysql_master_send_query(mysql, q, length))
    DBUG_RETURN(1);
  DBUG_RETURN((*mysql->methods->read_query_result)(mysql));
}

my_bool STDCALL mysql_master_send_query(MYSQL *mysql, const char *q,
					unsigned long length)
{
  MYSQL *master = mysql->master;
  DBUG_ENTER("mysql_master_send_query");
  if (!master->net.vio && !mysql_real_connect(master,0,0,0,0,0,0,0))
    DBUG_RETURN(1);
  mysql->last_used_con = master;
  DBUG_RETURN(simple_command(master, COM_QUERY, q, length, 1));
}


/* perform query on slave */
my_bool STDCALL mysql_slave_query(MYSQL *mysql, const char *q,
				  unsigned long length)
{
  DBUG_ENTER("mysql_slave_query");
  if (mysql_slave_send_query(mysql, q, length))
    DBUG_RETURN(1);
  DBUG_RETURN((*mysql->methods->read_query_result)(mysql));
}


my_bool STDCALL mysql_slave_send_query(MYSQL *mysql, const char *q,
				   unsigned long length)
{
  MYSQL* last_used_slave, *slave_to_use = 0;
  DBUG_ENTER("mysql_slave_send_query");

  if ((last_used_slave = mysql->last_used_slave))
    slave_to_use = last_used_slave->next_slave;
  else
    slave_to_use = mysql->next_slave;
  /*
    Next_slave is always safe to use - we have a circular list of slaves
    if there are no slaves, mysql->next_slave == mysql
  */
  mysql->last_used_con = mysql->last_used_slave = slave_to_use;
  if (!slave_to_use->net.vio && !mysql_real_connect(slave_to_use, 0,0,0,
						    0,0,0,0))
    DBUG_RETURN(1);
  DBUG_RETURN(simple_command(slave_to_use, COM_QUERY, q, length, 1));
}


/* enable/disable parsing of all queries to decide
   if they go on master or slave */
void STDCALL mysql_enable_rpl_parse(MYSQL* mysql)
{
  mysql->options.rpl_parse = 1;
}

void STDCALL mysql_disable_rpl_parse(MYSQL* mysql)
{
  mysql->options.rpl_parse = 0;
}

/* get the value of the parse flag */
int STDCALL mysql_rpl_parse_enabled(MYSQL* mysql)
{
  return mysql->options.rpl_parse;
}

/*  enable/disable reads from master */
void STDCALL mysql_enable_reads_from_master(MYSQL* mysql)
{
  mysql->options.no_master_reads = 0;
}

void STDCALL mysql_disable_reads_from_master(MYSQL* mysql)
{
  mysql->options.no_master_reads = 1;
}

/* get the value of the master read flag */
my_bool STDCALL mysql_reads_from_master_enabled(MYSQL* mysql)
{
  return !(mysql->options.no_master_reads);
}


/*
  We may get an error while doing replication internals.
  In this case, we add a special explanation to the original
  error
*/

static void expand_error(MYSQL* mysql, int error)
{
  char tmp[MYSQL_ERRMSG_SIZE];
  char *p;
  uint err_length;
  strmake(tmp, mysql->net.last_error, MYSQL_ERRMSG_SIZE-1);
  p = strmake(mysql->net.last_error, ER(error), MYSQL_ERRMSG_SIZE-1);
  err_length= (uint) (p - mysql->net.last_error);
  strmake(p, tmp, MYSQL_ERRMSG_SIZE-1 - err_length);
  mysql->net.last_errno = error;
}

/*
  This function assumes we have just called SHOW SLAVE STATUS and have
  read the given result and row
*/

static my_bool get_master(MYSQL* mysql, MYSQL_RES* res, MYSQL_ROW row)
{
  MYSQL* master;
  DBUG_ENTER("get_master");
  if (mysql_num_fields(res) < 3)
    DBUG_RETURN(1); /* safety */

  /* use the same username and password as the original connection */
  if (!(master = spawn_init(mysql, row[0], atoi(row[2]), 0, 0)))
    DBUG_RETURN(1);
  mysql->master = master;
  DBUG_RETURN(0);
}


/*
  Assuming we already know that mysql points to a master connection,
  retrieve all the slaves
*/

static my_bool get_slaves_from_master(MYSQL* mysql)
{
  MYSQL_RES* res = 0;
  MYSQL_ROW row;
  my_bool error = 1;
  int has_auth_info;
  int port_ind;
  DBUG_ENTER("get_slaves_from_master");

  if (!mysql->net.vio && !mysql_real_connect(mysql,0,0,0,0,0,0,0))
  {
    expand_error(mysql, CR_PROBE_MASTER_CONNECT);
    DBUG_RETURN(1);
  }

  if (mysql_query(mysql, "SHOW SLAVE HOSTS") ||
      !(res = mysql_store_result(mysql)))
  {
    expand_error(mysql, CR_PROBE_SLAVE_HOSTS);
    DBUG_RETURN(1);
  }

  switch (mysql_num_fields(res)) {
  case 5:
    has_auth_info = 0;
    port_ind=2;
    break;
  case 7:
    has_auth_info = 1;
    port_ind=4;
    break;
  default:
    goto err;
  }

  while ((row = mysql_fetch_row(res)))
  {
    MYSQL* slave;
    const char* tmp_user, *tmp_pass;

    if (has_auth_info)
    {
      tmp_user = row[2];
      tmp_pass = row[3];
    }
    else
    {
      tmp_user = mysql->user;
      tmp_pass = mysql->passwd;
    }

    if (!(slave = spawn_init(mysql, row[1], atoi(row[port_ind]),
			     tmp_user, tmp_pass)))
      goto err;

    /* Now add slave into the circular linked list */
    slave->next_slave = mysql->next_slave;
    mysql->next_slave = slave;
  }
  error = 0;
err:
  if (res)
    mysql_free_result(res);
  DBUG_RETURN(error);
}


my_bool STDCALL mysql_rpl_probe(MYSQL* mysql)
{
  MYSQL_RES *res= 0;
  MYSQL_ROW row;
  my_bool error= 1;
  DBUG_ENTER("mysql_rpl_probe");

  /*
    First determine the replication role of the server we connected to
    the most reliable way to do this is to run SHOW SLAVE STATUS and see
    if we have a non-empty master host. This is still not fool-proof -
    it is not a sin to have a master that has a dormant slave thread with
    a non-empty master host. However, it is more reliable to check
    for empty master than whether the slave thread is actually running
  */
  if (mysql_query(mysql, "SHOW SLAVE STATUS") ||
      !(res = mysql_store_result(mysql)))
  {
    expand_error(mysql, CR_PROBE_SLAVE_STATUS);
    DBUG_RETURN(1);
  }

  row= mysql_fetch_row(res);
  /*
    Check master host for emptiness/NULL
    For MySQL 4.0 it's enough to check for row[0]
  */
  if (row && row[0] && *(row[0]))
  {
    /* this is a slave, ask it for the master */
    if (get_master(mysql, res, row) || get_slaves_from_master(mysql))
      goto err;
  }
  else
  {
    mysql->master = mysql;
    if (get_slaves_from_master(mysql))
      goto err;
  }

  error = 0;
err:
  if (res)
    mysql_free_result(res);
  DBUG_RETURN(error);
}


/*
  Make a not so fool-proof decision on where the query should go, to
  the master or the slave. Ideally the user should always make this
  decision himself with mysql_master_query() or mysql_slave_query().
  However, to be able to more easily port the old code, we support the
  option of an educated guess - this should work for most applications,
  however, it may make the wrong decision in some particular cases. If
  that happens, the user would have to change the code to call
  mysql_master_query() or mysql_slave_query() explicitly in the place
  where we have made the wrong decision
*/

enum mysql_rpl_type
STDCALL mysql_rpl_query_type(const char* q, int len)
{
  const char *q_end= q + len;
  for (; q < q_end; ++q)
  {
    char c;
    if (my_isalpha(&my_charset_latin1, (c= *q)))
    {
      switch (my_tolower(&my_charset_latin1,c)) {
      case 'i':  /* insert */
      case 'u':  /* update or unlock tables */
      case 'l':  /* lock tables or load data infile */
      case 'd':  /* drop or delete */
      case 'a':  /* alter */
	return MYSQL_RPL_MASTER;
      case 'c':  /* create or check */
	return my_tolower(&my_charset_latin1,q[1]) == 'h' ? MYSQL_RPL_ADMIN :
	  MYSQL_RPL_MASTER;
      case 's': /* select or show */
	return my_tolower(&my_charset_latin1,q[1]) == 'h' ? MYSQL_RPL_ADMIN :
	  MYSQL_RPL_SLAVE;
      case 'f': /* flush */
      case 'r': /* repair */
      case 'g': /* grant */
	return MYSQL_RPL_ADMIN;
      default:
	return MYSQL_RPL_SLAVE;
      }
    }
  }
  return MYSQL_RPL_MASTER;		/* By default, send to master */
}


/**************************************************************************
  Connect to sql server
  If host == 0 then use localhost
**************************************************************************/

#ifdef USE_OLD_FUNCTIONS
MYSQL * STDCALL
mysql_connect(MYSQL *mysql,const char *host,
	      const char *user, const char *passwd)
{
  MYSQL *res;
  mysql=mysql_init(mysql);			/* Make it thread safe */
  {
    DBUG_ENTER("mysql_connect");
    if (!(res=mysql_real_connect(mysql,host,user,passwd,NullS,0,NullS,0)))
    {
      if (mysql->free_me)
	my_free((gptr) mysql,MYF(0));
    }
    DBUG_RETURN(res);
  }
}
#endif


#ifdef CHECK_LICENSE
/*
  Check server side variable 'license'.
  If the variable does not exist or does not contain 'Commercial', 
  we're talking to non-commercial server from commercial client.
  SYNOPSIS
    check_license()
  RETURN VALUE
    0  success
   !0  network error or the server is not commercial.
       Error code is saved in mysql->net.last_errno.
*/

static int check_license(MYSQL *mysql)
{
  MYSQL_ROW row;
  MYSQL_RES *res;
  NET *net= &mysql->net;
  static const char query[]= "SELECT @@license";
  static const char required_license[]= STRINGIFY_ARG(LICENSE);

  if (mysql_real_query(mysql, query, sizeof(query)-1))
  {
    if (net->last_errno == ER_UNKNOWN_SYSTEM_VARIABLE)
    {
      net->last_errno= CR_WRONG_LICENSE;
      sprintf(net->last_error, ER(net->last_errno), required_license);
    }
    return 1;
  }
  if (!(res= mysql_use_result(mysql)))
    return 1;
  row= mysql_fetch_row(res);
  /* 
    If no rows in result set, or column value is NULL (none of these
    two is ever true for server variables now), or column value
    mismatch, set wrong license error.
  */
  if (!net->last_errno &&
      (!row || !row[0] ||
       strncmp(row[0], required_license, sizeof(required_license))))
  {
    net->last_errno= CR_WRONG_LICENSE;
    sprintf(net->last_error, ER(net->last_errno), required_license);
  }
  mysql_free_result(res);
  return net->last_errno;
}
#endif /* CHECK_LICENSE */


/**************************************************************************
  Change user and database
**************************************************************************/
int cli_read_change_user_result(MYSQL *mysql, char *buff, const char *passwd)
{
  NET *net= &mysql->net;
  ulong pkt_length;

  pkt_length= net_safe_read(mysql);
  
  if (pkt_length == packet_error)
    return 1;

  if (pkt_length == 1 && net->read_pos[0] == 254 &&
      mysql->server_capabilities & CLIENT_SECURE_CONNECTION)
  {
    /*
      By sending this very specific reply server asks us to send scrambled
      password in old format. The reply contains scramble_323.
    */
    scramble_323(buff, mysql->scramble, passwd);
    if (my_net_write(net, buff, SCRAMBLE_LENGTH_323 + 1) || net_flush(net))
    {
      net->last_errno= CR_SERVER_LOST;
      strmov(net->sqlstate, unknown_sqlstate);
      strmov(net->last_error,ER(net->last_errno));
      return 1;
    }
    /* Read what server thinks about out new auth message report */
    if (net_safe_read(mysql) == packet_error)
      return 1;
  }
  return 0;
}


my_bool	STDCALL mysql_change_user(MYSQL *mysql, const char *user,
				  const char *passwd, const char *db)
{
  char buff[512],*end=buff;
  DBUG_ENTER("mysql_change_user");

  if (!user)
    user="";
  if (!passwd)
    passwd="";

  /* Store user into the buffer */
  end=strmov(end,user)+1;

  /* write scrambled password according to server capabilities */
  if (passwd[0])
  {
    if (mysql->server_capabilities & CLIENT_SECURE_CONNECTION)
    {
      *end++= SCRAMBLE_LENGTH;
      scramble(end, mysql->scramble, passwd);
      end+= SCRAMBLE_LENGTH;
    }
    else
    {
      scramble_323(end, mysql->scramble, passwd);
      end+= SCRAMBLE_LENGTH_323 + 1;
    }
  }
  else
    *end++= '\0';                               /* empty password */
  /* Add database if needed */
  end= strmov(end, db ? db : "") + 1;

  /* Write authentication package */
  simple_command(mysql,COM_CHANGE_USER, buff,(ulong) (end-buff),1);

  if ((*mysql->methods->read_change_user_result)(mysql, buff, passwd))
    DBUG_RETURN(1);
  /* Free old connect information */
  my_free(mysql->user,MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->passwd,MYF(MY_ALLOW_ZERO_PTR));
  my_free(mysql->db,MYF(MY_ALLOW_ZERO_PTR));

  /* alloc new connect information */
  mysql->user=  my_strdup(user,MYF(MY_WME));
  mysql->passwd=my_strdup(passwd,MYF(MY_WME));
  mysql->db=    db ? my_strdup(db,MYF(MY_WME)) : 0;
  DBUG_RETURN(0);
}

#if defined(HAVE_GETPWUID) && defined(NO_GETPWUID_DECL)
struct passwd *getpwuid(uid_t);
char* getlogin(void);
#endif

#if defined(__NETWARE__)
/* Default to value of USER on NetWare, if unset use "UNKNOWN_USER" */
void read_user_name(char *name)
{
  char *str=getenv("USER");
  strmake(name, str ? str : "UNKNOWN_USER", USERNAME_LENGTH);
}

#elif !defined(MSDOS) && ! defined(VMS) && !defined(__WIN__) && !defined(OS2)

void read_user_name(char *name)
{
  DBUG_ENTER("read_user_name");
  if (geteuid() == 0)
    (void) strmov(name,"root");		/* allow use of surun */
  else
  {
#ifdef HAVE_GETPWUID
    struct passwd *skr;
    const char *str;
    if ((str=getlogin()) == NULL)
    {
      if ((skr=getpwuid(geteuid())) != NULL)
	str=skr->pw_name;
      else if (!(str=getenv("USER")) && !(str=getenv("LOGNAME")) &&
	       !(str=getenv("LOGIN")))
	str="UNKNOWN_USER";
    }
    (void) strmake(name,str,USERNAME_LENGTH);
#elif HAVE_CUSERID
    (void) cuserid(name);
#else
    strmov(name,"UNKNOWN_USER");
#endif
  }
  DBUG_VOID_RETURN;
}

#else /* If MSDOS || VMS */

void read_user_name(char *name)
{
  char *str=getenv("USER");		/* ODBC will send user variable */
  strmake(name,str ? str : "ODBC", USERNAME_LENGTH);
}

#endif

my_bool handle_local_infile(MYSQL *mysql, const char *net_filename)
{
  my_bool result= 1;
  uint packet_length=MY_ALIGN(mysql->net.max_packet-16,IO_SIZE);
  NET *net= &mysql->net;
  int readcount;
  void *li_ptr;          /* pass state to local_infile functions */
  char *buf;		/* buffer to be filled by local_infile_read */
  struct st_mysql_options *options= &mysql->options;
  DBUG_ENTER("handle_local_infile");

  /* check that we've got valid callback functions */
  if (!(options->local_infile_init &&
	options->local_infile_read &&
	options->local_infile_end &&
	options->local_infile_error))
  {
    /* if any of the functions is invalid, set the default */
    mysql_set_local_infile_default(mysql);
  }

  /* copy filename into local memory and allocate read buffer */
  if (!(buf=my_malloc(packet_length, MYF(0))))
  {
    strmov(net->sqlstate, unknown_sqlstate);
    strmov(net->last_error, ER(net->last_errno=CR_OUT_OF_MEMORY));
    DBUG_RETURN(1);
  }

  /* initialize local infile (open file, usually) */
  if ((*options->local_infile_init)(&li_ptr, net_filename,
    options->local_infile_userdata))
  {
    my_net_write(net,"",0);		/* Server needs one packet */
    net_flush(net);
    strmov(net->sqlstate, unknown_sqlstate);
    net->last_errno= (*options->local_infile_error)(li_ptr,
						    net->last_error,
						    sizeof(net->last_error)-1);
    goto err;
  }

  /* read blocks of data from local infile callback */
  while ((readcount =
	  (*options->local_infile_read)(li_ptr, buf,
					packet_length)) > 0)
  {
    if (my_net_write(net,buf,readcount))
    {
      DBUG_PRINT("error",
		 ("Lost connection to MySQL server during LOAD DATA of local file"));
      strmov(net->sqlstate, unknown_sqlstate);
      net->last_errno=CR_SERVER_LOST;
      strmov(net->last_error,ER(net->last_errno));
      goto err;
    }
  }

  /* Send empty packet to mark end of file */
  if (my_net_write(net,"",0) || net_flush(net))
  {
    strmov(net->sqlstate, unknown_sqlstate);
    net->last_errno=CR_SERVER_LOST;
    sprintf(net->last_error,ER(net->last_errno),errno);
    goto err;
  }

  if (readcount < 0)
  {
    net->last_errno= (*options->local_infile_error)(li_ptr,
						    net->last_error,
						    sizeof(net->last_error)-1);
    goto err;
  }

  result=0;					/* Ok */

err:
  /* free up memory allocated with _init, usually */
  (*options->local_infile_end)(li_ptr);
  DBUG_RETURN(result);
}


/****************************************************************************
  Default handlers for LOAD LOCAL INFILE
****************************************************************************/

typedef struct st_default_local_infile
{
  int fd;
  int error_num;
  const char *filename;
  char error_msg[LOCAL_INFILE_ERROR_LEN];
} default_local_infile_data;


/*
  Open file for LOAD LOCAL INFILE

  SYNOPSIS
    default_local_infile_init()
    ptr			Store pointer to internal data here
    filename		File name to open. This may be in unix format !


  NOTES
    Even if this function returns an error, the load data interface
    guarantees that default_local_infile_end() is called.

  RETURN
    0	ok
    1	error
*/

static int default_local_infile_init(void **ptr, const char *filename,
             void *userdata __attribute__ ((unused)))
{
  default_local_infile_data *data;
  char tmp_name[FN_REFLEN];

  if (!(*ptr= data= ((default_local_infile_data *)
		     my_malloc(sizeof(default_local_infile_data),  MYF(0)))))
    return 1; /* out of memory */

  data->error_msg[0]= 0;
  data->error_num=    0;
  data->filename= filename;

  fn_format(tmp_name, filename, "", "", MY_UNPACK_FILENAME);
  if ((data->fd = my_open(tmp_name, O_RDONLY, MYF(0))) < 0)
  {
    data->error_num= my_errno;
    my_snprintf(data->error_msg, sizeof(data->error_msg)-1,
                EE(EE_FILENOTFOUND), tmp_name, data->error_num);
    return 1;
  }
  return 0; /* ok */
}


/*
  Read data for LOAD LOCAL INFILE

  SYNOPSIS
    default_local_infile_read()
    ptr			Points to handle allocated by _init
    buf			Read data here
    buf_len		Ammount of data to read

  RETURN
    > 0		number of bytes read
    == 0	End of data
    < 0		Error
*/

static int default_local_infile_read(void *ptr, char *buf, uint buf_len)
{
  int count;
  default_local_infile_data*data = (default_local_infile_data *) ptr;

  if ((count= (int) my_read(data->fd, (byte *) buf, buf_len, MYF(0))) < 0)
  {
    data->error_num= EE_READ; /* the errmsg for not entire file read */
    my_snprintf(data->error_msg, sizeof(data->error_msg)-1,
		EE(EE_READ),
		data->filename, my_errno);
  }
  return count;
}


/*
  Read data for LOAD LOCAL INFILE

  SYNOPSIS
    default_local_infile_end()
    ptr			Points to handle allocated by _init
			May be NULL if _init failed!

  RETURN
*/

static void default_local_infile_end(void *ptr)
{
  default_local_infile_data *data= (default_local_infile_data *) ptr;
  if (data)					/* If not error on open */
  {
    if (data->fd >= 0)
      my_close(data->fd, MYF(MY_WME));
    my_free(ptr, MYF(MY_WME));
  }
}


/*
  Return error from LOAD LOCAL INFILE

  SYNOPSIS
    default_local_infile_end()
    ptr			Points to handle allocated by _init
			May be NULL if _init failed!
    error_msg		Store error text here
    error_msg_len	Max lenght of error_msg

  RETURN
    error message number
*/

static int
default_local_infile_error(void *ptr, char *error_msg, uint error_msg_len)
{
  default_local_infile_data *data = (default_local_infile_data *) ptr;
  if (data)					/* If not error on open */
  {
    strmake(error_msg, data->error_msg, error_msg_len);
    return data->error_num;
  }
  /* This can only happen if we got error on malloc of handle */
  strmov(error_msg, ER(CR_OUT_OF_MEMORY));
  return CR_OUT_OF_MEMORY;
}


void
mysql_set_local_infile_handler(MYSQL *mysql,
                               int (*local_infile_init)(void **, const char *,
                               void *),
                               int (*local_infile_read)(void *, char *, uint),
                               void (*local_infile_end)(void *),
                               int (*local_infile_error)(void *, char *, uint),
                               void *userdata)
{
  mysql->options.local_infile_init=  local_infile_init;
  mysql->options.local_infile_read=  local_infile_read;
  mysql->options.local_infile_end=   local_infile_end;
  mysql->options.local_infile_error= local_infile_error;
  mysql->options.local_infile_userdata = userdata;
}


void mysql_set_local_infile_default(MYSQL *mysql)
{
  mysql->options.local_infile_init=  default_local_infile_init;
  mysql->options.local_infile_read=  default_local_infile_read;
  mysql->options.local_infile_end=   default_local_infile_end;
  mysql->options.local_infile_error= default_local_infile_error;
}


/**************************************************************************
  Do a query. If query returned rows, free old rows.
  Read data by mysql_store_result or by repeat call of mysql_fetch_row
**************************************************************************/

int STDCALL
mysql_query(MYSQL *mysql, const char *query)
{
  return mysql_real_query(mysql,query, (uint) strlen(query));
}


static MYSQL* spawn_init(MYSQL* parent, const char* host,
			 unsigned int port, const char* user,
			 const char* passwd)
{
  MYSQL* child;
  DBUG_ENTER("spawn_init");
  if (!(child= mysql_init(0)))
    DBUG_RETURN(0);

  child->options.user= my_strdup((user) ? user :
				 (parent->user ? parent->user :
				  parent->options.user), MYF(0));
  child->options.password= my_strdup((passwd) ? passwd :
				     (parent->passwd ?
				      parent->passwd :
				      parent->options.password), MYF(0));
  child->options.port= port;
  child->options.host= my_strdup((host) ? host :
				 (parent->host ?
				  parent->host :
				  parent->options.host), MYF(0));
  if (parent->db)
    child->options.db= my_strdup(parent->db, MYF(0));
  else if (parent->options.db)
    child->options.db= my_strdup(parent->options.db, MYF(0));

  /*
    rpl_pivot is set to 1 in mysql_init();  Reset it as we are not doing
    replication here
  */
  child->rpl_pivot= 0;
  DBUG_RETURN(child);
}


int
STDCALL mysql_set_master(MYSQL* mysql, const char* host,
			 unsigned int port, const char* user,
			 const char* passwd)
{
  if (mysql->master != mysql && !mysql->master->rpl_pivot)
    mysql_close(mysql->master);
  if (!(mysql->master = spawn_init(mysql, host, port, user, passwd)))
    return 1;
  return 0;
}


int
STDCALL mysql_add_slave(MYSQL* mysql, const char* host,
			unsigned int port,
			const char* user,
			const char* passwd)
{
  MYSQL* slave;
  if (!(slave = spawn_init(mysql, host, port, user, passwd)))
    return 1;
  slave->next_slave = mysql->next_slave;
  mysql->next_slave = slave;
  return 0;
}

/**************************************************************************
  Return next field of the query results
**************************************************************************/

MYSQL_FIELD * STDCALL
mysql_fetch_field(MYSQL_RES *result)
{
  if (result->current_field >= result->field_count)
    return(NULL);
  return &result->fields[result->current_field++];
}


/**************************************************************************
  Get column lengths of the current row
  If one uses mysql_use_result, res->lengths contains the length information,
  else the lengths are calculated from the offset between pointers.
**************************************************************************/

ulong * STDCALL
mysql_fetch_lengths(MYSQL_RES *res)
{
  MYSQL_ROW column;

  if (!(column=res->current_row))
    return 0;					/* Something is wrong */
  if (res->data)
    (*res->methods->fetch_lengths)(res->lengths, column, res->field_count);
  return res->lengths;
}


/**************************************************************************
  Move to a specific row and column
**************************************************************************/

void STDCALL
mysql_data_seek(MYSQL_RES *result, my_ulonglong row)
{
  MYSQL_ROWS	*tmp=0;
  DBUG_PRINT("info",("mysql_data_seek(%ld)",(long) row));
  if (result->data)
    for (tmp=result->data->data; row-- && tmp ; tmp = tmp->next) ;
  result->current_row=0;
  result->data_cursor = tmp;
}


/*************************************************************************
  put the row or field cursor one a position one got from mysql_row_tell()
  This doesn't restore any data. The next mysql_fetch_row or
  mysql_fetch_field will return the next row or field after the last used
*************************************************************************/

MYSQL_ROW_OFFSET STDCALL
mysql_row_seek(MYSQL_RES *result, MYSQL_ROW_OFFSET row)
{
  MYSQL_ROW_OFFSET return_value=result->data_cursor;
  result->current_row= 0;
  result->data_cursor= row;
  return return_value;
}


MYSQL_FIELD_OFFSET STDCALL
mysql_field_seek(MYSQL_RES *result, MYSQL_FIELD_OFFSET field_offset)
{
  MYSQL_FIELD_OFFSET return_value=result->current_field;
  result->current_field=field_offset;
  return return_value;
}


/*****************************************************************************
  List all databases
*****************************************************************************/

MYSQL_RES * STDCALL
mysql_list_dbs(MYSQL *mysql, const char *wild)
{
  char buff[255];
  DBUG_ENTER("mysql_list_dbs");

  append_wild(strmov(buff,"show databases"),buff+sizeof(buff),wild);
  if (mysql_query(mysql,buff))
    DBUG_RETURN(0);
  DBUG_RETURN (mysql_store_result(mysql));
}


/*****************************************************************************
  List all tables in a database
  If wild is given then only the tables matching wild is returned
*****************************************************************************/

MYSQL_RES * STDCALL
mysql_list_tables(MYSQL *mysql, const char *wild)
{
  char buff[255];
  DBUG_ENTER("mysql_list_tables");

  append_wild(strmov(buff,"show tables"),buff+sizeof(buff),wild);
  if (mysql_query(mysql,buff))
    DBUG_RETURN(0);
  DBUG_RETURN (mysql_store_result(mysql));
}


MYSQL_FIELD *cli_list_fields(MYSQL *mysql)
{
  MYSQL_DATA *query;
  if (!(query= cli_read_rows(mysql,(MYSQL_FIELD*) 0, 
			     protocol_41(mysql) ? 8 : 6)))
    return NULL;

  mysql->field_count= (uint) query->rows;
  return unpack_fields(query,&mysql->field_alloc,
		       mysql->field_count, 1, mysql->server_capabilities);
}


/**************************************************************************
  List all fields in a table
  If wild is given then only the fields matching wild is returned
  Instead of this use query:
  show fields in 'table' like "wild"
**************************************************************************/

MYSQL_RES * STDCALL
mysql_list_fields(MYSQL *mysql, const char *table, const char *wild)
{
  MYSQL_RES   *result;
  MYSQL_FIELD *fields;
  char	     buff[257],*end;
  DBUG_ENTER("mysql_list_fields");
  DBUG_PRINT("enter",("table: '%s'  wild: '%s'",table,wild ? wild : ""));

  end=strmake(strmake(buff, table,128)+1,wild ? wild : "",128);
  free_old_query(mysql);
  if (simple_command(mysql,COM_FIELD_LIST,buff,(ulong) (end-buff),1) ||
      !(fields= (*mysql->methods->list_fields)(mysql)))
    DBUG_RETURN(NULL);

  if (!(result = (MYSQL_RES *) my_malloc(sizeof(MYSQL_RES),
					 MYF(MY_WME | MY_ZEROFILL))))
    DBUG_RETURN(NULL);

  result->methods= mysql->methods;
  result->field_alloc=mysql->field_alloc;
  mysql->fields=0;
  result->field_count = mysql->field_count;
  result->fields= fields;
  result->eof=1;
  DBUG_RETURN(result);
}

/* List all running processes (threads) in server */

MYSQL_RES * STDCALL
mysql_list_processes(MYSQL *mysql)
{
  MYSQL_DATA *fields;
  uint field_count;
  uchar *pos;
  DBUG_ENTER("mysql_list_processes");

  LINT_INIT(fields);
  if (simple_command(mysql,COM_PROCESS_INFO,0,0,0))
    DBUG_RETURN(0);
  free_old_query(mysql);
  pos=(uchar*) mysql->net.read_pos;
  field_count=(uint) net_field_length(&pos);
  if (!(fields = (*mysql->methods->read_rows)(mysql,(MYSQL_FIELD*) 0,
					      protocol_41(mysql) ? 7 : 5)))
    DBUG_RETURN(NULL);
  if (!(mysql->fields=unpack_fields(fields,&mysql->field_alloc,field_count,0,
				    mysql->server_capabilities)))
    DBUG_RETURN(0);
  mysql->status=MYSQL_STATUS_GET_RESULT;
  mysql->field_count=field_count;
  DBUG_RETURN(mysql_store_result(mysql));
}


#ifdef USE_OLD_FUNCTIONS
int  STDCALL
mysql_create_db(MYSQL *mysql, const char *db)
{
  DBUG_ENTER("mysql_createdb");
  DBUG_PRINT("enter",("db: %s",db));
  DBUG_RETURN(simple_command(mysql,COM_CREATE_DB,db, (ulong) strlen(db),0));
}


int  STDCALL
mysql_drop_db(MYSQL *mysql, const char *db)
{
  DBUG_ENTER("mysql_drop_db");
  DBUG_PRINT("enter",("db: %s",db));
  DBUG_RETURN(simple_command(mysql,COM_DROP_DB,db,(ulong) strlen(db),0));
}
#endif


int STDCALL
mysql_shutdown(MYSQL *mysql)
{
  DBUG_ENTER("mysql_shutdown");
  DBUG_RETURN(simple_command(mysql,COM_SHUTDOWN,0,0,0));
}


int STDCALL
mysql_refresh(MYSQL *mysql,uint options)
{
  uchar bits[1];
  DBUG_ENTER("mysql_refresh");
  bits[0]= (uchar) options;
  DBUG_RETURN(simple_command(mysql,COM_REFRESH,(char*) bits,1,0));
}


int STDCALL
mysql_kill(MYSQL *mysql,ulong pid)
{
  char buff[4];
  DBUG_ENTER("mysql_kill");
  int4store(buff,pid);
  DBUG_RETURN(simple_command(mysql,COM_PROCESS_KILL,buff,sizeof(buff),0));
}


int STDCALL
mysql_set_server_option(MYSQL *mysql, enum enum_mysql_set_option option)
{
  char buff[2];
  DBUG_ENTER("mysql_set_server_option");
  int2store(buff, (uint) option);
  DBUG_RETURN(simple_command(mysql, COM_SET_OPTION, buff, sizeof(buff), 0));
}


int STDCALL
mysql_dump_debug_info(MYSQL *mysql)
{
  DBUG_ENTER("mysql_dump_debug_info");
  DBUG_RETURN(simple_command(mysql,COM_DEBUG,0,0,0));
}


const char *cli_read_statistics(MYSQL *mysql)
{
  mysql->net.read_pos[mysql->packet_length]=0;	/* End of stat string */
  if (!mysql->net.read_pos[0])
  {
    strmov(mysql->net.sqlstate, unknown_sqlstate);
    mysql->net.last_errno=CR_WRONG_HOST_INFO;
    strmov(mysql->net.last_error, ER(mysql->net.last_errno));
    return mysql->net.last_error;
  }
  return (char*) mysql->net.read_pos;
}


const char * STDCALL
mysql_stat(MYSQL *mysql)
{
  DBUG_ENTER("mysql_stat");
  if (simple_command(mysql,COM_STATISTICS,0,0,0))
    return mysql->net.last_error;
  DBUG_RETURN((*mysql->methods->read_statistics)(mysql));
}


int STDCALL
mysql_ping(MYSQL *mysql)
{
  DBUG_ENTER("mysql_ping");
  DBUG_RETURN(simple_command(mysql,COM_PING,0,0,0));
}


const char * STDCALL
mysql_get_server_info(MYSQL *mysql)
{
  return((char*) mysql->server_version);
}


/*
  Get version number for server in a form easy to test on

  SYNOPSIS
    mysql_get_server_version()
    mysql		Connection

  EXAMPLE
    4.1.0-alfa ->  40100
  
  NOTES
    We will ensure that a newer server always has a bigger number.

  RETURN
   Signed number > 323000
*/

ulong STDCALL
mysql_get_server_version(MYSQL *mysql)
{
  uint major, minor, version;
  char *pos= mysql->server_version, *end_pos;
  major=   (uint) strtoul(pos, &end_pos, 10);	pos=end_pos+1;
  minor=   (uint) strtoul(pos, &end_pos, 10);	pos=end_pos+1;
  version= (uint) strtoul(pos, &end_pos, 10);
  return (ulong) major*10000L+(ulong) (minor*100+version);
}


const char * STDCALL
mysql_get_host_info(MYSQL *mysql)
{
  return(mysql->host_info);
}


uint STDCALL
mysql_get_proto_info(MYSQL *mysql)
{
  return (mysql->protocol_version);
}

const char * STDCALL
mysql_get_client_info(void)
{
  return (char*) MYSQL_SERVER_VERSION;
}

ulong STDCALL mysql_get_client_version(void)
{
  return MYSQL_VERSION_ID;
}

my_bool STDCALL mysql_eof(MYSQL_RES *res)
{
  return res->eof;
}

MYSQL_FIELD * STDCALL mysql_fetch_field_direct(MYSQL_RES *res,uint fieldnr)
{
  return &(res)->fields[fieldnr];
}

MYSQL_FIELD * STDCALL mysql_fetch_fields(MYSQL_RES *res)
{
  return (res)->fields;
}

MYSQL_ROW_OFFSET STDCALL mysql_row_tell(MYSQL_RES *res)
{
  return res->data_cursor;
}

MYSQL_FIELD_OFFSET STDCALL mysql_field_tell(MYSQL_RES *res)
{
  return (res)->current_field;
}

/* MYSQL */

unsigned int STDCALL mysql_field_count(MYSQL *mysql)
{
  return mysql->last_used_con->field_count;
}

my_ulonglong STDCALL mysql_affected_rows(MYSQL *mysql)
{
  return mysql->last_used_con->affected_rows;
}

my_ulonglong STDCALL mysql_insert_id(MYSQL *mysql)
{
  return mysql->last_used_con->insert_id;
}

const char *STDCALL mysql_sqlstate(MYSQL *mysql)
{
  return mysql->net.sqlstate;
}

uint STDCALL mysql_warning_count(MYSQL *mysql)
{
  return mysql->warning_count;
}

const char *STDCALL mysql_info(MYSQL *mysql)
{
  return mysql->info;
}

ulong STDCALL mysql_thread_id(MYSQL *mysql)
{
  return (mysql)->thread_id;
}

const char * STDCALL mysql_character_set_name(MYSQL *mysql)
{
  return mysql->charset->name;
}


uint STDCALL mysql_thread_safe(void)
{
#ifdef THREAD
  return 1;
#else
  return 0;
#endif
}

/****************************************************************************
  Some support functions
****************************************************************************/

/*
  Functions called my my_net_init() to set some application specific variables
*/

void my_net_local_init(NET *net)
{
  net->max_packet=   (uint) net_buffer_length;
  net->read_timeout= (uint) net_read_timeout;
  net->write_timeout=(uint) net_write_timeout;
  net->retry_count=  1;
  net->max_packet_size= max(net_buffer_length, max_allowed_packet);
}

/*
  Add escape characters to a string (blob?) to make it suitable for a insert
  to should at least have place for length*2+1 chars
  Returns the length of the to string
*/

ulong STDCALL
mysql_escape_string(char *to,const char *from,ulong length)
{
  return escape_string_for_mysql(default_charset_info, to, from, length);
}

ulong STDCALL
mysql_real_escape_string(MYSQL *mysql, char *to,const char *from,
			 ulong length)
{
  return escape_string_for_mysql(mysql->charset, to, from, length);
}


char * STDCALL
mysql_odbc_escape_string(MYSQL *mysql,
			 char *to, ulong to_length,
			 const char *from, ulong from_length,
			 void *param,
			 char * (*extend_buffer)
			 (void *, char *, ulong *))
{
  char *to_end=to+to_length-5;
  const char *end;
#ifdef USE_MB
  my_bool use_mb_flag=use_mb(mysql->charset);
#endif

  for (end=from+from_length; from != end ; from++)
  {
    if (to >= to_end)
    {
      to_length = (ulong) (end-from)+512;	/* We want this much more */
      if (!(to=(*extend_buffer)(param, to, &to_length)))
	return to;
      to_end=to+to_length-5;
    }
#ifdef USE_MB
    {
      int l;
      if (use_mb_flag && (l = my_ismbchar(mysql->charset, from, end)))
      {
	while (l--)
	  *to++ = *from++;
	from--;
	continue;
      }
    }
#endif
    switch (*from) {
    case 0:				/* Must be escaped for 'mysql' */
      *to++= '\\';
      *to++= '0';
      break;
    case '\n':				/* Must be escaped for logs */
      *to++= '\\';
      *to++= 'n';
      break;
    case '\r':
      *to++= '\\';
      *to++= 'r';
      break;
    case '\\':
      *to++= '\\';
      *to++= '\\';
      break;
    case '\'':
      *to++= '\\';
      *to++= '\'';
      break;
    case '"':				/* Better safe than sorry */
      *to++= '\\';
      *to++= '"';
      break;
    case '\032':			/* This gives problems on Win32 */
      *to++= '\\';
      *to++= 'Z';
      break;
    default:
      *to++= *from;
    }
  }
  return to;
}

void STDCALL
myodbc_remove_escape(MYSQL *mysql,char *name)
{
  char *to;
#ifdef USE_MB
  my_bool use_mb_flag=use_mb(mysql->charset);
  char *end;
  LINT_INIT(end);
  if (use_mb_flag)
    for (end=name; *end ; end++) ;
#endif

  for (to=name ; *name ; name++)
  {
#ifdef USE_MB
    int l;
    if (use_mb_flag && (l = my_ismbchar( mysql->charset, name , end ) ) )
    {
      while (l--)
	*to++ = *name++;
      name--;
      continue;
    }
#endif
    if (*name == '\\' && name[1])
      name++;
    *to++= *name;
  }
  *to=0;
}

/********************************************************************

 Implementation of new client-server prototypes for 4.1 version
 starts from here ..

 mysql_* are real prototypes used by applications

*********************************************************************/

/********************************************************************
 Misc Utility functions
********************************************************************/

/*
  Set the internal stmt error messages
*/

static void set_stmt_error(MYSQL_STMT * stmt, int errcode,
			   const char *sqlstate)
{
  DBUG_ENTER("set_stmt_error");
  DBUG_PRINT("enter", ("error: %d '%s'", errcode, ER(errcode)));
  DBUG_ASSERT(stmt != 0);

  stmt->last_errno= errcode;
  strmov(stmt->last_error, ER(errcode));
  strmov(stmt->sqlstate, sqlstate);

  DBUG_VOID_RETURN;
}


/*
  Copy error message to statement handler
*/

void set_stmt_errmsg(MYSQL_STMT * stmt, const char *err, int errcode,
		     const char *sqlstate)
{
  DBUG_ENTER("set_stmt_error_msg");
  DBUG_PRINT("enter", ("error: %d/%s '%s'", errcode, sqlstate, err));
  DBUG_ASSERT(stmt != 0);

  stmt->last_errno= errcode;
  if (err && err[0])
    strmov(stmt->last_error, err);
  strmov(stmt->sqlstate, sqlstate);

  DBUG_VOID_RETURN;
}



/*
  Reallocate the NET package to be at least of 'length' bytes

  SYNPOSIS
   my_realloc_str()
   net			The NET structure to modify
   int length		Ensure that net->buff is at least this big

  RETURN VALUES
  0	ok
  1	Error

*/

static my_bool my_realloc_str(NET *net, ulong length)
{
  ulong buf_length= (ulong) (net->write_pos - net->buff);
  my_bool res=0;
  DBUG_ENTER("my_realloc_str");
  if (buf_length + length > net->max_packet)
  {
    res= net_realloc(net, buf_length + length);
    net->write_pos= net->buff+ buf_length;
  }
  DBUG_RETURN(res);
}

/********************************************************************
  Prepare related implementations
********************************************************************/

static int stmt_read_row_unbuffered(MYSQL_STMT *stmt, unsigned char **row);
static int stmt_read_row_buffered(MYSQL_STMT *stmt, unsigned char **row);
static int stmt_read_row_no_data(MYSQL_STMT *stmt, unsigned char **row);

/*
  Read the prepared statement results ..

  NOTE
    This is only called for connection to servers that supports
    prepared statements (and thus the 4.1 protocol)

  RETURN VALUES
    0	ok
    1	error
*/

my_bool cli_read_prepare_result(MYSQL *mysql, MYSQL_STMT *stmt)
{
  uchar *pos;
  uint field_count, param_count;
  MYSQL_DATA *fields_data;
  DBUG_ENTER("read_prepare_result");

  mysql= mysql->last_used_con;
  if (net_safe_read(mysql) == packet_error)
    DBUG_RETURN(1);

  pos= (uchar*) mysql->net.read_pos;
  stmt->stmt_id= uint4korr(pos+1); pos+= 5;
  field_count=   uint2korr(pos);   pos+= 2;
  param_count=   uint2korr(pos);   pos+= 2;

  if (param_count != 0)
  {
    MYSQL_DATA *param_data;

    /* skip parameters data: we don't support it yet */
    if (!(param_data= (*mysql->methods->read_rows)(mysql, (MYSQL_FIELD*)0, 7)))
      DBUG_RETURN(1);
    free_rows(param_data);
  }

  if (field_count != 0)
  {
    if (!(mysql->server_status & SERVER_STATUS_AUTOCOMMIT))
      mysql->server_status|= SERVER_STATUS_IN_TRANS;

    mysql->extra_info= net_field_length_ll(&pos);
    if (!(fields_data= (*mysql->methods->read_rows)(mysql,(MYSQL_FIELD*)0,7)))
      DBUG_RETURN(1);
    if (!(stmt->fields= unpack_fields(fields_data,&stmt->mem_root,
				      field_count,0,
				      mysql->server_capabilities)))
      DBUG_RETURN(1);
  }
  stmt->field_count=  (uint) field_count;
  stmt->param_count=  (ulong) param_count;

  DBUG_RETURN(0);
}

#ifdef HAVE_DEPRECATED_411_API
MYSQL_STMT * STDCALL mysql_prepare(MYSQL *mysql, const char *query,
                                   unsigned long query_length)
{
  MYSQL_STMT *stmt;
  DBUG_ENTER("mysql_prepare");

  stmt= mysql_stmt_init(mysql);
  if (stmt && mysql_stmt_prepare(stmt, query, query_length))
  {
    mysql_stmt_close(stmt);
    DBUG_RETURN(0);
  }
  DBUG_RETURN(stmt);
}
#endif

/*
  Allocate memory and init prepared statement structure
  SYNOPSIS
    mysql_stmt_init()
    mysql connection handle

  RETURN VALUE
    statement structure upon success and NULL if out of
    memory
*/

MYSQL_STMT * STDCALL
mysql_stmt_init(MYSQL *mysql)
{
  MYSQL_STMT *stmt;
  DBUG_ENTER("mysql_stmt_init");

  if (!(stmt= (MYSQL_STMT *) my_malloc(sizeof(MYSQL_STMT),
                                       MYF(MY_WME | MY_ZEROFILL))))
  {
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    DBUG_RETURN(0);
  }

  init_alloc_root(&stmt->mem_root, 2048, 2048);
  init_alloc_root(&stmt->result.alloc, 4096, 4096);
  stmt->result.alloc.min_malloc= sizeof(MYSQL_ROWS);
  mysql->stmts= list_add(mysql->stmts, &stmt->list);
  stmt->list.data= stmt;
  stmt->state= MYSQL_STMT_INIT_DONE;
  stmt->mysql= mysql;
  stmt->read_row_func= stmt_read_row_no_data;
  /* The rest of statement members was bzeroed inside malloc */

  DBUG_RETURN(stmt);
}

/*
  Prepare server side statement with query:
  SYNOPSIS
    mysql_stmt_prepare()
    query  statement to prepare
    length statement length

  DESCRIPTION
  - if this is a re-prepare of the statement, first close previous data 
    structure on the server and free old statement data
  - send the query to server and get back number of placeholders,
    number of columns in result set (if any), and result set metadata.
    At the same time allocate memory for input and output parameters 
    to have less checks in mysql_stmt_bind_{param, result}.

  RETURN VALUES
    0  success
   !0  error 
*/


int STDCALL
mysql_stmt_prepare(MYSQL_STMT *stmt, const char *query, ulong length)
{
  MYSQL *mysql= stmt->mysql;
  DBUG_ENTER("mysql_stmt_prepare");

  if (!mysql)
  {
    /* mysql can be reset in mysql_close called from mysql_reconnect */
    set_stmt_error(stmt, CR_SERVER_LOST, unknown_sqlstate);
    DBUG_RETURN(1);
  }

  if ((int) stmt->state > (int) MYSQL_STMT_INIT_DONE)
  {
    /* This is second prepare with another statement */
    char buff[MYSQL_STMT_HEADER];               /* 4 bytes - stmt id */

    mysql_stmt_free_result(stmt);
    /*
      These members must be reset for API to 
      function in case of error or misuse.
    */
    stmt->bind_param_done= stmt->bind_result_done= FALSE;
    stmt->param_count= stmt->field_count= 0;
    stmt->last_errno= 0;
    stmt->last_error[0]= '\0';
    free_root(&stmt->mem_root, MYF(MY_KEEP_PREALLOC));

    int4store(buff, stmt->stmt_id);
    /*
      If there was a 'use' result from another statement, or from
      mysql_use_result it won't be freed in mysql_stmt_free_result and
      we should get 'Commands out of sync' here.
    */
    if (simple_command(mysql, COM_CLOSE_STMT, buff, 4, 1))
    {
      set_stmt_errmsg(stmt, mysql->net.last_error, mysql->net.last_errno,
                      mysql->net.sqlstate);
      DBUG_RETURN(1);
    }
    stmt->state= MYSQL_STMT_INIT_DONE;
  }

  if (simple_command(mysql, COM_PREPARE, query, length, 1))
  {
    set_stmt_errmsg(stmt, mysql->net.last_error, mysql->net.last_errno,
                    mysql->net.sqlstate);
    DBUG_RETURN(1);
  }

  if ((*mysql->methods->read_prepare_result)(mysql, stmt))
  {
    set_stmt_errmsg(stmt, mysql->net.last_error, mysql->net.last_errno,
                    mysql->net.sqlstate);
    DBUG_RETURN(1);
  }

  /*
    alloc_root will return valid address even in case param_count 
    and field_count are zero. Thus we should never rely on stmt->bind
    or stmt->params when checking for existence of placeholders or
    result set.
  */
  if (!(stmt->params= (MYSQL_BIND *) alloc_root(&stmt->mem_root,
						sizeof(MYSQL_BIND)*
                                                (stmt->param_count + 
                                                 stmt->field_count))))
  {
    set_stmt_error(stmt, CR_OUT_OF_MEMORY, unknown_sqlstate);
    DBUG_RETURN(1);
  }
  stmt->bind= stmt->params + stmt->param_count;
  stmt->state= MYSQL_STMT_PREPARE_DONE;
  DBUG_PRINT("info", ("Parameter count: %ld", stmt->param_count));
  DBUG_RETURN(0);
}

/*
  Get the execute query meta information for non-select 
  statements.
*/

static unsigned int alloc_stmt_fields(MYSQL_STMT *stmt)
{
  MYSQL_FIELD *fields, *field, *end;
  MEM_ROOT *alloc= &stmt->mem_root;
  MYSQL *mysql= stmt->mysql->last_used_con;
  
  stmt->field_count= mysql->field_count;
  
  /*
    Get the field information for non-select statements 
    like SHOW and DESCRIBE commands
  */
  if (!(stmt->fields= (MYSQL_FIELD *) alloc_root(alloc, 
						 sizeof(MYSQL_FIELD) *
						 stmt->field_count)) || 
      !(stmt->bind= (MYSQL_BIND *) alloc_root(alloc, 
					      sizeof(MYSQL_BIND) *
					      stmt->field_count)))
    return 0;
  
  for (fields= mysql->fields, end= fields+stmt->field_count, 
	 field= stmt->fields;
       field && fields < end; fields++, field++)
  {
    field->db       = strdup_root(alloc,fields->db);
    field->table    = strdup_root(alloc,fields->table);
    field->org_table= strdup_root(alloc,fields->org_table);
    field->name     = strdup_root(alloc,fields->name);
    field->org_name = strdup_root(alloc,fields->org_name);
    field->charsetnr= fields->charsetnr;
    field->length   = fields->length;
    field->type     = fields->type;
    field->flags    = fields->flags;
    field->decimals = fields->decimals;
    field->def      = fields->def ? strdup_root(alloc,fields->def): 0;
    field->max_length= 0;
  }
  return stmt->field_count;
}

/*
  Returns prepared meta information in the form of resultset
  to client.
*/

MYSQL_RES * STDCALL
mysql_stmt_result_metadata(MYSQL_STMT *stmt)
{
  MYSQL_RES *result;
  DBUG_ENTER("mysql_stmt_result_metadata");
  
  /*
    stmt->fields is only defined if stmt->field_count is not null;
    stmt->field_count is initialized in prepare.
  */
  if (!stmt->field_count)
     DBUG_RETURN(0);

  if (!(result=(MYSQL_RES*) my_malloc(sizeof(*result),
                                      MYF(MY_WME | MY_ZEROFILL))))
  {
    set_stmt_error(stmt, CR_OUT_OF_MEMORY, unknown_sqlstate);
    DBUG_RETURN(0);
  }

  result->methods=	stmt->mysql->methods;
  result->eof=		1;                      /* Marker for buffered */
  result->fields=	stmt->fields;
  result->field_count=	stmt->field_count;
  /* The rest of members of 'result' was bzeroed inside malloc */
  DBUG_RETURN(result);
}

/*
  Returns parameter columns meta information in the form of 
  resultset.
*/

MYSQL_RES * STDCALL
mysql_stmt_param_metadata(MYSQL_STMT *stmt)
{
  DBUG_ENTER("mysql_stmt_param_metadata");
  
  if (!stmt->param_count)
    DBUG_RETURN(0);

  /*
    TODO: Fix this when server sends the information. 
    Till then keep a dummy prototype 
  */
  DBUG_RETURN(0); 
}


/********************************************************************
 Prepare-execute, and param handling
*********************************************************************/

/****************************************************************************
  Functions to store parameter data from a prepared statement.

  All functions have the following characteristics:

  SYNOPSIS
    store_param_xxx()
    net			MySQL NET connection
    param		MySQL bind param

  RETURN VALUES
    0	ok
    1	Error	(Can't alloc net->buffer)
****************************************************************************/

static void store_param_tinyint(NET *net, MYSQL_BIND *param)
{
  *(net->write_pos++)= (uchar) *param->buffer;
}

static void store_param_short(NET *net, MYSQL_BIND *param)
{
  short value= *(short*) param->buffer;
  int2store(net->write_pos,value);
  net->write_pos+=2;
}

static void store_param_int32(NET *net, MYSQL_BIND *param)
{
  int32 value= *(int32*) param->buffer;
  int4store(net->write_pos,value);
  net->write_pos+=4;
}

static void store_param_int64(NET *net, MYSQL_BIND *param)
{
  longlong value= *(longlong*) param->buffer;
  int8store(net->write_pos,value);
  net->write_pos+= 8;
}

static void store_param_float(NET *net, MYSQL_BIND *param)
{
  float value= *(float*) param->buffer;
  float4store(net->write_pos, value);
  net->write_pos+= 4;
}

static void store_param_double(NET *net, MYSQL_BIND *param)
{
  double value= *(double*) param->buffer;
  float8store(net->write_pos, value);
  net->write_pos+= 8;
}

static void store_param_time(NET *net, MYSQL_BIND *param)
{
  MYSQL_TIME *tm= (MYSQL_TIME *) param->buffer;
  char buff[15], *pos;
  uint length;

  pos= buff+1;
  pos[0]= tm->neg ? 1: 0;
  int4store(pos+1, tm->day);
  pos[5]= (uchar) tm->hour;
  pos[6]= (uchar) tm->minute;
  pos[7]= (uchar) tm->second;
  int4store(pos+8, tm->second_part);
  if (tm->second_part)
    length= 11;
  else if (tm->hour || tm->minute || tm->second || tm->day)
    length= 8;
  else
    length= 0;
  buff[0]= (char) length++;  
  memcpy((char *)net->write_pos, buff, length);
  net->write_pos+= length;
}

static void net_store_datetime(NET *net, MYSQL_TIME *tm)
{
  char buff[12], *pos;
  uint length;

  pos= buff+1;

  int2store(pos, tm->year);
  pos[2]= (uchar) tm->month;
  pos[3]= (uchar) tm->day;
  pos[4]= (uchar) tm->hour;
  pos[5]= (uchar) tm->minute;
  pos[6]= (uchar) tm->second;
  int4store(pos+7, tm->second_part);
  if (tm->second_part)
    length= 11;
  else if (tm->hour || tm->minute || tm->second)
    length= 7;
  else if (tm->year || tm->month || tm->day)
    length= 4;
  else
    length= 0;
  buff[0]= (char) length++;  
  memcpy((char *)net->write_pos, buff, length);
  net->write_pos+= length;
}

static void store_param_date(NET *net, MYSQL_BIND *param)
{
  MYSQL_TIME *tm= (MYSQL_TIME *) param->buffer;
  tm->hour= tm->minute= tm->second= 0;
  tm->second_part= 0;
  net_store_datetime(net, tm);
}

static void store_param_datetime(NET *net, MYSQL_BIND *param)
{
  MYSQL_TIME *tm= (MYSQL_TIME *) param->buffer;
  net_store_datetime(net, tm);
}
    
static void store_param_str(NET *net, MYSQL_BIND *param)
{
  ulong length= param->length ? *param->length : param->buffer_length;
  char *to= (char *) net_store_length((char *) net->write_pos, length);
  memcpy(to, param->buffer, length);
  net->write_pos= (uchar*) to+length;
}


/*
  Mark if the parameter is NULL.

  SYNOPSIS
    store_param_null()
    net			MySQL NET connection
    param		MySQL bind param

  DESCRIPTION
    A data package starts with a string of bits where we set a bit
    if a parameter is NULL
*/

static void store_param_null(NET *net, MYSQL_BIND *param)
{
  uint pos= param->param_number;
  net->buff[pos/8]|=  (uchar) (1 << (pos & 7));
}


/*
  Set parameter data by reading from input buffers from the
  client application
*/

static my_bool store_param(MYSQL_STMT *stmt, MYSQL_BIND *param)
{
  NET *net= &stmt->mysql->net;
  DBUG_ENTER("store_param");
  DBUG_PRINT("enter",("type: %d, buffer:%lx, length: %lu  is_null: %d",
		      param->buffer_type,
		      param->buffer ? param->buffer : "0", *param->length,
		      *param->is_null));

  if (*param->is_null)
    store_param_null(net, param);
  else
  {
    /*
      Param->length should ALWAYS point to the correct length for the type
      Either to the length pointer given by the user or param->buffer_length
    */
    if ((my_realloc_str(net, 9 + *param->length)))
    {
      set_stmt_error(stmt, CR_OUT_OF_MEMORY, unknown_sqlstate);
      DBUG_RETURN(1);
    }
    (*param->store_param_func)(net, param);
  }
  DBUG_RETURN(0);
}


/*
  Send the prepared query to server for execution
*/

static my_bool execute(MYSQL_STMT * stmt, char *packet, ulong length)
{
  MYSQL *mysql= stmt->mysql;
  NET	*net= &mysql->net;
  char buff[4 /* size of stmt id */ + 
            5 /* execution flags */];
  DBUG_ENTER("execute");
  DBUG_PRINT("enter",("packet: %s, length :%d",packet ? packet :" ", length));

  mysql->last_used_con= mysql;
  int4store(buff, stmt->stmt_id);		/* Send stmt id to server */
  buff[4]= (char) 0;                            /* no flags */
  int4store(buff+5, 1);                         /* iteration count */
  if (cli_advanced_command(mysql, COM_EXECUTE, buff, sizeof(buff),
                           packet, length, 1) ||
      (*mysql->methods->read_query_result)(mysql))
  {
    set_stmt_errmsg(stmt, net->last_error, net->last_errno, net->sqlstate);
    DBUG_RETURN(1);
  }
  stmt->affected_rows= mysql->affected_rows;
  stmt->insert_id= mysql->insert_id;
  DBUG_RETURN(0);
}
        

static void store_param_type(char **pos, MYSQL_BIND *param)
{
  uint typecode= param->buffer_type | (param->is_unsigned ? 32768 : 0);
  int2store(*pos, typecode);
  *pos+= 2;
}


int cli_stmt_execute(MYSQL_STMT *stmt)
{
  DBUG_ENTER("cli_stmt_execute");

  if (stmt->param_count)
  {
    NET        *net= &stmt->mysql->net;
    MYSQL_BIND *param, *param_end;
    char       *param_data;
    ulong length;
    uint null_count;
    my_bool    result;

    if (!stmt->bind_param_done)
    {
      set_stmt_error(stmt, CR_PARAMS_NOT_BOUND, unknown_sqlstate);
      DBUG_RETURN(1);
    }

    net_clear(net);				/* Sets net->write_pos */
    /* Reserve place for null-marker bytes */
    null_count= (stmt->param_count+7) /8;
    bzero((char*) net->write_pos, null_count);
    net->write_pos+= null_count;
    param_end= stmt->params + stmt->param_count;

    /* In case if buffers (type) altered, indicate to server */
    *(net->write_pos)++= (uchar) stmt->send_types_to_server;
    if (stmt->send_types_to_server)
    {
      /*
	Store types of parameters in first in first package
	that is sent to the server.
      */
      for (param= stmt->params;	param < param_end ; param++)
        store_param_type((char**) &net->write_pos, param);
    }

    for (param= stmt->params; param < param_end; param++)
    {
      /* check if mysql_long_data() was used */
      if (param->long_data_used)
	param->long_data_used= 0;	/* Clear for next execute call */
      else if (store_param(stmt, param))
	DBUG_RETURN(1);
    }
    length= (ulong) (net->write_pos - net->buff);
    /* TODO: Look into avoding the following memdup */
    if (!(param_data= my_memdup((const char*) net->buff, length, MYF(0))))
    {
      set_stmt_error(stmt, CR_OUT_OF_MEMORY, unknown_sqlstate);
      DBUG_RETURN(1);
    }
    result= execute(stmt, param_data, length);
    stmt->send_types_to_server=0;
    my_free(param_data, MYF(MY_WME));
    DBUG_RETURN(result);
  }
  DBUG_RETURN((int) execute(stmt,0,0));
}

/*
  Read one row from buffered result set.  Result set is created by prior
  call to mysql_stmt_store_result().
  SYNOPSIS
    stmt_read_row_buffered()

  RETURN VALUE
    0             - success; *row is set to valid row pointer (row data
                    is stored in result set buffer)
    MYSQL_NO_DATA - end of result set. *row is set to NULL
*/

static int stmt_read_row_buffered(MYSQL_STMT *stmt, unsigned char **row)
{
  if (stmt->data_cursor)
  {
    *row= (uchar *) stmt->data_cursor->data;
    stmt->data_cursor= stmt->data_cursor->next;
    return 0;
  }
  *row= 0;
  return MYSQL_NO_DATA;
}

/*
  Read one row from network: unbuffered non-cursor fetch.
  If last row was read, or error occured, erase this statement
  from record pointing to object unbuffered fetch is performed from.

  SYNOPSIS
    stmt_read_row_unbuffered()
    stmt  statement handle
    row   pointer to write pointer to row data;

  RETURN VALUE
    0           - success; *row contains valid address of a row;
                  row data is stored in network buffer
    1           - error; error code is written to
                  stmt->last_{errno,error}; *row is not changed 
  MYSQL_NO_DATA - end of file was read from network;
                  *row is to NULL 
*/

static int stmt_read_row_unbuffered(MYSQL_STMT *stmt, unsigned char **row)
{
  int rc= 1;
  MYSQL *mysql= stmt->mysql;
  /* 
    This function won't be called if stmt->field_count is zero
    or execution wasn't done: this is ensured by mysql_stmt_execute.
  */
  if (!mysql)
  {
    set_stmt_error(stmt, CR_SERVER_LOST, unknown_sqlstate);
    return 1;
  }
  if (mysql->status != MYSQL_STATUS_GET_RESULT)
  {
    set_stmt_error(stmt, stmt->unbuffered_fetch_cancelled ?
                   CR_FETCH_CANCELLED : CR_COMMANDS_OUT_OF_SYNC,
                   unknown_sqlstate);
    goto error;
  }
  if ((*mysql->methods->unbuffered_fetch)(mysql, (char**) row))
  {
    set_stmt_errmsg(stmt, mysql->net.last_error, mysql->net.last_errno,
                    mysql->net.sqlstate);
    goto error;
  }
  if (!*row)
  {
    mysql->status= MYSQL_STATUS_READY;
    rc= MYSQL_NO_DATA;
    goto error;
  }
  return 0;
error:
  if (mysql->unbuffered_fetch_owner == &stmt->unbuffered_fetch_cancelled)
    mysql->unbuffered_fetch_owner= 0;
  return rc;
}

/*
  Default read row function to not SIGSEGV in client in
  case of wrong sequence of API calls.
*/

static int
stmt_read_row_no_data(MYSQL_STMT *stmt  __attribute__((unused)),
                      unsigned char **row  __attribute__((unused)))
{
  if ((int) stmt->state < (int) MYSQL_STMT_PREPARE_DONE)
  {
    set_stmt_error(stmt, CR_NO_PREPARE_STMT, unknown_sqlstate);
    return 1;
  }
  return MYSQL_NO_DATA;
}


/*
  Get/set statement attributes

  SYNOPSIS
    mysql_stmt_attr_get()
    mysql_stmt_attr_set()

    attr_type  statemenet attribute
    value      casted to const void * pointer to value.

  RETURN VALUE
    0 success
   !0 wrong attribute type
*/
        
my_bool STDCALL mysql_stmt_attr_set(MYSQL_STMT *stmt,
                                    enum enum_stmt_attr_type attr_type,
                                    const void *value)
{
  switch (attr_type) {
  case STMT_ATTR_UPDATE_MAX_LENGTH:
    stmt->update_max_length= value ? *(const my_bool*) value : 0;
    break;
  default: 
    return TRUE;
  }
  return FALSE;
}


my_bool STDCALL mysql_stmt_attr_get(MYSQL_STMT *stmt, 
                                    enum enum_stmt_attr_type attr_type,
                                    void *value)
{
  switch (attr_type) {
  case STMT_ATTR_UPDATE_MAX_LENGTH:
    *(unsigned long *) value= stmt->update_max_length;
    break;
  default: 
    return TRUE;
  }
  return FALSE;
}


/*
  Execute the prepared query
*/

int STDCALL mysql_stmt_execute(MYSQL_STMT *stmt)
{
  MYSQL *mysql= stmt->mysql;
  DBUG_ENTER("mysql_stmt_execute");

  if (!mysql)
  {
    set_stmt_error(stmt, CR_SERVER_LOST, unknown_sqlstate);
    DBUG_RETURN(1);
  }

  mysql_stmt_free_result(stmt);
  /*
    No need to check for stmt->state: if the statement wasn't
    prepared we'll get 'unknown statemenet handler' error from server.
  */
  if (mysql->methods->stmt_execute(stmt))
    DBUG_RETURN(1);
  if (!stmt->field_count && mysql->field_count)
  {
    /* 
      This is 'SHOW'/'EXPLAIN'-like query.  Current implementation of
      prepared statements can't send result set metadata for this queries
      on prepare stage. Read it now.
    */
    alloc_stmt_fields(stmt);
  }
      
  stmt->state= MYSQL_STMT_EXECUTE_DONE;
  if (stmt->field_count)
  {
    stmt->mysql->unbuffered_fetch_owner= &stmt->unbuffered_fetch_cancelled;
    stmt->unbuffered_fetch_cancelled= FALSE;
    stmt->read_row_func= stmt_read_row_unbuffered;
  }
  DBUG_RETURN(0);
}


/*
  Return total parameters count in the statement
*/

ulong STDCALL mysql_stmt_param_count(MYSQL_STMT * stmt)
{
  DBUG_ENTER("mysql_stmt_param_count");
  DBUG_RETURN(stmt->param_count);
}

/*
  Return total affected rows from the last statement
*/

my_ulonglong STDCALL mysql_stmt_affected_rows(MYSQL_STMT *stmt)
{
  return stmt->affected_rows;
}


/*
  Return last inserted id for auto_increment columns
*/

my_ulonglong STDCALL mysql_stmt_insert_id(MYSQL_STMT *stmt)
{
  return stmt->insert_id;
}

static my_bool int_is_null_true= 1;		/* Used for MYSQL_TYPE_NULL */
static my_bool int_is_null_false= 0;

/*
  Setup the parameter data buffers from application
*/

my_bool STDCALL mysql_stmt_bind_param(MYSQL_STMT *stmt, MYSQL_BIND * bind)
{
  uint count=0;
  MYSQL_BIND *param, *end;
  DBUG_ENTER("mysql_stmt_bind_param");

  if (!stmt->param_count)
  {
    if ((int) stmt->state < (int) MYSQL_STMT_PREPARE_DONE)
    {
      set_stmt_error(stmt, CR_NO_PREPARE_STMT, unknown_sqlstate);
      DBUG_RETURN(1);
    }
    DBUG_RETURN(0);
  }

  /* Allocated on prepare */
  memcpy((char*) stmt->params, (char*) bind,
	 sizeof(MYSQL_BIND) * stmt->param_count);

  for (param= stmt->params, end= param+stmt->param_count;
       param < end ;
       param++)
  {
    param->param_number= count++;
    param->long_data_used= 0;

    /* If param->is_null is not set, then the value can never be NULL */
    if (!param->is_null)
      param->is_null= &int_is_null_false;

    /* Setup data copy functions for the different supported types */
    switch (param->buffer_type) {
    case MYSQL_TYPE_NULL:
      param->is_null= &int_is_null_true;
      break;
    case MYSQL_TYPE_TINY:
      /* Force param->length as this is fixed for this type */
      param->length= &param->buffer_length;
      param->buffer_length= 1;
      param->store_param_func= store_param_tinyint;
      break;
    case MYSQL_TYPE_SHORT:
      param->length= &param->buffer_length;
      param->buffer_length= 2;
      param->store_param_func= store_param_short;
      break;
    case MYSQL_TYPE_LONG:
      param->length= &param->buffer_length;
      param->buffer_length= 4;
      param->store_param_func= store_param_int32;
      break;
    case MYSQL_TYPE_LONGLONG:
      param->length= &param->buffer_length;
      param->buffer_length= 8;
      param->store_param_func= store_param_int64;
      break;
    case MYSQL_TYPE_FLOAT:
      param->length= &param->buffer_length;
      param->buffer_length= 4;
      param->store_param_func= store_param_float;
      break;
    case MYSQL_TYPE_DOUBLE:
      param->length= &param->buffer_length;
      param->buffer_length= 8;
      param->store_param_func= store_param_double;
      break;
    case MYSQL_TYPE_TIME:
      /* Buffer length ignored for DATE, TIME and DATETIME */
      param->store_param_func= store_param_time;
      break;
    case MYSQL_TYPE_DATE:
      param->store_param_func= store_param_date;
      break;
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      param->store_param_func= store_param_datetime;
      break;
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
      param->store_param_func= store_param_str;
      break;
    default:
      strmov(stmt->sqlstate, unknown_sqlstate);
      sprintf(stmt->last_error,
	      ER(stmt->last_errno= CR_UNSUPPORTED_PARAM_TYPE),
	      param->buffer_type, count);
      DBUG_RETURN(1);
    }
    /*
      If param->length is not given, change it to point to buffer_length.
      This way we can always use *param->length to get the length of data
    */
    if (!param->length)
      param->length= &param->buffer_length;
  }
  /* We have to send/resendtype information to MySQL */
  stmt->send_types_to_server= TRUE;
  stmt->bind_param_done= TRUE;
  DBUG_RETURN(0);
}


/********************************************************************
 Long data implementation
*********************************************************************/

/*
  Send long data in pieces to the server

  SYNOPSIS
    mysql_stmt_send_long_data()
    stmt			Statement handler
    param_number		Parameter number (0 - N-1)
    data			Data to send to server
    length			Length of data to send (may be 0)

  RETURN VALUES
    0	ok
    1	error
*/


my_bool STDCALL
mysql_stmt_send_long_data(MYSQL_STMT *stmt, uint param_number,
		     const char *data, ulong length)
{
  MYSQL_BIND *param;
  DBUG_ENTER("mysql_stmt_send_long_data");
  DBUG_ASSERT(stmt != 0);
  DBUG_PRINT("enter",("param no : %d, data : %lx, length : %ld",
		      param_number, data, length));
  
  /*
    We only need to check for stmt->param_count, if it's not null
    prepare was done.
  */
  if (param_number >= stmt->param_count)
  {
    set_stmt_error(stmt, CR_INVALID_PARAMETER_NO, unknown_sqlstate);
    DBUG_RETURN(1);
  }

  param= stmt->params+param_number;
  if (param->buffer_type < MYSQL_TYPE_TINY_BLOB ||
      param->buffer_type > MYSQL_TYPE_STRING)
  {
    /*
      Long data handling should be used only for string/binary
      types only
    */
    strmov(stmt->sqlstate, unknown_sqlstate);
    sprintf(stmt->last_error, ER(stmt->last_errno= CR_INVALID_BUFFER_USE),
	    param->param_number);
    DBUG_RETURN(1);
  }

  /* 
    Send long data packet if there is data or we're sending long data
    for the first time.
  */
  if (length || param->long_data_used == 0)
  {
    MYSQL *mysql= stmt->mysql;
    /* Packet header: stmt id (4 bytes), param no (2 bytes) */
    char buff[MYSQL_LONG_DATA_HEADER]; 
    
    int4store(buff, stmt->stmt_id);
    int2store(buff + 4, param_number);
    param->long_data_used= 1;

    /*
      Note that we don't get any ok packet from the server in this case
      This is intentional to save bandwidth.
    */
    if ((*mysql->methods->advanced_command)(mysql, COM_LONG_DATA, buff,
					    sizeof(buff), data, length, 1))
    {
      set_stmt_errmsg(stmt, mysql->net.last_error,
		      mysql->net.last_errno, mysql->net.sqlstate);
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}


/********************************************************************
  Fetch-bind related implementations
*********************************************************************/

/****************************************************************************
  Functions to fetch data to application buffers

  All functions have the following characteristics:

  SYNOPSIS
    fetch_result_xxx()
    param   MySQL bind param
    row     Row value

  RETURN VALUES
    0	ok
    1	Error	(Can't alloc net->buffer)
****************************************************************************/

static void set_zero_time(MYSQL_TIME *tm)
{
  tm->year= tm->month= tm->day= 0;
  tm->hour= tm->minute= tm->second= 0;
  tm->second_part= 0;
  tm->neg= (bool)0;
}

/* Read TIME from binary packet and return it to MYSQL_TIME */
static uint read_binary_time(MYSQL_TIME *tm, uchar **pos)
{
  uchar *to;
  uint  length;
 
  if (!(length= net_field_length(pos)))
  {
    set_zero_time(tm);
    return 0;
  }
  
  to= *pos;     
  tm->second_part= (length > 8 ) ? (ulong) sint4korr(to+7): 0;

  tm->day=    (ulong) sint4korr(to+1);
  tm->hour=   (uint) to[5];
  tm->minute= (uint) to[6];
  tm->second= (uint) to[7];

  tm->year= tm->month= 0;
  tm->neg= (bool)to[0];
  return length;
}

/* Read DATETIME from binary packet and return it to MYSQL_TIME */
static uint read_binary_datetime(MYSQL_TIME *tm, uchar **pos)
{
  uchar *to;
  uint  length;
 
  if (!(length= net_field_length(pos)))
  {
    set_zero_time(tm);
    return 0;
  }
  
  to= *pos;     
  tm->second_part= (length > 7 ) ? (ulong) sint4korr(to+7): 0;
    
  if (length > 4)
  {
    tm->hour=   (uint) to[4];
    tm->minute= (uint) to[5];
    tm->second= (uint) to[6];
  }
  else
    tm->hour= tm->minute= tm->second= 0;
    
  tm->year=   (uint) sint2korr(to);
  tm->month=  (uint) to[2];
  tm->day=    (uint) to[3];
  tm->neg=    0;
  return length;
}

/* Read DATE from binary packet and return it to MYSQL_TIME */
static uint read_binary_date(MYSQL_TIME *tm, uchar **pos)
{
  uchar *to;
  uint  length;
 
  if (!(length= net_field_length(pos)))
  {
    set_zero_time(tm);
    return 0;
  }
  
  to= *pos;     
  tm->year =  (uint) sint2korr(to);
  tm->month=  (uint) to[2];
  tm->day= (uint) to[3];

  tm->hour= tm->minute= tm->second= 0;
  tm->second_part= 0;
  tm->neg= 0;
  return length;
}

/* Convert Numeric to buffer types */
static void send_data_long(MYSQL_BIND *param, MYSQL_FIELD *field,
			   longlong value)
{  
  char *buffer= param->buffer;
  uint field_is_unsigned= (field->flags & UNSIGNED_FLAG);

  switch (param->buffer_type) {
  case MYSQL_TYPE_NULL: /* do nothing */
    break;
  case MYSQL_TYPE_TINY:
    *param->buffer= (uchar) value;
    break;
  case MYSQL_TYPE_SHORT:
    int2store(buffer, value);
    break;
  case MYSQL_TYPE_LONG:
    int4store(buffer, value);
    break;
  case MYSQL_TYPE_LONGLONG:
    int8store(buffer, value);
    break;
  case MYSQL_TYPE_FLOAT:
  {
    float data= (field_is_unsigned ? (float) ulonglong2double(value) :
		 (float) value);
    float4store(buffer, data);
    break;
  }
  case MYSQL_TYPE_DOUBLE:
  {
    double data= (field_is_unsigned ? ulonglong2double(value) :
		 (double) value);
    float8store(buffer, data);
    break;
  }
  default:
  {
    char tmp[22];				/* Enough for longlong */
    uint length= (uint)(longlong10_to_str(value,(char *)tmp,
					  field_is_unsigned ? 10: -10) -
			tmp);
    ulong copy_length= min((ulong)length-param->offset, param->buffer_length);
    if ((long) copy_length < 0)
      copy_length=0;
    else
      memcpy(buffer, (char *)tmp+param->offset, copy_length);
    *param->length= length;		
  
    if (copy_length != param->buffer_length)
      *(buffer+copy_length)= '\0';
  }
  } 
}


/* Convert Double to buffer types */

static void send_data_double(MYSQL_BIND *param, double value)
{  
  char *buffer= param->buffer;

  switch(param->buffer_type) {
  case MYSQL_TYPE_NULL: /* do nothing */
    break;
  case MYSQL_TYPE_TINY:
    *buffer= (uchar)value;
    break;
  case MYSQL_TYPE_SHORT:
    int2store(buffer, (short)value);
    break;
  case MYSQL_TYPE_LONG:
    int4store(buffer, (long)value);
    break;
  case MYSQL_TYPE_LONGLONG:
    int8store(buffer, (longlong)value);
    break;
  case MYSQL_TYPE_FLOAT:
  {
    float data= (float)value;
    float4store(buffer, data);
    break;
  }
  case MYSQL_TYPE_DOUBLE:
  {
    double data= (double)value;
    float8store(buffer, data);
    break;
  }
  default:
  {
    char tmp[128];
    uint length= my_sprintf(tmp,(tmp,"%g",value));
    ulong copy_length= min((ulong)length-param->offset, param->buffer_length);
    if ((long) copy_length < 0)
      copy_length=0;
    else
      memcpy(buffer, (char *)tmp+param->offset, copy_length);
    *param->length= length;		
  
    if (copy_length != param->buffer_length)
      *(buffer+copy_length)= '\0';
  }
  } 
}


/* Convert string to buffer types */

static void send_data_str(MYSQL_BIND *param, char *value, uint length)
{  
  char *buffer= param->buffer;
  int err=0;

  switch(param->buffer_type) {
  case MYSQL_TYPE_NULL: /* do nothing */
    break;
  case MYSQL_TYPE_TINY:
  {
    uchar data= (uchar)my_strntol(&my_charset_latin1,value,length,10,NULL,
				  &err);
    *buffer= data;
    break;
  }
  case MYSQL_TYPE_SHORT:
  {
    short data= (short)my_strntol(&my_charset_latin1,value,length,10,NULL,
				  &err);
    int2store(buffer, data);
    break;
  }
  case MYSQL_TYPE_LONG:
  {
    int32 data= (int32)my_strntol(&my_charset_latin1,value,length,10,NULL,
				  &err);
    int4store(buffer, data);    
    break;
  }
  case MYSQL_TYPE_LONGLONG:
  {
    longlong data= my_strntoll(&my_charset_latin1,value,length,10,NULL,&err);
    int8store(buffer, data);
    break;
  }
  case MYSQL_TYPE_FLOAT:
  {
    float data = (float)my_strntod(&my_charset_latin1,value,length,NULL,&err);
    float4store(buffer, data);
    break;
  }
  case MYSQL_TYPE_DOUBLE:
  {
    double data= my_strntod(&my_charset_latin1,value,length,NULL,&err);
    float8store(buffer, data);
    break;
  }
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_BLOB:
    *param->length= length;
    length= min(length-param->offset, param->buffer_length);
    if ((long) length > 0)
      memcpy(buffer, value+param->offset, length);
    break;
  default:
    *param->length= length;
    length= min(length-param->offset, param->buffer_length);
    if ((long) length < 0)
      length= 0;
    else
      memcpy(buffer, value+param->offset, length);
    if (length != param->buffer_length)
      buffer[length]= '\0';
  }
}


static void send_data_time(MYSQL_BIND *param, MYSQL_TIME ltime, 
                           uint length)
{
  switch (param->buffer_type) {
  case MYSQL_TYPE_NULL: /* do nothing */
    break;

  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
  {
    MYSQL_TIME *tm= (MYSQL_TIME *)param->buffer;
    
    tm->year= ltime.year;
    tm->month= ltime.month;
    tm->day= ltime.day;

    tm->hour= ltime.hour;
    tm->minute= ltime.minute;
    tm->second= ltime.second;

    tm->second_part= ltime.second_part;
    tm->neg= ltime.neg;
    break;   
  }
  default:
  {
    char buff[25];
    
    if (!length)
      ltime.time_type= MYSQL_TIMESTAMP_NONE;
    switch (ltime.time_type) {
    case MYSQL_TIMESTAMP_DATE:
      length= my_sprintf(buff,(buff, "%04d-%02d-%02d", ltime.year,
			       ltime.month,ltime.day));      
      break;
    case MYSQL_TIMESTAMP_FULL:
      length= my_sprintf(buff,(buff, "%04d-%02d-%02d %02d:%02d:%02d",
	                       ltime.year,ltime.month,ltime.day,
	                       ltime.hour,ltime.minute,ltime.second));
      break;
    case MYSQL_TIMESTAMP_TIME:
      length= my_sprintf(buff, (buff, "%02d:%02d:%02d",
				ltime.hour,ltime.minute,ltime.second));
      break;
    default:
      length= 0;
      buff[0]='\0';
    }
    send_data_str(param, (char *)buff, length); 
  }
  }
}
                              

/* Fetch data to buffers */

static void fetch_results(MYSQL_BIND *param, MYSQL_FIELD *field, uchar **row)
{
  ulong length;
  enum enum_field_types field_type= field->type;

  switch (field_type) {
  case MYSQL_TYPE_TINY:
  {
    char value= (char) **row;
    uint field_is_unsigned= (field->flags & UNSIGNED_FLAG);
    longlong data= ((field_is_unsigned) ? (longlong) (unsigned char) value:
		    (longlong) value);
    send_data_long(param, field, data);
    length= 1;
    break;
  }
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_YEAR:
  {
    short value= sint2korr(*row);
    uint field_is_unsigned= (field->flags & UNSIGNED_FLAG);
    longlong data= ((field_is_unsigned) ? (longlong) (unsigned short) value:
		    (longlong) value);
    send_data_long(param, field, data);
    length= 2;    
    break;
  }
  case MYSQL_TYPE_LONG:
  {
    long value= sint4korr(*row);
    uint field_is_unsigned= (field->flags & UNSIGNED_FLAG);
    longlong data= ((field_is_unsigned) ? (longlong) (unsigned long) value:
		    (longlong) value);
    send_data_long(param, field, data);
    length= 4;
    break;
  }
  case MYSQL_TYPE_LONGLONG:
  {
    longlong value= (longlong)sint8korr(*row);
    send_data_long(param, field, value);
    length= 8;
    break;
  }
  case MYSQL_TYPE_FLOAT:
  {
    float value;
    float4get(value,*row);
    send_data_double(param,value);
    length= 4;
    break;
  }
  case MYSQL_TYPE_DOUBLE:
  {
    double value;
    float8get(value,*row);
    send_data_double(param,value);
    length= 8;
    break;
  }
  case MYSQL_TYPE_DATE:
  {
    MYSQL_TIME tm;
 
    length= read_binary_date(&tm, row);
    tm.time_type= MYSQL_TIMESTAMP_DATE;
    send_data_time(param, tm, length);
    break;
  }
  case MYSQL_TYPE_TIME:
  {
    MYSQL_TIME tm;
 
    length= read_binary_time(&tm, row);
    tm.time_type= MYSQL_TIMESTAMP_TIME;
    send_data_time(param, tm, length);
    break;
  }
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIMESTAMP:
  {
    MYSQL_TIME tm;
 
    length= read_binary_datetime(&tm, row);
    tm.time_type= MYSQL_TIMESTAMP_FULL;
    send_data_time(param, tm, length);
    break;
  }
  default:      
    length= net_field_length(row); 
    send_data_str(param,(char*) *row,length);
    break;
  }
  *row+= length;
}


static void fetch_result_tinyint(MYSQL_BIND *param, uchar **row)
{
  *param->buffer= **row;
  (*row)++;
}

static void fetch_result_short(MYSQL_BIND *param, uchar **row)
{
  short value = (short)sint2korr(*row);
  shortstore(param->buffer, value);
  *row+= 2;
}

static void fetch_result_int32(MYSQL_BIND *param, uchar **row)
{
  int32 value= (int32)sint4korr(*row);
  longstore(param->buffer, value);
  *row+= 4;
}

static void fetch_result_int64(MYSQL_BIND *param, uchar **row)
{  
  longlong value= (longlong)sint8korr(*row);
  longlongstore(param->buffer, value);
  *row+= 8;
}

static void fetch_result_float(MYSQL_BIND *param, uchar **row)
{
  float value;
  float4get(value,*row);
  floatstore(param->buffer, value);
  *row+= 4;
}

static void fetch_result_double(MYSQL_BIND *param, uchar **row)
{
  double value;
  float8get(value,*row);
  doublestore(param->buffer, value);
  *row+= 8;
}

static void fetch_result_time(MYSQL_BIND *param, uchar **row)
{
  MYSQL_TIME *tm= (MYSQL_TIME *)param->buffer;
  *row+= read_binary_time(tm, row);
}

static void fetch_result_date(MYSQL_BIND *param, uchar **row)
{
  MYSQL_TIME *tm= (MYSQL_TIME *)param->buffer;
  *row+= read_binary_date(tm, row);
}

static void fetch_result_datetime(MYSQL_BIND *param, uchar **row)
{
  MYSQL_TIME *tm= (MYSQL_TIME *)param->buffer;
  *row+= read_binary_datetime(tm, row);
}

static void fetch_result_bin(MYSQL_BIND *param, uchar **row)
{  
  ulong length= net_field_length(row);
  ulong copy_length= min(length, param->buffer_length);
  memcpy(param->buffer, (char *)*row, copy_length);
  *param->length= length;		
  *row+= length;
}

static void fetch_result_str(MYSQL_BIND *param, uchar **row)
{
  ulong length= net_field_length(row);
  ulong copy_length= min(length, param->buffer_length);
  memcpy(param->buffer, (char *)*row, copy_length);
  /* Add an end null if there is room in the buffer */
  if (copy_length != param->buffer_length)
    *(param->buffer+copy_length)= '\0';
  *param->length= length;			/* return total length */
  *row+= length;
}


/*
  functions to calculate max lengths for strings during
  mysql_stmt_store_result()
*/

static void skip_result_fixed(MYSQL_BIND *param,
			      MYSQL_FIELD *field __attribute__((unused)),
			      uchar **row)

{
  (*row)+= param->pack_length;
}


static void skip_result_with_length(MYSQL_BIND *param __attribute__((unused)),
				    MYSQL_FIELD *field __attribute__((unused)),
				    uchar **row)

{
  ulong length= net_field_length(row);
  (*row)+= length;
}


static void skip_result_string(MYSQL_BIND *param __attribute__((unused)),
			       MYSQL_FIELD *field,
			       uchar **row)

{
  ulong length= net_field_length(row);
  (*row)+= length;
  if (field->max_length < length)
    field->max_length= length;
}



/*
  Setup the bind buffers for resultset processing
*/

my_bool STDCALL mysql_stmt_bind_result(MYSQL_STMT *stmt, MYSQL_BIND *bind)
{
  MYSQL_BIND *param, *end;
  MYSQL_FIELD *field;
  ulong       bind_count;
  uint        param_count= 0;
  DBUG_ENTER("mysql_stmt_bind_result");
  DBUG_ASSERT(stmt != 0);

  if (!stmt->field_count)
  {
    if ((int) stmt->state < (int) MYSQL_STMT_PREPARE_DONE)
    {
      set_stmt_error(stmt, CR_NO_PREPARE_STMT, unknown_sqlstate);
    }
    DBUG_RETURN(0);
  }
  bind_count= stmt->field_count;

  /*
    We only need to check that stmt->field_count - if it is not null
    stmt->bind was initialized in mysql_stmt_prepare
   */
  
  memcpy((char*) stmt->bind, (char*) bind, sizeof(MYSQL_BIND) * bind_count);

  for (param= stmt->bind, end= param + bind_count, field= stmt->fields ;
       param < end ;
       param++, field++)
  {
    /*
      Set param->is_null to point to a dummy variable if it's not set.
      This is to make the execute code easier
    */
    if (!param->is_null)
      param->is_null= &param->internal_is_null;

    if (!param->length)
      param->length= &param->internal_length;

    param->param_number= param_count++;
    param->offset= 0;

    /* Setup data copy functions for the different supported types */
    switch (param->buffer_type) {
    case MYSQL_TYPE_NULL: /* for dummy binds */
      *param->length= 0;
      break;
    case MYSQL_TYPE_TINY:
      param->fetch_result= fetch_result_tinyint;
      *param->length= 1;
      break;
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_YEAR:
      param->fetch_result= fetch_result_short;
      *param->length= 2;
      break;
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      param->fetch_result= fetch_result_int32;
      *param->length= 4;
      break;
    case MYSQL_TYPE_LONGLONG:
      param->fetch_result= fetch_result_int64;
      *param->length= 8;
      break;
    case MYSQL_TYPE_FLOAT:
      param->fetch_result= fetch_result_float;
      *param->length= 4;
      break;
    case MYSQL_TYPE_DOUBLE:
      param->fetch_result= fetch_result_double;
      *param->length= 8;
      break;
    case MYSQL_TYPE_TIME:
      param->fetch_result= fetch_result_time;
      *param->length= sizeof(MYSQL_TIME);
      break;
    case MYSQL_TYPE_DATE:
      param->fetch_result= fetch_result_date;
      *param->length= sizeof(MYSQL_TIME);
      break;
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      param->fetch_result= fetch_result_datetime;
      *param->length= sizeof(MYSQL_TIME);
      break;
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
      DBUG_ASSERT(param->buffer_length != 0);
      param->fetch_result= fetch_result_bin;
      break;
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
      DBUG_ASSERT(param->buffer_length != 0);
      param->fetch_result= fetch_result_str;
      break;
    default:
      strmov(stmt->sqlstate, unknown_sqlstate);
      sprintf(stmt->last_error,
	      ER(stmt->last_errno= CR_UNSUPPORTED_PARAM_TYPE),
	      param->buffer_type, param_count);
      DBUG_RETURN(1);
    }

    /* Setup skip_result functions (to calculate max_length) */
    param->skip_result= skip_result_fixed;
    switch (field->type) {
    case MYSQL_TYPE_NULL: /* for dummy binds */
      param->pack_length= 0;
      break;
    case MYSQL_TYPE_TINY:
      param->pack_length= 1;
      break;
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_SHORT:
      param->pack_length= 2;
      break;
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      param->pack_length= 4;
      break;
    case MYSQL_TYPE_LONGLONG:
      param->pack_length= 8;
      break;
    case MYSQL_TYPE_FLOAT:
      param->pack_length= 4;
      break;
    case MYSQL_TYPE_DOUBLE:
      param->pack_length= 8;
      break;
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      param->skip_result= skip_result_with_length;
      break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
      param->skip_result= skip_result_string;
      break;
    default:
      strmov(stmt->sqlstate, unknown_sqlstate);
      sprintf(stmt->last_error,
	      ER(stmt->last_errno= CR_UNSUPPORTED_PARAM_TYPE),
	      field->type, param_count);
      DBUG_RETURN(1);
    }
  }
  stmt->bind_result_done= TRUE;
  DBUG_RETURN(0);
}


/*
  Fetch row data to bind buffers
*/

static int stmt_fetch_row(MYSQL_STMT *stmt, uchar *row)
{
  MYSQL_BIND  *bind, *end;
  MYSQL_FIELD *field;
  uchar *null_ptr, bit;
  /*
    Precondition: if stmt->field_count is zero or row is NULL, read_row_*
    function must return no data.
  */
  DBUG_ASSERT(stmt->field_count);
  DBUG_ASSERT(row);

  if (!stmt->bind_result_done)
  {
    /* If output parameters were not bound we should just return success */
    return 0;
  }
  
  null_ptr= row; 
  row+= (stmt->field_count+9)/8;		/* skip null bits */
  bit= 4;					/* first 2 bits are reserved */
  
  /* Copy complete row to application buffers */
  for (bind= stmt->bind, end= bind + stmt->field_count, field= stmt->fields ;
       bind < end ;
       bind++, field++)
  {         
    if (*null_ptr & bit)
    {
      /*
        We should set both inter_buffer and is_null to be able to see
        nulls in mysql_stmt_fetch_column. This is because is_null may point
        to user data which can be overwritten between mysql_stmt_fetch and
        mysql_stmt_fetch_column, and in this case nullness of column will be
        lost. See mysql_stmt_fetch_column for details.
      */
      bind->inter_buffer= NULL;
      *bind->is_null= 1;
    }
    else
    { 
      *bind->is_null= 0;
      bind->inter_buffer= row;
      if (field->type == bind->buffer_type)
        (*bind->fetch_result)(bind, &row);
      else 
        fetch_results(bind, field, &row);
    }
    if (!((bit<<=1) & 255))
    {
      bit= 1;					/* To next byte */
      null_ptr++;
    }
  }
  return 0;
}


int cli_unbuffered_fetch(MYSQL *mysql, char **row)
{
  if (packet_error == net_safe_read(mysql))
    return 1;

  *row= ((mysql->net.read_pos[0] == 254) ? NULL :
	 (char*) (mysql->net.read_pos+1));
  return 0;
}


/*
  Fetch and return row data to bound buffers, if any
*/

int STDCALL mysql_stmt_fetch(MYSQL_STMT *stmt)
{
  int rc;
  uchar *row;
  DBUG_ENTER("mysql_stmt_fetch");

  if ((rc= (*stmt->read_row_func)(stmt, &row)) ||
      (rc= stmt_fetch_row(stmt, row)))
  {
    stmt->state= MYSQL_STMT_PREPARE_DONE;       /* XXX: this is buggy */
    stmt->read_row_func= stmt_read_row_no_data;
  }
  else
  {
    /* This is to know in mysql_stmt_fetch_column that data was fetched */
    stmt->state= MYSQL_STMT_FETCH_DONE;
  }
  DBUG_RETURN(rc);
}


/*
  Fetch data for one specified column data

  SYNOPSIS
    mysql_stmt_fetch_column()
    stmt		Prepared statement handler
    bind		Where data should be placed. Should be filled in as
			when calling mysql_stmt_bind_result()
    column		Column to fetch (first column is 0)
    ulong offset	Offset in result data (to fetch blob in pieces)
			This is normally 0
  RETURN 
    0	ok
    1	error
*/

int STDCALL mysql_stmt_fetch_column(MYSQL_STMT *stmt, MYSQL_BIND *bind, 
                                    uint column, ulong offset)
{
  MYSQL_BIND *param= stmt->bind+column; 
  DBUG_ENTER("mysql_stmt_fetch_column");

  if ((int) stmt->state < (int) MYSQL_STMT_FETCH_DONE)
  {
    set_stmt_error(stmt, CR_NO_DATA, unknown_sqlstate);
    return 1;
  }
  if (column >= stmt->field_count)
  {
    set_stmt_error(stmt, CR_INVALID_PARAMETER_NO, unknown_sqlstate);
    DBUG_RETURN(1);
  }

  if (param->inter_buffer)
  {
    MYSQL_FIELD *field= stmt->fields+column; 
    uchar *row= param->inter_buffer;
    bind->offset= offset;
    if (bind->is_null)
      *bind->is_null= 0;
    if (bind->length) /* Set the length if non char/binary types */
      *bind->length= *param->length;
    else
      bind->length= &param->internal_length;	/* Needed for fetch_result() */
    fetch_results(bind, field, &row);
  }
  else
  {
    if (bind->is_null)
      *bind->is_null= 1;
  }
  DBUG_RETURN(0);
}


/*
  Read all rows of data from server  (binary format)
*/

int cli_read_binary_rows(MYSQL_STMT *stmt)
{
  ulong      pkt_len;
  uchar      *cp;
  MYSQL      *mysql= stmt->mysql;
  MYSQL_DATA *result= &stmt->result;
  MYSQL_ROWS *cur, **prev_ptr= &result->data;
  NET        *net = &mysql->net;
  DBUG_ENTER("cli_read_binary_rows");

  mysql= mysql->last_used_con;

  while ((pkt_len= net_safe_read(mysql)) != packet_error)
  {
    cp= net->read_pos;
    if (cp[0] != 254 || pkt_len >= 8)
    {
      if (!(cur= (MYSQL_ROWS*) alloc_root(&result->alloc,
                                          sizeof(MYSQL_ROWS) + pkt_len - 1)))
      {
        set_stmt_error(stmt, CR_OUT_OF_MEMORY, unknown_sqlstate);
        goto err;
      }
      cur->data= (MYSQL_ROW) (cur+1);
      *prev_ptr= cur;
      prev_ptr= &cur->next;
      memcpy((char *) cur->data, (char *) cp+1, pkt_len-1);
      cur->length= pkt_len;		/* To allow us to do sanity checks */
      result->rows++;
    }
    else
    {
      /* end of data */
      *prev_ptr= 0;
      mysql->warning_count= uint2korr(cp+1);
      mysql->server_status= uint2korr(cp+3);
      DBUG_RETURN(0);
    }
  }
  set_stmt_errmsg(stmt, net->last_error, net->last_errno, net->sqlstate);

err:
  DBUG_RETURN(1);
}


/*
  Store or buffer the binary results to stmt
*/

int STDCALL mysql_stmt_store_result(MYSQL_STMT *stmt)
{
  MYSQL *mysql= stmt->mysql;
  MYSQL_DATA *result= &stmt->result;
  DBUG_ENTER("mysql_stmt_store_result");

  mysql= mysql->last_used_con;

  if (!stmt->field_count)
    DBUG_RETURN(0);
  if ((int) stmt->state < (int) MYSQL_STMT_EXECUTE_DONE ||
      mysql->status != MYSQL_STATUS_GET_RESULT)
  {
    set_stmt_error(stmt, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
    DBUG_RETURN(1);
  }
  if (result->data)
  {
    free_root(&result->alloc, MYF(MY_KEEP_PREALLOC));
    result->data= NULL;
    result->rows= 0;
    stmt->data_cursor= NULL;
  }

  if (stmt->update_max_length && !stmt->bind_result_done)
  {
    /*
      We must initalize the bind structure to be able to calculate
      max_length
    */
    MYSQL_BIND  *bind, *end;
    MYSQL_FIELD *field;
    bzero((char*) stmt->bind, sizeof(*stmt->bind)* stmt->field_count);

    for (bind= stmt->bind, end= bind + stmt->field_count, field= stmt->fields;
	 bind < end ;
	 bind++, field++)
    {
      bind->buffer_type= field->type;
      bind->buffer_length=1;
    }

    mysql_stmt_bind_result(stmt, stmt->bind);
    stmt->bind_result_done= 0;			/* No normal bind done */
  }

  if ((*mysql->methods->read_binary_rows)(stmt))
  {
    free_root(&result->alloc, MYF(MY_KEEP_PREALLOC));
    result->data= NULL;
    result->rows= 0;
    DBUG_RETURN(1);
  }

  if (stmt->update_max_length)
  {
    MYSQL_ROWS *cur= result->data;
    for(; cur; cur=cur->next)
      stmt_update_metadata(stmt, cur);
  }

  stmt->data_cursor= result->data;
  mysql->affected_rows= stmt->affected_rows= result->rows;
  stmt->read_row_func= stmt_read_row_buffered;
  mysql->unbuffered_fetch_owner= 0;             /* set in stmt_execute */
  mysql->status= MYSQL_STATUS_READY;		/* server is ready */
  DBUG_RETURN(0); /* Data buffered, must be fetched with mysql_stmt_fetch() */
}


/*
  Seek to desired row in the statement result set
*/

MYSQL_ROW_OFFSET STDCALL
mysql_stmt_row_seek(MYSQL_STMT *stmt, MYSQL_ROW_OFFSET row)
{
  MYSQL_ROW_OFFSET offset= stmt->data_cursor;
  DBUG_ENTER("mysql_stmt_row_seek");
  
  stmt->data_cursor= row;
  DBUG_RETURN(offset);
}


/*
  Return the current statement row cursor position
*/

MYSQL_ROW_OFFSET STDCALL 
mysql_stmt_row_tell(MYSQL_STMT *stmt)
{
  DBUG_ENTER("mysql_stmt_row_tell");
  
  DBUG_RETURN(stmt->data_cursor);
}


/*
  Move the stmt result set data cursor to specified row
*/

void STDCALL
mysql_stmt_data_seek(MYSQL_STMT *stmt, my_ulonglong row)
{
  MYSQL_ROWS *tmp= stmt->result.data;
  DBUG_ENTER("mysql_stmt_data_seek");
  DBUG_PRINT("enter",("row id to seek: %ld",(long) row));
  
  for (; tmp && row; --row, tmp= tmp->next)
    ;
  stmt->data_cursor= tmp;
  DBUG_VOID_RETURN;
}


/*
  Return total rows the current statement result set
*/

my_ulonglong STDCALL mysql_stmt_num_rows(MYSQL_STMT *stmt)
{
  DBUG_ENTER("mysql_stmt_num_rows");
    
  DBUG_RETURN(stmt->result.rows);
}

my_bool STDCALL mysql_stmt_free_result(MYSQL_STMT *stmt)
{
  MYSQL_DATA *result= &stmt->result;
  DBUG_ENTER("mysql_stmt_free_result");

  DBUG_ASSERT(stmt != 0);
  
  if ((int) stmt->state > (int) MYSQL_STMT_INIT_DONE)
  {
    MYSQL *mysql= stmt->mysql;

    if (result->data)
    {
      /* Result buffered */
      free_root(&result->alloc, MYF(MY_KEEP_PREALLOC));
      result->data= NULL;
      result->rows= 0;
      stmt->data_cursor= NULL;
    }

    if (mysql && stmt->field_count &&
        (int) stmt->state > (int) MYSQL_STMT_PREPARE_DONE)
    {
      if (mysql->unbuffered_fetch_owner == &stmt->unbuffered_fetch_cancelled)
        mysql->unbuffered_fetch_owner= 0;
      if (mysql->status != MYSQL_STATUS_READY)
      {
        /* There is a result set and it belongs to this statement */
        flush_use_result(mysql);
        mysql->status= MYSQL_STATUS_READY;
      }
    }
    stmt->state= MYSQL_STMT_PREPARE_DONE;
    stmt->read_row_func= stmt_read_row_no_data;
  }
  DBUG_RETURN(0);
}

/********************************************************************
 statement error handling and close
*********************************************************************/

/*
  Close the statement handle by freeing all alloced resources

  SYNOPSIS
    mysql_stmt_close()
    stmt	       Statement handle

  RETURN VALUES
    0	ok
    1	error
*/

my_bool STDCALL mysql_stmt_close(MYSQL_STMT *stmt)
{
  MYSQL *mysql= stmt->mysql;
  int rc= 0;
  DBUG_ENTER("mysql_stmt_close");

  free_root(&stmt->result.alloc, MYF(0));
  free_root(&stmt->mem_root, MYF(0));

  if (mysql)
  {
    mysql->stmts= list_delete(mysql->stmts, &stmt->list);
    if ((int) stmt->state > (int) MYSQL_STMT_INIT_DONE)
    {
      char buff[MYSQL_STMT_HEADER];             /* 4 bytes - stmt id */

      if (mysql->unbuffered_fetch_owner == &stmt->unbuffered_fetch_cancelled)
        mysql->unbuffered_fetch_owner= 0;
      if (mysql->status != MYSQL_STATUS_READY)
      {
        /* 
          Flush result set of the connection. If it does not belong
          to this statement, set a warning.
        */
        flush_use_result(mysql);
        if (mysql->unbuffered_fetch_owner)
          *mysql->unbuffered_fetch_owner= TRUE;
        mysql->status= MYSQL_STATUS_READY;
      }
      int4store(buff, stmt->stmt_id);
      if ((rc= simple_command(mysql, COM_CLOSE_STMT, buff, 4, 1)))
      {
        set_stmt_errmsg(stmt, mysql->net.last_error, mysql->net.last_errno,
                        mysql->net.sqlstate);
      }
    }
  }

  my_free((gptr) stmt, MYF(MY_WME));

  DBUG_RETURN(test(rc));
}

/*
  Reset the statement buffers in server
*/

my_bool STDCALL mysql_stmt_reset(MYSQL_STMT *stmt)
{
  char buff[MYSQL_STMT_HEADER]; /* packet header: 4 bytes for stmt id */
  MYSQL *mysql;
  MYSQL_BIND *param, *param_end;
  DBUG_ENTER("mysql_stmt_reset");
  DBUG_ASSERT(stmt != 0);

  /* If statement hasnt been prepared there is nothing to reset */
  if ((int) stmt->state < (int) MYSQL_STMT_PREPARE_DONE)
    DBUG_RETURN(0);
  
  mysql= stmt->mysql->last_used_con;
  int4store(buff, stmt->stmt_id);		/* Send stmt id to server */
  if ((*mysql->methods->advanced_command)(mysql, COM_RESET_STMT, buff,
                                          sizeof(buff), 0, 0, 0))
  {
    set_stmt_errmsg(stmt, mysql->net.last_error, mysql->net.last_errno, 
                    mysql->net.sqlstate);
    DBUG_RETURN(1);
  }

  /* Clear long_data_used for next call (as we do in mysql_stmt_execute() */
  for (param= stmt->params, param_end= param + stmt->param_count;
       param < param_end;
       param++)
    param->long_data_used= 0;

  DBUG_RETURN(0);
}

/*
  Return statement error code
*/

uint STDCALL mysql_stmt_errno(MYSQL_STMT * stmt)
{
  DBUG_ENTER("mysql_stmt_errno");
  DBUG_RETURN(stmt->last_errno);
}

const char *STDCALL mysql_stmt_sqlstate(MYSQL_STMT * stmt)
{
  DBUG_ENTER("mysql_stmt_sqlstate");
  DBUG_RETURN(stmt->sqlstate);
}

/*
  Return statement error message
*/

const char *STDCALL mysql_stmt_error(MYSQL_STMT * stmt)
{
  DBUG_ENTER("mysql_stmt_error");
  DBUG_RETURN(stmt->last_error);
}


/*
  Update meta data for statement

  SYNOPSIS
    stmt_update_metadata()
    stmt			Statement handler
    row				Binary data

  NOTES
    Only updates MYSQL_FIELD->max_length for strings

*/

static void stmt_update_metadata(MYSQL_STMT *stmt, MYSQL_ROWS *data)
{
  MYSQL_BIND  *bind, *end;
  MYSQL_FIELD *field;
  uchar *null_ptr, bit;
  uchar *row= (uchar*) data->data;
#ifndef DBUG_OFF
  uchar *row_end= row + data->length;
#endif

  null_ptr= row; 
  row+= (stmt->field_count+9)/8;		/* skip null bits */
  bit= 4;					/* first 2 bits are reserved */
  
  /* Go throw all fields and calculate metadata */
  for (bind= stmt->bind, end= bind + stmt->field_count, field= stmt->fields ;
       bind < end ;
       bind++, field++)
  {
    if (!(*null_ptr & bit))
      (*bind->skip_result)(bind, field, &row);
    DBUG_ASSERT(row <= row_end);
    if (!((bit<<=1) & 255))
    {
      bit= 1;					/* To next byte */
      null_ptr++;
    }
  }
}


/********************************************************************
 Transactional APIs
*********************************************************************/

/*
  Commit the current transaction
*/

my_bool STDCALL mysql_commit(MYSQL * mysql)
{
  DBUG_ENTER("mysql_commit");
  DBUG_RETURN((my_bool) mysql_real_query(mysql, "commit", 6));
}

/*
  Rollback the current transaction
*/

my_bool STDCALL mysql_rollback(MYSQL * mysql)
{
  DBUG_ENTER("mysql_rollback");
  DBUG_RETURN((my_bool) mysql_real_query(mysql, "rollback", 8));
}


/*
  Set autocommit to either true or false
*/

my_bool STDCALL mysql_autocommit(MYSQL * mysql, my_bool auto_mode)
{
  DBUG_ENTER("mysql_autocommit");
  DBUG_PRINT("enter", ("mode : %d", auto_mode));

  if (auto_mode) /* set to true */
    DBUG_RETURN((my_bool) mysql_real_query(mysql, "set autocommit=1", 16));
  DBUG_RETURN((my_bool) mysql_real_query(mysql, "set autocommit=0", 16));
}


/********************************************************************
 Multi query execution + SPs APIs
*********************************************************************/

/*
  Returns true/false to indicate whether any more query results exist
  to be read using mysql_next_result()
*/

my_bool STDCALL mysql_more_results(MYSQL *mysql)
{
  my_bool res;
  DBUG_ENTER("mysql_more_results");
  
  res= ((mysql->last_used_con->server_status & SERVER_MORE_RESULTS_EXISTS) ? 
	1: 0);
  DBUG_PRINT("exit",("More results exists ? %d", res)); 
  DBUG_RETURN(res);
}


/*
  Reads and returns the next query results
*/
int STDCALL mysql_next_result(MYSQL *mysql)
{
  DBUG_ENTER("mysql_next_result");
  
  if (mysql->status != MYSQL_STATUS_READY)
  {
    strmov(mysql->net.sqlstate, unknown_sqlstate);
    strmov(mysql->net.last_error,
	   ER(mysql->net.last_errno=CR_COMMANDS_OUT_OF_SYNC));
    DBUG_RETURN(1);
  }

  mysql->net.last_error[0]= 0;
  mysql->net.last_errno= 0;
  strmov(mysql->net.sqlstate, not_error_sqlstate);
  mysql->affected_rows= ~(my_ulonglong) 0;

  if (mysql->last_used_con->server_status & SERVER_MORE_RESULTS_EXISTS)
    DBUG_RETURN((*mysql->methods->next_result)(mysql));

  DBUG_RETURN(-1);				/* No more results */
}


MYSQL_RES * STDCALL mysql_use_result(MYSQL *mysql)
{
  return (*mysql->methods->use_result)(mysql);
}

my_bool STDCALL mysql_read_query_result(MYSQL *mysql)
{
  return (*mysql->methods->read_query_result)(mysql);
}

