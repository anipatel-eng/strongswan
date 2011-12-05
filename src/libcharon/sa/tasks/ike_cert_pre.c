/*
 * Copyright (C) 2008 Tobias Brunner
 * Copyright (C) 2006-2009 Martin Willi
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include "ike_cert_pre.h"

#include <daemon.h>
#include <sa/ike_sa.h>
#include <encoding/payloads/cert_payload.h>
#include <encoding/payloads/sa_payload.h>
#include <encoding/payloads/certreq_payload.h>
#include <credentials/certificates/x509.h>


typedef struct private_ike_cert_pre_t private_ike_cert_pre_t;

/**
 * Private members of a ike_cert_pre_t task.
 */
struct private_ike_cert_pre_t {

	/**
	 * Public methods and task_t interface.
	 */
	ike_cert_pre_t public;

	/**
	 * Assigned IKE_SA.
	 */
	ike_sa_t *ike_sa;

	/**
	 * Are we the initiator?
	 */
	bool initiator;

	/**
	 * Do we accept HTTP certificate lookup requests
	 */
	bool do_http_lookup;

	/**
	 * wheter this is the final authentication round
	 */
	bool final;

	/** states of ike cert pre */
	enum {
		CP_INIT,
		CP_SA,
		CP_SA_POST,
		CP_REQ_SENT,
		CP_NO_CERT,
	} state;

	/**
	 * type of certicate request to send
	 */
	payload_type_t cert_req_payload_type;
};

/**
 * add certificate to auth
 */
static bool add_certificate(auth_cfg_t *auth, chunk_t keyid, id_type_t id_type )
{
	identification_t *id = NULL;
	certificate_t *cert;
	bool status = TRUE;

	id = identification_create_from_encoding(id_type, keyid);

	if (!id)
	{
		return FALSE;
	}

	cert = lib->credmgr->get_cert(lib->credmgr,
									CERT_X509, KEY_ANY, id, TRUE);
	if (cert)
	{
		DBG1(DBG_IKE, "received cert request for \"%Y\"",
			 cert->get_subject(cert));
		auth->add(auth, AUTH_RULE_CA_CERT, cert);
	}
	else
	{
		DBG2(DBG_IKE, "received cert request for unknown ca "
						"with keyid %Y", id);
		status = FALSE;
	}

	id->destroy(id);

	return status;
}

/**
 * read certificate requests
 */
static void process_certreqs(private_ike_cert_pre_t *this, message_t *message)
{
	enumerator_t *enumerator;
	payload_t *payload;
	auth_cfg_t *auth;

	auth = this->ike_sa->get_auth_cfg(this->ike_sa, TRUE);

	enumerator = message->create_payload_enumerator(message);
	while (enumerator->enumerate(enumerator, &payload))
	{
		switch (payload->get_type(payload))
		{
			case CERTIFICATE_REQUEST:
			case CERTIFICATE_REQUEST_V1:
			{
				certreq_payload_t *certreq = (certreq_payload_t*)payload;
				enumerator_t *enumerator;
				u_int unknown = 0;
				chunk_t keyid;

				this->ike_sa->set_condition(this->ike_sa, COND_CERTREQ_SEEN, TRUE);

				if (certreq->get_cert_type(certreq) != CERT_X509)
				{
					DBG1(DBG_IKE, "cert payload %N not supported - ignored",
						 certificate_type_names, certreq->get_cert_type(certreq));
					break;
				}

				if (payload->get_type(payload) == CERTIFICATE_REQUEST)
				{
					enumerator = certreq->create_keyid_enumerator(certreq);
					while (enumerator->enumerate(enumerator, &keyid))
					{
						if (!add_certificate(auth, keyid, ID_KEY_ID))
						{
							unknown++;
						}
					}
					enumerator->destroy(enumerator);
				}
				else
				{
					keyid = certreq->get_dn(certreq);

					/* In case client (iPhone) is sending empty cert requests */
					if (!keyid.ptr || !keyid.len ||
						!add_certificate(auth, keyid, ID_DER_ASN1_DN))
					{
						unknown++;
					}
				}

				if (unknown)
				{
					DBG1(DBG_IKE, "received %u cert requests for an unknown ca",
						 unknown);
				}
				break;
			}
			case NOTIFY:
			{
				notify_payload_t *notify = (notify_payload_t*)payload;

				/* we only handle one type of notify here */
				if (notify->get_notify_type(notify) == HTTP_CERT_LOOKUP_SUPPORTED)
				{
					this->ike_sa->enable_extension(this->ike_sa, EXT_HASH_AND_URL);
				}
				break;
			}
			default:
				/* ignore other payloads here, these are handled elsewhere */
				break;
		}
	}
	enumerator->destroy(enumerator);
}

