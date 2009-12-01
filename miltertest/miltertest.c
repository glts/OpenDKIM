/*
**  Copyright (c) 2009, Murray S. Kucherawy.  All rights reserved.
**
**  $Id: miltertest.c,v 1.1.2.1 2009/12/01 18:31:24 cm-msk Exp $
*/

#ifndef lint
static char miltertest_c_id[] = "$Id: miltertest.c,v 1.1.2.1 2009/12/01 18:31:24 cm-msk Exp $";
#endif /* ! lint */

/* system includes */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sysexits.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <netdb.h>

/* libmilter includes */
#include <libmilter/mfapi.h>

/* Lua includes */
#include <lua.h>
#include <lualib.h>

/* macros */
#ifndef FALSE
# define FALSE			0
#endif /* ! FALSE */
#ifndef TRUE
# define TRUE			1
#endif /* ! TRUE */

#define	BUFRSZ			1024
#define	CHUNKSZ			65536

#define	CMDLINEOPTS		"D:s:v"

#define	DEFBODY			"Dummy message body.\r\n"
#define	DEFCLIENTPORT		12345
#define DEFCLIENTHOST		"test.example.com"
#define DEFCLIENTIP		"12.34.56.78"
#define	DEFHEADERNAME		"From"
#define DEFSENDER		"<sender@example.com>"
#define	DEFTIMEOUT		10
#define DEFRECIPIENT		"<recipient@example.com>"

#define	STATE_UNKNOWN		(-1)
#define	STATE_INIT		0
#define	STATE_NEGOTIATED	1
#define	STATE_CONNINFO		2
#define	STATE_HELO		3
#define	STATE_ENVFROM		4
#define	STATE_ENVRCPT		5
#define	STATE_HEADER		6
#define	STATE_EOH		7
#define	STATE_BODY		8
#define	STATE_EOM		9
#define	STATE_DEAD		99

#define MT_HDRADD		1
#define MT_HDRINSERT		2
#define MT_HDRCHANGE		3
#define MT_HDRDELETE		4
#define MT_RCPTADD		5
#define MT_RCPTDELETE		6
#define MT_BODYCHANGE		7
#define MT_QUARANTINE		8
#define MT_SMTPREPLY		9

/* data types */
struct mt_eom_request
{
	char		eom_request;		/* request code */
	size_t		eom_rlen;		/* request length */
	char *		eom_rdata;		/* request data */
	struct mt_eom_request * eom_next;	/* next request */
};

struct mt_context
{
	char		ctx_response;		/* milter response code */
	int		ctx_fd;			/* descriptor */
	int		ctx_state;		/* current state */
	struct mt_eom_request * ctx_eomreqs;	/* EOM requests */
};

struct mt_lua_io
{
	_Bool		lua_io_done;
	const char *	lua_io_script;
};

/* globals */
int verbose;
unsigned int tmo;
pid_t filterpid;
char scriptbuf[BUFRSZ];
char *progname;

/*
**  MT_LUA_READER -- "read" a script and make it available to Lua
**
**  Parameters:
**  	l -- Lua state
**  	data -- pointer to a Lua I/O structure
**  	size -- size (returned)
**
**  Return value:
**  	Pointer to the data.
*/

static const char *
mt_lua_reader(lua_State *l, void *data, size_t *size)
{
	struct mt_lua_io *io;

	assert(l != NULL);
	assert(data != NULL);
	assert(size != NULL);

	io = (struct mt_lua_io *) data;

	if (io->lua_io_done)
	{
		*size = 0;
		return NULL;
	}
	else if (io->lua_io_script != NULL)
	{
		io->lua_io_done = TRUE;
		*size = strlen(io->lua_io_script);
		return io->lua_io_script;
	}
	else
	{
		size_t rlen;

		memset(scriptbuf, '\0', sizeof scriptbuf);

		if (feof(stdin))
		{
			*size = 0;
			io->lua_io_done = TRUE;
			return NULL;
		}

		rlen = fread(scriptbuf, 1, sizeof scriptbuf, stdin);
		*size = rlen;
		return (const char *) scriptbuf;
	}
}

/*
**  MT_LUA_ALLOC -- allocate memory
**
**  Parameters:
**  	ud -- context (not used)
**  	ptr -- pointer (for realloc())
**  	osize -- old size
**  	nsize -- new size
**
**  Return value:
**  	Allocated memory, or NULL on failure.
*/

static void *
mt_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	if (nsize == 0 && osize != 0)
	{
		free(ptr);
		return NULL;
	}
	else if (nsize != 0 && osize == 0)
	{
		return malloc(nsize);
	}
	else
	{
		return realloc(ptr, nsize);
	}
}

/*
**  MT_FLUSH_EOMREQS -- free EOM requests
**
**  Parameters:
**  	ctx -- mt_context handle
**
**  Return value:
**  	None.
*/

void
mt_flush_eomreqs(struct mt_context *ctx)
{
	struct mt_eom_request *r;

	assert(ctx != NULL);

	while (ctx->ctx_eomreqs != NULL)
	{
		r = ctx->ctx_eomreqs;
		if (r->eom_rdata != NULL)
			free(r->eom_rdata);
		ctx->ctx_eomreqs = r->eom_next;
		free(r);
	}
}

/*
**  MT_EOM_REQUEST -- record a request received during EOM
**
**  Parameters:
**  	ctx -- mt_context handle
**  	cmd -- command received
**  	len -- length of data
**  	data -- data received (i.e. request parameters)
**
**  Return value:
**  	TRUE iff addition was successful.
*/

_Bool
mt_eom_request(struct mt_context *ctx, char cmd, size_t len, char *data)
{
	struct mt_eom_request *r;

	assert(ctx != NULL);

	r = (struct mt_eom_request *) malloc(sizeof *r);
	if (r == NULL)
		return FALSE;

	r->eom_request = cmd;
	r->eom_rlen = len;
	r->eom_rdata = malloc(len);
	if (r->eom_rdata == NULL)
	{
		free(r);
		return FALSE;
	}
	memcpy(r->eom_rdata, data, len);

	r->eom_next = ctx->ctx_eomreqs;
	ctx->ctx_eomreqs = r;

	return TRUE;
}

/*
**  MT_MILTER_READ -- read from a connected filter
**
**  Parameters:
**  	fd -- descriptor to which to write
**  	cmd -- milter command received (returned)
** 	buf -- where to write data
**  	buflen -- bytes available at "buf" (updated)
** 
**  Return value:
**  	TRUE iff successful.
*/

_Bool
mt_milter_read(int fd, char *cmd, const char *buf, size_t *len)
{
	int i;
	int expl;
	size_t rlen;
	fd_set fds;
	struct timeval timeout;
	char data[MILTER_LEN_BYTES + 1];

	assert(fd >= 0);

	FD_ZERO(&fds);
	FD_SET(fd, &fds);

	timeout.tv_sec = tmo;
	timeout.tv_usec = 0;

	i = select(fd + 1, &fds, NULL, NULL, &timeout);
	if (i == 0)
	{
		fprintf(stderr, "%s: select(): timeout on fd %d\n", progname,
		        fd);

		return FALSE;
	}
	else if (i == -1)
	{
		fprintf(stderr, "%s: select(): fd %d: %s\n", progname, fd,
		        strerror(errno));

		return FALSE;
	}

	rlen = read(fd, data, sizeof data);
	if (rlen != sizeof data)
	{
		fprintf(stderr, "%s: read(%d): returned %ld, expected %ld\n",
		        progname, fd, (long) rlen, (long) sizeof data);

		return FALSE;
	}

	*cmd = data[MILTER_LEN_BYTES];
	data[MILTER_LEN_BYTES] = '\0';
	(void) memcpy(&i, data, MILTER_LEN_BYTES);
	expl = ntohl(i) - 1;

	rlen = 0;

	if (expl > 0)
	{
		rlen = read(fd, (void *) buf, expl);
		if (rlen != expl)
		{
			fprintf(stderr,
			        "%s: read(%d): returned %ld, expected %ld\n",
			        progname, fd, (long) rlen, expl);

			return FALSE;
		}
	}

	if (verbose > 1)
	{
		fprintf(stdout, "%s: mt_milter_read(%d): cmd %c, len %ld\n",
		        progname, fd, *cmd, (long) rlen);
	}

	*len = rlen;

	return (expl == rlen);
}

