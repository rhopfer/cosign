/*
 * Copyright (c) 1998 Regents of The University of Michigan.
 * All Rights Reserved.  See LICENSE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <string.h>
#include <syslog.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <utime.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <snet.h>

#include "command.h"
#include "config.h"
#include "cparse.h"
#include "mkcookie.h"

#define MIN(a,b)        ((a)<(b)?(a):(b))
#define MAX(a,b)        ((a)>(b)?(a):(b))

#define TKT_PREFIX	_COSIGN_TICKET_CACHE

#define IDLE_OUT	7200
#define GREY		1800

static int	f_noop( SNET *, int, char *[], SNET * );
static int	f_quit( SNET *, int, char *[], SNET * );
static int	f_help( SNET *, int, char *[], SNET * );
static int	f_notauth( SNET *, int, char *[], SNET * );
static int	f_login( SNET *, int, char *[], SNET * );
static int	f_logout( SNET *, int, char *[], SNET * );
static int	f_register( SNET *, int, char *[], SNET * );
static int	f_check( SNET *, int, char *[], SNET * );
static int	f_retr( SNET *, int, char *[], SNET * );
static int	f_time( SNET *, int, char *[], SNET * );
static int	f_daemon( SNET *, int, char *[], SNET * );
static int	f_starttls( SNET *, int, char *[], SNET * );

static int	do_register( char *, char * );
static int	retr_ticket( SNET *, struct cinfo * );
static int	retr_proxy( SNET *, char *, SNET * );

struct command {
    char	*c_name;
    int		(*c_func)( SNET *, int, char *[], SNET * );
};

struct command	unauth_commands[] = {
    { "NOOP",		f_noop },
    { "QUIT",		f_quit },
    { "HELP",		f_help },
    { "STARTTLS",	f_starttls },
    { "LOGIN",		f_notauth },
    { "LOGOUT",		f_notauth },
    { "REGISTER",	f_notauth },
    { "CHECK",		f_notauth },
    { "RETR",		f_notauth },
    { "TIME",		f_notauth },
    { "DAEMON",		f_notauth },
};

struct command	auth_commands[] = {
    { "NOOP",		f_noop },
    { "QUIT",		f_quit },
    { "HELP",		f_help },
    { "STARTTLS",	f_starttls },
    { "LOGIN",		f_login },
    { "LOGOUT",		f_logout },
    { "REGISTER",	f_register },
    { "CHECK",		f_check },
    { "RETR",		f_retr },
    { "TIME",		f_time },
    { "DAEMON",		f_daemon },
};

extern char	*cosign_version;
extern int	debug;
extern SSL_CTX	*ctx;
struct command 	*commands = unauth_commands;
struct chosts	*ch = NULL;
int		replicate = 1;
int	ncommands = sizeof( unauth_commands ) / sizeof(unauth_commands[ 0 ] );

    int
f_quit( SNET *sn, int ac, char *av[], SNET *pushersn )
{
    snet_writef( sn, "%d Service closing transmission channel\r\n", 221 );
    exit( 0 );
}

    int
f_noop( SNET *sn, int ac, char *av[], SNET *pushersn )
{
    snet_writef( sn, "%d cosign v%s\r\n", 250, cosign_version );
    return( 0 );
}

    int
f_help( SNET *sn, int ac, char *av[], SNET *pushersn )
{
    snet_writef( sn, "%d Slainte Mhath!\r\n", 203 );
    return( 0 );
}

    int
f_notauth( SNET *sn, int ac, char *av[], SNET *pushersn )
{
    snet_writef( sn, "%d You must call STARTTLS first!\r\n", 550 );
    return( 0 );
}

    int
f_starttls( SNET *sn, int ac, char *av[], SNET *pushersn )
{

    int				rc;
    X509			*peer;
    char			buf[ 1024 ];

    if ( ac != 1 ) {
	snet_writef( sn, "%d Syntax error\r\n", 501 );
	return( 1 );
    }

    snet_writef( sn, "%d Ready to start TLS\r\n", 220 );

    /*
     * Begin TLS
     */
    if (( rc = snet_starttls( sn, ctx, 1 )) != 1 ) {
	syslog( LOG_ERR, "f_starttls: snet_starttls: %s",
		ERR_error_string( ERR_get_error(), NULL ) );
	snet_writef( sn, "%d SSL didn't work error!\r\n", 501 );
	return( 1 );
    }
    if (( peer = SSL_get_peer_certificate( sn->sn_ssl ))
	    == NULL ) {
	syslog( LOG_ERR, "no peer certificate" );
	return( -1 );
    }

    X509_NAME_get_text_by_NID( X509_get_subject_name( peer ),
		NID_commonName, buf, sizeof( buf ));
    if (( ch = chosts_find( buf )) == NULL ) {
	syslog( LOG_ERR, "f_starttls: No access for %s", buf );
	X509_free( peer );
	exit( 1 );
    }

    X509_free( peer );

    commands = auth_commands;
    ncommands = sizeof( auth_commands ) / sizeof( auth_commands[ 0 ] );
    return( 0 );
}


    int
