/* $OpenLDAP$ */
/* 
 * Copyright 1999 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted only
 * as authorized by the OpenLDAP Public License.  A copy of this
 * license is available at http://www.OpenLDAP.org/license.html or
 * in file LICENSE in the top-level directory of the distribution.
 */

/*
 * LDAPv3 Extended Operation Request
 *	ExtendedRequest ::= [APPLICATION 23] SEQUENCE {
 *		requestName      [0] LDAPOID,
 *		requestValue     [1] OCTET STRING OPTIONAL
 *	}
 *
 * LDAPv3 Extended Operation Response
 *	ExtendedResponse ::= [APPLICATION 24] SEQUENCE {
 *		COMPONENTS OF LDAPResult,
 *		responseName     [10] LDAPOID OPTIONAL,
 *		response         [11] OCTET STRING OPTIONAL
 *	}
 *
 */

#include "portable.h"

#include <stdio.h>
#include <ac/socket.h>

#include "slap.h"

#define MAX_OID_LENGTH	128

typedef struct extop_list_t {
	struct extop_list_t *next;
	char *oid;
	SLAP_EXTOP_MAIN_FN ext_main;
} extop_list_t;

extop_list_t *supp_ext_list = NULL;

static extop_list_t *find_extop( extop_list_t *list, char *oid );

static int extop_callback(
	Connection *conn, Operation *op,
	int msg, int arg, void *argp);

char *
get_supported_extop (int index)
{
	extop_list_t *ext;

	/* linear scan is slow, but this way doesn't force a
	 * big change on root_dse.c, where this routine is used.
	 */
	for (ext = supp_ext_list; ext != NULL && --index >= 0; ext = ext->next) ;
	if (ext == NULL)
		return(NULL);
	return(ext->oid);
}

int
do_extended(
    Connection	*conn,
    Operation	*op
)
{
	int rc = LDAP_SUCCESS;
	char* oid;
	struct berval *reqdata;
	ber_tag_t tag;
	ber_len_t len;
	extop_list_t *ext;
	char *text;
	struct berval *rspdata;

	Debug( LDAP_DEBUG_TRACE, "do_extended\n", 0, 0, 0 );

	oid = NULL;
	reqdata = NULL;

	if( op->o_protocol < LDAP_VERSION3 ) {
		Debug( LDAP_DEBUG_ANY, "do_extended: protocol version (%d) too low\n",
			op->o_protocol, 0 ,0 );
		send_ldap_disconnect( conn, op,
			LDAP_PROTOCOL_ERROR, "requires LDAPv3" );
		rc = -1;
		goto done;
	}

	if ( ber_scanf( op->o_ber, "{a" /*}*/, &oid ) == LBER_ERROR ) {
		Debug( LDAP_DEBUG_ANY, "do_extended: ber_scanf failed\n", 0, 0 ,0 );
		send_ldap_disconnect( conn, op,
			LDAP_PROTOCOL_ERROR, "decoding error" );
		rc = -1;
		goto done;
	}

	if( !(ext = find_extop(supp_ext_list, oid)) ) {
		Debug( LDAP_DEBUG_ANY, "do_extended: unsupported operation \"%s\"\n",
			oid, 0 ,0 );
		send_ldap_result( conn, op, rc = LDAP_PROTOCOL_ERROR,
			NULL, "unsupported extended operation", NULL, NULL );
		goto done;
	}

	tag = ber_peek_tag( op->o_ber, &len );
	
	if( ber_peek_tag( op->o_ber, &len ) == LDAP_TAG_EXOP_REQ_VALUE ) {
		if( ber_scanf( op->o_ber, "O", &reqdata ) == LBER_ERROR ) {
			Debug( LDAP_DEBUG_ANY, "do_extended: ber_scanf failed\n", 0, 0 ,0 );
			send_ldap_disconnect( conn, op,
				LDAP_PROTOCOL_ERROR, "decoding error" );
			rc = -1;
			goto done;
		}
	}

	if( (rc = get_ctrls( conn, op, 1 )) != LDAP_SUCCESS ) {
		Debug( LDAP_DEBUG_ANY, "do_extended: get_ctrls failed\n", 0, 0 ,0 );
		return rc;
	} 

	Debug( LDAP_DEBUG_ARGS, "do_extended: oid=%s\n", oid, 0 ,0 );

	rspdata = NULL;
	text = NULL;

	rc = (ext->ext_main)( extop_callback, conn, op,
		oid, reqdata, &rspdata, &text );

	if( rc != SLAPD_ABANDON ) {
		send_ldap_extended( conn, op, rc, NULL, text,
			oid, rspdata );
	}

	if ( rspdata != NULL )
		ber_bvfree( rspdata );

	if ( text != NULL )
		free(text);

done:
	if ( reqdata != NULL ) {
		ber_bvfree( reqdata );
	}
	if ( oid != NULL ) {
		free( oid );
	}

	return rc;
}

int
load_extop(
	const char *ext_oid,
	SLAP_EXTOP_MAIN_FN ext_main )
{
	extop_list_t *ext;

	if( ext_oid == NULL || *ext_oid == '\0' ) return -1; 
	if(!ext_main) return -1; 

	ext = ch_calloc(1, sizeof(extop_list_t));
	if (ext == NULL)
		return(-1);

	ext->oid = ch_strdup( ext_oid );
	if (ext->oid == NULL) {
		free(ext);
		return(-1);
	}

	ext->ext_main = ext_main;
	ext->next = supp_ext_list;

	supp_ext_list = ext;

	return(0);
}


static extop_list_t *
find_extop( extop_list_t *list, char *oid )
{
	extop_list_t *ext;

	for (ext = list; ext; ext = ext->next) {
		if (strcmp(ext->oid, oid) == 0)
			return(ext);
	}
	return(NULL);
}

int
extop_callback(
	Connection *conn, Operation *op,
	int msg, int arg, void *argp)
{
	if (argp == NULL)
		return(-1);

	switch (msg) {
	case SLAPD_EXTOP_GETVERSION:
		*(int *)argp = 1;
		return(0);

	case SLAPD_EXTOP_GETPROTO:
		*(int *)argp = op->o_protocol;
		return(0);
	
	case SLAPD_EXTOP_GETAUTH:
		*(int *)argp = op->o_authtype;
		return(0);
	
	case SLAPD_EXTOP_GETDN:
		*(char **)argp = op->o_dn;
		return(0);
	
	case SLAPD_EXTOP_GETCLIENT:
		if (conn->c_peer_domain != NULL && *conn->c_peer_domain != 0) {
			*(char **)argp = conn->c_peer_domain;
			return(0);
		}
		if (conn->c_peer_name != NULL && *conn->c_peer_name != 0) {
			*(char **)argp = conn->c_peer_name;
			return(0);
		}
		break;
	
	default:
		break;
	}
	return(-1);
}