/*
**  MT_MILTER_WRITE -- write to a connected filter
**
**  Parameters:
**  	fd -- descriptor to which to write
**  	cmd -- command to send (an SMFIC_* constant)
**  	buf -- command data (or NULL)
**  	len -- length of data at "buf"
**
**  Return value:
**  	TRUE iff successful.
*/

_Bool
mt_milter_write(int fd, int cmd, const char *buf, size_t len)
{
	char command = (char) cmd;
	ssize_t sl, i;
	int num_vectors;
	uint32_t nl;
	char data[MILTER_LEN_BYTES + 1];
	struct iovec vector[2];

	assert(fd >= 0);

	if (verbose > 1)
	{
		fprintf(stdout, "%s: mt_milter_write(%d): cmd %c, len %ld\n",
		        progname, fd, command, (long) len);
	}

	nl = htonl(len + 1);
	(void) memcpy(data, (char *) &nl, MILTER_LEN_BYTES);
	data[MILTER_LEN_BYTES] = command;
	sl = MILTER_LEN_BYTES + 1;

	/* set up the vector for the size / command */
	vector[0].iov_base = (void *) data;
	vector[0].iov_len  = sl;

	/*
	**  Determine if there is command data.  If so, there will be two
	**  vectors.  If not, there will be only one.  The vectors are set
	**  up here and 'num_vectors' and 'sl' are set appropriately.
	*/

	if (len <= 0 || buf == NULL)
	{
		num_vectors = 1;
	}
	else
	{
		num_vectors = 2;
		sl += len;
		vector[1].iov_base = (void *) buf;
		vector[1].iov_len  = len;
	}

	/* write the vector(s) */
	i = writev(fd, vector, num_vectors);
	if (i != sl)
	{
		fprintf(stderr, "%s: writev(%d): returned %ld, expected %ld\n",
		        progname, fd, (long) i, (long) sl);
	}

	return (i == sl);
}

/*
**  MT_ASSERT_STATE -- bring a connection up to a given state
**
**  Parameters:
**  	ctx -- miltertest context
**  	state -- desired state
**
**  Return value:
**  	TRUE if successful, FALSE otherwise.
*/

_Bool
mt_assert_state(struct mt_context *ctx, int state)
{
	size_t len;
	size_t s;
	uint16_t port;
	char buf[BUFRSZ];

	assert(ctx != NULL);

	if (state >= STATE_NEGOTIATED && ctx->ctx_state < STATE_NEGOTIATED)
	{
		char rcmd;
		size_t buflen;
		uint32_t mta_version;
		uint32_t mta_protoopts;
		uint32_t mta_actions;
		uint32_t nvers;
		uint32_t npopts;
		uint32_t nacts;

		buflen = sizeof buf;

		mta_version = SMFI_PROT_VERSION;
		mta_protoopts = SMFI_CURR_PROT;
		mta_actions = SMFI_CURR_ACTS;

		nvers = htonl(mta_version);
		nacts = htonl(mta_actions);
		npopts = htonl(mta_protoopts);

		(void) memcpy(buf, (char *) &nvers, MILTER_LEN_BYTES);
		(void) memcpy(buf + MILTER_LEN_BYTES,
		              (char *) &nacts, MILTER_LEN_BYTES);
		(void) memcpy(buf + (MILTER_LEN_BYTES * 2),
		              (char *) &npopts, MILTER_LEN_BYTES);

		if (!mt_milter_write(ctx->ctx_fd, SMFIC_OPTNEG, buf,
		                     MILTER_OPTLEN))
			return FALSE;

		if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
			return FALSE;

		if (rcmd != SMFIC_OPTNEG)
		{
			if (verbose > 0)
			{
				fprintf(stdout,
				        "%s: filter returned status %d to option negotiation on fd %d\n", 
				        progname, rcmd, ctx->ctx_fd);
			}

			ctx->ctx_state = STATE_DEAD;
			return FALSE;
		}

		ctx->ctx_state = STATE_NEGOTIATED;
	}

	if (state >= STATE_CONNINFO && ctx->ctx_state < STATE_CONNINFO)
	{
		char rcmd;
		size_t buflen;

		buflen = sizeof buf;

		port = htons(DEFCLIENTPORT);
		len = strlcpy(buf, DEFCLIENTHOST, sizeof buf);
		buf[len++] = '\0';
		buf[len++] = '4';		/* IPv4 only for now */
		memcpy(&buf[len], &port, sizeof port);
		len += sizeof port;
		memcpy(&buf[len], DEFCLIENTIP, strlen(DEFCLIENTIP) + 1);

		s = len + strlen(DEFCLIENTIP) + 1;

		if (!mt_milter_write(ctx->ctx_fd, SMFIC_CONNECT, buf, s))
			return FALSE;

		if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
			return FALSE;

		ctx->ctx_response = rcmd;

		if (rcmd != SMFIR_CONTINUE)
		{
			if (verbose > 0)
			{
				fprintf(stdout,
				        "%s: filter returned status %d to connection information on fd %d\n", 
				        progname, rcmd, ctx->ctx_fd);
			}

			ctx->ctx_state = STATE_DEAD;
		}

		ctx->ctx_state = STATE_CONNINFO;
	}

	if (state >= STATE_HELO && ctx->ctx_state < STATE_HELO)
	{
		char rcmd;
		size_t buflen;

		buflen = sizeof buf;

		len = strlcpy(buf, DEFCLIENTHOST, sizeof buf);
		buf[len++] = '\0';

		if (!mt_milter_write(ctx->ctx_fd, SMFIC_HELO, buf, len))
			return FALSE;

		if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
			return FALSE;

		ctx->ctx_response = rcmd;

		if (rcmd != SMFIR_CONTINUE)
		{
			if (verbose > 0)
			{
				fprintf(stdout,
				        "%s: filter returned status %d to HELO on fd %d\n", 
				        progname, rcmd, ctx->ctx_fd);
			}

			ctx->ctx_state = STATE_DEAD;
		}

		ctx->ctx_state = STATE_HELO;
	}

	if (state >= STATE_ENVFROM && ctx->ctx_state < STATE_ENVFROM)
	{
		char rcmd;
		size_t buflen;

		buflen = sizeof buf;

		len = strlcpy(buf, DEFSENDER, sizeof buf);
		buf[len++] = '\0';

		if (!mt_milter_write(ctx->ctx_fd, SMFIC_MAIL, buf, len))
			return FALSE;

		if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
			return FALSE;

		ctx->ctx_response = rcmd;

		if (rcmd != SMFIR_CONTINUE)
		{
			if (verbose > 0)
			{
				fprintf(stdout,
				        "%s: filter returned status %d to MAIL on fd %d\n", 
				        progname, rcmd, ctx->ctx_fd);
			}

			ctx->ctx_state = STATE_DEAD;
		}

		ctx->ctx_state = STATE_ENVFROM;
	}

	if (state >= STATE_ENVRCPT && ctx->ctx_state < STATE_ENVRCPT)
	{
		char rcmd;
		size_t buflen;

		buflen = sizeof buf;

		len = strlcpy(buf, DEFRECIPIENT, sizeof buf);
		buf[len++] = '\0';

		if (!mt_milter_write(ctx->ctx_fd, SMFIC_RCPT, buf, len))
			return FALSE;

		if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
			return FALSE;

		ctx->ctx_response = rcmd;

		if (rcmd != SMFIR_CONTINUE)
		{
			if (verbose > 0)
			{
				fprintf(stdout,
				        "%s: filter returned status %d to RCPT on fd %d\n", 
				        progname, rcmd, ctx->ctx_fd);
			}

			ctx->ctx_state = STATE_DEAD;
		}

		ctx->ctx_state = STATE_ENVRCPT;
	}

	if (state >= STATE_HEADER && ctx->ctx_state < STATE_HEADER)
	{
		char rcmd;
		size_t buflen;

		buflen = sizeof buf;

		len = strlcpy(buf, DEFHEADERNAME, sizeof buf);
		buf[len++] = '\0';
		len += strlcpy(buf + len, DEFSENDER, sizeof buf - len);
		buf[len++] = '\0';

		if (!mt_milter_write(ctx->ctx_fd, SMFIC_HEADER, buf, len))
			return FALSE;

		if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
			return FALSE;

		ctx->ctx_response = rcmd;

		if (rcmd != SMFIR_CONTINUE)
		{
			if (verbose > 0)
			{
				fprintf(stdout,
				        "%s: filter returned status %d to header on fd %d\n", 
				        progname, rcmd, ctx->ctx_fd);
			}

			ctx->ctx_state = STATE_DEAD;
		}

		ctx->ctx_state = STATE_HEADER;
	}

	if (state >= STATE_EOH && ctx->ctx_state < STATE_EOH)
	{
		char rcmd;
		size_t buflen;

		buflen = sizeof buf;

		if (!mt_milter_write(ctx->ctx_fd, SMFIC_EOH, NULL, 0))
			return FALSE;

		if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
			return FALSE;

		ctx->ctx_response = rcmd;

		if (rcmd != SMFIR_CONTINUE)
		{
			if (verbose > 0)
			{
				fprintf(stdout,
				        "%s: filter returned status %d to EOH on fd %d\n", 
				        progname, rcmd, ctx->ctx_fd);
			}

			ctx->ctx_state = STATE_DEAD;
		}

		ctx->ctx_state = STATE_EOH;
	}

	if (state >= STATE_BODY && ctx->ctx_state < STATE_BODY)
	{
		char rcmd;
		size_t buflen;

		buflen = sizeof buf;

		if (!mt_milter_write(ctx->ctx_fd, SMFIC_BODY, DEFBODY,
		                     strlen(DEFBODY)))
			return FALSE;

		if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
			return FALSE;

		ctx->ctx_response = rcmd;

		if (rcmd != SMFIR_CONTINUE)
		{
			if (verbose > 0)
			{
				fprintf(stdout,
				        "%s: filter returned status %d to body on fd %d\n", 
				        progname, rcmd, ctx->ctx_fd);
			}

			ctx->ctx_state = STATE_DEAD;
		}

		ctx->ctx_state = STATE_BODY;
	}

	return TRUE;
}

