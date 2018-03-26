/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <ruby/ruby.h>
#include <ruby/thread.h>

#include <string.h>

#include "rb_channel_credentials.h"
#include "rb_grpc_imports.generated.h"

#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>

#include "rb_call_credentials.h"
#include "rb_grpc.h"

/* grpc_rb_cChannelCredentials is the ruby class that proxies
   grpc_channel_credentials. */
static VALUE grpc_rb_cChannelCredentials = Qnil;

static char *pem_root_certs = NULL;

/* grpc_rb_channel_credentials wraps a grpc_channel_credentials.  It provides a
 * mark object that is used to hold references to any objects used to create
 * the credentials. */
typedef struct grpc_rb_channel_credentials {
  /* Holder of ruby objects involved in constructing the credentials */
  VALUE mark;

  /* The actual credentials */
  grpc_channel_credentials *wrapped;
} grpc_rb_channel_credentials;

/* Destroys the credentials instances. */
static void grpc_rb_channel_credentials_free(void *p) {
  grpc_rb_channel_credentials *wrapper = NULL;
  if (p == NULL) {
    return;
  };
  wrapper = (grpc_rb_channel_credentials *)p;
  grpc_channel_credentials_release(wrapper->wrapped);
  wrapper->wrapped = NULL;

  xfree(p);
}

/* Protects the mark object from GC */
static void grpc_rb_channel_credentials_mark(void *p) {
  grpc_rb_channel_credentials *wrapper = NULL;
  if (p == NULL) {
    return;
  }
  wrapper = (grpc_rb_channel_credentials *)p;

  if (wrapper->mark != Qnil) {
    rb_gc_mark(wrapper->mark);
  }
}

static rb_data_type_t grpc_rb_channel_credentials_data_type = {
    "grpc_channel_credentials",
    {grpc_rb_channel_credentials_mark,
     grpc_rb_channel_credentials_free,
     GRPC_RB_MEMSIZE_UNAVAILABLE,
     {NULL, NULL}},
    NULL,
    NULL,
#ifdef RUBY_TYPED_FREE_IMMEDIATELY
    RUBY_TYPED_FREE_IMMEDIATELY
#endif
};

/* Allocates ChannelCredential instances.
   Provides safe initial defaults for the instance fields. */
static VALUE grpc_rb_channel_credentials_alloc(VALUE cls) {
  grpc_rb_channel_credentials *wrapper = ALLOC(grpc_rb_channel_credentials);
  wrapper->wrapped = NULL;
  wrapper->mark = Qnil;
  return TypedData_Wrap_Struct(cls, &grpc_rb_channel_credentials_data_type,
                               wrapper);
}

/* Creates a wrapping object for a given channel credentials. This should only
 * be called with grpc_channel_credentials objects that are not already
 * associated with any Ruby object. */
VALUE grpc_rb_wrap_channel_credentials(grpc_channel_credentials *c,
                                       VALUE mark) {
  VALUE rb_wrapper;
  grpc_rb_channel_credentials *wrapper;
  if (c == NULL) {
    return Qnil;
  }
  rb_wrapper = grpc_rb_channel_credentials_alloc(grpc_rb_cChannelCredentials);
  TypedData_Get_Struct(rb_wrapper, grpc_rb_channel_credentials,
                       &grpc_rb_channel_credentials_data_type, wrapper);
  wrapper->wrapped = c;
  wrapper->mark = mark;
  return rb_wrapper;
}

/* The attribute used on the mark object to hold the pem_root_certs. */
static ID id_pem_root_certs;

/* The attribute used on the mark object to hold the pem_private_key. */
static ID id_pem_private_key;

/* The attribute used on the mark object to hold the pem_private_key. */
static ID id_pem_cert_chain;

/* The attribute used on the mark object to hold the checkServerIdentity callback. */
static ID id_check_server_identity_cb;


struct verify_callback_params {
    VALUE cb;
    const char *servername;
    const char *cert;
};

static VALUE verify_peer_callback_try_wrapper(VALUE arg) {
    VALUE cb;
    VALUE servername;
    VALUE cert;

    cb = rb_ary_entry(arg, 0);
    servername = rb_ary_entry(arg, 1);
    cert = rb_ary_entry(arg, 2);

    if (rb_class_of(cb) == rb_cProc) {
        rb_funcall(cb, rb_intern("call"), 2, servername, cert);
    } else if (rb_class_of(cb) == rb_cSymbol) {
        rb_funcall(rb_class_of(cb), rb_to_id(cb), 2, servername, cert);
    } else {
        printf("Callback argument in verify_peer_callback_try_wrapper is an invalid type!\n");
        return INT2NUM(1);
    }
    return INT2NUM(0);
}

