/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 *
 * Portions of this software are based upon public domain software
 * originally written at the National Center for Supercomputing Applications,
 * University of Illinois, Urbana-Champaign.
 */

#include "ap_config.h"

#ifdef APACHE_XLATE

#include "httpd.h"
#include "http_log.h"
#include "http_core.h"
#include "util_ebcdic.h"

/* Note: these variables really belong in util_charset.c, but it seems silly
 * to create util_charset.c at the moment since it would only contain the
 * variables.
 */

/* ap_hdrs_to_ascii, ap_hdrs_from_ascii
 *
 * These are the translation handles used to translate between the network
 * format of protocol headers and the local machine format.
 *
 * For an EBCDIC machine, these are valid handles which are set up at
 * initialization to translate between ISO-8859-1 and the code page of
 * the source code.
 *
 * For an ASCII machine, these remain NULL so that when they are stored
 * in the BUFF via ap_bsetop(BO_WXLATE or BO_RXLATE) it ensures that no
 * translation is performed.
 */
 
ap_xlate_t *ap_hdrs_to_ascii, *ap_hdrs_from_ascii;

/* ap_locale_to_ascii, ap_locale_from_ascii
 *
 * These handles are used for the translation of content, unless a
 * configuration module overrides them.
 *
 * For an EBCDIC machine, these are valid handles which are set up at
 * initialization to translate between ISO-8859-1 and the code page of
 * the httpd process's locale.
 *
 * For an ASCII machine, these remain NULL so that no translation is
 * performed (unless a configuration module does something, of course).
 */

ap_xlate_t *ap_locale_to_ascii, *ap_locale_from_ascii;

#endif /*APACHE_XLATE*/

#ifdef CHARSET_EBCDIC

ap_status_t ap_init_ebcdic(ap_pool_t *pool)
{
    ap_status_t rv;
    char buf[80];

    rv = ap_xlate_open(&ap_hdrs_to_ascii, "ISO8859-1", APR_DEFAULT_CHARSET, pool);
    if (rv) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, NULL,
                     "ap_xlate_open() failed");
        return rv;
    }

    rv = ap_xlate_open(&ap_hdrs_from_ascii, APR_DEFAULT_CHARSET, "ISO8859-1", pool);
    if (rv) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, NULL,
                     "ap_xlate_open() failed");
        return rv;
    }

    ap_locale_to_ascii = ap_hdrs_to_ascii; /* TODO: "ISO8859-1" to APR_LOCALE_CHARSET */
    ap_locale_from_ascii = ap_hdrs_from_ascii; /* TODO: APR_LOCALE_CHARSET to "ISO8859-1" */

    rv = ap_MD5InitEBCDIC(ap_hdrs_to_ascii);
    if (rv) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, NULL,
                     "ap_MD5InitEBCDIC() failed");
        return rv;
    }
    
    rv = ap_base64init_ebcdic(ap_hdrs_to_ascii, ap_hdrs_from_ascii);
    if (rv) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, NULL,
                     "ap_base64init_ebcdic() failed");
        return rv;
    }
    
    rv = ap_SHA1InitEBCDIC(ap_hdrs_to_ascii);
    if (rv) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, NULL,
                     "ap_SHA1InitEBCDIC() failed");
        return rv;
    }
    
    return APR_SUCCESS;
}

#define ASCIITEXT_MAGIC_TYPE_PREFIX "text/x-ascii-" /* Text files whose content-type starts with this are passed thru unconverted */

/* Check the Content-Type to decide if conversion is needed */
ap_xlate_t *ap_checkconv(struct request_rec *r)
{
    int convert_to_ascii;
    const char *type;
    ap_xlate_t *zero = NULL;

    /* To make serving of "raw ASCII text" files easy (they serve faster 
     * since they don't have to be converted from EBCDIC), a new
     * "magic" type prefix was invented: text/x-ascii-{plain,html,...}
     * If we detect one of these content types here, we simply correct
     * the type to the real text/{plain,html,...} type. Otherwise, we
     * set a flag that translation is required later on.
     */

    type = (r->content_type == NULL) ? ap_default_type(r) : r->content_type;

    /* If no content type is set then treat it as (ebcdic) text/plain */
    convert_to_ascii = (type == NULL);

    /* Conversion is applied to text/ files only, if ever. */
    if (type && (strncasecmp(type, "text/", 5) == 0 ||
		 strncasecmp(type, "message/", 8) == 0)) {
	if (strncasecmp(type, ASCIITEXT_MAGIC_TYPE_PREFIX,
			sizeof(ASCIITEXT_MAGIC_TYPE_PREFIX)-1) == 0)
	    r->content_type = ap_pstrcat(r->pool, "text/",
					 type+sizeof(ASCIITEXT_MAGIC_TYPE_PREFIX)-1,
					 NULL);
        else
	    /* translate EBCDIC to ASCII */
	    convert_to_ascii = 1;
    }
    /* Enable conversion if it's a text document */
    if (convert_to_ascii) {
        ap_bsetopt(r->connection->client, BO_WXLATE, &ap_locale_to_ascii);
    }
    else {
        ap_bsetopt(r->connection->client, BO_WXLATE, &zero);
    }

    return convert_to_ascii ? ap_locale_to_ascii : NULL;
}

#endif /* CHARSET_EBCDIC */