/*
**  MT_SET_TIMEOUT -- set read timeout
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_set_timeout(lua_State *l)
{
	assert(l != NULL);

	if (lua_gettop(l) != 1 || !lua_isnumber(l, 1))
	{
		lua_pushstring(l, "mt_set_timeout(): Invalid argument");
		lua_error(l);
	}

	tmo = (unsigned int) lua_tonumber(l, 1);
	lua_pop(l, 1);

	return 0;
}

/*
**  MT_STARTFILTER -- start a filter
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_startfilter(lua_State *l)
{
	const char **argv;
	int c;
	int status;
	int args;
	int fds[2];
	pid_t child;

	assert(l != NULL);

	args = lua_gettop(l);
	if (args < 1)
	{
		lua_pushstring(l, "mt_startfilter(): Invalid argument");
		lua_error(l);
	}

	for (c = 1; c <= args; c++)
	{
		if (!lua_isstring(l, c))
		{
			lua_pushstring(l,
			               "mt_startfilter(): Invalid argument");
			lua_error(l);
		}
	}

	argv = (const char **) malloc(sizeof(char *) * (args + 1));
	if (argv == NULL)
	{
		lua_pushfstring(l, "mt_startfilter(): malloc(): %s",
		                strerror(errno));
		lua_error(l);
	}

	for (c = 1; c <= args; c++)
		argv[c - 1] = lua_tostring(l, c);
	argv[c] = NULL;
	lua_pop(l, c);

	if (pipe(fds) != 0)
	{
		lua_pushfstring(l, "mt_startfilter(): pipe(): %s",
		                strerror(errno));
		lua_error(l);
	}

	if (fcntl(fds[1], F_SETFD, FD_CLOEXEC) != 0)
	{
		lua_pushfstring(l, "mt_startfilter(): fcntl(): %s",
		                strerror(errno));
		lua_error(l);
	}

	child = fork();
	switch (child)
	{
	  case -1:
		lua_pushfstring(l, "mt_startfilter(): fork(): %s",
		                strerror(errno));
		lua_error(l);

	  case 0:
		close(fds[0]);
		execv(argv[0], (char * const *) argv);
		exit(1);

	  default:
		close(fds[1]);

		c = read(fds[0], &args, sizeof(args));
		if (c == -1)
		{
			lua_pushfstring(l, "mt_startfilter(): read(): %s",
			                strerror(errno));
			lua_error(l);
		}
		else if (c != 0)
		{
			lua_pushfstring(l,
			                "mt_startfilter(): read(): got %d, expecting 0",
			                c);
			lua_error(l);
		}

		close(fds[0]);

		filterpid = child;

		child = wait4(filterpid, &status, WNOHANG, NULL);
		if (child != 0)
		{
			lua_pushfstring(l,
			                "mt_startfilter(): wait4(): child %d exited prematurely, status %d",
			                child, status);
			lua_error(l);
		}

		if (verbose > 0)
		{
			fprintf(stderr, "%s: `%s' started in process %d\n",
			        progname, argv[0], filterpid);
		}

		free((void *) argv);

		break;
	}

	lua_pushnil(l);

	return 1;
}

/*
**  MT_CONNECT -- connect to a filter, returning a handle
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	A new connection handle (on the Lua stack).
*/