f_login( SNET *sn, int ac, char *av[], SNET *pushersn )
{
    FILE		*tmpfile;
    char		tmppath[ MAXCOOKIELEN ];
    char		tmpkrb[ 16 ], krbpath [ MAXPATHLEN ];
    char                *sizebuf, *line;
    char                buf[ 8192 ];
    int			fd, krb = 0;
    struct timeval	tv;
    struct cinfo	ci;
    unsigned int        len, rc;
    extern int		errno;

    /* LOGIN login_cookie ip principal realm [tgt] */

    if ( ch->ch_key != CGI ) {
	syslog( LOG_ERR, "%s not allowed to login", ch->ch_hostname );
	snet_writef( sn, "%d LOGIN: %s not allowed to login.\r\n",
		400, ch->ch_hostname );
	return( 1 );
    }

    if (( ac != 5 ) && ( ac != 6 )) {
	snet_writef( sn, "%d LOGIN: Wrong number of args.\r\n", 500 );
	return( 1 );
    }

    if ( ac == 6 ) {
	if ( strcmp( av[ 5 ], "kerberos" ) == 0 ) {
	    krb = 1;
	    if ( mkcookie( sizeof( tmpkrb ), tmpkrb ) != 0 ) {
		syslog( LOG_ERR, "f_login: mkcookie error." );
		return( -1 );
	    }
	    if ( snprintf( krbpath, sizeof( krbpath ), "%s/%s",
		    TKT_PREFIX, tmpkrb ) > sizeof( krbpath )) {
		syslog( LOG_ERR, "f_login: krbpath too long." );
		return( -1 );
	    }
	} else {
	    snet_writef( sn, "%d LOGIN: Ticket type not supported.\r\n", 507 );
	    return( 1 );
	}
    }

    if ( strchr( av[ 1 ], '/' ) != NULL ) {
	syslog( LOG_ERR, "f_login: cookie name contains '/'" );
	snet_writef( sn, "%d LOGIN: Invalid cookie name.\r\n", 501 );
	return( 1 );
    }

    if ( strlen( av[ 1 ] ) >= MAXCOOKIELEN ) {
	syslog( LOG_ERR, "f_login: cookie too long" );
	snet_writef( sn, "%d LOGIN: Cookie too long.\r\n", 502 );
	return( 1 );
    }

    if ( gettimeofday( &tv, NULL ) != 0 ){
	syslog( LOG_ERR, "f_login: gettimeofday: %m" );
	return( -1 );
    }

    if ( snprintf( tmppath, sizeof( tmppath ), "%x%x.%i",
	    tv.tv_sec, tv.tv_usec, (int)getpid()) >= sizeof( tmppath )) {
	syslog( LOG_ERR, "f_login: tmppath too long" );
	return( -1 );
    }

    if (( fd = open( tmppath, O_CREAT|O_EXCL|O_WRONLY, 0644 )) < 0 ) {
	syslog( LOG_ERR, "f_login: open: %m" );
	return( -1 );
    }

    if (( tmpfile = fdopen( fd, "w" )) == NULL ) {
	/* close */
	if ( unlink( tmppath ) != 0 ) {
	    syslog( LOG_ERR, "f_login: unlink: %m" );
	}
	syslog( LOG_ERR, "f_login: fdopen: %m" );
	return( -1 );
    }

    fprintf( tmpfile, "v0\n" );
    fprintf( tmpfile, "s1\n" );	 /* 1 is logged in, 0 is logged out */
    if ( strlen( av[ 2 ] ) >= sizeof( ci.ci_ipaddr )) {
	goto file_err;
    }
    fprintf( tmpfile, "i%s\n", av[ 2 ] );
    if ( strlen( av[ 3 ] ) >= sizeof( ci.ci_user )) {
	goto file_err;
    }
    fprintf( tmpfile, "p%s\n", av[ 3 ] );
    if ( strlen( av[ 4 ] ) >= sizeof( ci.ci_realm )) {
	goto file_err;
    }
    fprintf( tmpfile, "r%s\n", av[ 4 ] );
    fprintf( tmpfile, "t%lu\n", tv.tv_sec );
    if ( krb ) {
	fprintf( tmpfile, "k%s\n", krbpath );
    }

    if ( fclose ( tmpfile ) != 0 ) {
	if ( unlink( tmppath ) != 0 ) {
	    syslog( LOG_ERR, "f_login: unlink: %m" );
	}
	syslog( LOG_ERR, "f_login: fclose: %m" );
	return( -1 );
    }

    if ( link( tmppath, av[ 1 ] ) != 0 ) {
	if ( unlink( tmppath ) != 0 ) {
	    syslog( LOG_ERR, "f_login: unlink: %m" );
	}
	if ( errno == EEXIST ) {
	    syslog( LOG_ERR, "f_login: file already exists: %s", av[ 1 ]);
	    if ( read_cookie( av[ 1 ], &ci ) != 0 ) {
		syslog( LOG_ERR, "f_login: read_cookie" );
		snet_writef( sn, "%d LOGIN error: Sorry\r\n", 503 );
		return( 1 );
	    }
	    if ( ci.ci_state == 0 ) {
		syslog( LOG_ERR,
			"f_login: %s already logged out", av[ 1 ] );
		snet_writef( sn, "%d LOGIN: Already logged out\r\n", 505 );
		return( 1 );
	    }
	    if ( strcmp( av[ 3 ], ci.ci_user ) != 0 ) {
		syslog( LOG_ERR, "%s in cookie %s does not match %s",
			ci.ci_user, av[ 1 ], av[ 3 ] );
		snet_writef( sn,
			"%d user name given does not match cookie\r\n", 402 );
		return( 1 );
	    }
	    snet_writef( sn,
		    "%d LOGIN: Cookie already exists\r\n", 201 );
	    return( 1 );
	}
	syslog( LOG_ERR, "f_login: link: %m" );
	return( -1 );
    }

    if ( unlink( tmppath ) != 0 ) {
	syslog( LOG_ERR, "f_login: unlink: %m" );
    }

    if ( !krb ) {
	snet_writef( sn, "%d LOGIN successful: Cookie Stored.\r\n", 200 );
	if (( pushersn != NULL ) && ( replicate )) {
	    snet_writef( pushersn, "LOGIN %s %s %s %s\r\n",
		    av[ 1 ], av[ 2 ], av[ 3 ], av[ 4 ]);
	}
	return( 0 );
    }

    snet_writef( sn, "%d LOGIN: Send length then file.\r\n", 300 );

    if (( fd = open( krbpath, O_CREAT|O_EXCL|O_WRONLY, 0644 )) < 0 ) {
	syslog( LOG_ERR, "f_login: open: %s: %m", krbpath );
	return( -1 );
    }

    tv.tv_sec = 60 * 2;
    tv.tv_usec = 0;
    if (( sizebuf = snet_getline( sn, &tv )) == NULL ) {
        syslog( LOG_ERR, "f_login: snet_getline: %m" );
        return( -1 );
    }
    /* Will there be a limit? */
    len = atoi( sizebuf );

    for ( ; len > 0; len -= rc ) {
        tv.tv_sec = 60 * 2;
        tv.tv_usec = 0;
        if (( rc = snet_read(
                sn, buf, (int)MIN( len, sizeof( buf )), &tv )) <= 0 ) {
            syslog( LOG_ERR, "f_login: snet_read: %m" );
            return( -1 );
        }

        if ( write( fd, buf, rc ) != rc ) {
            snet_writef( sn, "%d %s: %s\r\n", 504, krbpath, strerror( errno ));
            return( 1 );
        }
    }

    if ( close( fd ) < 0 ) {
        snet_writef( sn, "%d %s: %s\r\n", 504, krbpath, strerror( errno ));
        return( 1 );
    }


    tv.tv_sec = 60 * 2;
    tv.tv_usec = 0;
    if (( line = snet_getline( sn, &tv )) == NULL ) {
        syslog( LOG_ERR, "f_login: snet_getline: %m" );
        return( -1 );
    }

    /* make sure client agrees we're at the end */
    if ( strcmp( line, "." ) != 0 ) {
        snet_writef( sn, "%d Length doesn't match sent data\r\n", 505 );
        (void)unlink( krbpath );

	if ( unlink( av[ 1 ] ) != 0 ) {
	    syslog( LOG_ERR, "f_login: unlink: %m" );
	}

	/* if the krb tkt didn't store, unlink the cookie as well */

        tv.tv_sec = 60 * 2;
        tv.tv_usec = 0;
        for (;;) {
            if (( line = snet_getline( sn, &tv )) == NULL ) {
                syslog( LOG_ERR, "f_login: snet_getline: %m" );
                return( -1 );
            }
            if ( strcmp( line, "." ) == 0 ) {
                break;
            }
        }
        return( -1 );
    }

    snet_writef( sn, "%d LOGIN successful: Cookie & Ticket Stored.\r\n", 201 );
    if (( pushersn != NULL ) && ( replicate )) {
	snet_writef( pushersn, "LOGIN %s %s %s %s %s\r\n",
		av[ 1 ], av[ 2 ], av[ 3 ], av[ 4 ], av[ 5 ]);
    }
    return( 0 );

file_err:
    (void)fclose( tmpfile );
    if ( unlink( tmppath ) != 0 ) {
	syslog( LOG_ERR, "f_login: unlink: %m" );
    }
    syslog( LOG_ERR, "f_login: bad file format" );
    snet_writef( sn, "%d LOGIN Syntax Error: Bad File Format\r\n", 504 );
    return( 1 );
}

    int