/**
 * tries to extract a certificate from the cert payload or the credential
 * manager (based on the hash of a "Hash and URL" encoded cert).
 * Note: the returned certificate (if any) has to be destroyed
 */
static certificate_t *try_get_cert(cert_payload_t *cert_payload)
{
	certificate_t *cert = NULL;

	switch (cert_payload->get_cert_encoding(cert_payload))
	{
		case ENC_X509_SIGNATURE:
		{
			cert = cert_payload->get_cert(cert_payload);
			break;
		}
		case ENC_X509_HASH_AND_URL:
		{
			identification_t *id;
			chunk_t hash = cert_payload->get_hash(cert_payload);
			if (!hash.ptr)
			{
				/* invalid "Hash and URL" data (logged elsewhere) */
				break;
			}
			id = identification_create_from_encoding(ID_KEY_ID, hash);
			cert = lib->credmgr->get_cert(lib->credmgr,
										  CERT_X509, KEY_ANY, id, FALSE);
			id->destroy(id);
			break;
		}
		default:
		{
			break;
		}
	}
	return cert;
}

/**
 * import certificates
 */
static void process_certs(private_ike_cert_pre_t *this, message_t *message)
{
	enumerator_t *enumerator;
	payload_t *payload;
	auth_cfg_t *auth;
	bool first = TRUE;

	auth = this->ike_sa->get_auth_cfg(this->ike_sa, FALSE);

	enumerator = message->create_payload_enumerator(message);
	while (enumerator->enumerate(enumerator, &payload))
	{
		if (payload->get_type(payload) == CERTIFICATE ||
				payload->get_type(payload) == CERTIFICATE_V1)
		{
			cert_payload_t *cert_payload;
			cert_encoding_t encoding;
			certificate_t *cert;
			char *url;

			cert_payload = (cert_payload_t*)payload;
			encoding = cert_payload->get_cert_encoding(cert_payload);

			switch (encoding)
			{
				case ENC_X509_HASH_AND_URL:
				{
					if (!this->do_http_lookup)
					{
						DBG1(DBG_IKE, "received hash-and-url encoded cert, but"
								" we don't accept them, ignore");
						break;
					}
					/* FALL */
				}
				case ENC_X509_SIGNATURE:
				{
					cert = try_get_cert(cert_payload);
					if (cert)
					{
						if (first)
						{	/* the first is an end entity certificate */
							DBG1(DBG_IKE, "received end entity cert \"%Y\"",
								 cert->get_subject(cert));
							auth->add(auth, AUTH_HELPER_SUBJECT_CERT, cert);
							first = FALSE;
						}
						else
						{
							DBG1(DBG_IKE, "received issuer cert \"%Y\"",
								 cert->get_subject(cert));
							auth->add(auth, AUTH_HELPER_IM_CERT, cert);
						}
					}
					else if (encoding == ENC_X509_HASH_AND_URL)
					{
						/* we fetch the certificate not yet, but only if
						 * it is really needed during authentication */
						url = cert_payload->get_url(cert_payload);
						if (!url)
						{
							DBG1(DBG_IKE, "received invalid hash-and-url "
								 "encoded cert, ignore");
							break;
						}
						url = strdup(url);
						if (first)
						{	/* first URL is for an end entity certificate */
							DBG1(DBG_IKE, "received hash-and-url for end"
								 " entity cert \"%s\"", url);
							auth->add(auth, AUTH_HELPER_SUBJECT_HASH_URL, url);
							first = FALSE;
						}
						else
						{
							DBG1(DBG_IKE, "received hash-and-url for issuer"
									" cert \"%s\"", url);
							auth->add(auth, AUTH_HELPER_IM_HASH_URL, url);
						}
					}
					break;
				}
				case ENC_CRL:
					cert = cert_payload->get_cert(cert_payload);
					if (cert)
					{
						DBG1(DBG_IKE, "received CRL \"%Y\"",
							 cert->get_subject(cert));
						auth->add(auth, AUTH_HELPER_REVOCATION_CERT, cert);
					}
					break;
				case ENC_PKCS7_WRAPPED_X509:
				case ENC_PGP:
				case ENC_DNS_SIGNED_KEY:
				case ENC_KERBEROS_TOKEN:
				case ENC_ARL:
				case ENC_SPKI:
				case ENC_X509_ATTRIBUTE:
				case ENC_RAW_RSA_KEY:
				case ENC_X509_HASH_AND_URL_BUNDLE:
				case ENC_OCSP_CONTENT:
				default:
					DBG1(DBG_ENC, "certificate encoding %N not supported",
						 cert_encoding_names, encoding);
			}
		}
	}
	enumerator->destroy(enumerator);
}