int
mt_connect(lua_State *l)
{
	int af;
	int fd;
	char *at;
	char *p;
	const char *sockinfo;
	struct mt_context *new;

	assert(l != NULL);

	if (lua_gettop(l) != 1 ||
	    !lua_isstring(l, 1))
	{
		lua_pushstring(l, "mt_connect(): Invalid argument");
		lua_error(l);
	}

	sockinfo = lua_tostring(l, 1);
	lua_pop(l, 1);

	af = AF_UNSPEC;
	p = strchr(sockinfo, ':');
	if (p == NULL)
	{
		af = AF_UNIX;
	}
	else
	{
		*p = '\0';
		if (strcasecmp(sockinfo, "inet") == 0)
			af = AF_INET;
		else if (strcasecmp(sockinfo, "unix") == 0 ||
		         strcasecmp(sockinfo, "local") == 0)
			af = AF_UNIX;
		*p = ':';
	}

	if (af == AF_UNSPEC)
	{
		lua_pushstring(l, "mt_connect(): Invalid argument");
		lua_error(l);
	}

	switch (af)
	{
	  case AF_UNIX:
	  {
		struct sockaddr_un sa;

		memset(&sa, '\0', sizeof sa);
		sa.sun_family = AF_UNIX;
		sa.sun_len = sizeof sa;
		strlcpy(sa.sun_path, p + 1, sizeof sa.sun_path);

		fd = socket(PF_UNIX, SOCK_STREAM, 0);
		if (fd < 0)
		{
			lua_pushfstring(l, "mt_connect(): socket(): %s",
			                strerror(errno));
			lua_error(l);
		}

		if (connect(fd, (struct sockaddr *) &sa, sizeof sa) < 0)
		{
			lua_pushfstring(l, "mt_connect(): connect(): %s",
			                strerror(errno));
			lua_error(l);
		}

		break;
	  }

	  case AF_INET:
	  {
		struct servent *srv;
		struct sockaddr_in sa;

		memset(&sa, '\0', sizeof sa);
		sa.sin_family = AF_INET;

		p++;

		at = strchr(p, '@');
		if (at == NULL)
		{
			sa.sin_addr.s_addr = INADDR_ANY;
		}
		else
		{
			struct hostent *h;

			*at = '\0';

			h = gethostbyname(at + 1);
			if (h != NULL)
			{
				memcpy(&sa.sin_addr.s_addr, h->h_addr,
				       sizeof sa.sin_addr.s_addr);
			}
			else
			{
				sa.sin_addr.s_addr = inet_addr(at + 1);
			}
		}

		srv = getservbyname(p, "tcp");
		if (srv != NULL)
		{
			sa.sin_port = srv->s_port;
		}
		else
		{
			int port;
			char *q;

			port = strtoul(p, &q, 10);
			if (*q != '\0')
			{
				lua_pushstring(l,
				               "mt_connect(): Invalid argument");
				lua_error(l);
			}

			sa.sin_port = htons(port);
		}

		if (at != NULL)
			*at = '@';

		fd = socket(PF_INET, SOCK_STREAM, 0);
		if (fd < 0)
		{
			lua_pushfstring(l, "mt_connect(): socket(): %s",
			                strerror(errno));
			lua_error(l);
		}

		if (connect(fd, (struct sockaddr *) &sa, sizeof sa) < 0)
		{
			lua_pushfstring(l, "mt_connect(): connect(): %s",
			                strerror(errno));
			lua_error(l);
		}

		break;
	  }

	  default:
		assert(0);
	}

	new = (struct mt_context *) malloc(sizeof *new);
	if (new == NULL)
	{
		lua_pushfstring(l, "mt_connect(): malloc(): %s",
		                strerror(errno));
		lua_error(l);
	}

	new->ctx_state = STATE_INIT;
	new->ctx_fd = fd;
	new->ctx_response = '\0';
	new->ctx_eomreqs = NULL;

	lua_pushlightuserdata(l, new);

	if (verbose > 0)
	{
		fprintf(stdout, "%s: connected to `%s', fd %d\n",
		        progname, sockinfo, fd);
	}

	return 1;
}

