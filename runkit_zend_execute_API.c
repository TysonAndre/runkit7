#include "php_runkit_zend_execute_API.h"

#include "main/php_version.h"
#include "Zend/zend_API.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_generators.h"
#include "Zend/zend_globals_macros.h"
#include "Zend/zend_types.h"

#ifndef GC_ADDREF
#define GC_ADDREF(x) GC_REFCOUNT((x))++
#endif

#if PHP_VERSION_ID >= 70300
#define RUNKIT_IS_FCI_CACHE_INITIALIZED(fci_cache) ((fci_cache)->function_handler != NULL)
#define RUNKIT_CLEAR_FCI_CACHE(fci_cache) do {(fci_cache)->function_handler = NULL; } while (0)
#else
#define RUNKIT_IS_FCI_CACHE_INITIALIZED(fci_cache) ((fci_cache)->initialized)
#define RUNKIT_CLEAR_FCI_CACHE(fci_cache) do {(fci_cache)->initialized = 0; } while (0)
#endif

/**
 * Copies of internal methods from Zend/zend_execute_API.c
 * These are used to call internal methods (not in the function table) from the external method.
 * TODO: See if xdebug works
 */
int runkit_forward_call_user_function(zend_function *fbc, zend_function *fbc_inner, INTERNAL_FUNCTION_PARAMETERS) /* {{{ */
{
	uint32_t i;
	zend_execute_data *call, dummy_execute_data;
	zend_fcall_info_cache fci_cache_local = {0};
	zend_function *func;
#if PHP_VERSION_ID >= 70400
	uint32_t call_info;
	void *object_or_called_scope;
#endif
	/* {{{ patch for runkit */
	zend_fcall_info fci = {0};
	zend_fcall_info_cache *fci_cache = NULL;

	fci.size = sizeof(fci);
	fci.object = NULL; // FIXME for methods? // object ? Z_OBJ_P(object) : NULL;
	ZVAL_STR(&fci.function_name, fbc_inner->common.function_name);
	zend_string_addref(fbc_inner->common.function_name);
	fci.retval = return_value;
	fci.param_count = ZEND_CALL_NUM_ARGS(EG(current_execute_data));
	fci.params = ZEND_CALL_ARG(EG(current_execute_data), 1);  // params and param_count From zend_API.c
	fci.no_separation = (zend_bool)1;			  // ???
	/* end patch for runkit }}} */

	ZVAL_UNDEF(fci.retval);

	if (!EG(active)) {
		return FAILURE; /* executor is already inactive */
	}

	if (EG(exception)) {
		return FAILURE; /* we would result in an unstable executor otherwise */
	}

	/* Initialize execute_data */
	if (!EG(current_execute_data)) {
		/* This only happens when we're called outside any execute()'s
		 * It shouldn't be strictly necessary to NULL execute_data out,
		 * but it may make bugs easier to spot
		 */
		memset(&dummy_execute_data, 0, sizeof(zend_execute_data));
		EG(current_execute_data) = &dummy_execute_data;
	} else if (EG(current_execute_data)->func &&
	           ZEND_USER_CODE(EG(current_execute_data)->func->common.type) &&
	           EG(current_execute_data)->opline->opcode != ZEND_DO_FCALL &&
	           EG(current_execute_data)->opline->opcode != ZEND_DO_ICALL &&
	           EG(current_execute_data)->opline->opcode != ZEND_DO_UCALL &&
	           EG(current_execute_data)->opline->opcode != ZEND_DO_FCALL_BY_NAME) {
		/* Insert fake frame in case of include or magic calls */
		dummy_execute_data = *EG(current_execute_data);
		dummy_execute_data.prev_execute_data = EG(current_execute_data);
		dummy_execute_data.call = NULL;
		dummy_execute_data.opline = NULL;
		dummy_execute_data.func = NULL;
		EG(current_execute_data) = &dummy_execute_data;
	}

	if (!fci_cache || !RUNKIT_IS_FCI_CACHE_INITIALIZED(fci_cache)) {
		zend_string *callable_name;
		char *error = NULL;

		if (!fci_cache) {
			fci_cache = &fci_cache_local;
		}

		if (!zend_is_callable_ex(&fci.function_name, fci.object, IS_CALLABLE_CHECK_SILENT, &callable_name, fci_cache, &error)) {
			if (error) {
				zend_error(E_WARNING, "Invalid callback %s, %s", ZSTR_VAL(callable_name), error);
				efree(error);
			}
			if (callable_name) {
				zend_string_release(callable_name);
			}
			if (EG(current_execute_data) == &dummy_execute_data) {
				EG(current_execute_data) = dummy_execute_data.prev_execute_data;
			}
			return FAILURE;
		} else if (error) {
			/* Capitalize the first latter of the error message */
			if (error[0] >= 'a' && error[0] <= 'z') {
				error[0] += ('A' - 'a');
			}
			zend_error(E_DEPRECATED, "%s", error);
			efree(error);
		}
		zend_string_release(callable_name);
	}

	func = fbc_inner;

#if PHP_VERSION_ID < 70400
    fci.object = (func->common.fn_flags & ZEND_ACC_STATIC) ?
	   NULL : fci_cache->object;
    call = zend_vm_stack_push_call_frame(ZEND_CALL_TOP_FUNCTION | ZEND_CALL_DYNAMIC,
	   func, fci.param_count, fci_cache->called_scope, fci.object);
#else
    if ((func->common.fn_flags & ZEND_ACC_STATIC) || !fci_cache->object) {
		fci.object = NULL;
		object_or_called_scope = fci_cache->called_scope;
		call_info = ZEND_CALL_TOP_FUNCTION | ZEND_CALL_DYNAMIC;
	} else {
		fci.object = fci_cache->object;
		object_or_called_scope = fci.object;
		call_info = ZEND_CALL_TOP_FUNCTION | ZEND_CALL_DYNAMIC | ZEND_CALL_HAS_THIS;
	}
    call = zend_vm_stack_push_call_frame(call_info,
	   func, fci.param_count, object_or_called_scope);
#endif
	if (fci.object &&
	    (!EG(objects_store).object_buckets ||
	     !IS_OBJ_VALID(EG(objects_store).object_buckets[fci.object->handle]))) {
		if (EG(current_execute_data) == &dummy_execute_data) {
			EG(current_execute_data) = dummy_execute_data.prev_execute_data;
		}
		return FAILURE;
	}

	if (func->common.fn_flags & (ZEND_ACC_ABSTRACT | ZEND_ACC_DEPRECATED)) {
		if (func->common.fn_flags & ZEND_ACC_ABSTRACT) {
			zend_throw_error(NULL, "Cannot call abstract method %s::%s()", ZSTR_VAL(func->common.scope->name), ZSTR_VAL(func->common.function_name));
			if (EG(current_execute_data) == &dummy_execute_data) {
				EG(current_execute_data) = dummy_execute_data.prev_execute_data;
			}
			return FAILURE;
		}
		if (func->common.fn_flags & ZEND_ACC_DEPRECATED) {
 			zend_error(E_DEPRECATED, "Function %s%s%s() is deprecated",
				func->common.scope ? ZSTR_VAL(func->common.scope->name) : "",
				func->common.scope ? "::" : "",
				ZSTR_VAL(func->common.function_name));
		}
	}

	for (i = 0; i < fci.param_count; i++) {
		zval *param;
		zval *arg = &fci.params[i];

		if (ARG_SHOULD_BE_SENT_BY_REF(func, i + 1)) {
			if (UNEXPECTED(!Z_ISREF_P(arg))) {
				if (!fci.no_separation) {
					/* Separation is enabled -- create a ref */
					ZVAL_NEW_REF(arg, arg);
				} else if (!ARG_MAY_BE_SENT_BY_REF(func, i + 1)) {
					/* By-value send is not allowed -- emit a warning,
					 * but still perform the call with a by-value send. */
					zend_error(E_WARNING,
						"Parameter %d to %s%s%s() expected to be a reference, value given", i + 1,
						func->common.scope ? ZSTR_VAL(func->common.scope->name) : "",
						func->common.scope ? "::" : "",
						ZSTR_VAL(func->common.function_name));
				}
			}
		} else {
			if (Z_ISREF_P(arg) &&
			    !(func->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE)) {
				/* don't separate references for __call */
				arg = Z_REFVAL_P(arg);
			}
		}
		param = ZEND_CALL_ARG(call, i + 1);
		ZVAL_COPY(param, arg);
	}

	if (UNEXPECTED(func->op_array.fn_flags & ZEND_ACC_CLOSURE)) {
		ZEND_ASSERT(GC_TYPE((zend_object *)func->op_array.prototype) == IS_OBJECT);
		GC_ADDREF((zend_object *)func->op_array.prototype);
		ZEND_ADD_CALL_FLAG(call, ZEND_CALL_CLOSURE);
	}

	if (func->type == ZEND_USER_FUNCTION) {
		int call_via_handler = (func->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0;
		zend_init_execute_data(call, &func->op_array, fci.retval);
		zend_execute_ex(call);
		if (call_via_handler) {
			/* We must re-initialize function again */
			RUNKIT_CLEAR_FCI_CACHE(fci_cache);
		}
	} else if (func->type == ZEND_INTERNAL_FUNCTION) {
		int call_via_handler = (func->common.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE) != 0;
		ZVAL_NULL(fci.retval);
		call->prev_execute_data = EG(current_execute_data);
		call->return_value = NULL; /* this is not a constructor call */
		EG(current_execute_data) = call;
		if (EXPECTED(zend_execute_internal == NULL)) {
			/* saves one function call if zend_execute_internal is not used */
			func->internal_function.handler(call, fci.retval);
		} else {
			zend_execute_internal(call, fci.retval);
		}
		EG(current_execute_data) = call->prev_execute_data;
		zend_vm_stack_free_args(call);

		/*  We shouldn't fix bad extensions here,
			because it can break proper ones (Bug #34045)
		if (!EX(function_state).function->common.return_reference)
		{
			INIT_PZVAL(f->retval);
		}*/
		if (EG(exception)) {
			zval_ptr_dtor(fci.retval);
			ZVAL_UNDEF(fci.retval);
		}

		if (call_via_handler) {
			/* We must re-initialize function again */
			RUNKIT_CLEAR_FCI_CACHE(fci_cache);
		}
	} else { /* ZEND_OVERLOADED_FUNCTION */
#if PHP_VERSION_ID < 70400
		ZVAL_NULL(fci.retval);

		/* Not sure what should be done here if it's a static method */
		if (fci.object) {
			call->prev_execute_data = EG(current_execute_data);
			EG(current_execute_data) = call;
			fci.object->handlers->call_method(func->common.function_name, fci.object, call, fci.retval);
			EG(current_execute_data) = call->prev_execute_data;
		} else {
			zend_throw_error(NULL, "Cannot call overloaded function for non-object");
		}

		zend_vm_stack_free_args(call);

		if (func->type == ZEND_OVERLOADED_FUNCTION_TEMPORARY) {
			zend_string_release(func->common.function_name);
		}
		efree(func);

		if (EG(exception)) {
			zval_ptr_dtor(fci.retval);
			ZVAL_UNDEF(fci.retval);
		}
#else
		// php 7.4 got rid of ZEND_OVERLOADED_FUNCTION
		ZEND_ASSERT(func->type == ZEND_INTERNAL_FUNCTION);
#endif
	}

	zend_vm_stack_free_call_frame(call);

	if (EG(current_execute_data) == &dummy_execute_data) {
		EG(current_execute_data) = dummy_execute_data.prev_execute_data;
	}

	if (EG(exception)) {
		zend_throw_exception_internal(NULL);
	}
	return SUCCESS;
}
