/*
 * This file is part of the Sofia-SIP package
 *
 * Copyright (C) 2005 Nokia Corporation.
 *
 * Contact: Pekka Pessi <pekka.pessi@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

/**@CFILE tport_tls.c
 * @brief TLS interface
 * 
 * @author Mikko Haataja <ext-Mikko.A.Haataja@nokia.com>
 * @author Pekka Pessi <ext-Pekka.Pessi@nokia.com>
 *
 * Copyright 2001, 2002 Nokia Research Center.  All rights reserved.
 *
 */

#include "config.h"

#define OPENSSL_NO_KRB5 oh-no

#include <openssl/lhash.h>
#include <openssl/bn.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/bio.h>
#include <openssl/opensslv.h>

#include <sofia-sip/su_types.h>
#include <sofia-sip/su.h>
#include <sofia-sip/su_wait.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HAVE_SIGPIPE
#include <signal.h>
#endif

#include "tport_tls.h"

char const tls_version[] = OPENSSL_VERSION_TEXT;

enum  { tls_master, tls_slave };

struct tls_s {
  SSL_CTX *ctx;
  SSL *con;
  BIO *bio_con;
  BIO *bio_err;
  int type;
  int verified;

  /* Receiving */
  int read_events;
  void *read_buffer;
  size_t read_buffer_len;

  /* Sending */
  int   write_events;
  void *write_buffer;
  size_t write_buffer_len;

  /* Host names */
  char *hosts[TLS_MAX_HOSTS + 1];
};

enum { tls_buffer_size = 16384 };

static
tls_t *tls_create(int type)
{
  tls_t *tls = calloc(1, sizeof(*tls));

  if (tls)
    tls->type = type;

  return tls;
}

static
void tls_set_default(tls_issues_t *i)
{
  i->verify_depth = i->verify_depth == 0 ? 2 : i->verify_depth;
  i->cert = i->cert ? i->cert : "agent.pem";
  i->key = i->key ? i->key : i->cert;
  i->randFile = i->randFile ? i->randFile : "tls_seed.dat";
  i->CAfile = i->CAfile ? i->CAfile : "cafile.pem";
  i->cipher = i->cipher ? i->cipher : "ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH";
  /* Default SIP cipher */
  /* "RSA-WITH-AES-128-CBC-SHA"; */
  /* RFC-2543-compatibility ciphersuite */
  /* TLS_RSA_WITH_3DES_EDE_CBC_SHA; */
}

static
int tls_verify_cb(int ok, X509_STORE_CTX *store)
{
  char data[256];

  X509 *cert = X509_STORE_CTX_get_current_cert(store);
  int  depth = X509_STORE_CTX_get_error_depth(store);
  int  err = X509_STORE_CTX_get_error(store);

#if nomore 
  509_NAME_oneline(X509_get_subject_name(cert), data, 256);
  fprintf(stderr,"depth=%d %s\n",depth,data);
#endif

  if (!ok)
  {
    fprintf(stderr, "-Error with certificate at depth: %i\n", depth);
    X509_NAME_oneline(X509_get_issuer_name(cert), data, 256);
    fprintf(stderr, "  issuer   = %s\n", data);
    X509_NAME_oneline(X509_get_subject_name(cert), data, 256);
    fprintf(stderr, "  subject  = %s\n", data);
    fprintf(stderr, "  err %i:%s\n", err, X509_verify_cert_error_string(err));
  }
 
  return 1;			/* Always return "ok" */
}