/*
**  MT_SLEEP -- sleep
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_sleep(lua_State *l)
{
	int secs;

	assert(l != NULL);

	if (lua_gettop(l) != 1 ||
	    !lua_isnumber(l, 1))
	{
		lua_pushstring(l, "mt_sleep(): Invalid argument");
		lua_error(l);
	}

	secs = lua_tonumber(l, 1);
	lua_pop(l, 1);

	sleep(secs);

	lua_pushnil(l);

	return 1;
}

/*
**  MT_DISCONNECT -- disconnect from a filter
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_disconnect(lua_State *l)
{
	struct mt_context *ctx;

	assert(l != NULL);

	if (lua_gettop(l) != 1 ||
	    !lua_islightuserdata(l, 1))
	{
		lua_pushstring(l, "mt_disconnect(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);
	lua_pop(l, 1);

	(void) mt_milter_write(ctx->ctx_fd, SMFIC_QUIT, NULL, 0);

	(void) close(ctx->ctx_fd);

	if (verbose > 0)
	{
		fprintf(stdout, "%s: disconnected fd %d\n",
		        progname, ctx->ctx_fd);
	}

	free(ctx);

	lua_pushnil(l);

	return 1;
}

/*
**  MT_NEGOTIATE -- option negotiation
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_negotiate(lua_State *l)
{
	char rcmd;
	size_t buflen;
	uint32_t mta_version;
	uint32_t mta_protoopts;
	uint32_t mta_actions;
	uint32_t nvers;
	uint32_t npopts;
	uint32_t nacts;
	struct mt_context *ctx;
	char buf[BUFRSZ];

	if (lua_gettop(l) != 4 ||
	    !lua_islightuserdata(l, 1) ||
	    (!lua_isnil(l, 2) && !lua_isnumber(l, 2)) ||
	    (!lua_isnil(l, 3) && !lua_isnumber(l, 3)) ||
	    (!lua_isnil(l, 4) && !lua_isnumber(l, 4)))
	{
		lua_pushstring(l, "mt_negotiate(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);

	buflen = sizeof buf;

	if (lua_isnumber(l, 2))
		mta_version = lua_tonumber(l, 2);
	else
		mta_version = SMFI_PROT_VERSION;

	if (lua_isnumber(l, 3))
		mta_protoopts = lua_tonumber(l, 3);
	else
		mta_protoopts = SMFI_CURR_PROT;

	if (lua_isnumber(l, 4))
		mta_actions = lua_tonumber(l, 4);
	else
		mta_actions = SMFI_CURR_ACTS;

	lua_pop(l, lua_gettop(l));

	nvers = htonl(mta_version);
	nacts = htonl(mta_actions);
	npopts = htonl(mta_protoopts);

	(void) memcpy(buf, (char *) &nvers, MILTER_LEN_BYTES);
	(void) memcpy(buf + MILTER_LEN_BYTES,
	              (char *) &nacts, MILTER_LEN_BYTES);
	(void) memcpy(buf + (MILTER_LEN_BYTES * 2),
	              (char *) &npopts, MILTER_LEN_BYTES);

	if (!mt_milter_write(ctx->ctx_fd, SMFIC_OPTNEG, buf, MILTER_OPTLEN))
	{
		lua_pushstring(l, "mt_milter_write() failed");
		return 1;
	}

	buflen = sizeof buf;

	if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
	{
		lua_pushstring(l, "mt_milter_read() failed");
		return 1;
	}

	if (rcmd != SMFIC_OPTNEG)
	{
		if (verbose > 0)
		{
			fprintf(stdout,
			        "%s: filter returned status %d to option negotiation on fd %d\n", 
			        progname, rcmd, ctx->ctx_fd);
		}

		ctx->ctx_state = STATE_DEAD;

		lua_pushnil(l);
		return 1;
	}

	ctx->ctx_response = rcmd;
	ctx->ctx_state = STATE_NEGOTIATED;

	if (verbose > 0)
	{
		fprintf(stdout,
		        "%s: option negotiation sent on fd %d, reply `%c'\n",
		        progname, ctx->ctx_fd, rcmd);
	}

	lua_pushnil(l);
	return 1;
}

/*
**  MT_MACRO -- send a macro
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_macro(lua_State *l)
{
	size_t buflen;
	size_t len;
	size_t s;
	struct mt_context *ctx;
	char *bp;
	char *name;
	char *value;
	char *type;
	char buf[BUFRSZ];

	assert(l != NULL);

	if (lua_gettop(l) != 4 ||
	    !lua_islightuserdata(l, 1) ||
	    !lua_isstring(l, 2) ||
	    !lua_isstring(l, 3) ||
	    !lua_isstring(l, 4))
	{
		lua_pushstring(l, "mt_macro(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);
	type = (char *) lua_tostring(l, 2);
	name = (char *) lua_tostring(l, 3);
	value = (char *) lua_tostring(l, 4);

	lua_pop(l, 4);

	if (!mt_assert_state(ctx, STATE_NEGOTIATED))
		lua_error(l);

	s = 1 + strlen(name) + 1 + strlen(value) + 1;
	buf[0] = type[0];
	bp = buf + 1;
	memcpy(bp, name, strlen(name) + 1);
	bp += strlen(name) + 1;
	memcpy(bp, value, strlen(value) + 1);

	if (!mt_milter_write(ctx->ctx_fd, SMFIC_MACRO, buf, s))
	{
		lua_pushstring(l, "mt_milter_write() failed");
		return 1;
	}

	if (verbose > 0)
	{
		fprintf(stdout, "%s: `%c' macro `%s' sent on fd %d\n",
		        progname, type[0], name, ctx->ctx_fd);
	}

	lua_pushnil(l);

	return 1;
}

/*
**  MT_CONNINFO -- send connection information
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_conninfo(lua_State *l)
{
	char rcmd;
	char family = '4';			/* IPv4 only for now */
	size_t buflen;
	size_t len;
	size_t s;
	uint16_t port;
	struct mt_context *ctx;
	char *host;
	char *bp;
	char *ipstr;
	struct in_addr sa;
	char buf[BUFRSZ];

	assert(l != NULL);

	if (lua_gettop(l) != 3 ||
	    !lua_islightuserdata(l, 1) ||
	    (!lua_isnil(l, 2) && !lua_isstring(l, 2)) ||
	    (!lua_isnil(l, 3) && !lua_isstring(l, 3)))
	{
		lua_pushstring(l, "mt_conninfo(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);
	if (lua_isstring(l, 2))
		host = (char *) lua_tostring(l, 2);
	else
		host = DEFCLIENTHOST;
	if (lua_isstring(l, 3))
		ipstr = (char *) lua_tostring(l, 3);
	else
		ipstr = NULL;

	lua_pop(l, 3);

	if (!mt_assert_state(ctx, STATE_NEGOTIATED))
		lua_error(l);

	if (ipstr == NULL)
	{
		struct hostent *h;

		h = gethostbyname(host);
		if (h == NULL)
		{
			lua_pushfstring(l, "mt_conninfo(): host `%s' unknown",
			                host);
			lua_error(l);
		}

		memcpy(&sa.s_addr, h->h_addr, sizeof sa.s_addr);
		ipstr = inet_ntoa(sa);
	}
	else
	{
		sa.s_addr = inet_addr(ipstr);
		if (sa.s_addr == INADDR_NONE)
		{
			lua_pushfstring(l,
			                "mt_conninfo(): invalid IPv4 address `%s'",
			                ipstr);
			lua_error(l);
		}
	}

	s = strlen(host) + 1 + sizeof(char) + sizeof port + strlen(ipstr) + 1;

	port = htons(DEFCLIENTPORT);		/* don't really need this */

	bp = buf;
	memcpy(bp, host, strlen(host));
	bp += strlen(host);
	*bp++ = '\0';
	memcpy(bp, &family, sizeof family);
	bp += sizeof family;
	memcpy(bp, &port, sizeof port);
	bp += sizeof port;
	memcpy(bp, ipstr, strlen(ipstr) + 1);

	if (!mt_milter_write(ctx->ctx_fd, SMFIC_CONNECT, buf, s))
	{
		lua_pushstring(l, "mt_milter_write() failed");
		return 1;
	}

	if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
	{
		lua_pushstring(l, "mt_milter_read() failed");
		return 1;
	}

	ctx->ctx_response = rcmd;
	ctx->ctx_state = STATE_CONNINFO;

	if (verbose > 0)
	{
		fprintf(stdout,
		        "%s: connection details sent on fd %d, reply `%c'\n",
		        progname, ctx->ctx_fd, rcmd);
	}

	lua_pushnil(l);

	return 1;
}

/*
**  MT_HELO -- send HELO information
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_helo(lua_State *l)
{
	char rcmd;
	size_t buflen;
	size_t s;
	struct mt_context *ctx;
	char *host;
	char *bp;
	char buf[BUFRSZ];

	assert(l != NULL);

	if (lua_gettop(l) != 2 ||
	    !lua_islightuserdata(l, 1) ||
	    !lua_isstring(l, 2))
	{
		lua_pushstring(l, "mt_helo(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);
	host = (char *) lua_tostring(l, 2);
	lua_pop(l, 2);

	if (!mt_assert_state(ctx, STATE_CONNINFO))
		lua_error(l);

	s = strlen(host) + 1;

	bp = buf;
	memcpy(bp, host, strlen(host));
	bp += strlen(host);
	*bp++ = '\0';

	if (!mt_milter_write(ctx->ctx_fd, SMFIC_HELO, buf, s))
	{
		lua_pushstring(l, "mt_milter_write() failed");
		return 1;
	}

	buflen = sizeof buf;

	if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
	{
		lua_pushstring(l, "mt_milter_read() failed");
		return 1;
	}

	ctx->ctx_response = rcmd;
	ctx->ctx_state = STATE_HELO;

	if (verbose > 0)
	{
		fprintf(stdout,
		        "%s: HELO sent on fd %d, reply `%c'\n",
		        progname, ctx->ctx_fd, rcmd);
	}

	lua_pushnil(l);

	return 1;
}

/*
**  MT_MAILFROM -- send MAIL FROM information
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_mailfrom(lua_State *l)
{
	char rcmd;
	int c;
	size_t buflen;
	size_t s;
	char *p;
	char *bp;
	struct mt_context *ctx;
	char buf[BUFRSZ];

	assert(l != NULL);

	if (lua_gettop(l) < 2 ||
	    !lua_islightuserdata(l, 1))
	{
		lua_pushstring(l, "mt_mailfrom(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);

	s = 0;
	bp = buf;

	for (c = 2; c <= lua_gettop(l); c++)
	{
		p = (char *) lua_tostring(l, c);

		s += strlen(p) + 1;

		memcpy(bp, p, strlen(p) + 1);

		bp += strlen(p) + 1;

		/* XXX -- watch for overruns */
	}

	lua_pop(l, lua_gettop(l));

	if (!mt_assert_state(ctx, STATE_HELO))
		lua_error(l);

	if (!mt_milter_write(ctx->ctx_fd, SMFIC_MAIL, buf, s))
	{
		lua_pushstring(l, "mt_milter_write() failed");
		return 1;
	}

	buflen = sizeof buf;

	if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
	{
		lua_pushstring(l, "mt_milter_read() failed");
		return 1;
	}

	ctx->ctx_response = rcmd;
	ctx->ctx_state = STATE_ENVFROM;
	mt_flush_eomreqs(ctx);

	if (verbose > 0)
	{
		fprintf(stdout,
		        "%s: MAIL sent on fd %d, reply `%c'\n",
		        progname, ctx->ctx_fd, rcmd);
	}

	lua_pushnil(l);

	return 1;
}

