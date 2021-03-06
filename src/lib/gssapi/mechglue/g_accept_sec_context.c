/* #pragma ident	"@(#)g_accept_sec_context.c	1.19	04/02/23 SMI" */

/*
 * Copyright 1996 by Sun Microsystems, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without fee,
 * provided that the above copyright notice appears in all copies and
 * that both that copyright notice and this permission notice appear in
 * supporting documentation, and that the name of Sun Microsystems not be used
 * in advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission. Sun Microsystems makes no
 * representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * SUN MICROSYSTEMS DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL SUN MICROSYSTEMS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
 * USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 *  glue routine for gss_accept_sec_context
 */

#include "mglueP.h"
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <string.h>
#include <errno.h>
#include <time.h>

#ifndef LEAN_CLIENT
static OM_uint32
val_acc_sec_ctx_args(
    OM_uint32 *minor_status,
    gss_ctx_id_t *context_handle,
    gss_cred_id_t verifier_cred_handle,
    gss_buffer_t input_token_buffer,
    gss_channel_bindings_t input_chan_bindings,
    gss_name_t *src_name,
    gss_OID *mech_type,
    gss_buffer_t output_token,
    OM_uint32 *ret_flags,
    OM_uint32 *time_rec,
    gss_cred_id_t *d_cred)
{

    /* Initialize outputs. */

    if (minor_status != NULL)
	*minor_status = 0;

    if (src_name != NULL)
	*src_name = GSS_C_NO_NAME;

    if (mech_type != NULL)
	*mech_type = GSS_C_NO_OID;

    if (output_token != GSS_C_NO_BUFFER) {
	output_token->length = 0;
	output_token->value = NULL;
    }

    if (d_cred != NULL)
	*d_cred = GSS_C_NO_CREDENTIAL;

    /* Validate arguments. */

    if (minor_status == NULL)
	return (GSS_S_CALL_INACCESSIBLE_WRITE);

    if (context_handle == NULL)
	return (GSS_S_CALL_INACCESSIBLE_WRITE);

    if (input_token_buffer == GSS_C_NO_BUFFER)
	return (GSS_S_CALL_INACCESSIBLE_READ);

    if (output_token == GSS_C_NO_BUFFER)
	return (GSS_S_CALL_INACCESSIBLE_WRITE);

    return (GSS_S_COMPLETE);
}

OM_uint32 KRB5_CALLCONV
gss_accept_sec_context (minor_status,
                        context_handle,
                        verifier_cred_handle,
                        input_token_buffer,
                        input_chan_bindings,
                        src_name,
                        mech_type,
                        output_token,
                        ret_flags,
                        time_rec,
                        d_cred)

OM_uint32 *		minor_status;
gss_ctx_id_t *		context_handle;
gss_cred_id_t		verifier_cred_handle;
gss_buffer_t		input_token_buffer;
gss_channel_bindings_t	input_chan_bindings;
gss_name_t *		src_name;
gss_OID *		mech_type;
gss_buffer_t		output_token;
OM_uint32 *		ret_flags;
OM_uint32 *		time_rec;
gss_cred_id_t *		d_cred;