static
int tls_init_context(tls_t *tls, tls_issues_t const *ti)
{
  static int initialized = 0;

  if (!initialized) {
    initialized = 1;
    SSL_library_init();
    SSL_load_error_strings();

    if (ti->randFile &&
	!RAND_load_file(ti->randFile, 1024 * 1024)) {
      if (ti->configured > 1) {
	BIO_printf(tls->bio_err, "%s: cannot open randFile %s\n", 
		   "tls_init_context", ti->randFile);
	ERR_print_errors(tls->bio_err);
      }
      /* errno = EIO; */
      /* return -1; */
    }
  }

#if HAVE_SIGPIPE
  /* Avoid possible SIGPIPE when sending close_notify */
  signal(SIGPIPE, SIG_IGN);
#endif

  if (tls->bio_err == NULL)
    tls->bio_err = BIO_new_fp(stderr, BIO_NOCLOSE);

  if (tls->ctx == NULL) {
    SSL_METHOD *meth;

    /* meth = SSLv3_method(); */
    /* meth = SSLv23_method(); */

    if (ti->version)
      meth = TLSv1_method();
    else
      meth = SSLv23_method();

    tls->ctx = SSL_CTX_new(meth);
  }

  if (tls->ctx == NULL) {
    ERR_print_errors(tls->bio_err);
    errno = EIO;
    return -1;
  }

  if (!SSL_CTX_use_certificate_file(tls->ctx, 
				    ti->cert,
				    SSL_FILETYPE_PEM)) {
    if (ti->configured > 0) {
      BIO_printf(tls->bio_err, "%s: invalid certificate: %s\n",
		 "tls_init_context", ti->cert);
      ERR_print_errors(tls->bio_err);
#if require_client_certificate
      errno = EIO;
      return -1;
#endif
    }
  }

  if (!SSL_CTX_use_PrivateKey_file(tls->ctx, 
                                   ti->key, 
                                   SSL_FILETYPE_PEM)) {
    if (ti->configured > 0) {
      ERR_print_errors(tls->bio_err);
#if require_client_certificate
      errno = EIO;
      return -1;
#endif
    }
  }

  if (!SSL_CTX_check_private_key(tls->ctx)) {
    if (ti->configured > 0) {
      BIO_printf(tls->bio_err,
		 "Private key does not match the certificate public key\n");
    }
#if require_client_certificate
    errno = EIO;
    return -1;
#endif
  }

  if (!SSL_CTX_load_verify_locations(tls->ctx, 
                                     ti->CAfile, 
                                     ti->CApath)) {
    if (ti->configured > 0)
      ERR_print_errors(tls->bio_err);
    errno = EIO;
    return -1;
  }

  SSL_CTX_set_verify_depth(tls->ctx, ti->verify_depth);

  SSL_CTX_set_verify(tls->ctx, 
		     getenv("SSL_VERIFY_PEER") ? SSL_VERIFY_PEER : SSL_VERIFY_NONE
		     /* SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT */,
                     tls_verify_cb);

  if (!SSL_CTX_set_cipher_list(tls->ctx, ti->cipher)) {
    BIO_printf(tls->bio_err,"error setting cipher list\n");
    ERR_print_errors(tls->bio_err);
    errno = EIO;
    return -1;
  }

  return 0;
}

void tls_free(tls_t *tls)
{
  int k;

  if (!tls)
    return;

  if (tls->read_buffer)
    free(tls->read_buffer), tls->read_buffer = NULL;

  if (tls->con != NULL)
    SSL_shutdown(tls->con);

  if (tls->ctx != NULL && tls->type != tls_slave)
    SSL_CTX_free(tls->ctx);

  if (tls->bio_con != NULL)
    BIO_free(tls->bio_con);

  if (tls->bio_err != NULL && tls->type != tls_slave)
    BIO_free(tls->bio_err);

  for (k = 0; k < TLS_MAX_HOSTS; k++)
    free(tls->hosts[k]), tls->hosts[k] = NULL;

  free(tls);
}

int tls_get_socket(tls_t *tls)
{
  int sock = -1;

  if (tls != NULL && tls->bio_con != NULL)
    BIO_get_fd(tls->bio_con, &sock);

  return sock;
}

