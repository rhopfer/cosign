/*
 * Copyright (c) 2002 Regents of The University of Michigan.
 * All Rights Reserved.  See LICENSE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/param.h>
#include <krb5.h>
#include <snet.h>
#include "cgi.h"
#include "cosigncgi.h"
#include "network.h"

#define LOGIN_HTML	"../html-ssl/login.html"
#define ERROR_HTML	"../html-ssl/error.html"

char			*err = NULL;
char			*url = "http://www.umich.edu/";
char			*title = NULL;
char			*host = "beothuk.web.itd.umich.edu";
int			port = 6663;

struct cgi_list cl[] = {
#define CL_UNIQNAME	0
        { "uniqname", NULL },
#define CL_PASSWORD	1
        { "password", NULL },
        { NULL, NULL },
};

void            (*logger)( char * ) = NULL;

    void
subfile( char *filename )
{
    FILE	*fs;
    int 	c;

    fputs( "Content-type: text/html\n\n", stdout );

    if (( fs = fopen( filename, "r" )) == NULL ) {
	perror( filename );
	exit( 1 );
    }

    while (( c = getc( fs )) != EOF ) {
	if ( c == '$' ) {

	    switch ( c = getc( fs )) {
	    case 't':
		if ( title != NULL ) {
		    printf( "%s", title );
		}
		break;

	    case 'e':
		if ( err != NULL ) {
		    printf( "%s", err );
		}
		break;

	    case 'u':
                if (( cl[ CL_UNIQNAME ].cl_data != NULL ) &&
                        ( *cl[ CL_UNIQNAME ].cl_data != '\0' )) {
                    printf( "%s", cl[ CL_UNIQNAME ].cl_data );
                }
		break;

	    case 's':
		printf( "%s", getenv( "SCRIPT_NAME" ));
		break;

	    case EOF:
		putchar( '$' );
		break;

	    case '$':
		putchar( c );
		break;

	    default:
		putchar( '$' );
		putchar( c );
	    }
	} else {
	    putchar( c );
	}
    }

    if ( fclose( fs ) != 0 ) {
	perror( filename );
    }
    exit( 0 );
}


    int
main()
{
    krb5_error_code		kerror;
    krb5_context		kcontext;
    krb5_principal		kprinc;
    krb5_get_init_creds_opt	kopts;
    krb5_creds			kcreds;
    krb5_data			kd_rcs, kd_rs;
    int				len;
    char                	new_cookiebuf[ 128 ];
    char        		new_cookie[ 255 ];
    char			*data, *ip_addr, *referer;
    char			*cookie = NULL, *method, *script, *qs;
    char			*tmpl = LOGIN_HTML;

    if (( data = getenv( "HTTP_COOKIE" )) != NULL ) {
	cookie = strtok( data, ";" );
	if ( strncmp( cookie, "cosign=", 7 ) != 0 ) {
	    while (( cookie = strtok( NULL, ";" )) != NULL ) {
		if ( *cookie == ' ' ) ++cookie;
		if ( strncmp( cookie, "cosign=", 7 ) == 0 ) {
		    break;
		}
	    }
	}
    }

    method = getenv( "REQUEST_METHOD" );
    script = getenv( "SCRIPT_NAME" );
    ip_addr = getenv( "REMOTE_ADDR" );
    if (( referer = getenv( "HTTP_REFERER" )) == NULL ) {
	referer = "http://www.umich.edu/~clunis";
    }

    if ((( qs = getenv( "QUERY_STRING" )) != NULL ) && ( *qs != '\0' )) {
	if ( cookie == NULL ) {
	    tmpl = ERROR_HTML;
	    err = "You are not logged in yet. A link would be here.";
	    subfile( tmpl );
	}
	if ( strncmp( qs, "cosign-", 7 ) != 0 ) {
	    tmpl = ERROR_HTML;
	    err = "You mock me with your query string.";
	    subfile( tmpl );
	}
	if (( len = strlen( qs )) > MAXPATHLEN ) {
	    fprintf( stderr, "Query String too big\n" );
	    tmpl = ERROR_HTML;
	    err = "You mock me with your TOO LONG query string.";
	    subfile( tmpl );
	}
	qs[ len - 1 ] = '\0';
	if ( cosign_register( cookie, ip_addr, qs ) < 0 ) {
	    fprintf( stderr, "%s: cosign_register failed\n", script );
	    tmpl = ERROR_HTML;
	    err = "Register Failed. Oh Well.";
	    subfile( tmpl );

	}
	printf( "Location: %s\n\n", referer );
	exit( 0 );
	/* if no referer, redirect to top of site from conf file */
    }

    if ( cookie == NULL ) {
	if ( strcmp( method, "POST" ) == 0 ) {
	    /* turn on cookies */
	    fprintf( stderr, "we're posting cookie\n" );
	    tmpl = ERROR_HTML;
	    err = "Turn on cookies. Someday we'll have a link";
	    subfile( tmpl );
	}

	goto loginscreen;
    }

    /* no query string, yes cookie */

    if ( strcmp( method, "POST" ) != 0 ) {
	if ( cosign_check( cookie ) < 0 ) {
	    fprintf( stderr, "no longer logged in\n" );
	    err = "You are not logged in. Please log in now.";
	    goto loginscreen;
	}

	tmpl = ERROR_HTML;
	err = "This would be a service menu!";
	subfile( tmpl );
    }

    if ( cgi_info( CGI_STDIN, cl ) != 0 ) {
	fprintf( stderr, "%s: cgi_info failed\n", script );
	exit( 1 );
    }

    if (( cl[ CL_UNIQNAME ].cl_data == NULL ) ||
	    ( *cl[ CL_UNIQNAME ].cl_data == '\0' )) {
	err = "Please enter your uniqname and password.";
        subfile ( tmpl );
    }

    if (( cl[ CL_PASSWORD ].cl_data == NULL ) ||
	    ( *cl[ CL_PASSWORD ].cl_data == '\0' )) {
	err = "Unable to login because password is a required field.";
	title = "( missing password )";

        subfile ( tmpl );
    }

    if (( kerror = krb5_init_context( &kcontext ))) {
	err = (char *)error_message( kerror );
	title = "( kerberos error )";

	tmpl = ERROR_HTML;
	subfile ( tmpl );
    }

    if (( kerror = krb5_parse_name( kcontext, cl[ CL_UNIQNAME ].cl_data,
	    &kprinc ))) {
	err = (char *)error_message( kerror );
	title = "( kerberos error )";

	tmpl = ERROR_HTML;
	subfile ( tmpl );
    }

    krb5_get_init_creds_opt_init( &kopts );
    krb5_get_init_creds_opt_set_tkt_life( &kopts, 5*60 );
    krb5_get_init_creds_opt_set_renew_life( &kopts, 0 );
    krb5_get_init_creds_opt_set_forwardable( &kopts, 0 );
    krb5_get_init_creds_opt_set_proxiable( &kopts, 0 );

    if (( kerror = krb5_get_init_creds_password( kcontext, &kcreds, 
	    kprinc, cl[ CL_PASSWORD ].cl_data, krb5_prompter_posix, NULL, 0, 
	    "kadmin/changepw", &kopts ))) {

	if ( kerror == KRB5KRB_AP_ERR_BAD_INTEGRITY ) {

	    err = "Password incorrect.  Is [caps lock] on?";
	    title = "( Password Incorrect )";

	    subfile ( tmpl );
	} else {
	    err = (char *)error_message( kerror );
	    title = "( Password Error )";
	    
	    subfile ( tmpl );
	}
    }

    krb5_free_data_contents( kcontext, &kd_rs );
    krb5_free_data_contents( kcontext, &kd_rcs );

    /* password has been accepted, tell cosignd */
    err = "Your password has been accepted.";
    title = "Succeeded";
    tmpl = ERROR_HTML;

    if ( cosign_login( cookie, ip_addr, 
	    cl[ CL_UNIQNAME ].cl_data, "UMICH.EDU" ) < 0 ) {
	    fprintf( stderr, "%s: login failed\n", script ) ;
	    exit( 2 );
    }

    subfile ( tmpl );
    exit( 0 );

loginscreen:

    if ( mkcookie( sizeof( new_cookiebuf ), new_cookiebuf ) != 0 ) {
	fprintf( stderr, "%s: mkcookie: failed\n", script );
	exit( 1 );
    }

    sprintf( new_cookie, "cosign=%s", new_cookiebuf );
fprintf( stderr, "new_cookie is %s\n", new_cookie );
    printf( "Set-Cookie: %s; path=/; secure\n"
	    "Cache-Control: private, must-revalidate, no-cache\n"
	    "Expires: Mon, 16 Apr 1973 02:10:00 GMT\n"
	    "Pragma: no cache\n", new_cookie );
    subfile( tmpl );
    exit( 0 );
}