f_daemon( SNET *sn, int ac, char *av[], SNET *pushersn )
{

    char	hostname[ MAXHOSTNAMELEN ];

    if ( ch->ch_key != CGI ) {
	syslog( LOG_ERR, "%s is not a daemon", ch->ch_hostname );
	snet_writef( sn, "%d DAEMON: %s not a daemon.\r\n",
		460, ch->ch_hostname );
	return( 1 );
    }

    if ( ac != 2 ) {
	snet_writef( sn, "%d Syntax error\r\n", 571 );
	return( 1 );
    }

    if ( gethostname( hostname, sizeof( hostname )) < 0 ) {
	syslog( LOG_ERR, "f_daemon: %m" );
	snet_writef( sn, "%d DAEMON error. Sorry!\r\n", 572 );
	return( 1 );
    }

    if ( strcasecmp( hostname, av[ 1 ] ) == 0 ) {
	snet_writef( sn, "%d Schizophrenia!\r\n", 471 );
	return( 1 );
    }
    replicate = 0;

    snet_writef( sn, "%d Daemon flag set\r\n", 271 );
    return( 0 );
}

    int
f_time( SNET *sn, int ac, char *av[], SNET *pushersn )
{
    struct utimbuf	new_time;
    struct stat		st;
    struct timeval	tv;
    int			timestamp, state;
    char		*line;

    /* TIME */
    /* 3xx */
    /* login_cookie timestamp state */
    /* . */

    if ( ch->ch_key != CGI ) {
	syslog( LOG_ERR, "%s not allowed to tell time", ch->ch_hostname );
	snet_writef( sn, "%d TIME: %s not allowed to propogate time.\r\n",
		460, ch->ch_hostname );
	return( 1 );
    }

    if ( ac != 1 ) {
	snet_writef( sn, "%d TIME: Wrong number of args.\r\n", 560 );
	return( 1 );
    }

    snet_writef( sn, "%d TIME: Send timestamps.\r\n", 360 );

    tv.tv_sec = 60 * 10;
    tv.tv_usec = 0;
    while (( line = snet_getline( sn, &tv )) != NULL ) {
	tv.tv_sec = 60 * 10;
	tv.tv_usec = 0;
	if (( ac = argcargv( line, &av )) < 0 ) {
	    syslog( LOG_ERR, "argcargv: %m" );
	    break;
	}

	if ( strcmp( line, "." ) == 0 ) {
	    break;
	}

	if ( ac != 3 ) {
	    syslog( LOG_ERR, "f_time: wrong number of args" );
	    continue;
	}

	if ( strchr( av[ 0 ], '/' ) != NULL ) {
	    syslog( LOG_ERR, "f_time: cookie name contains '/'" );
	    continue;
	}

	if ( strncmp( av[ 0 ], "cosign=", 7 ) != 0 ) {
	    syslog( LOG_ERR, "f_time: cookie name malformat" );
	    continue;
	}

	if ( strlen( av[ 0 ] ) >= MAXCOOKIELEN ) {
	    syslog( LOG_ERR, "f_time: cookie name too long" );
	    continue;
	}

	if ( stat( av[ 0 ], &st ) != 0 ) {
	    syslog( LOG_DEBUG, "f_time: %s: %m", av[ 0 ] );
	    continue;
	}

	timestamp = atoi( av[ 1 ] ); 
	if ( timestamp > st.st_mtime ) {
	    new_time.modtime = timestamp;
	    utime( av[ 0 ], &new_time );
	}

	state = atoi( av[ 2 ] );
	if ( state == 0 ) {
	    if ( do_logout( av[ 0 ] ) < 0 ) {
		syslog( LOG_ERR, "f_time: %s should be logged out!", av[ 0 ] );
	    }
	}
    }

    snet_writef( sn, "%d TIME successful: we are now up-to-date\r\n", 260 );
    return( 0 );

}
    int