/**
 * add the keyid of a certificate to the certificate request payload
 */
static void add_certreq(private_ike_cert_pre_t *this,
												certreq_payload_t **req, certificate_t *cert)
{
	switch (cert->get_type(cert))
	{
		case CERT_X509:
		{
			public_key_t *public;
			chunk_t keyid;
			x509_t *x509 = (x509_t*)cert;

			if (!(x509->get_flags(x509) & X509_CA))
			{	/* no CA cert, skip */
				break;
			}
			public = cert->get_public_key(cert);
			if (!public)
			{
				break;
			}

			if (*req == NULL)
			{
				*req = certreq_payload_create_type(this->cert_req_payload_type, CERT_X509);
			}

			if (this->cert_req_payload_type == CERTIFICATE_REQUEST)
			{
				if (public->get_fingerprint(public, KEYID_PUBKEY_INFO_SHA1, &keyid))
				{
					(*req)->add_keyid(*req, keyid);
					DBG1(DBG_IKE, "sending cert request for \"%Y\"",
						 cert->get_subject(cert));
				}
			}
			else
			{
				identification_t *id;
				id = cert->get_subject(cert);

				(*req)->set_dn(*req, id->get_encoding(id));
				DBG1(DBG_IKE, "sending cert request for \"%Y\"",
						 cert->get_subject(cert));

			}
			public->destroy(public);
			break;
		}
		default:
			break;
	}
}

/**
 * add a auth_cfg's CA certificates to the certificate request
 */
static void add_certreqs(private_ike_cert_pre_t *this,
												 certreq_payload_t **req, auth_cfg_t *auth)
{
	enumerator_t *enumerator;
	auth_rule_t type;
	void *value;

	enumerator = auth->create_enumerator(auth);
	while (enumerator->enumerate(enumerator, &type, &value))
	{
		switch (type)
		{
			case AUTH_RULE_CA_CERT:
				add_certreq(this, req, (certificate_t*)value);
				break;
			default:
				break;
		}
	}
	enumerator->destroy(enumerator);
}

/**
 * add a auth_cfg's CA certificates to the certificate request
 */
static void add_certreqs_v1(private_ike_cert_pre_t *this,
														certreq_payload_t **req,
														auth_cfg_t *auth, message_t *message)
{
	enumerator_t *enumerator;
	auth_rule_t type;
	void *value;

	enumerator = auth->create_enumerator(auth);
	while (enumerator->enumerate(enumerator, &type, &value))
	{
		switch (type)
		{
			case AUTH_RULE_CA_CERT:
				add_certreq(this, req, (certificate_t*)value);
				if (req)
				{
					message->add_payload(message,(payload_t*)req);
				}
				break;
			default:
				break;
		}
	}
	enumerator->destroy(enumerator);
}

/**
 * build certificate requests
 */
static void build_certreqs(private_ike_cert_pre_t *this, message_t *message)
{
	enumerator_t *enumerator;
	ike_cfg_t *ike_cfg;
	peer_cfg_t *peer_cfg;
	certificate_t *cert;
	auth_cfg_t *auth;
	certreq_payload_t *req = NULL;

	ike_cfg = this->ike_sa->get_ike_cfg(this->ike_sa);
	if (!ike_cfg->send_certreq(ike_cfg))
	{
		return;
	}

	/* check if we require a specific CA for that peer */
	peer_cfg = this->ike_sa->get_peer_cfg(this->ike_sa);
	if (peer_cfg)
	{
		enumerator = peer_cfg->create_auth_cfg_enumerator(peer_cfg, FALSE);
		while (enumerator->enumerate(enumerator, &auth))
		{
			add_certreqs(this, &req, auth);
		}
		enumerator->destroy(enumerator);
	}

	if (!req)
	{
		/* otherwise add all trusted CA certificates */
		enumerator = lib->credmgr->create_cert_enumerator(lib->credmgr,
												CERT_ANY, KEY_ANY, NULL, TRUE);
		while (enumerator->enumerate(enumerator, &cert))
		{
			add_certreq(this, &req, cert);
		}
		enumerator->destroy(enumerator);
	}

	if (req)
	{
		message->add_payload(message, (payload_t*)req);

		if (lib->settings->get_bool(lib->settings, "charon.hash_and_url", FALSE))
		{
			message->add_notify(message, FALSE, HTTP_CERT_LOOKUP_SUPPORTED,
								chunk_empty);
			this->do_http_lookup = TRUE;
		}
	}
}