/*
**  MT_RCPTTO -- send RCPT TO information
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_rcptto(lua_State *l)
{
	char rcmd;
	int c;
	size_t buflen;
	size_t s;
	char *p;
	char *bp;
	struct mt_context *ctx;
	char buf[BUFRSZ];

	assert(l != NULL);

	if (lua_gettop(l) < 2 ||
	    !lua_islightuserdata(l, 1))
	{
		lua_pushstring(l, "mt_rcptto(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);

	s = 0;
	bp = buf;

	for (c = 2; c <= lua_gettop(l); c++)
	{
		p = (char *) lua_tostring(l, c);

		s += strlen(p) + 1;

		memcpy(bp, p, strlen(p) + 1);

		bp += strlen(p) + 1;

		/* XXX -- watch for overruns */
	}

	lua_pop(l, lua_gettop(l));

	if (!mt_assert_state(ctx, STATE_ENVFROM))
		lua_error(l);

	if (!mt_milter_write(ctx->ctx_fd, SMFIC_RCPT, buf, s))
	{
		lua_pushstring(l, "mt_milter_write() failed");
		return 1;
	}

	buflen = sizeof buf;

	if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
	{
		lua_pushstring(l, "mt_milter_read() failed");
		return 1;
	}

	ctx->ctx_response = rcmd;
	ctx->ctx_state = STATE_ENVRCPT;

	if (verbose > 0)
	{
		fprintf(stdout,
		        "%s: RCPT sent on fd %d, reply `%c'\n",
		        progname, ctx->ctx_fd, rcmd);
	}

	lua_pushnil(l);

	return 1;
}