f_logout( SNET *sn, int ac, char *av[], SNET *pushersn )
{
    struct cinfo	ci;

    /*LOGOUT login_cookie ip */

    if ( ch->ch_key != CGI ) {
	syslog( LOG_ERR, "%s not allowed to logout", ch->ch_hostname );
	snet_writef( sn, "%d LOGOUT: %s not allowed to logout.\r\n",
		410, ch->ch_hostname );
	return( 1 );
    }

    if ( ac != 3 ) {
	snet_writef( sn, "%d LOGOUT: Wrong number of args.\r\n", 510 );
	return( 1 );
    }

    if ( strchr( av[ 1 ], '/' ) != NULL ) {
	syslog( LOG_ERR, "f_logout: cookie name contains '/'" );
	snet_writef( sn, "%d LOGOUT: Invalid cookie name.r\n", 511 );
	return( 1 );
    }

    if ( strlen( av[ 1 ] ) >= MAXCOOKIELEN ) {
	snet_writef( sn, "%d LOGOUT: Cookie too long\r\n", 512 );
	return( 1 );
    }

    if ( read_cookie( av[ 1 ], &ci ) != 0 ) {
	syslog( LOG_ERR, "f_logout: read_cookie" );
	snet_writef( sn, "%d LOGOUT error: Sorry\r\n", 513 );
	return( 1 );
    }

    if ( ci.ci_state == 0 ) {
	syslog( LOG_ERR, "f_logout: %s already logged out", av[ 1 ] );
	snet_writef( sn, "%d LOGOUT: Already logged out\r\n", 411 );
	return( 1 );
    }

    if ( do_logout( av[ 1 ] ) < 0 ) {
	syslog( LOG_ERR, "f_logout: %s: %m", av[ 1 ] );
	return( -1 );
    }

    snet_writef( sn, "%d LOGOUT successful: cookie no longer valid\r\n", 210 );
    if (( pushersn != NULL ) && ( replicate )) {
	snet_writef( pushersn, "LOGOUT %s %s\r\n", av[ 1 ], av [ 2 ] );
    }
    return( 0 );

}