static VALUE verify_peer_callback_catch_wrapper(VALUE arg, VALUE exception_object) {
    // Catch just always returns a failure signal.
    return INT2NUM(1);
}

/* Before we jump back from native code (which doesn't have the GVL), it's important
   to re-acquire it otherwise badness can happen. So this method should be invoked
   with the GVL (i.e. by using the rb_thread_call_with_gvl() method). */
static void* invoke_rb_verify_callback_with_gvl(void *arg) {
    VALUE result;
    VALUE passthrough;
    struct verify_callback_params* params = (struct verify_callback_params*)arg;

    passthrough = rb_ary_new();
    rb_ary_store(passthrough, 0, params->cb);
    rb_ary_store(passthrough, 1, params->servername != NULL ? rb_str_new2(params->servername) : Qnil);
    rb_ary_store(passthrough, 2, params->cert != NULL ? rb_str_new2(params->cert) : Qnil);

    result = rb_rescue(verify_peer_callback_try_wrapper, passthrough, verify_peer_callback_catch_wrapper, Qnil);
    return NUM2INT(result) == 0 ? NULL : arg;
}

static int verify_peer_callback_wrapper(const char* servername, const char* cert, void* userdata) {
    struct verify_callback_params params;
    if (userdata == NULL) {
        printf("Error! Callback function wasn't set!\n");
        return 1;
    }

    params.cb = (VALUE)userdata;
    params.servername = servername;
    params.cert = cert;

    return rb_thread_call_with_gvl(invoke_rb_verify_callback_with_gvl, &params) == NULL ? 0 : 1;
}

/*
  call-seq:
    creds1 = Credentials.new()
    ...
    creds2 = Credentials.new(pem_root_certs)
    ...
    creds3 = Credentials.new(pem_root_certs, pem_private_key,
                             pem_cert_chain)
    ...
    creds4 = Credentials.new(pem_root_certs, pem_private_key,
                             pem_cert_chain, verify_options)
    pem_root_certs: (optional) PEM encoding of the server root certificate
    pem_private_key: (optional) PEM encoding of the client's private key
    pem_cert_chain: (optional) PEM encoding of the client's cert chain
    verify_options: (optional) A Hash with key-value pairs defining additional peer verification options
    Initializes Credential instances. */
static VALUE grpc_rb_channel_credentials_init(int argc, VALUE *argv,
                                              VALUE self) {
  VALUE pem_root_certs = Qnil;
  VALUE pem_private_key = Qnil;
  VALUE pem_cert_chain = Qnil;
  grpc_rb_channel_credentials *wrapper = NULL;
  grpc_channel_credentials *creds = NULL;
  grpc_ssl_pem_key_cert_pair key_cert_pair;
  const char *pem_root_certs_cstr = NULL;
  VALUE options_hash = Qnil;
  VALUE option_value = Qnil;
  verify_peer_options vp_options = {NULL, NULL, NULL};
  MEMZERO(&key_cert_pair, grpc_ssl_pem_key_cert_pair, 1);

  grpc_ruby_once_init();

  /* "04" == no mandatory arg, 4 optional */
  rb_scan_args(argc, argv, "04", &pem_root_certs, &pem_private_key,
               &pem_cert_chain, &options_hash);

  TypedData_Get_Struct(self, grpc_rb_channel_credentials,
                       &grpc_rb_channel_credentials_data_type, wrapper);
  if (pem_root_certs != Qnil) {
    pem_root_certs_cstr = RSTRING_PTR(pem_root_certs);
  }
  if (options_hash != Qnil) {
    option_value = rb_hash_aref(options_hash, rb_str_new2("checkServerIdentity"));
    if (option_value != Qnil) {
      if (rb_class_of(option_value) != rb_cProc && rb_class_of(option_value) != rb_cSymbol) {
          rb_raise(rb_eTypeError, "Expected Proc or Symbol callback");
          return Qnil;
      }
      vp_options.verify_peer_callback = verify_peer_callback_wrapper;
      vp_options.verify_peer_callback_userdata = (void*)option_value;
      // The userdata object is marked on the credentials object as a hidden private member, so it will be automatically
      // garbage collected by ruby (same as the PEM certs on the credentials object).
      vp_options.verify_peer_destruct = NULL;
      rb_ivar_set(self, id_check_server_identity_cb, option_value);
    }
  }
  if (pem_private_key == Qnil && pem_cert_chain == Qnil) {
    creds = grpc_ssl_credentials_create(pem_root_certs_cstr, NULL, &vp_options, NULL);
  } else {
    key_cert_pair.private_key = RSTRING_PTR(pem_private_key);
    key_cert_pair.cert_chain = RSTRING_PTR(pem_cert_chain);
    creds =
        grpc_ssl_credentials_create(pem_root_certs_cstr, &key_cert_pair, &vp_options, NULL);
  }
  if (creds == NULL) {
    rb_raise(rb_eRuntimeError, "could not create a credentials, not sure why");
    return Qnil;
  }
  wrapper->wrapped = creds;

  /* Add the input objects as hidden fields to preserve them. */
  rb_ivar_set(self, id_pem_cert_chain, pem_cert_chain);
  rb_ivar_set(self, id_pem_private_key, pem_private_key);
  rb_ivar_set(self, id_pem_root_certs, pem_root_certs);

  return self;
}