tls_t *tls_init_master(tls_issues_t *ti)
{
  /* Default id in case RAND fails */ 
  unsigned char sessionId[32] = "sofia/tls"; 
  tls_t *tls;

#if HAVE_SIGPIPE
  signal(SIGPIPE, SIG_IGN);  /* Ignore spurios SIGPIPE from OpenSSL */
#endif

  tls_set_default(ti);

  if (!(tls = tls_create(tls_master)))
    return NULL;

  if (tls_init_context(tls, ti) < 0) {
    int err = errno;
    tls_free(tls);
    errno = err;
    return NULL;
  }

  RAND_pseudo_bytes(sessionId, sizeof(sessionId));

  SSL_CTX_set_session_id_context(tls->ctx,
                                 (void*) sessionId,
				 sizeof(sessionId)); 
  
  if (ti->CAfile != NULL)
    SSL_CTX_set_client_CA_list(tls->ctx,
                               SSL_load_client_CA_file(ti->CAfile));

#if 0
  if (sock != -1) {
    tls->bio_con = BIO_new_socket(sock, BIO_NOCLOSE);

    if (tls->bio_con == NULL) {
      BIO_printf(tls->bio_err, "tls_init_master: BIO_new_socket failed\n");
      ERR_print_errors(tls->bio_err);
      tls_free(tls);
      errno = EIO;
      return NULL;
    }
  }
#endif

  return tls;
}

#if 0
#include <poll.h>

static 
int tls_accept(tls_t *tls)
{
  int ret = SSL_accept(tls->con);
  int verify_result;

  if (ret <= 0) {
    int err = SSL_get_error(tls->con, ret);
    switch(err) {
    case SSL_ERROR_WANT_READ:
      return errno = EAGAIN, tls->read_events = SU_WAIT_IN, 0;
    case SSL_ERROR_WANT_WRITE:
      return errno = EAGAIN, tls->read_events = SU_WAIT_OUT, 0;

    default:    
      BIO_printf(tls->bio_err, "SSL_connect failed: %d %s\n", 
                 err,
                 ERR_error_string(err, NULL));
      ERR_print_errors(tls->bio_err);
      return -1;
    }
  }

  verify_result = SSL_get_verify_result(tls->con);

  if (verify_result != X509_V_OK) {
    BIO_printf(tls->bio_err, 
               "Client certificate doesn't verify: %s\n",
               X509_verify_cert_error_string(verify_result));
#if 0
    tls_free(tls);
    return NULL;
#endif
  }

  if (SSL_get_peer_certificate(tls->con) == NULL) {
    BIO_printf(tls->bio_err, "Client didn't send certificate\n");
#if 0
    tls_free(tls);
    return NULL;
#endif
  }

  return 1;
}
#endif