/*
 * associate serivce with login
 * 0 = OK
 * -1 = unknown fatal error
 * 1 = already registered
 */
    int
do_register( char *login, char *scookie )
{
    int			fd;
    char		tmppath[ MAXCOOKIELEN ];
    FILE		*tmpfile;
    struct timeval	tv;

    if ( gettimeofday( &tv, NULL ) != 0 ){
	syslog( LOG_ERR, "f_register: gettimeofday: %m" );
	return( -1 );
    }

    if ( snprintf( tmppath, sizeof( tmppath ), "%x%x.%i",
	    tv.tv_sec, tv.tv_usec, (int)getpid()) >= sizeof( tmppath )) {
	syslog( LOG_ERR, "f_register: tmppath too long" );
	return( -1 );
    }

    if (( fd = open( tmppath, O_CREAT|O_EXCL|O_WRONLY, 0644 )) < 0 ) {
	syslog( LOG_ERR, "f_register: open: %m" );
	return( -1 );
    }

    if (( tmpfile = fdopen( fd, "w" )) == NULL ) {
	if ( unlink( tmppath ) != 0 ) {
	    syslog( LOG_ERR, "f_register: unlink: %m" );
	}
	syslog( LOG_ERR, "f_register: fdopen: %m" );
	return( -1 );
    }

    /* the service cookie file contains the login cookie only */
    fprintf( tmpfile, "l%s\n", login );

    if ( fclose ( tmpfile ) != 0 ) {
	if ( unlink( tmppath ) != 0 ) {
	    syslog( LOG_ERR, "f_register: unlink: %m" );
	}
	return( -1 );
    }

    if ( link( tmppath, scookie ) != 0 ) {
	if ( unlink( tmppath ) != 0 ) {
	    syslog( LOG_ERR, "f_register: unlink: %m" );
	}
	if ( errno == EEXIST ) {
	    syslog( LOG_ERR,
		    "f_register: service cookie already exists: %s", scookie );
	    return( 1 );
	}
	syslog( LOG_ERR, "f_register: link: %m" );
	return( -1 );
    }

    if ( unlink( tmppath ) != 0 ) {
	syslog( LOG_ERR, "f_register: unlink: %m" );
	return( -1 );
    }

    utime( login, NULL );

    return( 0 );
}

    int