static VALUE grpc_rb_channel_credentials_compose(int argc, VALUE *argv,
                                                 VALUE self) {
  grpc_channel_credentials *creds;
  grpc_call_credentials *other;
  grpc_channel_credentials *prev = NULL;
  VALUE mark;
  if (argc == 0) {
    return self;
  }
  mark = rb_ary_new();
  rb_ary_push(mark, self);
  creds = grpc_rb_get_wrapped_channel_credentials(self);
  for (int i = 0; i < argc; i++) {
    rb_ary_push(mark, argv[i]);
    other = grpc_rb_get_wrapped_call_credentials(argv[i]);
    creds = grpc_composite_channel_credentials_create(creds, other, NULL);
    if (prev != NULL) {
      grpc_channel_credentials_release(prev);
    }
    prev = creds;

    if (creds == NULL) {
      rb_raise(rb_eRuntimeError,
               "Failed to compose channel and call credentials");
    }
  }
  return grpc_rb_wrap_channel_credentials(creds, mark);
}

static grpc_ssl_roots_override_result get_ssl_roots_override(
    char **pem_root_certs_ptr) {
  *pem_root_certs_ptr = pem_root_certs;
  if (pem_root_certs == NULL) {
    return GRPC_SSL_ROOTS_OVERRIDE_FAIL;
  } else {
    return GRPC_SSL_ROOTS_OVERRIDE_OK;
  }
}

static VALUE grpc_rb_set_default_roots_pem(VALUE self, VALUE roots) {
  char *roots_ptr = StringValueCStr(roots);
  size_t length = strlen(roots_ptr);
  (void)self;
  pem_root_certs = gpr_malloc((length + 1) * sizeof(char));
  memcpy(pem_root_certs, roots_ptr, length + 1);
  return Qnil;
}

void Init_grpc_channel_credentials() {
  grpc_rb_cChannelCredentials = rb_define_class_under(
      grpc_rb_mGrpcCore, "ChannelCredentials", rb_cObject);

  /* Allocates an object managed by the ruby runtime */
  rb_define_alloc_func(grpc_rb_cChannelCredentials,
                       grpc_rb_channel_credentials_alloc);

  /* Provides a ruby constructor and support for dup/clone. */
  rb_define_method(grpc_rb_cChannelCredentials, "initialize",
                   grpc_rb_channel_credentials_init, -1);
  rb_define_method(grpc_rb_cChannelCredentials, "initialize_copy",
                   grpc_rb_cannot_init_copy, 1);
  rb_define_method(grpc_rb_cChannelCredentials, "compose",
                   grpc_rb_channel_credentials_compose, -1);
  rb_define_module_function(grpc_rb_cChannelCredentials,
                            "set_default_roots_pem",
                            grpc_rb_set_default_roots_pem, 1);

  grpc_set_ssl_roots_override_callback(get_ssl_roots_override);

  id_pem_cert_chain = rb_intern("__pem_cert_chain");
  id_pem_private_key = rb_intern("__pem_private_key");
  id_pem_root_certs = rb_intern("__pem_root_certs");
  id_check_server_identity_cb = rb_intern("__check_server_identity_cb");
}

/* Gets the wrapped grpc_channel_credentials from the ruby wrapper */
grpc_channel_credentials *grpc_rb_get_wrapped_channel_credentials(VALUE v) {
  grpc_rb_channel_credentials *wrapper = NULL;
  TypedData_Get_Struct(v, grpc_rb_channel_credentials,
                       &grpc_rb_channel_credentials_data_type, wrapper);
  return wrapper->wrapped;
}