/**
 * build certificate requests
 */
static void build_certreqs_v1(private_ike_cert_pre_t *this, message_t *message)
{
	enumerator_t *enumerator;
	ike_cfg_t *ike_cfg;
	peer_cfg_t *peer_cfg;
	certificate_t *cert;
	auth_cfg_t *auth;
	certreq_payload_t *req = NULL;

	ike_cfg = this->ike_sa->get_ike_cfg(this->ike_sa);
	if (!ike_cfg->send_certreq(ike_cfg))
	{
		return;
	}

	/* check if we require a specific CA for that peer */
	/* Get the first authentcation config from peer config */
	peer_cfg = this->ike_sa->get_peer_cfg(this->ike_sa);
	if (peer_cfg)
	{
		enumerator = peer_cfg->create_auth_cfg_enumerator(peer_cfg, FALSE);
		if (enumerator->enumerate(enumerator, &auth))
		{
			add_certreqs_v1(this, &req, auth, message);
			if (req)
			{
				message->add_payload(message, (payload_t*)req);
			}
		}
		enumerator->destroy(enumerator);
	}

	if (!req)
	{
		/* otherwise add all trusted CA certificates */
		enumerator = lib->credmgr->create_cert_enumerator(lib->credmgr,
												CERT_ANY, KEY_ANY, NULL, TRUE);
		while (enumerator->enumerate(enumerator, &cert))
		{
			add_certreq(this, &req, cert);
			if (req)
			{
				message->add_payload(message, (payload_t*)req);
			}
		}
		enumerator->destroy(enumerator);
	}
}

/**
 * Check if this is the final authentication round
 */
static bool final_auth(message_t *message)
{
	/* we check for an AUTH payload without a ANOTHER_AUTH_FOLLOWS notify */
	if (message->get_payload(message, AUTHENTICATION) == NULL)
	{
		return FALSE;
	}
	if (message->get_notify(message, ANOTHER_AUTH_FOLLOWS))
	{
		return FALSE;
	}
	return TRUE;
}

/**
 * Checks for the auth_method to see if this task should handle certificates.
 * (IKEv1 only)
 */
static status_t check_auth_method(private_ike_cert_pre_t *this,
																	message_t *message)
{
	enumerator_t *enumerator;
	payload_t *payload;
	status_t status = SUCCESS;

	enumerator = message->create_payload_enumerator(message);
	while (enumerator->enumerate(enumerator, &payload))
	{
		if (payload->get_type(payload) == SECURITY_ASSOCIATION_V1)
		{
			sa_payload_t *sa_payload = (sa_payload_t*)payload;

			switch (sa_payload->get_auth_method(sa_payload))
			{
				case 	AUTH_RSA:
				case 	AUTH_XAUTH_INIT_RSA:
				case  AUTH_XAUTH_RESP_RSA:
					DBG3(DBG_IKE, "handling certs method (%d)",
								sa_payload->get_auth_method(sa_payload));
					status = NEED_MORE;
					break;
				default:
					DBG3(DBG_IKE, "not handling certs method (%d)",
								sa_payload->get_auth_method(sa_payload));
					status = SUCCESS;
					break;
			}

			this->state = CP_SA;
			break;
		}
	}
	enumerator->destroy(enumerator);

	if (status != NEED_MORE)
	{
		this->state = CP_NO_CERT;
		this->final = TRUE;
	}

	return status;
}

METHOD(task_t, build_i, status_t,
	private_ike_cert_pre_t *this, message_t *message)
{
	if (message->get_message_id(message) == 1)
	{	/* initiator sends CERTREQs in first IKE_AUTH */
		build_certreqs(this, message);
	}
	return NEED_MORE;
}