tls_t *tls_clone(tls_t *master, int sock, int accept)
{
  tls_t *tls = tls_create(tls_slave);

  if (tls) {
    tls->ctx = master->ctx;
    tls->bio_err = master->bio_err;

    if (!(tls->read_buffer = malloc(tls_buffer_size)))
      free(tls), tls = NULL;
  }
  if (!tls)
    return tls;

  assert(sock != -1);

  tls->bio_con = BIO_new_socket(sock, BIO_NOCLOSE); 
  tls->con = SSL_new(tls->ctx);

  if (tls->con == NULL) {
    BIO_printf(tls->bio_err, "tls_clone: SSL_new failed\n");
    ERR_print_errors(tls->bio_err);
    tls_free(tls);
    errno = EIO;
    return NULL;
  }

  SSL_set_bio(tls->con, tls->bio_con, tls->bio_con);
  if (accept)
    SSL_set_accept_state(tls->con);
  else
    SSL_set_connect_state(tls->con);
  SSL_set_mode(tls->con, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

  su_setblocking(sock, 0);
  tls_read(tls); /* XXX - works only with non-blocking sockets */

  return tls;
}

tls_t *tls_init_slave(tls_t *master, int sock)
{
  int accept;
  return tls_clone(master, sock, accept = 1);
}

tls_t *tls_init_client(tls_t *master, int sock)
{
  int accept;
  return tls_clone(master, sock, accept = 0);
}

static char *tls_strdup(char const *s)
{
  if (s) {
    size_t len = strlen(s) + 1;
    char *d = malloc(len);
    if (d)
      memcpy(d, s, len);
    return d;
  }
  return NULL;
}

static
int tls_post_connection_check(tls_t *tls)
{
  X509 *cert;
  int extcount;
  int k, i, j, error;

  if (!tls) return -1;

  cert = SSL_get_peer_certificate(tls->con); 
  if (!cert)
    return X509_V_OK;
  
  extcount = X509_get_ext_count(cert);

  for (k = 0; k < TLS_MAX_HOSTS && tls->hosts[k]; k++)
    ;

  /* Find matching subjectAltName.DNS */
  for (i = 0; i < extcount; i++) {
    X509_EXTENSION *ext;
    char const *name;
    X509V3_EXT_METHOD *vp;
    STACK_OF(CONF_VALUE) *values;
    CONF_VALUE *value;
    void *d2i;

    ext = X509_get_ext(cert, i);
    name = OBJ_nid2sn(OBJ_obj2nid(X509_EXTENSION_get_object(ext)));
    
    if (strcmp(name, "subjectAltName") != 0)
      continue;
      
    vp = X509V3_EXT_get(ext); if (!vp) continue;
    d2i = X509V3_EXT_d2i(ext);
    values = vp->i2v(vp, d2i, NULL);
    
    for (j = 0; j < sk_CONF_VALUE_num(values); j++) {
      value = sk_CONF_VALUE_value(values, j);
      if (strcmp(value->name, "DNS") == 0) {
	if (k < TLS_MAX_HOSTS) {
	  tls->hosts[k] = tls_strdup(value->value);
	  k += tls->hosts[k] != NULL;
	}
      }
      else if (strcmp(value->name, "URI") == 0) {
	char const *uri = strchr(value->value, ':');
	if (uri ++ && k < TLS_MAX_HOSTS) {
	  tls->hosts[k] = tls_strdup(uri);
	  k += tls->hosts[k] != NULL;
	}
      }
    }
  }
   
  if (k < TLS_MAX_HOSTS) {
    X509_NAME *subject;
    char name[256];

    subject = X509_get_subject_name(cert);
    if (subject) {
      if (X509_NAME_get_text_by_NID(subject, NID_commonName, 
				    name, sizeof name) > 0) {
	name[(sizeof name) - 1] = '\0';

	for (i = 0; tls->hosts[i]; i++) 
	  if (strcasecmp(tls->hosts[i], name) == 0)
	    break;

	if (i == k)
	  tls->hosts[k++] = tls_strdup(name);
      }
    }
  }

  X509_free(cert);

  error = SSL_get_verify_result(tls->con);

  if (error == X509_V_OK)
    tls->verified = 1;
  
  return error;
}

int tls_check_hosts(tls_t *tls, char const *hosts[TLS_MAX_HOSTS])
{
  int i, j;

  if (tls == NULL) { errno = EINVAL; return -1; }
  if (!tls->verified) { errno = EAGAIN; return -1; }

  if (!hosts) 
    return 0;

  for (i = 0; hosts[i]; i++) {
    for (j = 0; tls->hosts[j]; j++) {
      if (strcasecmp(hosts[i], tls->hosts[j]) == 0)
	break;
    }
    if (tls->hosts[j] == NULL) {
      errno = EACCES;
      return -1;
    }
  }
  
  return 0;
}

static
int tls_error(tls_t *tls, int ret, char const *who, char const *operation,
	      void *buf, int size)
{
  char errorbuf[128];
  int events = 0;
  int err = SSL_get_error(tls->con, ret);

  switch (err) {
  case SSL_ERROR_WANT_WRITE:
    events = SU_WAIT_OUT;
    break;

  case SSL_ERROR_WANT_READ:
    events = SU_WAIT_IN;
    break;

  case SSL_ERROR_ZERO_RETURN:
    return 0;

  case SSL_ERROR_SYSCALL:
    if (SSL_get_shutdown(tls->con) & SSL_RECEIVED_SHUTDOWN)
      return 0;			/* EOS */
    if (errno == 0)
      return 0;			/* EOS */
    return -1;

  default:
    BIO_printf(tls->bio_err, "%s: %s failed (%d): %s\n", 
	       who, operation, err, ERR_error_string(err, errorbuf));
    ERR_print_errors(tls->bio_err);
    errno = EIO;
    return -1;
  }

  if (buf) {
    tls->write_events = events;
    tls->write_buffer = buf, tls->write_buffer_len = size;
  }
  else {
    tls->read_events = events;
  }

  errno = EAGAIN;
  return -1;
}

ssize_t tls_read(tls_t *tls)
{
  ssize_t ret;

  if (tls == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (0)
    fprintf(stderr, "tls_read(%p) called on %s (events %u)\n", (void *)tls,
	    tls->type == tls_slave ? "server" : "client",
	    tls->read_events);

  if (tls->read_buffer_len)
    return (ssize_t)tls->read_buffer_len;

  tls->read_events = SU_WAIT_IN;

  ret = SSL_read(tls->con, tls->read_buffer, tls_buffer_size);
  if (ret <= 0)
    return tls_error(tls, ret, "tls_read", "SSL_read", NULL, 0);

  if (!tls->verified) {
    int err = tls_post_connection_check(tls);

    if (err != X509_V_OK && 
	err != SSL_ERROR_SYSCALL &&
	err != SSL_ERROR_WANT_WRITE &&
	err != SSL_ERROR_WANT_READ) {
      BIO_printf(tls->bio_err, 
		 "%s: server certificate doesn't verify\n", 
		 "tls_read");
    }
  }

  return (ssize_t)(tls->read_buffer_len = ret);
}

void *tls_read_buffer(tls_t *tls, size_t N)
{
  assert(N == tls->read_buffer_len);
  tls->read_buffer_len = 0;
  return tls->read_buffer;
}

int tls_pending(tls_t const *tls)
{
  return tls && tls->con && SSL_pending(tls->con);
}

int tls_want_read(tls_t *tls, int events)
{
  if (tls && (events & tls->read_events)) {
    int ret = tls_read(tls);

    if (ret > 0)
      return 1;
    else if (ret == 0)
      return 0;
    else if (errno == EAGAIN)
      return 2;
    else
      return -1;
  }

  return 0;
}

ssize_t tls_write(tls_t *tls, void *buf, size_t size)
{
  ssize_t ret;

  if (0) 
    fprintf(stderr, "tls_write(%p, %p, "MOD_ZU") called on %s\n", 
	    (void *)tls, buf, size,
	    tls && tls->type == tls_slave ? "server" : "client");

  if (tls == NULL || buf == NULL) {
    errno = EINVAL;
    return -1;
  }

  if (tls->write_buffer) {
    assert(buf == tls->write_buffer);
    assert(size >= tls->write_buffer_len);
    assert(tls->write_events == 0);

    if (tls->write_events ||
	buf != tls->write_buffer || 
	size < tls->write_buffer_len) {
      errno = EIO;		
      return -1;
    }

    ret = tls->write_buffer_len;

    tls->write_buffer = NULL;
    tls->write_buffer_len = 0;

    return ret;
  }

  if (size == 0)
    return 0;

  tls->write_events = 0;

  if (!tls->verified) {
    if (tls_post_connection_check(tls) != X509_V_OK) {
      BIO_printf(tls->bio_err, 
		 "tls_read: server certificate doesn't verify\n");
    }
  }

  ret = SSL_write(tls->con, buf, size);
  if (ret < 0)
    return tls_error(tls, ret, "tls_write", "SSL_write", buf, size);

  return ret;
}

int tls_want_write(tls_t *tls, int events)
{
  if (tls && (events & tls->write_events)) {
    int ret;
    void *buf = tls->write_buffer;
    size_t size = tls->write_buffer_len;

    tls->write_events = 0;

    /* remove buf */
    tls->write_buffer = NULL;
    tls->write_buffer_len = 0;

    ret = tls_write(tls, buf, size);

    if (ret >= 0)
      /* Restore buf */
      return tls->write_buffer = buf, tls->write_buffer_len = ret;
    else if (errno == EAGAIN)
      return 0;
    else
      return -1;
  }
  return 0;
}

int tls_events(tls_t const *tls, int mask)
{

  if (!tls)
    return mask;

  if (tls->type == tls_master)
    return mask;
  
  return
    (mask & ~(SU_WAIT_IN|SU_WAIT_OUT)) |
    ((mask & SU_WAIT_IN) ? tls->read_events : 0) | 
    ((mask & SU_WAIT_OUT) ? tls->write_events : 0);
}