/*
**  MT_HEADER -- send header field information
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_header(lua_State *l)
{
	char rcmd;
	int c;
	size_t buflen;
	size_t s;
	char *bp;
	char *name;
	char *value;
	struct mt_context *ctx;
	char buf[BUFRSZ];

	assert(l != NULL);

	if (lua_gettop(l) != 3 ||
	    !lua_islightuserdata(l, 1) ||
	    !lua_isstring(l, 2) ||
	    !lua_isstring(l, 3))
	{
		lua_pushstring(l, "mt_header(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);
	name = (char *) lua_tostring(l, 2);
	value = (char *) lua_tostring(l, 3);
	lua_pop(l, 3);

	s = strlen(name) + 1 + strlen(value) + 1;

	bp = buf;
	memcpy(buf, name, strlen(name) + 1);
	bp += strlen(name) + 1;
	memcpy(bp, value, strlen(value) + 1);

	if (!mt_assert_state(ctx, STATE_ENVRCPT))
		lua_error(l);

	if (!mt_milter_write(ctx->ctx_fd, SMFIC_HEADER, buf, s))
	{
		lua_pushstring(l, "mt_milter_write() failed");
		return 1;
	}

	buflen = sizeof buf;

	if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
	{
		lua_pushstring(l, "mt_milter_read() failed");
		return 1;
	}

	ctx->ctx_response = rcmd;
	ctx->ctx_state = STATE_HEADER;

	if (verbose > 0)
	{
		fprintf(stdout,
		        "%s: header sent on fd %d, reply `%c'\n",
		        progname, ctx->ctx_fd, rcmd);
	}

	lua_pushnil(l);

	return 1;
}

/*
**  MT_EOH -- send end-of-header notice
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_eoh(lua_State *l)
{
	char rcmd;
	int c;
	size_t buflen;
	size_t s;
	char *bp;
	char *name;
	char *value;
	struct mt_context *ctx;
	char buf[BUFRSZ];

	assert(l != NULL);

	if (lua_gettop(l) != 1 ||
	    !lua_islightuserdata(l, 1))
	{
		lua_pushstring(l, "mt_eoh(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);
	lua_pop(l, 1);

	if (!mt_assert_state(ctx, STATE_HEADER))
		lua_error(l);

	if (!mt_milter_write(ctx->ctx_fd, SMFIC_EOH, NULL, 0))
	{
		lua_pushstring(l, "mt_milter_write() failed");
		return 1;
	}

	buflen = sizeof buf;

	if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
	{
		lua_pushstring(l, "mt_milter_read() failed");
		return 1;
	}

	ctx->ctx_response = rcmd;
	ctx->ctx_state = STATE_EOH;

	if (verbose > 0)
	{
		fprintf(stdout,
		        "%s: EOH sent on fd %d, reply `%c'\n",
		        progname, ctx->ctx_fd, rcmd);
	}

	lua_pushnil(l);

	return 1;
}

/*
**  MT_BODYSTRING -- send a string of body
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_bodystring(lua_State *l)
{
	char rcmd;
	int c;
	size_t buflen;
	size_t s;
	char *bp;
	char *str;
	struct mt_context *ctx;
	char buf[BUFRSZ];

	assert(l != NULL);

	if (lua_gettop(l) != 2 ||
	    !lua_islightuserdata(l, 1) ||
	    !lua_isstring(l, 2))
	{
		lua_pushstring(l, "mt_bodystring(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);
	str = (char *) lua_tostring(l, 2);
	lua_pop(l, 2);

	if (!mt_assert_state(ctx, STATE_EOH))
		lua_error(l);

	if (!mt_milter_write(ctx->ctx_fd, SMFIC_BODY, str, strlen(str)))
	{
		lua_pushstring(l, "mt_milter_write() failed");
		return 1;
	}

	buflen = sizeof buf;

	if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
	{
		lua_pushstring(l, "mt_milter_read() failed");
		return 1;
	}

	ctx->ctx_response = rcmd;
	ctx->ctx_state = STATE_BODY;

	if (verbose > 0)
	{
		fprintf(stdout,
		        "%s: %d byte(s) of body sent on fd %d, reply `%c'\n",
		        progname, strlen(str), ctx->ctx_fd, rcmd);
	}

	lua_pushnil(l);

	return 1;
}

/*
**  MT_BODYRANDOM -- send a random chunk of body
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_bodyrandom(lua_State *l)
{
	char rcmd;
	unsigned long rw;
	unsigned long rl;
	int c;
	size_t buflen;
	size_t s;
	char *bp;
	char *str;
	struct mt_context *ctx;
	char buf[BUFRSZ];

	assert(l != NULL);

	if (lua_gettop(l) != 2 ||
	    !lua_islightuserdata(l, 1) ||
	    !lua_isnumber(l, 2))
	{
		lua_pushstring(l, "mt_bodyrandom(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);
	rw = (unsigned long) lua_tonumber(l, 2);
	lua_pop(l, 2);

	if (!mt_assert_state(ctx, STATE_EOH))
		lua_error(l);

	while (rw > 0)
	{
		memset(buf, '\0', sizeof buf);

		rl = random() % (sizeof buf - 3);
		if (rl > rw)
			rl = rw;

		for (c = 0; c < rl; c++)
			buf[c] = (random() % 95) + 32;
		strlcat(buf, "\r\n", sizeof buf);

		if (!mt_milter_write(ctx->ctx_fd, SMFIC_BODY, buf,
		                     strlen(buf)))
		{
			lua_pushstring(l, "mt_milter_write() failed");
			return 1;
		}

		buflen = sizeof buf;

		if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
		{
			lua_pushstring(l, "mt_milter_read() failed");
			return 1;
		}

		ctx->ctx_response = rcmd;
		ctx->ctx_state = STATE_BODY;

		if (verbose > 0)
		{
			fprintf(stdout,
			        "%s: %d byte(s) of body sent on fd %d, reply `%c'\n",
			        progname, strlen(buf), ctx->ctx_fd, rcmd);
		}

		rw -= rl;
	}

	lua_pushnil(l);

	return 1;
}

/*
**  MT_BODYFILE -- send contents of a file as body
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_bodyfile(lua_State *l)
{
	char rcmd;
	char *file;
	FILE *f;
	ssize_t rlen;
	struct mt_context *ctx;
	char chunk[CHUNKSZ];

	assert(l != NULL);

	if (lua_gettop(l) != 2 ||
	    !lua_islightuserdata(l, 1) ||
	    !lua_isstring(l, 2))
	{
		lua_pushstring(l, "mt_bodyfile(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);
	file = (char *) lua_tostring(l, 2);
	lua_pop(l, 2);

	if (!mt_assert_state(ctx, STATE_EOH))
		lua_error(l);

	f = fopen(file, "r");
	if (f == NULL)
	{
		lua_pushfstring(l, "mt_bodyfile(): %s: fopen(): %s",
		                file, strerror(errno));
		lua_error(l);
	}

	for (;;)
	{
		rlen =  fread(chunk, 1, sizeof chunk, f);

		if (rlen > 0)
		{
			size_t buflen;

			if (!mt_milter_write(ctx->ctx_fd, SMFIC_BODY, chunk,
			                     rlen))
			{
				fclose(f);
				lua_pushstring(l, "mt_milter_write() failed");
				return 1;
			}

			buflen = sizeof chunk;

			if (!mt_milter_read(ctx->ctx_fd, &rcmd, chunk,
			                    &buflen))
			{
				fclose(f);
				lua_pushstring(l, "mt_milter_read() failed");
				return 1;
			}

			if (verbose > 0)
			{
				fprintf(stdout,
				        "%s: %d byte(s) of body sent on fd %d, reply `%c'\n",
				        progname, rlen, ctx->ctx_fd, rcmd);
			}
		}

		if (rlen < sizeof chunk || rcmd != SMFIR_CONTINUE)
			break;
	}

	fclose(f);

	ctx->ctx_response = rcmd;
	ctx->ctx_state = STATE_BODY;

	lua_pushnil(l);

	return 1;
}

/*
**  MT_EOM -- send end-of-message notice, collect requests
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_eom(lua_State *l)
{
	char rcmd;
	int c;
	size_t buflen;
	size_t s;
	char *bp;
	char *name;
	char *value;
	struct mt_context *ctx;
	char buf[BUFRSZ];

	assert(l != NULL);

	if (lua_gettop(l) != 1 ||
	    !lua_islightuserdata(l, 1))
	{
		lua_pushstring(l, "mt_eom(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);
	lua_pop(l, 1);

	if (!mt_assert_state(ctx, STATE_BODY))
		lua_error(l);

	if (!mt_milter_write(ctx->ctx_fd, SMFIC_BODYEOB, NULL, 0))
	{
		lua_pushstring(l, "mt_milter_write() failed");
		return 1;
	}

	rcmd = '\0';

	for (;;)
	{
		buflen = sizeof buf;

		if (!mt_milter_read(ctx->ctx_fd, &rcmd, buf, &buflen))
		{
			lua_pushstring(l, "mt_milter_read() failed");
			return 1;
		}

		if (rcmd == SMFIR_CONTINUE ||
		    rcmd == SMFIR_ACCEPT ||
		    rcmd == SMFIR_REJECT ||
		    rcmd == SMFIR_TEMPFAIL ||
		    rcmd == SMFIR_DISCARD)
			break;

		if (!mt_eom_request(ctx, rcmd, buflen,
		                    buflen == 0 ? NULL : buf))
		{
			lua_pushstring(l, "mt_eom_request() failed");
			return 1;
		}
	}

	ctx->ctx_response = rcmd;
	ctx->ctx_state = STATE_EOM;

	if (verbose > 0)
	{
		fprintf(stdout, "%s: EOM sent on fd %d, reply `%c'\n",
		        progname, ctx->ctx_fd, rcmd);
	}

	lua_pushnil(l);

	return 1;
}

/*
**  MT_EOM_CHECK -- test for a specific end-of-message action
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_eom_check(lua_State *l)
{
	int op;
	struct mt_context *ctx;
	struct mt_eom_request *r;

	assert(l != NULL);

	if (lua_gettop(l) < 2 || lua_gettop(l) > 5 ||
	    !lua_islightuserdata(l, 1) ||
	    !lua_isnumber(l, 2))
	{
		lua_pushstring(l, "mt_eom_check(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);
	op = lua_tonumber(l, 2);

	switch (op)
	{
	  case MT_HDRINSERT:
	  {
		int idx = -1;
		char *name = NULL;
		char *value = NULL;

		if (lua_gettop(l) >= 3)
		{
			if (!lua_isstring(l, 3))
			{
				lua_pushstring(l,
				               "mt_eom_check(): Invalid argument");
				lua_error(l);
			}

			name = (char *) lua_tostring(l, 3);
		}

		if (lua_gettop(l) >= 4)
		{
			if (!lua_isstring(l, 4))
			{
				lua_pushstring(l,
				               "mt_eom_check(): Invalid argument");
				lua_error(l);
			}

			value = (char *) lua_tostring(l, 4);
		}

		if (lua_gettop(l) == 5)
		{
			if (!lua_isnumber(l, 5))
			{
				lua_pushstring(l,
				               "mt_eom_check(): Invalid argument");
				lua_error(l);
			}

			idx = lua_tonumber(l, 4);
		}

		lua_pop(l, lua_gettop(l));

		for (r = ctx->ctx_eomreqs; r != NULL; r = r->eom_next)
		{
			if (r->eom_request == SMFIR_INSHEADER)
			{
				int ridx;
				char *rname;
				char *rvalue;

				memcpy(&ridx, r->eom_rdata, MILTER_LEN_BYTES);
				ridx = ntohl(ridx);
				rname = r->eom_rdata + MILTER_LEN_BYTES;
				rvalue = r->eom_rdata + MILTER_LEN_BYTES +
				         strlen(rname) + 1;

				if ((name == NULL ||
				     strcmp(name, rname) == 0) &&
				    (value == NULL ||
				     strcmp(value, rvalue) == 0) &&
				    (idx == -1 || ridx == idx))
				{
					lua_pushboolean(l, 1);
					return 1;
				}
			}
		}

		lua_pushboolean(l, 0);
		return 1;
	  }

	  default:
		lua_pushstring(l, "mt_eom_check(): Invalid argument");
		lua_error(l);
	}

	return 1;
}

/*
**  MT_ABORT -- send transaction abort notice
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	nil (on the Lua stack)
*/