METHOD(task_t, process_r, status_t,
	private_ike_cert_pre_t *this, message_t *message)
{
	if (message->get_exchange_type(message) != IKE_SA_INIT)
	{	/* handle certreqs/certs in any IKE_AUTH, just in case */
		process_certreqs(this, message);
		process_certs(this, message);
	}
	this->final = final_auth(message);
	return NEED_MORE;
}

METHOD(task_t, build_r, status_t,
	private_ike_cert_pre_t *this, message_t *message)
{
	if (message->get_exchange_type(message) == IKE_SA_INIT)
	{
		build_certreqs(this, message);
	}
	if (this->final)
	{
		return SUCCESS;
	}
	return NEED_MORE;
}

METHOD(task_t, process_i, status_t,
	private_ike_cert_pre_t *this, message_t *message)
{
	if (message->get_exchange_type(message) == IKE_SA_INIT)
	{
		process_certreqs(this, message);
	}
	process_certs(this, message);

	if (final_auth(message))
	{
		return SUCCESS;
	}
	return NEED_MORE;
}

METHOD(task_t, process_r_v1, status_t,
	private_ike_cert_pre_t *this, message_t *message)
{
	switch (message->get_exchange_type(message))
	{
		case ID_PROT:
		{
			switch (this->state)
			{
				case CP_INIT:
					check_auth_method(this, message);
					break;
				case CP_SA:
					process_certreqs(this, message);
					this->state = CP_SA_POST;
					break;
				case CP_SA_POST:
					process_certreqs(this, message);
					process_certs(this, message);
					this->state = CP_REQ_SENT;
					this->final = TRUE;
					break;
				default:
					break;
			}
			break;
		}
		case AGGRESSIVE:
		{
			if (check_auth_method(this, message) == NEED_MORE)
			{
				process_certreqs(this, message);
				process_certs(this, message);
			}
			this->final = TRUE;
			break;
		}
		default:
			break;
	}

	return NEED_MORE;
}

METHOD(task_t, process_i_v1, status_t,
	private_ike_cert_pre_t *this, message_t *message)
{
	/* TODO: */
	return FAILED;
}

METHOD(task_t, build_r_v1, status_t,
	private_ike_cert_pre_t *this, message_t *message)
{

	switch (message->get_exchange_type(message))
	{
		case ID_PROT:
		{
			if (this->state == CP_SA_POST)
			{
				build_certreqs_v1(this, message);
			}
			break;
		}
		case AGGRESSIVE:
		{
			if (this->state != CP_NO_CERT)
			{
				build_certreqs_v1(this, message);
			}
		}
		default:
			break;

	}

	if (this->final)
	{
		return SUCCESS;
	}
	return NEED_MORE;
}

METHOD(task_t, build_i_v1, status_t,
	private_ike_cert_pre_t *this, message_t *message)
{
	/* TODO: */
	return FAILED;
}

METHOD(task_t, get_type, task_type_t,
	private_ike_cert_pre_t *this)
{
	return TASK_IKE_CERT_PRE;
}

METHOD(task_t, migrate, void,
	private_ike_cert_pre_t *this, ike_sa_t *ike_sa)
{
	this->ike_sa = ike_sa;
}

METHOD(task_t, destroy, void,
	private_ike_cert_pre_t *this)
{
	free(this);
}

/*
 * Described in header.
 */
ike_cert_pre_t *ike_cert_pre_create(ike_sa_t *ike_sa, bool initiator)
{
	private_ike_cert_pre_t *this;

	INIT(this,
		.public = {
			.task = {
				.get_type = _get_type,
				.migrate = _migrate,
				.destroy = _destroy,
			},
		},
		.ike_sa = ike_sa,
		.initiator = initiator,
	);

	if (ike_sa->get_version(ike_sa) == IKEV2)
	{
		if (initiator)
		{
			this->public.task.build = _build_i;
			this->public.task.process = _process_i;
		}
		else
		{
			this->public.task.build = _build_r;
			this->public.task.process = _process_r;
		}
		this->cert_req_payload_type = CERTIFICATE_REQUEST;
	}
	else
	{
		this->state = CP_INIT;
		if (initiator)
		{
			this->public.task.build = _build_i_v1;
			this->public.task.process = _process_i_v1;
		}
		else
		{
			this->public.task.build = _build_r_v1;
			this->public.task.process = _process_r_v1;
		}
		this->cert_req_payload_type = CERTIFICATE_REQUEST_V1;

	}

	return &this->public;
}