f_register( SNET *sn, int ac, char *av[], SNET *pushersn )
{
    struct cinfo	ci;
    struct timeval	tv;
    int			rc;

    /* REGISTER login_cookie ip service_cookie */

    if ( ch->ch_key != CGI ) {
	syslog( LOG_ERR, "%s not allowed to register", ch->ch_hostname );
	snet_writef( sn, "%d REGISTER: %s not allowed to register.\r\n",
		420, ch->ch_hostname );
	return( 1 );
    }

    if ( ac != 4 ) {
	snet_writef( sn, "%d REGISTER: Wrong number of args.\r\n", 520 );
	return( 1 );
    }

    if ( strchr( av[ 1 ], '/' ) != NULL ) {
	syslog( LOG_ERR, "f_register: cookie name contains '/'" );
	snet_writef( sn, "%d REGISTER: Invalid cookie name.\r\n", 521 );
	return( 1 );
    }

    if ( strlen( av[ 1 ] ) >= MAXCOOKIELEN ||
	    strlen( av[ 3 ] ) >= MAXCOOKIELEN ) {
	snet_writef( sn, "%d REGISTER: Cookie too long\r\n", 522 );
	return( 1 );
    }

    if ( read_cookie( av[ 1 ], &ci ) != 0 ) {
	syslog( LOG_DEBUG, "f_register: %s", av[ 1 ] );
	snet_writef( sn, "%d REGISTER error: Sorry\r\n", 523 );
	return( 1 );
    }

    if ( ci.ci_state == 0 ) {
	syslog( LOG_ERR,
		"f_register: %s already logged out, can't register", av[ 1 ] );
	snet_writef( sn, "%d REGISTER: Already logged out\r\n", 420 );
	return( 1 );
    }

    if ( gettimeofday( &tv, NULL ) != 0 ){
	syslog( LOG_ERR, "f_register: gettimeofday: %m" );
	return( -1 );
    }

    /* check for idle timeout, and if so, log'em out */
    if ( tv.tv_sec - ci.ci_itime > IDLE_OUT &&
	    tv.tv_sec - ci.ci_itime < (IDLE_OUT + GREY )) {
	snet_writef( sn, "%d REGISTER: Idle Grey Window\r\n", 521 );
	return( 1 );
     } else if ( tv.tv_sec - ci.ci_itime >  ( IDLE_OUT + GREY )) {
	syslog( LOG_INFO, "f_register: idle time out!\n" );
	if ( do_logout( av[ 1 ] ) < 0 ) {
	    syslog( LOG_ERR, "f_register: %s: %m", av[ 1 ] );
	    snet_writef( sn, "%d REGISTER error: Sorry!\r\n", 524 );
	    return( -1 );
	}
	snet_writef( sn, "%d REGISTER: Idle logged out\r\n", 421 );
	return( 1 );
    }

    if (( rc = do_register( av[ 1 ], av[ 3 ] )) < 0 ) {
	return( -1 );
    }

    /* because already registered is not fatal */
    if ( rc > 0 ) {
	snet_writef( sn,
		"%d REGISTER error: Cookie already exists\r\n", 226 );
	return( rc );
    }

    snet_writef( sn, "%d REGISTER successful: Cookie Stored \r\n", 220 );
    if (( pushersn != NULL ) && ( replicate )) {
	snet_writef( pushersn, "REGISTER %s %s %s\r\n",
		av[ 1 ], av[ 2 ], av [ 3 ] );
    }
    return( 0 );
}

    int
f_check( SNET *sn, int ac, char *av[], SNET *pushersn )
{
    struct cinfo 	ci;
    struct timeval	tv;
    char		login[ MAXCOOKIELEN ];
    int			status;

    /* CHECK (service/login)cookie */

    if (( ch->ch_key != CGI ) && ( ch->ch_key != SERVICE )) {
	syslog( LOG_ERR, "%s not allowed to check", ch->ch_hostname );
	snet_writef( sn, "%d CHECK: %s not allowed to check.\r\n",
		430, ch->ch_hostname );
	return( 1 );
    }

    if ( ac != 2 ) {
	snet_writef( sn, "%d CHECK: Wrong number of args.\r\n", 530 );
	return( 1 );
    }

    if ( strchr( av[ 1 ], '/' ) != NULL ) {
	syslog( LOG_ERR, "f_check: cookie name contains '/'" );
	snet_writef( sn, "%d CHECK: Invalid cookie name.\r\n", 531 );
	return( 1 );
    }

    if ( strlen( av[ 1 ] ) >= MAXCOOKIELEN ) {
	snet_writef( sn, "%d CHECK: Service Cookie too long\r\n", 532 );
	return( 1 );
    }

    if ( strncmp( av[ 1 ], "cosign-", 7 ) == 0 ) {
	status = 231;
	if ( service_to_login( av[ 1 ], login ) != 0 ) {
	    snet_writef( sn, "%d CHECK: cookie not in db!\r\n", 533 );
	    return( 1 );
	}
    } else {
	status = 232;
	strcpy( login, av[ 1 ] );
    }

    if ( read_cookie( login, &ci ) != 0 ) {
	syslog( LOG_DEBUG, "f_check: %s", login );
	snet_writef( sn, "%d CHECK: Who me? Dunno.\r\n", 534 );
	return( 1 );
    }

    if ( ci.ci_state == 0 ) {
	syslog( LOG_ERR,
		"f_check: %s logged out", login );
	snet_writef( sn, "%d CHECK: Already logged out\r\n", 430 );
	return( 1 );
    }


    /* check for idle timeout, and if so, log'em out */
    if ( gettimeofday( &tv, NULL ) != 0 ){
	syslog( LOG_ERR, "f_check: gettimeofday: %m" );
	return( -1 );
    }

    if ( tv.tv_sec - ci.ci_itime > IDLE_OUT &&
	    tv.tv_sec - ci.ci_itime < (IDLE_OUT + GREY )) {
	snet_writef( sn, "%d CHECK: Idle Grey Window\r\n", 531 );
	return( 1 );
    } else if ( tv.tv_sec - ci.ci_itime > IDLE_OUT ) {
	syslog( LOG_INFO, "f_check: idle time out!\n" );
	snet_writef( sn, "%d CHECK: Idle logged out\r\n", 431 );
	if ( do_logout( login ) < 0 ) {
	    syslog( LOG_ERR, "f_check: %s: %m", login );
	    return( -1 );
	}
	return( 1 );
    }

    /* prevent idle out if we are actually using it */
    utime( login, NULL );

    snet_writef( sn,
	    "%d %s %s %s\r\n", status, ci.ci_ipaddr, ci.ci_user, ci.ci_realm );
    return( 0 );
}

    int
