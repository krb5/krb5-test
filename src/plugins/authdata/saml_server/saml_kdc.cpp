/*
 * plugins/authdata/saml_server/saml_kdc.cpp
 *
 * Copyright 2009 by the Massachusetts Institute of Technology.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 * 
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 * 
 *
 * SAML KDC authorization data plugin
 */

#include <string.h>
#include <errno.h>

#include "saml_kdc.h"

krb5_error_code
saml_init(krb5_context ctx, void **data)
{
    SAMLConfig &config = SAMLConfig::getConfig();

    XMLToolingConfig& xmlconf = XMLToolingConfig::getConfig();

    if (getenv("SAML_DEBUG"))
        xmlconf.log_config("DEBUG");
    else
        xmlconf.log_config();

    if (!config.init()) {
        return KRB5KDC_ERR_SVC_UNAVAILABLE;
    }

    *data = &config;

    return 0;
}

void
saml_fini(krb5_context ctx, void *data)
{
    SAMLConfig *config = (SAMLConfig *)data;

    config->term();
}

static krb5_error_code
saml_unparse_name_xmlch(krb5_context context,
                        krb5_const_principal name,
                        XMLCh **unicodePrincipal)
{
    krb5_error_code code;
    char *utf8Name;

    code = krb5_unparse_name(context, name, &utf8Name);
    if (code != 0)
        return code;

    *unicodePrincipal = fromUTF8(utf8Name, false);

    if (*unicodePrincipal == NULL)
        code = ENOMEM;

    free(utf8Name);

    return code;
}

static krb5_error_code
saml_kdc_build_issuer(krb5_context context,
                      krb5_const_principal principal,
                      Issuer **pIssuer)
{
    Issuer *issuer;
    XMLCh *unicodePrincipal = NULL;
    krb5_error_code code;

    code = saml_unparse_name_xmlch(context, principal, &unicodePrincipal);
    if (code != 0)
        goto cleanup;

    issuer = IssuerBuilder::buildIssuer();
    issuer->setFormat(NameIDType::KERBEROS);
    issuer->setName(unicodePrincipal);

    *pIssuer = issuer;

cleanup:
    delete unicodePrincipal;

    return code;
}

static krb5_error_code
saml_kdc_build_subject(krb5_context context,
                       krb5_const_principal principal,
                       Subject **pSubject)
{
    NameID *nameID;
    Subject *subject;
    XMLCh *unicodePrincipal = NULL;
    krb5_error_code code;

    code = saml_unparse_name_xmlch(context, principal, &unicodePrincipal);
    if (code != 0)
        goto cleanup;

    nameID = NameIDBuilder::buildNameID();
    nameID->setName(unicodePrincipal);
    nameID->setFormat(NameIDType::KERBEROS);

    subject = SubjectBuilder::buildSubject();
    subject->setNameID(nameID);

    *pSubject = subject;

cleanup:
    delete unicodePrincipal;

    return code;
}

static krb5_error_code
saml_kdc_build_assertion(krb5_context context,
                         unsigned int flags,
                         krb5_const_principal client_princ,
                         krb5_db_entry *client,
                         krb5_db_entry *server,
                         krb5_db_entry *tgs,
                         krb5_enc_tkt_part *enc_tkt_request,
                         krb5_enc_tkt_part *enc_tkt_reply,
                         saml2::Assertion **pAssertion)
{
    krb5_error_code code;
    Issuer *issuer;
    Subject *subject;
    AttributeStatement *attrStatement;
    AuthnStatement *statement;
    AuthnContext *authnContext;
    AuthnContextClassRef *authnContextClass;
    Conditions *conditions;
    saml2::Assertion *assertion;
    DateTime authtime((time_t)enc_tkt_request->times.authtime);
    DateTime starttime((time_t)enc_tkt_request->times.starttime);
    DateTime endtime((time_t)enc_tkt_request->times.endtime);
    auto_ptr_XMLCh method("urn:oasis:names:tc:SAML:2.0:ac:classes:Kerberos");

    saml_kdc_build_issuer(context, tgs->princ, &issuer);
    saml_kdc_build_subject(context, client_princ, &subject);
    saml_kdc_build_attrs_ldap(context, client, server, &attrStatement);

    try {
        authnContext = AuthnContextBuilder::buildAuthnContext();
        authnContextClass = AuthnContextClassRefBuilder::buildAuthnContextClassRef();
        authnContextClass->setReference(method.get());
        authnContext->setAuthnContextClassRef(authnContextClass);

        statement = AuthnStatementBuilder::buildAuthnStatement();
        statement->setAuthnInstant(authtime.getFormattedString());
        statement->setAuthnContext(authnContext);


        conditions = ConditionsBuilder::buildConditions();
        conditions->setNotBefore(starttime.getFormattedString());
        conditions->setNotOnOrAfter(endtime.getFormattedString());

        assertion = AssertionBuilder::buildAssertion();
        assertion->setIssueInstant(authtime.getFormattedString());
        assertion->setConditions(conditions);
        assertion->setIssuer(issuer);
        assertion->setSubject(subject);
        assertion->getAuthnStatements().push_back(statement);
        if (attrStatement != NULL)
            assertion->getAttributeStatements().push_back(attrStatement);
        code = 0;
        *pAssertion = assertion;
    } catch (XMLToolingException &e) {
        code = ASN1_PARSE_ERROR; /* XXX */
        delete assertion;
    }

    return code;
}