int
mt_abort(lua_State *l)
{
	char rcmd;
	int c;
	size_t buflen;
	size_t s;
	char *bp;
	char *name;
	char *value;
	struct mt_context *ctx;
	char buf[BUFRSZ];

	assert(l != NULL);

	if (lua_gettop(l) != 1 ||
	    !lua_islightuserdata(l, 1))
	{
		lua_pushstring(l, "mt_abort(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);
	lua_pop(l, 1);

	if (!mt_milter_write(ctx->ctx_fd, SMFIC_ABORT, NULL, 0))
	{
		lua_pushstring(l, "mt_milter_write() failed");
		return 1;
	}

	ctx->ctx_state = STATE_HELO;

	if (verbose > 0)
	{
		fprintf(stdout, "%s: ABORT sent on fd %d\n",
		        progname, ctx->ctx_fd);
	}

	lua_pushnil(l);

	return 1;
}

/*
**  MT_GETREPLY -- get last reply
**
**  Parameters:
**  	l -- Lua state
**
**  Return value:
**   	Last reply received, as a string (on the Lua stack).
*/

int
mt_getreply(lua_State *l)
{
	char rcmd;
	int c;
	size_t buflen;
	size_t s;
	char *bp;
	char *name;
	char *value;
	struct mt_context *ctx;
	char buf[BUFRSZ];

	assert(l != NULL);

	if (lua_gettop(l) != 1 ||
	    !lua_islightuserdata(l, 1))
	{
		lua_pushstring(l, "mt_abort(): Invalid argument");
		lua_error(l);
	}

	ctx = (struct mt_context *) lua_touserdata(l, 1);
	lua_pop(l, 1);

	lua_pushfstring(l, "%c", ctx->ctx_response);

	return 1;
}

/*
**  USAGE -- print usage message
** 
**  Parameters:
**  	Not now.  Maybe later.
**
**  Return value:
**  	EX_USAGE
*/

int
usage(void)
{
	fprintf(stderr, "%s: usage: [options]\n"
	                "\t-D name[=value]\tdefine global variable\n"
	                "\t-s script      \tscript to run (default = stdin)\n"
	                "\t-v             \tverbose mode\n",
	                progname, progname);

	return EX_USAGE;
}

/*
**  MAIN -- program mainline
**
**  Parameters:
**  	argc, argv -- the usual
**
**  Return value:
**  	Exit status.
*/

int
main(int argc, char **argv)
{
	int c;
	int status;
	int fd;
	ssize_t rlen;
	char *p;
	char *script = NULL;
	lua_State *l;
	struct mt_lua_io io;
	struct stat s;

	progname = (p = strrchr(argv[0], '/')) == NULL ? argv[0] : p + 1;

	verbose = 0;
	filterpid = 0;
	tmo = DEFTIMEOUT;

	l = lua_newstate(mt_lua_alloc, NULL);
	if (l == NULL)
	{
		fprintf(stderr, "%s: unable to allocate new Lua state\n",
		        progname);
		return 1;
	}

	luaL_openlibs(l);

	while ((c = getopt(argc, argv, CMDLINEOPTS)) != -1)
	{
		switch (c)
		{
		  case 'D':
			p = strchr(optarg, '=');
			if (p != NULL)
			{
				*p = '\0';
				lua_pushstring(l, p + 1);
			}
			else
			{
				lua_pushnumber(l, 1);
			}

			lua_setglobal(l, optarg);

			break;

		  case 's':
			if (script != NULL)
			{
				fprintf(stderr,
				        "%s: multiple use of `-%c' not permitted\n",
				        progname, c);
				lua_close(l);
				return EX_USAGE;
			}

			script = optarg;
			break;

		  case 'v':
			verbose++;
			break;

		  default:
			lua_close(l);
			return usage();
		}
	}

	if (optind != argc)
	{
		lua_close(l);
		return usage();
	}

	io.lua_io_done = FALSE;

	if (script != NULL)
	{
		fd = open(script, O_RDONLY);
		if (fd < 0)
		{
			fprintf(stderr, "%s: %s: open(): %s\n", progname,
			        script, strerror(errno));
			lua_close(l);
			return 1;
		}

		if (fstat(fd, &s) != 0)
		{
			fprintf(stderr, "%s: %s: fstat(): %s\n", progname,
			        script, strerror(errno));
			close(fd);
			lua_close(l);
			return 1;
		}

		io.lua_io_script = (const char *) malloc(s.st_size);
		if (io.lua_io_script == NULL)
		{
			fprintf(stderr, "%s: malloc(): %s\n", progname,
			        strerror(errno));
			close(fd);
			lua_close(l);
			return 1;
		}

		rlen = read(fd, (void *) io.lua_io_script, s.st_size);
		if (rlen != s.st_size)
		{
			fprintf(stderr,
			        "%s: %s: read() returned %ld (expecting %ld)\n",
			        progname, script, rlen, s.st_size);
			free((void *) io.lua_io_script);
			close(fd);
			lua_close(l);
			return 1;
		}

		close(fd);
	}
	else
	{
		io.lua_io_script = NULL;
	}

	/* XXX -- register functions here */
	lua_register(l, "mt_sleep", mt_sleep);
	lua_register(l, "mt_startfilter", mt_startfilter);
	lua_register(l, "mt_getreply", mt_getreply);
	lua_register(l, "mt_connect", mt_connect);
	lua_register(l, "mt_negotiate", mt_negotiate);
	lua_register(l, "mt_macro", mt_macro);
	lua_register(l, "mt_conninfo", mt_conninfo);
	lua_register(l, "mt_helo", mt_helo);
	lua_register(l, "mt_mailfrom", mt_mailfrom);
	lua_register(l, "mt_rcptto", mt_rcptto);
	lua_register(l, "mt_header", mt_header);
	lua_register(l, "mt_eoh", mt_eoh);
	lua_register(l, "mt_bodyfile", mt_bodyfile);
	lua_register(l, "mt_bodyrandom", mt_bodyrandom);
	lua_register(l, "mt_bodystring", mt_bodystring);
	lua_register(l, "mt_eom", mt_eom);
	lua_register(l, "mt_eom_check", mt_eom_check);
	lua_register(l, "mt_set_timeout", mt_set_timeout);
	lua_register(l, "mt_abort", mt_abort);
	lua_register(l, "mt_disconnect", mt_disconnect);

	lua_pushnumber(l, MT_HDRINSERT);
	lua_setglobal(l, "MT_HDRINSERT");

	switch (lua_load(l, mt_lua_reader, (void *) &io,
	                 script == NULL ? "(stdin)" : script))
	{
	  case 0:
		break;

	  case LUA_ERRSYNTAX:
	  case LUA_ERRMEM:
		if (lua_isstring(l, 1))
		{
			fprintf(stderr, "%s: %s: %s\n", progname,
			        script == NULL ? "(stdin)" : script,
			        lua_tostring(l, 1));
		}
		lua_close(l);
		if (io.lua_io_script != NULL)
			free((void *) io.lua_io_script);
		return 1;

	  default:
		assert(0);
	}

	(void) srandom(time(NULL));

	status = lua_pcall(l, 0, LUA_MULTRET, 0);
	if (lua_isstring(l, 1))
	{
		fprintf(stderr, "%s: %s: %s\n", progname,
		        script == NULL ? "(stdin)" : script,
		        lua_tostring(l, 1));
	}

	lua_close(l);
	if (io.lua_io_script != NULL)
		free((void *) io.lua_io_script);

	if (filterpid != 0)
	{
		if (kill(filterpid, SIGTERM) != 0)
		{
			fprintf(stderr, "%s: %d: kill() %s\n", progname,
			        filterpid, strerror(errno));
		}
		else
		{
			if (verbose > 1)
			{
				fprintf(stdout,
				        "%s: waiting for process %d\n",
				        progname, filterpid);
			}

			(void) wait(&status);

			if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
			{
				fprintf(stderr,
				        "%s: filter process exited with status %d\n",
				        progname, WEXITSTATUS(status));
			}
			else if (WIFSIGNALED(status) &&
			         WTERMSIG(status) != SIGTERM)
			{
				fprintf(stderr,
				        "%s: filter process died with signal %d\n",
				        progname, WTERMSIG(status));
			}
		}
	}

	return status;
}