f_retr( SNET *sn, int ac, char *av[], SNET *pushersn )
{
    struct cinfo        ci;
    struct timeval      tv;
    char		login[ MAXCOOKIELEN ];

    if (( ch->ch_key != SERVICE ) || ( ch->ch_key == CGI )) {
	syslog( LOG_ERR, "%s not allowed to retreive", ch->ch_hostname );
	snet_writef( sn, "%d RETR: %s not allowed to retreive.\r\n",
		442, ch->ch_hostname );
	return( 1 );
    }

    if ( ac != 3 ) {
	snet_writef( sn, "%d RETR: Wrong number of args.\r\n", 540 );
	return( 1 );
    }

    if ( strchr( av[ 1 ], '/' ) != NULL ) {
	syslog( LOG_ERR, "f_retr: cookie name contains '/'" );
	snet_writef( sn, "%d RETR: Invalid cookie name.\r\n", 541 );
	return( 1 );
    }

    if ( strlen( av[ 1 ] ) >= MAXCOOKIELEN ) {
	snet_writef( sn, "%d RETR: Service Cookie too long\r\n", 542 );
	return( 1 );
    }

    if ( service_to_login( av[ 1 ], login ) != 0 ) {
	snet_writef( sn, "%d RETR: cookie not in db!\r\n", 543 );
	return( 1 );
    }

    if ( read_cookie( login, &ci ) != 0 ) {
	syslog( LOG_ERR, "f_retr: read_cookie" );
	snet_writef( sn, "%d RETR: Who me? Dunno.\r\n", 544 );
	return( 1 );
    }

    if ( ci.ci_state == 0 ) {
	syslog( LOG_ERR,
		"f_retr: %s logged out", login );
	snet_writef( sn, "%d RETR: Already logged out\r\n", 440 );
	return( 1 );
    }

    /* check for idle timeout, and if so, log'em out */
    if ( gettimeofday( &tv, NULL ) != 0 ){
	syslog( LOG_ERR, "f_retr: gettimeofday: %m" );
	return( -1 );
    }

    if ( tv.tv_sec - ci.ci_itime > IDLE_OUT &&
	    tv.tv_sec - ci.ci_itime < (IDLE_OUT + GREY )) {
	snet_writef( sn, "%d RETR: Idle Grey Window\r\n", 541 );
	return( 1 );
    } else if ( tv.tv_sec - ci.ci_itime > IDLE_OUT ) {
	syslog( LOG_INFO, "f_retr: idle time out!\n" );
	snet_writef( sn, "%d RETR: Idle logged out\r\n", 441 );
	if ( do_logout( login ) < 0 ) {
	    syslog( LOG_ERR, "f_retr: %s: %m", login );
	    return( -1 );
	}
	return( 1 );
    }

    if ( strcmp( av[ 2 ], "tgt") == 0 ) {
	return( retr_ticket( sn, &ci ));
    } else if ( strcmp( av[ 2 ], "cookies") == 0 ) {
	return( retr_proxy( sn, login, pushersn ));
    }

    syslog( LOG_ERR, "f_retr: no such retrieve type: %s", av[ 1 ] );
    snet_writef( sn, "%d RETR: No such retrieve type.\r\n", 441 );
    return( 1 );
}

    int
retr_proxy( SNET *sn, char *login, SNET *pushersn )
{
    char		cookiebuf[ 128 ];
    char		tmppath[ MAXCOOKIELEN ];
    struct proxies	*proxy;
    int			rc;

    if (( ch->ch_flag & CH_PROXY ) == 0 ) {
	syslog( LOG_ERR, "%s cannot retrieve cookies", ch->ch_hostname );
	snet_writef( sn, "%d RETR: %s cannot retrieve cookies.\r\n",
		443, ch->ch_hostname );
	return( 1 );
    }

    for ( proxy = ch->ch_proxies; proxy != NULL; proxy = proxy->pr_next ) {
	if ( mkcookie( sizeof( cookiebuf ), cookiebuf ) != 0 ) {
	    return( -1 );
	}

	if ( snprintf( tmppath, sizeof( tmppath ), "%s=%s",
		proxy->pr_cookie, cookiebuf ) >= sizeof( tmppath )) {
	    syslog( LOG_ERR, "retr_proxy: tmppath too long" );
	    return( -1 );
	}

	if (( rc = do_register( login, tmppath )) < 0 ) {
	    continue;
	}

	if (( pushersn != NULL ) && ( replicate )) {
	    snet_writef( pushersn, "REGISTER %s - %s\r\n",
		    login, tmppath );
	}
	snet_writef( sn, "%d-%s %s\r\n", 241, tmppath, proxy->pr_hostname );
    }
    snet_writef( sn, "%d Cookies registered and sent\r\n", 241 );

    return( 0 );
}

    int