{
    OM_uint32		status, temp_status, temp_minor_status;
    OM_uint32		temp_ret_flags = 0;
    gss_union_ctx_id_t	union_ctx_id;
    gss_cred_id_t	input_cred_handle = GSS_C_NO_CREDENTIAL;
    gss_cred_id_t	tmp_d_cred = GSS_C_NO_CREDENTIAL;
    gss_name_t		internal_name = GSS_C_NO_NAME;
    gss_name_t		tmp_src_name = GSS_C_NO_NAME;
    gss_OID_desc	token_mech_type_desc;
    gss_OID		token_mech_type = &token_mech_type_desc;
    gss_OID		actual_mech = GSS_C_NO_OID;
    gss_mechanism	mech = NULL;

    status = val_acc_sec_ctx_args(minor_status,
				  context_handle,
				  verifier_cred_handle,
				  input_token_buffer,
				  input_chan_bindings,
				  src_name,
				  mech_type,
				  output_token,
				  ret_flags,
				  time_rec,
				  d_cred);
    if (status != GSS_S_COMPLETE)
	return (status);

    /*
     * if context_handle is GSS_C_NO_CONTEXT, allocate a union context
     * descriptor to hold the mech type information as well as the
     * underlying mechanism context handle. Otherwise, cast the
     * value of *context_handle to the union context variable.
     */

    if(*context_handle == GSS_C_NO_CONTEXT) {

	if (input_token_buffer == GSS_C_NO_BUFFER)
	    return (GSS_S_CALL_INACCESSIBLE_READ);

	/* Get the token mech type */
	status = gssint_get_mech_type(token_mech_type, input_token_buffer);
	if (status)
	    return status;

	status = GSS_S_FAILURE;
	union_ctx_id = (gss_union_ctx_id_t)
	    malloc(sizeof(gss_union_ctx_id_desc));
	if (!union_ctx_id)
	    return (GSS_S_FAILURE);

	union_ctx_id->loopback = union_ctx_id;
	union_ctx_id->internal_ctx_id = GSS_C_NO_CONTEXT;
	status = generic_gss_copy_oid(&temp_minor_status,
				      token_mech_type,
				      &union_ctx_id->mech_type);
	if (status != GSS_S_COMPLETE) {
	    free(union_ctx_id);
	    return (status);
	}

	/* set the new context handle to caller's data */
	*context_handle = (gss_ctx_id_t)union_ctx_id;
    } else {
	union_ctx_id = (gss_union_ctx_id_t)*context_handle;
	token_mech_type = union_ctx_id->mech_type;
    }

    /*
     * get the appropriate cred handle from the union cred struct.
     */
    if (verifier_cred_handle != GSS_C_NO_CREDENTIAL) {
	input_cred_handle =
	    gssint_get_mechanism_cred((gss_union_cred_t)verifier_cred_handle,
				      token_mech_type);
	if (input_cred_handle == GSS_C_NO_CREDENTIAL) {
	    /* verifier credential specified but no acceptor credential found */
	    status = GSS_S_NO_CRED;
	    goto error_out;
	}
    }

    /*
     * now select the approprate underlying mechanism routine and
     * call it.
     */

    mech = gssint_get_mechanism (token_mech_type);
    if (mech && mech->gss_accept_sec_context) {

	    status = mech->gss_accept_sec_context(minor_status,
						  &union_ctx_id->internal_ctx_id,
						  input_cred_handle,
						  input_token_buffer,
						  input_chan_bindings,
						  src_name ? &internal_name : NULL,
						  &actual_mech,
						  output_token,
						  &temp_ret_flags,
						  time_rec,
					d_cred ? &tmp_d_cred : NULL);

	    /* If there's more work to do, keep going... */
	    if (status == GSS_S_CONTINUE_NEEDED)
		return GSS_S_CONTINUE_NEEDED;

	    /* if the call failed, return with failure */
	    if (status != GSS_S_COMPLETE) {
		map_error(minor_status, mech);
		goto error_out;
	    }

	    /*
	     * if src_name is non-NULL,
	     * convert internal_name into a union name equivalent
	     * First call the mechanism specific display_name()
	     * then call gss_import_name() to create
	     * the union name struct cast to src_name
	     */
	    if (src_name != NULL) {
		if (internal_name != GSS_C_NO_NAME) {
		    /* consumes internal_name regardless of success */
		    temp_status = gssint_convert_name_to_union_name(
			    &temp_minor_status, mech,
			    internal_name, &tmp_src_name);
		    if (temp_status != GSS_S_COMPLETE) {
			*minor_status = temp_minor_status;
			map_error(minor_status, mech);
			if (output_token->length)
			    (void) gss_release_buffer(&temp_minor_status,
						      output_token);
			return (temp_status);
		    }
		    *src_name = tmp_src_name;
		} else
		    *src_name = GSS_C_NO_NAME;
	    }

#define g_OID_prefix_equal(o1, o2) \
        (((o1)->length >= (o2)->length) && \
        (memcmp((o1)->elements, (o2)->elements, (o2)->length) == 0))

	    /* Ensure we're returning correct creds format */
	    if ((temp_ret_flags & GSS_C_DELEG_FLAG) &&
		tmp_d_cred != GSS_C_NO_CREDENTIAL) {
		if (actual_mech != GSS_C_NO_OID &&
		    !g_OID_prefix_equal(actual_mech, token_mech_type)) {
		    *d_cred = tmp_d_cred; /* unwrapped pseudo-mech */
		} else {
		    gss_union_cred_t d_u_cred = NULL;

		    d_u_cred = malloc(sizeof (gss_union_cred_desc));
		    if (d_u_cred == NULL) {
			status = GSS_S_FAILURE;
			goto error_out;
		    }
		    (void) memset(d_u_cred, 0, sizeof (gss_union_cred_desc));

		    d_u_cred->count = 1;

		    status = generic_gss_copy_oid(&temp_minor_status,
						  token_mech_type,
						  &d_u_cred->mechs_array);

		    if (status != GSS_S_COMPLETE) {
			free(d_u_cred);
			goto error_out;
		    }

		    d_u_cred->cred_array = malloc(sizeof(gss_cred_id_t));
		    if (d_u_cred->cred_array != NULL) {
			d_u_cred->cred_array[0] = tmp_d_cred;
		    } else {
			free(d_u_cred);
			status = GSS_S_FAILURE;
			goto error_out;
		    }

		    d_u_cred->loopback = d_u_cred;
		    *d_cred = (gss_cred_id_t)d_u_cred;
		}
	    }

	    if (mech_type != NULL)
		*mech_type = actual_mech;
	    else
		(void) gss_release_oid(&temp_minor_status, &actual_mech);
	    if (ret_flags != NULL)
		*ret_flags = temp_ret_flags;
	    return	(status);
    } else {

	status = GSS_S_BAD_MECH;
    }

error_out:
    if (union_ctx_id) {
	if (union_ctx_id->mech_type) {
	    if (union_ctx_id->mech_type->elements)
		free(union_ctx_id->mech_type->elements);
	    free(union_ctx_id->mech_type);
	}
	if (union_ctx_id->internal_ctx_id && mech &&
	    mech->gss_delete_sec_context) {
	    mech->gss_delete_sec_context(&temp_minor_status,
					 &union_ctx_id->internal_ctx_id,
					 GSS_C_NO_BUFFER);
	}
	free(union_ctx_id);
	*context_handle = GSS_C_NO_CONTEXT;
    }

    if (src_name)
	*src_name = GSS_C_NO_NAME;

    if (tmp_src_name != GSS_C_NO_NAME)
	(void) gss_release_buffer(&temp_minor_status,
				  (gss_buffer_t)tmp_src_name);

    return (status);
}
#endif /* LEAN_CLIENT */