static krb5_error_code
saml_kdc_issue(krb5_context context,
               unsigned int flags,
               krb5_const_principal client_princ,
               krb5_db_entry *client,
               krb5_db_entry *server,
               krb5_db_entry *tgs,
               krb5_enc_tkt_part *enc_tkt_request,
               krb5_enc_tkt_part *enc_tkt_reply,
               krb5_data **assertion_data)
{
    krb5_error_code code;
    saml2::Assertion *assertion = NULL;
    Signature *signature = NULL;
    XSECCryptoKey *key;
    string buf;
    krb5_data data;
    auto_ptr_XMLCh algorithm(URI_ID_HMAC_SHA512);

    *assertion_data = NULL;

    code = saml_kdc_build_assertion(context, flags, client_princ,
                                    client, server, tgs,
                                    enc_tkt_request, enc_tkt_reply,
                                    &assertion);
    if (code != 0)
        return code;

    code = saml_krb_derive_key(context, enc_tkt_reply->session, &key);
    if (code != 0) {
        delete assertion;
        return code;
    }

    try {
        signature = SignatureBuilder::buildSignature();
        signature->setSignatureAlgorithm(algorithm.get());
        signature->setSigningKey(key);

        assertion->addNamespace(Namespace(XSD_NS, XSD_PREFIX));
        assertion->addNamespace(Namespace(XSI_NS, XSI_PREFIX));
        assertion->addNamespace(Namespace(XMLSIG_NS, XMLSIG_PREFIX));
        assertion->addNamespace(Namespace(SAML20_NS, SAML20_PREFIX));
        assertion->addNamespace(Namespace(SAML20X500_NS, SAML20X500_PREFIX));

        assertion->setSignature(signature);
        vector <Signature *> signatures(1, signature);
        XMLHelper::serialize(assertion->marshall((DOMDocument *)NULL, &signatures, NULL), buf);
    } catch (XMLToolingException &e) {
        code = ASN1_PARSE_ERROR; /* XXX */
    }
  
    if (code == 0) { 
        data.data = (char *)buf.c_str();
        data.length = buf.length();

        code = krb5_copy_data(context, &data, assertion_data); 
    }

    delete assertion;

    return code;
}

#if 0
static krb5_error_code
saml_kdc_verify(krb5_context context,
                krb5_enc_tkt_part *enc_tkt_request,
                krb5_data **assertion)
{
    return 0;
}
#endif

static krb5_error_code
saml_kdc_sign(krb5_context context,
              krb5_enc_tkt_part *enc_tkt_reply,
              krb5_const_principal tgs,
              krb5_data *assertion)
{
    krb5_error_code code;
    krb5_authdata ad_datum, *ad_data[2], **kdc_issued = NULL;
    krb5_authdata **if_relevant = NULL;
    krb5_authdata **tkt_authdata;

    ad_datum.ad_type = KRB5_AUTHDATA_SAML;
    ad_datum.contents = (krb5_octet *)assertion->data;
    ad_datum.length = assertion->length;

    ad_data[0] = &ad_datum;
    ad_data[1] = NULL;

    code = krb5_encode_authdata_container(context,
                                          KRB5_AUTHDATA_IF_RELEVANT,
                                          ad_data,
                                          &if_relevant);
    if (code != 0) {
        krb5_free_authdata(context, kdc_issued);
        return code;
    }

    code = krb5_merge_authdata(context,
                               if_relevant,
                               enc_tkt_reply->authorization_data,
                               &tkt_authdata);
    if (code == 0) {
        krb5_free_authdata(context, enc_tkt_reply->authorization_data);
        enc_tkt_reply->authorization_data = tkt_authdata;
    } else {
        krb5_free_authdata(context, if_relevant);
    }

    krb5_free_authdata(context, kdc_issued);

    return code;
}

krb5_error_code
saml_authdata(krb5_context context,
              unsigned int flags,
              krb5_db_entry *client,
              krb5_db_entry *server,
              krb5_db_entry *tgs,
              krb5_keyblock *client_key,
              krb5_keyblock *server_key,
              krb5_keyblock *tgs_key,
              krb5_data *req_pkt,
              krb5_kdc_req *request,
              krb5_const_principal for_user_princ,
              krb5_enc_tkt_part *enc_tkt_request,
              krb5_enc_tkt_part *enc_tkt_reply)
{
    krb5_error_code code;
    krb5_data *assertion = NULL;
    krb5_const_principal client_princ;

    if (request->msg_type != KRB5_TGS_REQ)
        return 0;

    if (flags & KRB5_KDB_FLAG_PROTOCOL_TRANSITION)
        client_princ = for_user_princ;
    else
        client_princ = enc_tkt_reply->client;

    if (assertion == NULL) {
        if (client == NULL)
            return 0;

        code = saml_kdc_issue(context, flags,
                              client_princ, client,
                              server, tgs,
                              enc_tkt_request, enc_tkt_reply,
                              &assertion);
        if (code != 0)
            return code;
    }

    code = saml_kdc_sign(context, enc_tkt_reply, tgs->princ, assertion);

    krb5_free_data(context, assertion);

    return code;
}

krb5plugin_authdata_server_ftable_v2 authdata_server_2 = {
    "saml",
    saml_init,
    saml_fini,
    saml_authdata,
};