retr_ticket( SNET *sn, struct cinfo *ci )
{
    struct stat		st;
    int			fd;
    ssize_t             readlen;
    char                buf[8192];
    struct timeval      tv;

    /* RETR service-cookie TicketType */
    if (( ch->ch_flag & CH_TICKET ) == 0 ) {
	syslog( LOG_ERR, "%s not allowed to retrieve tkts", ch->ch_hostname );
	snet_writef( sn, "%d RETR: %s not allowed to retrieve tkts.\r\n",
		441, ch->ch_hostname );
	return( 1 );
    }

    /* if we get here, we can give them the data pointed to by k */

    if (( fd = open( ci->ci_krbtkt, O_RDONLY, 0 )) < 0 ) {
        syslog( LOG_ERR, "open: %s: %m", ci->ci_krbtkt );
        snet_writef( sn, "%d Unable to access %s.\r\n", 547, ci->ci_krbtkt );
        return( 1 );
    }
   
    /* dump file info */

    if ( fstat( fd, &st ) < 0 ) {
        syslog( LOG_ERR, "f_retr: fstat: %m" );
        snet_writef( sn, "%d Access Error: %s\r\n", 548, ci->ci_krbtkt );
        if ( close( fd ) < 0 ) {
            syslog( LOG_ERR, "close: %m" );
            return( -1 );
        }
        return( 1 );
    }

    snet_writef( sn, "%d Retrieving file\r\n", 240 );
    snet_writef( sn, "%d\r\n", (int)st.st_size );

    /* dump file */

    while (( readlen = read( fd, buf, sizeof( buf ))) > 0 ) {
        tv.tv_sec = 60 * 60 ;
        tv.tv_usec = 0;
        if ( snet_write( sn, buf, (int)readlen, &tv ) != readlen ) {
            syslog( LOG_ERR, "snet_write: %m" );
            return( -1 );
        }
    }

    if ( readlen < 0 ) {
        syslog( LOG_ERR, "read: %m" );
	close( fd );
        return( -1 );
    }

    if ( close( fd ) < 0 ) {
        syslog( LOG_ERR, "close: %m" );
        return( -1 );
    }

    snet_writef( sn, ".\r\n" );

    return( 0 );
}


    int
command( int fd, SNET *pushersn )
{
    SNET				*snet;
    int					ac, i;
    char				**av, *line;
    struct timeval			tv;
    extern int				errno;

    srandom( (unsigned)getpid());

    if (( snet = snet_attach( fd, 1024 * 1024 )) == NULL ) {
	syslog( LOG_ERR, "snet_attach: %m" );
	/* We *could* use write(2) to report an error before we exit here */
	exit( 1 );
    }

    /* for debugging, TLS not required but still available */
    if ( tlsopt ) {
	commands = auth_commands;
	ncommands = sizeof( auth_commands ) / sizeof( auth_commands[ 0 ] );
	if (( ch = chosts_find( "DEBUG" )) == NULL ) {
	    syslog( LOG_ERR, "No debugging access" );
	    snet_writef( snet, "%d No DEBUG access\r\n", 508 );
	    exit( 1 );
	}
    }

    snet_writef( snet, "%d COokie SIGNer ready\r\n", 220 );

    tv.tv_sec = 60 * 10;	/* 10 minutes, should get this from config */
    tv.tv_usec = 0;

    while (( line = snet_getline( snet, &tv )) != NULL ) {
	/* log everything we get to stdout if we're debugging */
	if ( debug ) {
	    printf( "debug: %s\n", line );
	}
	tv.tv_sec = 60 * 10;
	tv.tv_usec = 0;
	if (( ac = argcargv( line, &av )) < 0 ) {
	    syslog( LOG_ERR, "argcargv: %m" );
	    break;
	}

	if ( ac == 0 ) {
	    snet_writef( snet, "%d Command unrecognized\r\n", 501 );
	    continue;
	}

	for ( i = 0; i < ncommands; i++ ) {
	    if ( strcasecmp( av[ 0 ], commands[ i ].c_name ) == 0 ) {
		break;
	    }
	}
	if ( i >= ncommands ) {
	    snet_writef( snet, "%d Command %s unregcognized\r\n",
		    500, av[ 0 ] );
	    continue;
	}

	if ( (*(commands[ i ].c_func))( snet, ac, av, pushersn ) < 0 ) {
	    break;
	}
    }

    if ( line != NULL ) {
	snet_writef( snet,
		"421 Service not available, closing transmission channel\r\n" );
    } else {
	if ( snet_eof( snet )) {
	    exit( 0 );
	} else if ( errno == ETIMEDOUT ) {
	    exit( 0 );
	} else {
	    syslog( LOG_ERR, "snet_getline: %m" );
	}
    }

    exit( 1 );

}
