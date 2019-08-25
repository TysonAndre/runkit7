/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2006 The PHP Group, (c) 2008-2015 Dmitry Zenovich |
  | "runkit7" patches (c) 2015-2019 Tyson Andre                          |
  +----------------------------------------------------------------------+
  | This source file is subject to the new BSD license,                  |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.opensource.org/licenses/BSD-3-Clause                      |
  | or at https://github.com/runkit7/runkit7/blob/master/LICENSE         |
  +----------------------------------------------------------------------+
  | Author: Sara Golemon <pollita@php.net>                               |
  | Modified by Dmitry Zenovich <dzenovich@gmail.com>                    |
  | Modified for php7 by Tyson Andre <tysonandre775@hotmail.com>         |
  +----------------------------------------------------------------------+
*/

#include "runkit.h"
#include "php_runkit_hash.h"
#include "php_runkit_zend_execute_API.h"

#include "Zend/zend.h"

#ifdef ZEND_MAP_PTR_GET
#define RUNKIT_RUN_TIME_CACHE(op_array) \
       ZEND_MAP_PTR_GET((op_array)->run_time_cache)
#else
#define RUNKIT_RUN_TIME_CACHE(op_array) \
       ((op_array)->run_time_cache)
#endif


// Get lvalue of the aliased user function for a fake internal function.
#define RUNKIT_ALIASED_USER_FUNCTION(fe) ((fe)->internal_function.reserved[0])
#define RUNKIT_IS_ALIAS_FOR_USER_FUNCTION(fe) ((fe)->type == ZEND_INTERNAL_FUNCTION && (fe)->internal_function.handler == php_runkit_function_alias_handler)

extern ZEND_API void zend_vm_set_opcode_handler(zend_op *op);

#ifdef PHP_RUNKIT_MANIPULATION

/* {{{ Top level declarations */
static void php_runkit_function_copy_ctor_same_type(zend_function *fe, zend_string *newname);
/* }}} */

/* {{{ php_runkit_check_call_stack
 */
int php_runkit_check_call_stack(zend_op_array *op_array)
{
	zend_execute_data *ptr;

	ptr = EG(current_execute_data);

	while (ptr) {
		// TODO: ???
		if (ptr->func && ptr->func->op_array.opcodes == op_array->opcodes) {
			return FAILURE;
		}
		ptr = ptr->prev_execute_data;
	}

	return SUCCESS;
}
/* }}} */

/* Temporary function name. This function is manipulated with functions in this file. */
#define RUNKIT_TEMP_FUNCNAME  "__runkit_temporary_function__"
/* Temporary class name. A function in this class is manipulated with functions in this file. */
#define RUNKIT_TEMP_CLASSNAME  "__runkit_temporary_class__"
/* Temporary method name. A function in this class is manipulated with functions in this file. */
#define RUNKIT_TEMP_METHODNAME  "__runkit_temporary_method__"

/* Maintain order */
#define PHP_RUNKIT_FETCH_FUNCTION_INSPECT	0
#define PHP_RUNKIT_FETCH_FUNCTION_REMOVE	1
#define PHP_RUNKIT_FETCH_FUNCTION_RENAME	2

/* {{{ php_runkit_fetch_function
 */
static zend_function *php_runkit_fetch_function(zend_string *fname, int flag)
{
	zend_function *fe;
	zend_string *fname_lower;

	fname_lower = zend_string_tolower(fname);

	if ((fe = zend_hash_find_ptr(EG(function_table), fname_lower)) == NULL) {
		zend_string_release(fname_lower);
		php_error_docref(NULL, E_WARNING, "%s() not found", ZSTR_VAL(fname));
		return NULL;
	}

	if (fe->type == ZEND_INTERNAL_FUNCTION &&
		!RUNKIT_G(internal_override)) {
		zend_string_release(fname_lower);
		php_error_docref(NULL, E_WARNING, "%s() is an internal function and runkit.internal_override is disabled", ZSTR_VAL(fname));
		return NULL;
	}

	if (fe->type != ZEND_USER_FUNCTION &&
		fe->type != ZEND_INTERNAL_FUNCTION) {
		zend_string_release(fname_lower);
		php_error_docref(NULL, E_WARNING, "%s() is not a user or normal internal function", ZSTR_VAL(fname));
		return NULL;
	}

	if (fe->type == ZEND_INTERNAL_FUNCTION &&
		flag >= PHP_RUNKIT_FETCH_FUNCTION_REMOVE) {

		if (!RUNKIT_G(replaced_internal_functions)) {
			ALLOC_HASHTABLE(RUNKIT_G(replaced_internal_functions));
			zend_hash_init(RUNKIT_G(replaced_internal_functions), 4, NULL, NULL, 0);
		}
		// FIXME figure out what this is intended to do (Restoring internal functions on request shutdown)
		// This is also used to check if a function with a given name was originally internal.
		// Cloning this - It would otherwise be deleted with runkit_function_redefine called later... (TODO: Add 1 to the refcount?)
		// TODO: Properly specify the behavior to avoid memory leaks
		if (!zend_hash_exists(RUNKIT_G(replaced_internal_functions), fname_lower)) {
			Bucket *b;
			zend_function *fe_copy;
			// Copy over the original function - If the original function remains, it will be freed on module shutdown.
			zend_string_addref(fe->common.function_name);  // possibly unnecessary
			fe_copy = php_runkit_function_clone(fe, fe->common.function_name, ZEND_INTERNAL_FUNCTION);
			// Copy over the original persistent string - That string wouldn't be destroyed on request shutdown in fpm, and can be reused in future requests?
			b = php_runkit_zend_hash_find_bucket(EG(function_table), fname_lower);
			// php_error_docref(NULL, E_WARNING, "Finding persistent string %s: %llx\n", ZSTR_VAL(fname_lower), (long long)(uintptr_t)b);
			// It's a persistent string, not an interned string? Is it always a persistent string (E.g. in NTS, ZTS, maintainer ZTS)?
			if (b->key != NULL) {
				zend_string_addref(b->key);
				zend_string_release(fname_lower);
				fname_lower = b->key;
				// php_error_docref(NULL, E_WARNING, "Stealing persistent string %s\n", ZSTR_VAL(fname_lower));
			} else {
				zend_string_addref(fname_lower);
			}
			zend_hash_add_ptr(RUNKIT_G(replaced_internal_functions), fname_lower, fe_copy);
		}
		// printf("Adding fe %llx to replaced_internal_functions key=%s result=%llx\n", (long long)fe, ZSTR_VAL(fname_lower), (long long)result);
		/*
		 * If internal functions have been modified then runkit's request shutdown handler
		 * should be called after all other modules' ones.
		 */
		php_runkit_hash_move_runkit_to_front();
		EG(full_tables_cleanup) = 1; // dirty hack!
	}
	zend_string_release(fname_lower);

	return fe;
}
/* }}} */

/* {{{ php_runkit_ensure_misplaced_internal_functions_table_exists */
static inline void php_runkit_ensure_misplaced_internal_functions_table_exists()
{
	if (!RUNKIT_G(misplaced_internal_functions)) {
		ALLOC_HASHTABLE(RUNKIT_G(misplaced_internal_functions));
		zend_hash_init(RUNKIT_G(misplaced_internal_functions), 4, NULL, NULL, 0);
	}
}
/* }}} */

/* {{{ php_runkit_add_to_misplaced_internal_functions */
static inline void php_runkit_add_to_misplaced_internal_functions(zend_function *fe, zend_string *name_lower)
{
	if (fe->type == ZEND_INTERNAL_FUNCTION &&
	    (!RUNKIT_G(misplaced_internal_functions) ||
	     !zend_hash_exists(RUNKIT_G(misplaced_internal_functions), name_lower))
	) {
		zval tmp;
		php_runkit_ensure_misplaced_internal_functions_table_exists();
		// Add misplaced internal functions to a list of strings, to be wiped out on request shutdown (before restoring originals)
		ZVAL_STR(&tmp, name_lower);
		zend_string_addref(name_lower);  // TODO: Free this in calls to php_runkit_destroy_misplaced_internal_function on shutdown?
		zend_hash_next_index_insert(RUNKIT_G(misplaced_internal_functions), &tmp);
	}
}
/* }}} */

/* {{{ php_runkit_destroy_misplaced_internal_function */
static inline void php_runkit_destroy_misplaced_internal_function(zend_function *fe, zend_string *fname_lower)
{
	if (fe->type == ZEND_INTERNAL_FUNCTION && RUNKIT_G(misplaced_internal_functions) &&
	    zend_hash_exists(RUNKIT_G(misplaced_internal_functions), fname_lower)) {
		// PHP_RUNKIT_FREE_INTERNAL_FUNCTION_NAME would have been called, but function destructor already deletes those?
		zend_hash_del(RUNKIT_G(misplaced_internal_functions), fname_lower);
	}
}
/* }}} */

#ifdef RT_CONSTANT_EX
/* {{{ php_runkit_set_opcode_constant
		for absolute constant addresses (ZEND_USE_ABS_CONST_ADDR), changes op to point to the local copy of that literal.
		Modifies op's contents. */
static void php_runkit_set_opcode_constant(const zval *literals, znode_op *op, zval *literalI)
{
	debug_printf("php_runkit_set_opcode_constant(%llx, %llx, %d), USE_ABS=%d\n", (long long)literals, (long long)literalI, (int)sizeof(zval), ZEND_USE_ABS_CONST_ADDR);
#if ZEND_USE_ABS_CONST_ADDR
	RT_CONSTANT_EX(literals, *op) = literalI;
#else
	// TODO: Assert that this is in a meaningful range.
	// TODO: is this ever necessary for relative constant addresses?
	op->constant = ((char *)literalI) - ((char *)literals);
#endif
}
/* }}} */
#else
/* {{{ php_runkit_set_opcode_constant_relative
		for absolute constant addresses, creates a local copy of that literal.
		Modifies op's contents. */
static void php_runkit_set_opcode_constant_relative(const zend_op_array *op_array, const zend_op *opline, znode_op *op, zval *literalI)
{
	debug_printf("php_runkit_set_opcode_constant_relative(%llx, %d), USE_ABS=%d\n", (long long)literalI, (int)sizeof(zval), ZEND_USE_ABS_CONST_ADDR);
#if ZEND_USE_ABS_CONST_ADDR
	RUNKIT_RT_CONSTANT(op_array, opline, *op) = literalI;
#else
	debug_printf("opcodes=%llx literals=%llx opline=%llx op=%llx literalI=%llx\n", (long long)op_array->opcodes, (long long)op_array->literals, (long long)opline, (long long)op, (long long)literalI);
	// TODO: Assert that this is in a meaningful range.
	// TODO: is this ever necessary for relative constant addresses?
	// Opposite of ((zval*)(((char*)(opline)) + (int32_t)(node).constant))
	op->constant = ((char *)literalI) - ((char *)opline);
#endif
}
/* }}} */
#endif

/* {{{ php_runkit_function_alias_handler
    Used when an internal function is replaced by a user-defined/runkit function. Converts the ICALL to a UCALL.
    Params: zend_execute_data *execute_data, zval *return_value */
static ZEND_NAMED_FUNCTION(php_runkit_function_alias_handler)
{
	zend_function *fbc_inner;
	zend_function *fbc = execute_data->func;
	int result;
#if ZEND_DEBUG
	ZEND_ASSERT(fbc->type == ZEND_INTERNAL_FUNCTION);
#endif
	fbc_inner = (zend_function *)RUNKIT_ALIASED_USER_FUNCTION(fbc);
#if ZEND_DEBUG
	ZEND_ASSERT(fbc_inner->type == ZEND_USER_FUNCTION);
#endif

	// printf("In php_runkit_function_alias_handler! fbc=%llx fbc_inner=%llx execute_data=%llx return_value=%llx\n", (long long) fbc, (long long) fbc_inner, (long long)execute_data, (long long)return_value);
	// TODO: Pass a context?
	// TODO: Copy the implementation of zend_call_function, use it to set up an additional stack entry....
	// FIXME modify current_execute_data for stack traces.
	// FIXME : 4th param check_this should be 1 if this is a method.

	// Fake the stack and call the inner function.
	// Set up the execution of the new command.
	result = runkit_forward_call_user_function(fbc, fbc_inner, INTERNAL_FUNCTION_PARAM_PASSTHRU);
	(void)result;
	// printf("In php_runkit_function_alias_handler! execute_data=%llx return_value=%llx, return code = %d\n", (long long)execute_data, (long long)return_value, (int)result);
	// TODO: Throw an exception
	return;
	// FIXME: This is wrong, it did not start executing the command, it only set it up for execution. Try doing something similar to call_user_func_array?
}
/* }}} */

/* {{{ php_runkit_function_create_alias_internal_function */
/** Create a fake internal function which will call the duplicated user function instead. */
static void php_runkit_function_create_alias_internal_function(zend_function *fe, zend_function *fe_inner)
{
	// Zero out the parts that will be an internal function.
	memset((((uint8_t *)fe) + sizeof(fe->common)), 0, sizeof(zend_function) - sizeof(fe->common));
#if ZEND_DEBUG
	ZEND_ASSERT(fe_inner->type == ZEND_USER_FUNCTION);
#endif
	// TODO: Figure out refcount tracking of function names when aliases are used.
	fe->type = ZEND_INTERNAL_FUNCTION;
	fe->common.function_name = fe_inner->common.function_name;
	zend_string_addref(fe->common.function_name);
	debug_printf("Copying handler to %llx\n", (long long)(uintptr_t)php_runkit_function_alias_handler);
	fe->internal_function.handler = php_runkit_function_alias_handler;
	RUNKIT_ALIASED_USER_FUNCTION(fe) = fe_inner;
}
/* }}} */

/* {{{ php_runkit_function_copy_ctor
	Duplicate structures in a zend_function where necessary to make an outright duplicate.
	Does additional work to ensure that the type of the final function is orig_fe_type. */
int php_runkit_function_copy_ctor(zend_function *fe, zend_string *newname, char orig_fe_type)
{
	if (fe->type == orig_fe_type) {
		php_runkit_function_copy_ctor_same_type(fe, newname);
		return SUCCESS;
	} else if (orig_fe_type == ZEND_INTERNAL_FUNCTION) { /* We replaced an internal function with a user function. */
		zend_function *fe_inner = pemalloc(sizeof(zend_op_array), 1);
		memcpy(fe_inner, fe, sizeof(zend_op_array));
		php_runkit_function_copy_ctor_same_type(fe_inner, newname);
		php_runkit_function_create_alias_internal_function(fe, fe_inner);
		// printf("Allocated fe_inner=%llx type=%d\n", (long long)fe_inner, (int)fe_inner->type);
		return SUCCESS;
	}
	php_runkit_function_copy_ctor_same_type(fe, newname);
	return SUCCESS;
}
/* }}} */

/* {{{ runkit_allocate_opcode_copy
    Allocates a zend_op_array with enough memory to store the opcodes (As well as literals, if required for php 7.3) */
static zend_op *runkit_allocate_opcode_copy(const zend_op_array *const op_array)
{
#if PHP_VERSION_ID >= 70300
	// Adjustment for https://github.com/runkit7/runkit7/issues/126
	// I assume that PHP's pass_two() from Zend/zend_opcache.c would be done by now
	if (!(ZEND_USE_ABS_CONST_ADDR) &&
			(op_array->fn_flags & ZEND_ACC_DONE_PASS_TWO) &&
			op_array->literals) {
		// Allocate enough space for op_array->opcode_copy and op_array->last_literal()
		size_t bytes_to_allocate = ZEND_MM_ALIGNED_SIZE_EX(sizeof(zend_op) * op_array->last, 16) +
			sizeof(zval) * op_array->last_literal;
		zend_op *opcode = (zend_op *)emalloc(bytes_to_allocate);
		debug_printf("Allocating %d bytes for opcode array last=%d last_literal=%d\n", (int)bytes_to_allocate, (int)op_array->last, (int)op_array->last_literal);
		memset(opcode, 0, bytes_to_allocate);
		return opcode;
		// Skip the memcpy step for the op_array and the literals in php 7.3+
	}
#endif
	debug_printf("Allocating %d bytes for opcode array without literals\n", (int)(sizeof(zend_op) * op_array->last));
	return safe_emalloc(sizeof(zend_op), op_array->last, 0);
}
/* }}} */

/* {{{ runkit_allocate_literals */
static zval *runkit_allocate_literals(const zend_op_array *const op_array, zend_op *opcode_copy)
{
#if PHP_VERSION_ID >= 70300
	// Adjustment for https://github.com/runkit7/runkit7/issues/126
	// I assume that PHP's pass_two() from Zend/zend_opcode.c would be done by now
	if (!(ZEND_USE_ABS_CONST_ADDR) &&
			(op_array->fn_flags & ZEND_ACC_DONE_PASS_TWO)) {
		// Reuse the extra space allocated for runkit_allocate_opcode_copy
		debug_printf("Allocating literals at relative offset %d last=%d\n", (int)(ZEND_MM_ALIGNED_SIZE_EX(sizeof(zend_op) * op_array->last, 16)), (int)op_array->last);
		return (zval *)(((char *)opcode_copy) + ZEND_MM_ALIGNED_SIZE_EX(sizeof(zend_op) * op_array->last, 16));
	}
#else
	(void)opcode_copy;
#endif
	debug_printf("Allocating literals with emalloc last_literal=%d\n", op_array->last_literal);
	return safe_emalloc(op_array->last_literal, sizeof(zval), 0);
}
/* }}} */

/* {{{ php_runkit_function_copy_ctor_same_type
	Duplicates structures in an zend_function, creating a function of the same type (user/internal) as the original function
	This does the opposite of some parts of destroy_op_array() from Zend/zend_opcode.c
	*/
static void php_runkit_function_copy_ctor_same_type(zend_function *fe, zend_string *newname)
{

	zval *literals;
	void *run_time_cache;
	zend_string **dupvars;  // Array of zend_string*s
	zend_op *last_op;
	zend_op *opcode_copy;
	zend_op_array *const op_array = &(fe->op_array);
	uint32_t i;

	if (newname) {
		zend_string_addref(newname);
		fe->common.function_name = newname;
	} else {
		zend_string_addref(fe->common.function_name);
		// TODO: is this a memory leak?
		// fe->common.function_name = fe->common.function_name;
	}

	if (fe->common.type == ZEND_USER_FUNCTION) {
		if (op_array->vars) {
			i = op_array->last_var;
			dupvars = safe_emalloc(op_array->last_var, sizeof(zend_string *), 0);
			while (i > 0) {
				i--;
				dupvars[i] = op_array->vars[i];
				zend_string_addref(dupvars[i]);
			}
			op_array->vars = dupvars;
		}

		// TODO: Remove ZEND_ACC_IMMUTABLE from func.common.fn_flags, like php 7.4?
		// TODO: Look into other php 7.4 changes required for d57cd36e47b627dee5b825760163f8e62e23ab28

		if (op_array->static_variables) {
			// Similar to zend_closures.c's zend_create_closure copying static variables, zend_compile.c's do_bind_function
			// TODO: Does that work with references?
			// 979: This seems to be calling an internal function returning a reference, then crashing?
			// ZEND_ASSERT((call->func->common.fn_flags & ZEND_ACC_RETURN_REFERENCE)
			// 	? Z_ISREF_P(ret) : !Z_ISREF_P(ret));
			op_array->static_variables = zend_array_dup(op_array->static_variables);
#if PHP_VERSION_ID >= 70400
			ZEND_MAP_PTR_INIT(op_array->static_variables_ptr, &(op_array->static_variables));
#endif
		}

		if (RUNKIT_RUN_TIME_CACHE(op_array)) {
			// TODO: Garbage collect these, somehow?
			run_time_cache = pemalloc(op_array->cache_size, 1);
			memset(run_time_cache, 0, op_array->cache_size);
#ifdef ZEND_MAP_PTR_SET
			ZEND_MAP_PTR_SET(op_array->run_time_cache, run_time_cache);
#else
			op_array->run_time_cache = run_time_cache;
#endif
		}

		opcode_copy = runkit_allocate_opcode_copy(op_array);
		last_op = op_array->opcodes + op_array->last;
		// TODO: See if this code works on 32-bit PHP.
		for (i = 0; i < op_array->last; i++) {
			opcode_copy[i] = op_array->opcodes[i];
			debug_printf("opcode = %s, is_const=%d, constant=%d\n", zend_get_opcode_name((int)opcode_copy[i].opcode), (int)(opcode_copy[i].op1_type == IS_CONST), op_array->opcodes[i].op1.constant);
			if (opcode_copy[i].op1_type != IS_CONST) {
				// TODO: What about switch statements in php 7.2, etc?
				zend_op *opline;
				zend_op *jmp_addr_op1;
				switch (opcode_copy[i].opcode) {
#ifdef ZEND_GOTO
					case ZEND_GOTO:
#endif
#ifdef ZEND_FAST_CALL
					case ZEND_FAST_CALL:
#endif
					case ZEND_JMP:
						opline = &opcode_copy[i];
						jmp_addr_op1 = OP_JMP_ADDR(opline, opline->op1);
						// TODO: I don't know if this is really the same as jmp_addr. FIXME
						if (jmp_addr_op1 >= op_array->opcodes &&
							jmp_addr_op1 < last_op) {
#if ZEND_USE_ABS_JMP_ADDR
							opline->op1.jmp_addr = opcode_copy + (op_array->opcodes[i].op1.jmp_addr - op_array->opcodes);
#else
						opline->op1.jmp_offset += ((char *)op_array->opcodes) - ((char *)opcode_copy);
#endif
						}
				}
			}
			if (opcode_copy[i].op2_type != IS_CONST) {
				zend_op *opline;
				zend_op *jmp_addr_op2;
				switch (opcode_copy[i].opcode) {
					case ZEND_JMPZ:
					case ZEND_JMPNZ:
					case ZEND_JMPZ_EX:
					case ZEND_JMPNZ_EX:
#ifdef ZEND_JMP_SET
					case ZEND_JMP_SET:
#endif
#ifdef ZEND_JMP_SET_VAR
					case ZEND_JMP_SET_VAR:
#endif
						opline = &opcode_copy[i];
						jmp_addr_op2 = OP_JMP_ADDR(opline, opline->op2);
						// TODO: I don't know if this is really the same as jmp_addr. FIXME
						if (jmp_addr_op2 >= op_array->opcodes &&
							jmp_addr_op2 < last_op) {
#if ZEND_USE_ABS_JMP_ADDR
							opline->op2.jmp_addr = opcode_copy + (op_array->opcodes[i].op2.jmp_addr - op_array->opcodes);
#else
						opline->op2.jmp_offset += ((char *)op_array->opcodes) - ((char *)opcode_copy);
#endif
						}
				}
			}
		}

		debug_printf("op_array.literals = %llx, last_literal = %d\n", (long long)op_array->literals, op_array->last_literal);
		if (op_array->literals) {
			uint32_t k;
			debug_printf("   old op_array.literals = %llx, opcodes=%llx\n", (long long)op_array->literals, (long long)op_array->opcodes);
			literals = runkit_allocate_literals(op_array, opcode_copy);
			debug_printf("copied op_array.literals = %llx, opcodes=%llx\n", (long long)literals, (long long)opcode_copy);
			for (i = op_array->last_literal; i > 0; ) {
				i--;
				// This code copies the zvals from the original function's op array to the new function's op array.
				// TODO: Re-examine this line to see if reference counting was done properly.
				// TODO: fix all of the other places old types of copies were used.
				literals[i] = op_array->literals[i];
				Z_TRY_ADDREF(literals[i]);
				debug_printf("Copying literal of type %d\n", (int)Z_TYPE(literals[i]));
			}
			for (k = 0; k < op_array->last; k++) {
				zend_op *const new_op = &opcode_copy[k];
				zend_bool found_op1 = 0;
				zend_bool found_op2 = 0;
				debug_printf("%d(%s)\ttype=%d %d\n", k, zend_get_opcode_name(new_op->opcode), (int)new_op->op1_type, (int)new_op->op2_type);
				for (i = op_array->last_literal; i > 0; ) {
					i--;
					// TODO: This may be completely unnecessary on 64-bit systems. This may be broken on 32-bit systems.
#ifdef RT_CONSTANT_EX
					if (!found_op1 && new_op->op1_type == IS_CONST && RT_CONSTANT_EX(literals, new_op->op1) == &op_array->literals[i]) {
						debug_printf("old op1 constant #%d = %d\n", (int)k, new_op->op1.constant);
						php_runkit_set_opcode_constant(literals, &(new_op->op1), &literals[i]);
						debug_printf("new op1 constant #%d = %d\n", (int)k, new_op->op1.constant);
						found_op1 = 1;
					}
					if (!found_op2 && new_op->op2_type == IS_CONST && RT_CONSTANT_EX(literals, new_op->op2) == &op_array->literals[i]) {
						debug_printf("old op2 constant #%d = %d\n", (int)k, new_op->op2.constant);
						php_runkit_set_opcode_constant(literals, &(new_op->op2), &literals[i]);
						debug_printf("new op2 constant #%d = %d\n", (int)k, new_op->op2.constant);
						found_op2 = 1;
					}
#else
					// e.g. this is used on 64-bit builds of PHP.
					// The start of the literals zval array is memory aligned so the relative addressing may be different when copied.
					if (!found_op1 && new_op->op1_type == IS_CONST && RUNKIT_RT_CONSTANT(op_array, &op_array->opcodes[k], new_op->op1) == &op_array->literals[i]) {
						debug_printf("old op1 constant #%d = %d (v2)\n", (int)k, new_op->op1.constant);
						php_runkit_set_opcode_constant_relative(op_array, new_op, &(new_op->op1), &literals[i]);
						debug_printf("new op1 constant #%d = %d (v2)\n", (int)k, new_op->op1.constant);
						found_op1 = 1;
					}
					if (!found_op2 && new_op->op2_type == IS_CONST && RUNKIT_RT_CONSTANT(op_array, &op_array->opcodes[k], new_op->op2) == &op_array->literals[i]) {
						debug_printf("old op2 constant #%d = %d (v2)\n", (int)k, new_op->op2.constant);
						php_runkit_set_opcode_constant_relative(op_array, new_op, &(new_op->op2), &literals[i]);
						debug_printf("new op2 constant #%d = %d (v2)\n", (int)k, new_op->op2.constant);
						found_op2 = 1;
					}
#endif
				}
			}
			op_array->literals = literals;
		}
		op_array->opcodes = opcode_copy;
		op_array->refcount = (uint32_t *)emalloc(sizeof(uint32_t));
		*op_array->refcount = 1;

		// TODO: Check if this is an interned string...
		if (op_array->doc_comment) {
			zend_string_addref(op_array->doc_comment);
		}
		op_array->try_catch_array = (zend_try_catch_element *)estrndup((char *)op_array->try_catch_array, sizeof(zend_try_catch_element) * op_array->last_try_catch);
	if (op_array->live_range) {
			op_array->live_range = (zend_live_range *)estrndup((char *)op_array->live_range, sizeof(zend_live_range) * op_array->last_live_range);
	}

		if (op_array->arg_info) {
			zend_arg_info *tmpArginfo;
			zend_arg_info *originalArginfo;
			// num_args calculation taken from zend_opcode.c destroy_op_array
			// TODO: Add tests that functions with return types are properly created and destroyed.
			// TODO: Specify what runkit should do about return types, what is an error, what is valid.
			uint32_t num_args = op_array->num_args;
			int32_t offset = 0;

			if (op_array->fn_flags & ZEND_ACC_HAS_RETURN_TYPE) {
				offset++;
				num_args++;
			}
			if (op_array->fn_flags & ZEND_ACC_VARIADIC) {
				num_args++;
			}

			tmpArginfo = (zend_arg_info *)safe_emalloc(sizeof(zend_arg_info), num_args, 0);

			originalArginfo = &((op_array->arg_info)[-offset]);
			for (i = 0; i < num_args; i++) {
				tmpArginfo[i] = originalArginfo[i];
				if (tmpArginfo[i].name) {
					zend_string_addref((tmpArginfo[i].name));
				}
#if PHP_VERSION_ID < 70200
				if (tmpArginfo[i].class_name) {
					zend_string_addref(tmpArginfo[i].class_name);
				}
#else
				// php >= 7.2.0
				if (ZEND_TYPE_IS_CLASS(tmpArginfo[i].type)) {
					// This works as the opposite for both php 7.2 and 7.3 (zend_string_release or zend_string_release_ex
					zend_string_addref(ZEND_TYPE_NAME(tmpArginfo[i].type));
				}
#endif
			}
			op_array->arg_info = &tmpArginfo[offset];
		}
	}

	// I'm not sure why this was originally added in upstream -- Removing this isn't causing test failures in php 7.
	// NOTE: If this line is uncommented, then PHP 7.3 would fail during cleanup of op arrays,
	// because Zend's function destructor expects functions that didn't complete pass two to have a separate allocated array for literals.
	// fe->common.fn_flags &= ~ZEND_ACC_DONE_PASS_TWO;
	fe->common.prototype = fe;
}
/* }}} */

/* {{{ php_runkit_function_clone
   Makes a duplicate of fe that doesn't share any static variables, zvals, etc.
   TODO: Is there anything I can use from zend_duplicate_function? */
zend_function *php_runkit_function_clone(zend_function *fe, zend_string *newname, char orig_fe_type)
{
	// Make a persistent allocation.
	// TODO: Clean it up after a request?
	zend_function *new_function = pemalloc(sizeof(zend_function), 1);
	if (fe->type == ZEND_INTERNAL_FUNCTION) {
		memset(new_function, 0, sizeof(zend_function));
		memcpy(new_function, fe, sizeof(zend_internal_function));
	} else {
		memcpy(new_function, fe, sizeof(zend_function));
	}
	php_runkit_function_copy_ctor(new_function, newname, orig_fe_type);
	return new_function;
}
/* }}} */

void php_runkit_free_inner_if_aliased_function(zend_function *fe)
{
	if (RUNKIT_IS_ALIAS_FOR_USER_FUNCTION(fe)) {
		zval zv_inner;
		zend_function *fe_inner;
		fe_inner = (zend_function *)RUNKIT_ALIASED_USER_FUNCTION(fe);
		// TODO: Free these earlier if possible.
		debug_printf("Freeing internal function %llx of %llx: %s\n", (long long)(uintptr_t)(fe_inner), (long long)(uintptr_t)fe, (char *)ZSTR_VAL(fe_inner->common.function_name));
#if ZEND_DEBUG
		ZEND_ASSERT(fe_inner->type == ZEND_USER_FUNCTION);
#endif
		ZVAL_FUNC(&zv_inner, fe_inner);
		zend_function_dtor(&zv_inner);
		pefree(fe_inner, 1);
	}
}

/* {{{ php_runkit_function_dtor_impl - Destroys functions, handles special fields added if they're from runkit. */
void php_runkit_function_dtor_impl(zend_function *fe, zend_bool is_clone)
{
	zval zv;
	zend_bool is_user_function;
	is_user_function = fe->type == ZEND_USER_FUNCTION;
	php_runkit_free_inner_if_aliased_function(fe);
	ZVAL_FUNC(&zv, fe);
	zend_function_dtor(&zv);
	// Note: This can only be used with zend_functions created by php_runkit_function_clone.
	// ZEND_INTERNAL_FUNCTIONs are freed.
	if (is_clone && is_user_function) {
		pefree(fe, 1);
	}
}
/* }}} */

/* {{{ php_runkit_function_dtor - Only to be used if we are sure this was created by runkit with runkit_function_clone. */
void php_runkit_function_dtor(zend_function *fe)
{
	php_runkit_function_dtor_impl(fe, 1);
}
/* }}} */

// The original destructor of the affected function_table.
static dtor_func_t __function_table_orig_pDestructor = NULL;

static void php_runkit_function_table_dtor(zval *pDest)
{
	zend_function *fe = (zend_function *)Z_PTR_P(pDest);
	if (RUNKIT_IS_ALIAS_FOR_USER_FUNCTION(fe)) {
		php_runkit_free_inner_if_aliased_function(fe);
	} else {
		// Don't free the inner if it's one of the ZEND_INTERNAL_FUNCTIONs moved elsewhere
		if (fe->type != ZEND_INTERNAL_FUNCTION && __function_table_orig_pDestructor != NULL) {
			__function_table_orig_pDestructor(pDest);
		}
	}
}

/* {{{ php_runkit_function_create_clone_alias_user_function - Create a user function aliasing an internal function */
static zend_function *php_runkit_function_create_clone_alias_user_function(zend_function *sfe)
{
#if ZEND_DEBUG
	ZEND_ASSERT(sfe->type == ZEND_INTERNAL_FUNCTION);
#endif

	/*
	// FIXME support making user functions that are aliases of internal functions and callable.
	// Currently, we just leave them as the original internal function.
	if (1) {
		zend_function *fe;
		fe = emalloc(sizeof(zend_op_array), 1);
		memcpy(fe, sfe, sizeof(sfe->common));  // Copy the common parts of internal function to the user function
		memset((((uint8_t*) fe) + sizeof(fe->common)), 0, sizeof(zend_op) - sizeof(fe->common));
		fe->op_array.refcount = (uint32_t*)emalloc(sizeof(uint32_t));
		*(fe->op_array.refcount) = 1;
		// TODO: initialize op_array.opcodes with the desired operations.

		fe->reserved[0] = sfe;
		return fe;
	}
	*/
	return sfe;
}
/* }}} */

/* {{{ php_runkit_remove_from_function_table - This handles special cases where we associate aliased functions with the original */
int php_runkit_remove_from_function_table(HashTable *function_table, zend_string *func_lower)
{
	// To be called for internal functions (TODO: internal methods?)
	// TODO: Have a way to detect function clones.
	int result;
	__function_table_orig_pDestructor = function_table->pDestructor;
	function_table->pDestructor = php_runkit_function_table_dtor;
	result = zend_hash_del(function_table, func_lower);
	function_table->pDestructor = __function_table_orig_pDestructor;
	__function_table_orig_pDestructor = NULL;
	return result;
}
/* }}} */

/* {{{ php_runkit_update_ptr_in_function_table - This handles special cases where we associate aliased functions with the original, and then replace that with zend_hash_update */
void *php_runkit_update_ptr_in_function_table(HashTable *function_table, zend_string *func_lower, zend_function *f)
{
	// To be called for internal functions (TODO: internal methods?)
	// TODO: Have a way to detect function clones.
	void *result;
	__function_table_orig_pDestructor = function_table->pDestructor;
	function_table->pDestructor = php_runkit_function_table_dtor;
	result = zend_hash_update_ptr(function_table, func_lower, f);
	function_table->pDestructor = __function_table_orig_pDestructor;
	__function_table_orig_pDestructor = NULL;
	return result;
}
/* }}} */

/* {{{ runkit_zend_hash_add_or_update_function_table_ptr */
static inline void *runkit_zend_hash_add_or_update_function_table_ptr(HashTable *function_table, zend_string *key, void *pData, uint32_t flag)
{
	void *result;
	__function_table_orig_pDestructor = function_table->pDestructor;
	function_table->pDestructor = php_runkit_function_table_dtor;
	result = runkit_zend_hash_add_or_update_ptr(function_table, key, pData, flag);
	function_table->pDestructor = __function_table_orig_pDestructor;
	__function_table_orig_pDestructor = NULL;
	return result;
}
/* }}} */

/* {{{ php_runkit_clear_function_runtime_cache */
static void php_runkit_clear_function_runtime_cache(zend_function *f)
{
	zend_op_array *op_array;
	if (f->type != ZEND_USER_FUNCTION) {
		return;
	}
	op_array = &(f->op_array);
	if (op_array->cache_size == 0 || RUNKIT_RUN_TIME_CACHE(op_array) == NULL) {
		return;
	}

	// TODO: Does memset do what I want it to do?
	memset(RUNKIT_RUN_TIME_CACHE(op_array), 0, op_array->cache_size);
}
/* }}} */

/* {{{ php_runkit_clear_function_runtime_cache_for_function_table */
static void php_runkit_clear_function_runtime_cache_for_function_table(HashTable *function_table) {
	zend_function* f;
	ZEND_HASH_FOREACH_PTR(function_table, f) {
		php_runkit_clear_function_runtime_cache(f);
	} ZEND_HASH_FOREACH_END();
}
/* }}} */

/* {{{ php_runkit_clear_all_functions_runtime_cache */
void php_runkit_clear_all_functions_runtime_cache()
{
	uint32_t i;
	zend_execute_data *ptr;
	zend_class_entry *ce;

	php_runkit_clear_function_runtime_cache_for_function_table(EG(function_table));

	ZEND_HASH_FOREACH_PTR(EG(class_table), ce) {
		php_runkit_clear_function_runtime_cache_for_function_table(&(ce->function_table));
	} ZEND_HASH_FOREACH_END();

	// TODO: Does this make sense, does it work with a runtime cache?
	for (ptr = EG(current_execute_data); ptr != NULL; ptr = ptr->prev_execute_data) {
		// TODO: I assume that ptr->run_time_cache is the same pointer, if set?
		if (ptr->func == NULL || ptr->func->type == ZEND_INTERNAL_FUNCTION || ptr->func->op_array.cache_size == 0 || RUNKIT_RUN_TIME_CACHE(&(ptr->func->op_array)) == NULL) {
			continue;
		}
		memset(RUNKIT_RUN_TIME_CACHE(&(ptr->func->op_array)), 0, ptr->func->op_array.cache_size);
	}

	PHP_RUNKIT_ITERATE_THROUGH_OBJECTS_STORE_BEGIN(i)
		if (object->ce == zend_ce_closure) {
		zend_closure *cl = (zend_closure *)object;
			php_runkit_clear_function_runtime_cache(&cl->func);
		}
	PHP_RUNKIT_ITERATE_THROUGH_OBJECTS_STORE_END
}
/* }}} */

/* {{{ php_runkit_fix_hardcoded_stack_sizes */
static inline void php_runkit_fix_hardcoded_stack_sizes(zend_function *f, zend_string *called_name_lower, zend_function *called_f)
{
	// called_name_lower is the lowercase function name.
	// f is a function which may or may not call called_name_lower, and may or may not have already been fixed.
	// TODO: Make this work with namespaced function names
	// TODO: Do method names also need to be fixed?
	// TODO: Do I only need to worry about user function calls within the same file?

	// FIXME work on namespaced functions
	zend_op_array *op_array;
	zend_op *opline_it;
	zend_op *end;
	if (f == NULL || f->type != ZEND_USER_FUNCTION) {
		return;
	}

	op_array = &(f->op_array);
	opline_it = op_array->opcodes;
	end = opline_it + op_array->last;
	//printf("Calling php_runkit_fix_hardcoded_stack_sizes for user func last=%d\n", op_array->last);
	for (; opline_it < end; opline_it++) {
		// printf("Calling opcode=%d\n", opline_it->opcode);
		// PHP uses different constants (More constant space needed for ZEND_INIT_FCALL_BY_NAME),
		// instead of converting to ZEND_INIT_FCALL_BY_NAME, recalculate the maximum amount of stack space opline_it may need.
		if (opline_it->opcode == ZEND_INIT_FCALL) {
			zval *function_name = (zval *)(RUNKIT_RT_CONSTANT(op_array, opline_it, opline_it->op2));
			//printf("Checking init_fcall, function name = %s\n", ZSTR_VAL(Z_STR_P(function_name)));
			if (zend_string_equals(Z_STR_P(function_name), called_name_lower)) {
				// Modify that opline with the recalculated required stack size
				uint32_t new_size = zend_vm_calc_used_stack(opline_it->extended_value, called_f);
				if (new_size > opline_it->op1.num) {
					opline_it->op1.num = new_size;
				}
			}
		}
	}
}
/* }}} */

/* {{{ php_runkit_fix_hardcoded_stack_sizes_for_function_table */
static void php_runkit_fix_hardcoded_stack_sizes_for_function_table(HashTable *function_table, zend_string *called_name_lower, zend_function *called_f)
{
	zend_function *f;
	ZEND_HASH_FOREACH_PTR(function_table, f) {
		php_runkit_fix_hardcoded_stack_sizes(f, called_name_lower, called_f);
	} ZEND_HASH_FOREACH_END();
}
/* }}} */

/* {{{ php_runkit_fix_hardcoded_stack_sizes */
void php_runkit_fix_all_hardcoded_stack_sizes(zend_string *called_name_lower, zend_function *called_f)
{
	uint32_t i;
	zend_class_entry *ce;
	zend_execute_data *ptr;

	php_runkit_fix_hardcoded_stack_sizes_for_function_table(EG(function_table), called_name_lower, called_f);

	ZEND_HASH_FOREACH_PTR(EG(class_table), ce) {
		php_runkit_fix_hardcoded_stack_sizes_for_function_table(&(ce->function_table), called_name_lower, called_f);
	} ZEND_HASH_FOREACH_END();

	// This is also needed to get the top-level {main} script, which isn't in the function table
	// FIXME what about other scripts that are include()ed
	for (ptr = EG(current_execute_data); ptr != NULL; ptr = ptr->prev_execute_data) {
		// TODO: I assume that ptr->run_time_cache is the same pointer, if set?
		if (ptr->func == NULL || ptr->func->type != ZEND_USER_FUNCTION) {
			continue;
		}
		php_runkit_fix_hardcoded_stack_sizes(ptr->func, called_name_lower, called_f);
	}

	PHP_RUNKIT_ITERATE_THROUGH_OBJECTS_STORE_BEGIN(i)
		if (object->ce == zend_ce_closure) {
		zend_closure *cl = (zend_closure *)object;
			php_runkit_fix_hardcoded_stack_sizes(&cl->func, called_name_lower, called_f);
		}
	PHP_RUNKIT_ITERATE_THROUGH_OBJECTS_STORE_END
}
/* }}} */

/* {{{ php_runkit_reflection_update_property */
static void php_runkit_reflection_update_property(zend_object *object, const char *name, zval *value)
{
	// Copied from ext/reflection's reflection_update_property
#if PHP_VERSION_ID >= 80000
	zend_string *name_string = zend_string_init(name, strlen(name), 0);
	zend_std_write_property(object, name_string, value, NULL);
	zend_string_release(name_string);
#else
	zval obj;
	zval member;
	ZVAL_OBJ(&obj, object);
	ZVAL_STRING(&member, name);
	zend_std_write_property(&obj, &member, value, NULL);
	zval_ptr_dtor(&member);
#endif
	if (Z_REFCOUNTED_P(value)) {
		Z_DELREF_P(value);
	}
}
/* }}} */

/* {{{ php_runkit_update_reflection_object_name */
void php_runkit_update_reflection_object_name(zend_object *object, int handle, const char *name)
{
	zval prop_value;
	ZVAL_STRING(&prop_value, name);
	php_runkit_reflection_update_property(object, RUNKIT_G(name_str), &prop_value);
}
/* }}} */

/* {{{ php_runkit_free_reflection_function */
static void php_runkit_free_reflection_function(zend_function *fptr)
{
	// Exact copy of ext/reflection's _free_function
	if (fptr && (fptr->internal_function.fn_flags & ZEND_ACC_CALL_VIA_TRAMPOLINE)) {
		zend_string_release(fptr->internal_function.function_name);
		zend_free_trampoline(fptr);
	}
}
/* }}} */

/* {{{ reflection_object_from_obj
   Copied from ext/reflection/php_reflection.c */
static inline reflection_object *reflection_object_from_obj(zend_object *obj)
{
	return (reflection_object *)((char *)(obj)-XtOffsetOf(reflection_object, zo));
}
/* }}} */

/* {{{ php_runkit_delete_reflection_function_ptr
 	Frees the parts of a reflection object referring to the removed method/function(/parameter?)  */
static void php_runkit_delete_reflection_function_ptr(reflection_object *intern)
{
	// Copied from ext/reflection's reflection_free_objects_storage
	parameter_reference *reference;
	if (intern->ptr) {
		switch (intern->ref_type) {
			case REF_TYPE_PARAMETER:
			reference = (parameter_reference *)intern->ptr;
				php_runkit_free_reflection_function(reference->fptr);
				efree(intern->ptr);
				break;
			case REF_TYPE_FUNCTION:
				php_runkit_free_reflection_function(intern->ptr);
				break;
			case REF_TYPE_PROPERTY:
				efree(intern->ptr);
				break;
			default:
			php_error_docref(NULL, E_ERROR, "Attempted to free ReflectionObject of unexpected REF_TYPE %d\n", (int)(intern->ref_type));
				return;
		}
	}
	intern->ptr = NULL;
	// Do NOT call object destructors yet - ReflectionObject will do that in __destruct.
}
/* }}}*/

/* {{{ php_runkit_remove_function_from_reflection_objects */
void php_runkit_remove_function_from_reflection_objects(zend_function *fe)
{
	uint32_t i;
	extern PHPAPI zend_class_entry *reflection_function_ptr;
	extern PHPAPI zend_class_entry *reflection_method_ptr;
	extern PHPAPI zend_class_entry *reflection_parameter_ptr;

	PHP_RUNKIT_ITERATE_THROUGH_OBJECTS_STORE_BEGIN(i)
		if (object->ce == reflection_function_ptr) {
			reflection_object *refl_obj = reflection_object_from_obj(object);
			if (refl_obj->ptr == fe) {
				php_runkit_delete_reflection_function_ptr(refl_obj);
				refl_obj->ptr = RUNKIT_G(removed_function);
				php_runkit_update_reflection_object_name(object, i, RUNKIT_G(removed_function_str));
			}
		} else if (object->ce == reflection_method_ptr) {
			reflection_object *refl_obj = reflection_object_from_obj(object);
			if (refl_obj->ptr == fe) {
				zend_function *f = emalloc(sizeof(zend_function));
				memcpy(f, RUNKIT_G(removed_method), sizeof(zend_function));
				f->common.scope = fe->common.scope;
#ifdef ZEND_ACC_CALL_VIA_HANDLER
				f->internal_function.fn_flags |= ZEND_ACC_CALL_VIA_HANDLER; // This is a trigger to free it from destructor
#endif
				zend_string_addref(f->internal_function.function_name);
				php_runkit_delete_reflection_function_ptr(refl_obj);
				refl_obj->ptr = f;
				php_runkit_update_reflection_object_name(object, i, RUNKIT_G(removed_method_str));
			}
		} else if (object->ce == reflection_parameter_ptr) {
			reflection_object *refl_obj = reflection_object_from_obj(object);
		parameter_reference *reference = (parameter_reference *)refl_obj->ptr;
			if (reference && reference->fptr == fe) {
				php_runkit_delete_reflection_function_ptr(refl_obj);
				refl_obj->ptr = NULL;
				php_runkit_update_reflection_object_name(object, i, RUNKIT_G(removed_parameter_str));
			}
		}
	PHP_RUNKIT_ITERATE_THROUGH_OBJECTS_STORE_END
}
/* }}} */

/* {{{ php_runkit_generate_lambda_function
    Heavily borrowed from ZEND_FUNCTION(create_function).
    Used by runkit_function_add and runkit_function_redefine. Also see php_runkit_generate_lambda_method. */
int php_runkit_generate_lambda_function(const zend_string *arguments, const zend_string *return_type, const zend_bool is_strict, const zend_string *phpcode,
                                        zend_function **pfe, zend_bool return_ref)
{
	char *eval_code;
	char *eval_name;
	char *return_type_code;
	int eval_code_length;

	eval_code_length =
		(is_strict ? (sizeof("declare(strict_types=1);") - 1) : 0) +
		sizeof("function " RUNKIT_TEMP_FUNCNAME) +
		ZSTR_LEN(arguments) + 4 +
		ZSTR_LEN(phpcode) +
		(return_ref ? 1 : 0);
	if (return_type != NULL) {
		int return_type_code_length = ZSTR_LEN(return_type) + 4;
		return_type_code = (char *)emalloc(return_type_code_length + 1);
		snprintf(return_type_code, return_type_code_length + 4, " : %s ", ZSTR_VAL(return_type));
		eval_code_length += return_type_code_length;
	} else {
		return_type_code = (char *)emalloc(1);
		return_type_code[0] = '\0';
	}

	// TODO: ZEND_CALL_USES_STRICT_TYPES (ZEND_ACC_STRICT_TYPES) controls the strict mode behavior
	// I don't believe that that option changes the opcodes, so copy() can work.
	eval_code = (char *)emalloc(eval_code_length);
	snprintf(eval_code, eval_code_length, "%sfunction %s" RUNKIT_TEMP_FUNCNAME "(%s)%s{%s}", is_strict ? "declare(strict_types=1);" : "", (return_ref ? "&" : ""), ZSTR_VAL(arguments), return_type_code, ZSTR_VAL(phpcode));
	eval_name = zend_make_compiled_string_description("runkit runtime-created function");
	if (zend_eval_string(eval_code, NULL, eval_name) == FAILURE) {
		php_error_docref(NULL, E_ERROR, "Cannot create temporary function '%s'", eval_code);
		efree(eval_code);
		efree(eval_name);
		efree(return_type_code);
		zend_hash_str_del(EG(function_table), RUNKIT_TEMP_FUNCNAME, sizeof(RUNKIT_TEMP_FUNCNAME) - 1);
		return FAILURE;
	}
	efree(eval_code);
	efree(eval_name);
	efree(return_type_code);

	if ((*pfe = zend_hash_str_find_ptr(EG(function_table), RUNKIT_TEMP_FUNCNAME, sizeof(RUNKIT_TEMP_FUNCNAME) - 1)) == NULL) {
		php_error_docref(NULL, E_ERROR, "Unexpected inconsistency creating temporary runkit function");
		return FAILURE;
	}

	return SUCCESS;
}

/* {{{ php_runkit_generate_lambda_method
	Heavily borrowed from ZEND_FUNCTION(create_function).
	Used by runkit_method_add and runkit_method_redefine. Also see php_runkit_generate_lambda_function. */
int php_runkit_generate_lambda_method(const zend_string *arguments, const zend_string *return_type, const zend_bool is_strict, const zend_string *phpcode,

                                        zend_function **pfe, zend_bool return_ref, zend_bool is_static)
{
	char *eval_code;
	char *eval_name;
	char *return_type_code;
	int eval_code_length;
	zend_class_entry *ce;

	eval_code_length =
		(is_strict ? (sizeof("declare(strict_types=1);") - 1) : 0) +
		sizeof("class " RUNKIT_TEMP_CLASSNAME " { %sfunction " RUNKIT_TEMP_METHODNAME) +
		ZSTR_LEN(arguments) + 4 +
		ZSTR_LEN(phpcode) +
		(is_static ? (sizeof("static ") - 1) : 0) +
		(return_ref ? 1 : 0) +
		(sizeof("}") - 1);
		;
	if (return_type != NULL) {
		int return_type_code_length = ZSTR_LEN(return_type) + 4;
		return_type_code = (char *)emalloc(return_type_code_length + 1);
		snprintf(return_type_code, return_type_code_length + 4, " : %s ", ZSTR_VAL(return_type));
		eval_code_length += return_type_code_length;
	} else {
		return_type_code = (char *)emalloc(1);
		return_type_code[0] = '\0';
	}

	eval_code = (char *)emalloc(eval_code_length);
	snprintf(eval_code, eval_code_length,
			"%sclass " RUNKIT_TEMP_CLASSNAME " { %sfunction %s" RUNKIT_TEMP_METHODNAME "(%s)%s{%s}}",
			(is_strict ? "declare(strict_types=1);" : ""),
			(is_static ? "static " : ""),
			(return_ref ? "&" : ""),
			ZSTR_VAL(arguments),
			return_type_code,
			ZSTR_VAL(phpcode));
	eval_name = zend_make_compiled_string_description("runkit runtime-created method");
	if (zend_eval_string(eval_code, NULL, eval_name) == FAILURE) {
		efree(eval_code);
		efree(eval_name);
		efree(return_type_code);
		php_error_docref(NULL, E_ERROR, "Cannot create temporary method");
		zend_hash_str_del(EG(class_table), RUNKIT_TEMP_CLASSNAME, sizeof(RUNKIT_TEMP_CLASSNAME) - 1);
		return FAILURE;
	}
	efree(eval_code);
	efree(eval_name);
	efree(return_type_code);

	ce = zend_hash_str_find_ptr(EG(class_table), RUNKIT_TEMP_CLASSNAME, sizeof(RUNKIT_TEMP_CLASSNAME) - 1);
	if (ce == NULL) {
		php_error_docref(NULL, E_ERROR, "Unexpected inconsistency creating a temporary class");
		return FAILURE;
	}

	if ((*pfe = zend_hash_str_find_ptr(&(ce->function_table), RUNKIT_TEMP_METHODNAME, sizeof(RUNKIT_TEMP_METHODNAME) - 1)) == NULL) {
		php_error_docref(NULL, E_ERROR, "Unexpected inconsistency creating a temporary method");
	}

	return SUCCESS;
}

/* }}} */

/** {{{ php_runkit_cleanup_lambda_method
	Tries to free the temporary lambda function (from php_runkit_generate_lambda_function).
	If it fails, emits a warning and returns FAILURE.  */
int php_runkit_cleanup_lambda_function()
{
	if (zend_hash_str_del(EG(function_table), RUNKIT_TEMP_FUNCNAME, sizeof(RUNKIT_TEMP_FUNCNAME) - 1) == FAILURE) {
		php_error_docref(NULL, E_WARNING, "Unable to remove temporary function entry");
		return FAILURE;
	}
	return SUCCESS;
}
/* }}}*/

/** {{{ php_runkit_cleanup_lambda_method
	Tries to free the temporary lambda method (from php_runkit_generate_lambda_method).
	If it fails, emits a warning and returns FAILURE.  */
int php_runkit_cleanup_lambda_method()
{
	if (zend_hash_str_del(EG(class_table), RUNKIT_TEMP_CLASSNAME, sizeof(RUNKIT_TEMP_CLASSNAME) - 1) == FAILURE) {
		php_error_docref(NULL, E_WARNING, "Unable to remove temporary method entry");
		return FAILURE;
	}
	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_destroy_misplaced_functions
	Wipe old internal functions that were renamed to new targets
	They'll get replaced soon enough */
int php_runkit_destroy_misplaced_functions(zval *zv)
{
	// zend_function *fe;
	if (Z_TYPE_P(zv) != IS_STRING || Z_STRLEN_P(zv) == 0) {
		/* Nonsense, skip it */
		return ZEND_HASH_APPLY_REMOVE;
	}

	// TODO: Free string if it **isn't** found?
	/*if ((fe = zend_hash_find_ptr(EG(function_table), Z_STR_P(zv))) != NULL) {
		PHP_RUNKIT_FREE_INTERNAL_FUNCTION_NAME(fe);
	}*/

	// TODO: Valgrind complained about invalid reads if this is called for an internal function such as val_dump
	// Try to figure out how/when to properly delete functions, especially if references are shared.
	php_runkit_remove_from_function_table(EG(function_table), Z_STR_P(zv));

	return ZEND_HASH_APPLY_REMOVE;
}
/* }}} */

/* {{{ php_runkit_restore_internal_function
	Cleanup after modifications to an internal function. */
void php_runkit_restore_internal_function(zend_string *fname_lower, zend_function *f)
{
	if (!fname_lower || !ZSTR_LEN(fname_lower)) {  // Probably won't happen.
		return;
	}
	php_runkit_update_ptr_in_function_table(EG(function_table), fname_lower, f);
	// php_error_docref(NULL, E_WARNING, "%s() exists: %d", ZSTR_VAL(fname_lower), (int)(zend_hash_exists(EG(function_table), fname_lower) ? 1 : 0));

	// NOTE: moving to the front of the hash seems to be unnecessary in php 7 - Entries of function_table are removed with plain zend_hash_del
	/* It's possible for restored internal functions to now be blocking a ZEND_USER_FUNCTION
	 * which will screw up post-request cleanup.
	 * Avoid this by restoring internal functions to the front of the list where they won't be in the way
	 */
	// php_runkit_hash_move_to_front(EG(function_table), php_runkit_hash_get_bucket(EG(function_table), fname_lower));
}
/* }}} */

/* {{{ php_runkit_function_add_or_update */
static void php_runkit_function_add_or_update(INTERNAL_FUNCTION_PARAMETERS, int add_or_update)
{
	zend_string *funcname;
	zend_string *funcname_lower;
	zend_string *arguments = NULL;
	zend_string *phpcode = NULL;
	zend_string *doc_comment = NULL;  // TODO: Is this right?
	parsed_return_type return_type;
	parsed_is_strict is_strict;
	zend_bool return_ref = 0;
	zend_function *orig_fe = NULL, *source_fe = NULL, *func;
	char target_function_type;
	zval *args;
	int remove_temp = 0;
	long argc = ZEND_NUM_ARGS();
	long opt_arg_pos = 2;

	if (argc < 1 || zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, 1, "S", &funcname) == FAILURE || !ZSTR_LEN(funcname)) {
		php_error_docref(NULL, E_ERROR, "Function name should not be empty");
		RETURN_FALSE;
	}

	if (argc < 2) {
		php_error_docref(NULL, E_ERROR, "Function body should be provided");
		RETURN_FALSE;
	}

	if (!php_runkit_parse_args_to_zvals(argc, &args)) {
		RETURN_FALSE;
	}

	if (!php_runkit_parse_function_arg(argc, args, 1, &source_fe, &arguments, &phpcode, &opt_arg_pos, "Function")) {
		efree(args);
		RETURN_FALSE;
	}

	if (argc > opt_arg_pos && !source_fe) {
		switch (Z_TYPE(args[opt_arg_pos])) {
			case IS_NULL:
			case IS_TRUE:
			case IS_FALSE:
				convert_to_boolean_ex(&args[opt_arg_pos]);
				return_ref = Z_TYPE(args[opt_arg_pos]) == IS_TRUE;
				break;
			default:
				php_error_docref(NULL, E_WARNING, "return_ref should be boolean");
		}
		opt_arg_pos++;
	}

	doc_comment = php_runkit_parse_doc_comment_arg(argc, args, opt_arg_pos);

	return_type = php_runkit_parse_return_type_arg(argc, args, opt_arg_pos + 1);

	is_strict = php_runkit_parse_is_strict_arg(argc, args, opt_arg_pos + 2);

	efree(args);
	if (!return_type.valid) {
		RETURN_FALSE;
	}
	if (!is_strict.valid) {
		RETURN_FALSE;
	}

	if (source_fe && return_type.return_type) {
		// TODO: Check what needs to be needs to be changed in opcode array if return_type is changed
		php_error_docref(NULL, E_WARNING, "Overriding return_type is not currently supported for closures, use return type in closure definition instead (or pass in code as string)");
		RETURN_FALSE;
	}
	if (source_fe && is_strict.overridden) {
		// TODO: Check what needs to be needs to be changed in opcode array if is_strict is changed
		php_error_docref(NULL, E_WARNING, "Overriding is_strict is not currently supported for closures (pass in code as string)");
		RETURN_FALSE;
	}

	if (add_or_update == HASH_UPDATE &&
	    (orig_fe = php_runkit_fetch_function(funcname, PHP_RUNKIT_FETCH_FUNCTION_REMOVE)) == NULL) {
		RETURN_FALSE;
	}

	/* UTODO */
	funcname_lower = zend_string_tolower(funcname);

	if (add_or_update == HASH_ADD && zend_hash_exists(EG(function_table), funcname_lower)) {
		zend_string_release(funcname_lower);
		php_error_docref(NULL, E_WARNING, "Function %s() already exists", ZSTR_VAL(funcname));
		RETURN_FALSE;
	}

	if (!source_fe) {
		if (php_runkit_generate_lambda_function(arguments, return_type.return_type, is_strict.is_strict, phpcode, &source_fe, return_ref) == FAILURE) {
			zend_string_release(funcname_lower);
			RETURN_FALSE;
		}
		remove_temp = 1;
	}

	if (orig_fe) {
		// The function type should be preserved, before and after redefining.
		target_function_type = orig_fe->type;
	} else {
		// The original type is stored in a hash map - If an internal function is renamed, it has an entry in replaced_internal_functions.
		target_function_type = RUNKIT_G(replaced_internal_functions)
			&& zend_hash_exists(RUNKIT_G(replaced_internal_functions), funcname_lower) ? ZEND_INTERNAL_FUNCTION : ZEND_USER_FUNCTION;
	}
	func = php_runkit_function_clone(source_fe, funcname, target_function_type);
	//printf("Func function->handler = %llx, op=%s\n", source_fe->type == ZEND_USER_FUNCTION ? (long long)source_fe->internal_function.handler : 0, add_or_update == HASH_ADD ? "add" : "update");
	func->common.scope = NULL;
	func->common.fn_flags &= ~ZEND_ACC_CLOSURE;

	if (doc_comment == NULL && source_fe->op_array.doc_comment == NULL &&
	    orig_fe && orig_fe->type == ZEND_USER_FUNCTION && orig_fe->op_array.doc_comment) {
		doc_comment = orig_fe->op_array.doc_comment;
	}
	php_runkit_modify_function_doc_comment(func, doc_comment);

	/* When redefining or adding a function (which may have been removed before), update the stack sizes it will be called with. */
	php_runkit_fix_all_hardcoded_stack_sizes(funcname_lower, func);

	if (add_or_update == HASH_UPDATE) {
		php_runkit_remove_function_from_reflection_objects(orig_fe);
		php_runkit_destroy_misplaced_internal_function(orig_fe, funcname_lower);

		php_runkit_clear_all_functions_runtime_cache();
	}

	if (runkit_zend_hash_add_or_update_function_table_ptr(EG(function_table), funcname_lower, func, add_or_update) == NULL) {
		php_error_docref(NULL, E_WARNING, "Unable to add new function");
		zend_string_release(funcname_lower);
		if (remove_temp) {
			php_runkit_cleanup_lambda_function();
		}
		// TODO: Is there a chance this will accidentally delete the original function?
		php_runkit_function_dtor(func);
		RETURN_FALSE;
	}

	if (remove_temp) {
		php_runkit_cleanup_lambda_function();
	}

	if (zend_hash_find(EG(function_table), funcname_lower) == NULL) {
		// TODO: || !fe - what did that do?
		php_error_docref(NULL, E_WARNING, "Unable to locate newly added function");
		zend_string_release(funcname_lower);
		RETURN_FALSE;
	}

	zend_string_release(funcname_lower);

	RETURN_TRUE;
}
/* }}} */

/* *****************
   * Functions API *
   ***************** */

/* {{{  proto bool runkit7_function_add(string funcname, string arglist, string code[, bool return_by_reference=false[, string doc_comment = null, [string return_type = null, [bool is_strict = null]]]])
	proto bool runkit7_function_add(string funcname, closure code[, string doc_comment, [bool is_strict]])
	Add a new function, similar to create_function, but allows specifying name
	*/
PHP_FUNCTION(runkit7_function_add)
{
	php_runkit_function_add_or_update(INTERNAL_FUNCTION_PARAM_PASSTHRU, HASH_ADD);
}
/* }}} */

/* {{{ proto bool runkit7_function_remove(string funcname)
 */
PHP_FUNCTION(runkit7_function_remove)
{
	zend_string *fname;
	zend_string *fname_lower;
	int result;
	zend_function *fe;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "S", &fname) == FAILURE) {
		RETURN_FALSE;
	}

	// TODO what?
	if ((fe = php_runkit_fetch_function(fname, PHP_RUNKIT_FETCH_FUNCTION_REMOVE)) == NULL) {
		RETURN_FALSE;
	}

	fname_lower = zend_string_tolower(fname);

	php_runkit_remove_function_from_reflection_objects(fe);
	php_runkit_destroy_misplaced_internal_function(fe, fname_lower);

	result = (zend_hash_del(EG(function_table), fname_lower) == SUCCESS);
	zend_string_release(fname_lower);

	php_runkit_clear_all_functions_runtime_cache();

	RETURN_BOOL(result);
}
/* }}} */

/* {{{ PHP_RUNKIT_FUNCTION_PARSE_RENAME_COPY_PARAMS */
#define PHP_RUNKIT_FUNCTION_PARSE_RENAME_COPY_PARAMS \
	zend_string *sfunc; \
	zend_string *dfunc; \
	zend_string *sfunc_lower; \
	zend_string *dfunc_lower; \
	\
	if (zend_parse_parameters(ZEND_NUM_ARGS(), \
			"SS", \
			&sfunc, \
				  &dfunc) == FAILURE) {                                    \
		RETURN_FALSE; \
	} \
	\
	/* UTODO */ \
	dfunc_lower = zend_string_tolower(dfunc); \
	\
	if (zend_hash_exists(EG(function_table), dfunc_lower)) { \
		zend_string_release(dfunc_lower); \
		php_error_docref(NULL, E_WARNING, "%s() already exists", ZSTR_VAL(dfunc)); \
		RETURN_FALSE; \
	}
/* }}} */

/* {{{ ensure_misplaced_internal_functions_table_exists */
static void ensure_misplaced_internal_functions_table_exists()
{
	if (!RUNKIT_G(misplaced_internal_functions)) {
		ALLOC_HASHTABLE(RUNKIT_G(misplaced_internal_functions));
		zend_hash_init(RUNKIT_G(misplaced_internal_functions), 4, NULL, NULL, 0);
	}
}
/* }}} */

/* {{{record_misplaced_internal_function */
static void record_misplaced_internal_function(zend_string *fname_lower)
{
	zval tmp;
	ZVAL_STR(&tmp, fname_lower);
	ensure_misplaced_internal_functions_table_exists();
	zend_hash_next_index_insert(RUNKIT_G(misplaced_internal_functions), &tmp);
	// FIXME properly track string reference count.
	zend_string_addref(fname_lower);
}
/* }}} */

/* {{{ proto bool runkit7_function_rename(string funcname, string newname)
 */
PHP_FUNCTION(runkit7_function_rename)
{
	zend_function *func, *sfe;
	zend_bool was_internal_function;
	zend_bool should_invoke_dtor_on_original;
	char orig_dfunc_type;
	ZEND_RESULT_CODE deleted;
	// Check if the destination function already exists, and create strings for function names.
	PHP_RUNKIT_FUNCTION_PARSE_RENAME_COPY_PARAMS;

	if ((sfe = php_runkit_fetch_function(sfunc, PHP_RUNKIT_FETCH_FUNCTION_RENAME)) == NULL) {
		zend_string_release(dfunc_lower);
		RETURN_FALSE;
	}

	sfunc_lower = zend_string_tolower(sfunc);

	php_runkit_remove_function_from_reflection_objects(sfe);
	php_runkit_destroy_misplaced_internal_function(sfe, sfunc_lower);

	// TODO: Should I use clone instead?
	// This may cause errors.
	// TODO: this was calling the destructor of EG(function_table)
	// I want to extract the function, and want this to not happen.
	was_internal_function = RUNKIT_G(replaced_internal_functions)
		&& zend_hash_exists(RUNKIT_G(replaced_internal_functions), dfunc_lower);
	orig_dfunc_type = was_internal_function ? ZEND_INTERNAL_FUNCTION : ZEND_USER_FUNCTION;

	if (sfe->type == ZEND_INTERNAL_FUNCTION) {
		if (orig_dfunc_type == ZEND_INTERNAL_FUNCTION) {
			should_invoke_dtor_on_original = 0;
			func = sfe;
		} else {
			// Copying an alias to a user function, to a real user function.
			if (RUNKIT_IS_ALIAS_FOR_USER_FUNCTION(sfe)) {
				should_invoke_dtor_on_original = 1;
				sfe = RUNKIT_ALIASED_USER_FUNCTION(sfe);
				func = sfe;
				// the outer function sfe_orig (and only the outer function) is freed by zend_hash_del.
			} else {
				// Copy an internal function, to a real user function.
				should_invoke_dtor_on_original = 0;
				// TODO: Make a user function which invokes a "real"(non-runkit-generated) internal function hidden by runkit.
				// Need to
				// eval("function bar(args) { return __runkit_invoke_real(); }")
				// TODO: This is just a stub for now.
				func = php_runkit_function_create_clone_alias_user_function(sfe);
			}
		}
	} else {
		// TODO: Will need to check for user functions that are aliases of internal functions, once that is implemented.
		should_invoke_dtor_on_original = 1;
		func = php_runkit_function_clone(sfe, dfunc, orig_dfunc_type);
	}

	if (should_invoke_dtor_on_original) {
		// Want to call zend_hash_del instead of php_runkit - for ZEND_INTERNAL_FUNCTION, we copied the inner function RUNKIT_ALIASED_USER_FUNCTION, and want to delete the outer.
		deleted = zend_hash_del(EG(function_table), sfunc_lower) == FAILURE;
	} else {
		dtor_func_t orig = EG(function_table)->pDestructor;
		EG(function_table)->pDestructor = NULL;
		deleted = zend_hash_del(EG(function_table), sfunc_lower) == FAILURE;
		EG(function_table)->pDestructor = orig;
	}

	if (deleted) {
		zend_string_release(dfunc_lower);
		zend_string_release(sfunc_lower);
		php_error_docref(NULL, E_WARNING, "Error removing reference to old function name %s()", ZSTR_VAL(sfunc));
		PHP_RUNKIT_FREE_INTERNAL_FUNCTION_NAME(func);
		/* TODO: is this needed?
		ZVAL_FUNC(&tmpVal, &func);
		zend_function_dtor(&tmpVal);
		*/
		RETURN_FALSE;
	}
	zend_string_release(sfunc_lower);

	if (zend_hash_add_ptr(EG(function_table), dfunc_lower, func) == NULL) {
		zend_string_release(dfunc_lower);
		php_error_docref(NULL, E_WARNING, "Unable to add reference to new function name %s()", ZSTR_VAL(dfunc));
		// TODO: Figure out if this is needed?
		PHP_RUNKIT_FREE_INTERNAL_FUNCTION_NAME(func);
		// TODO: I'm not sure if this line will delete the zvals/strings of the original function,
		// which would cause segfaults.
		// cloning might make more sense above.
		if (func->type == ZEND_USER_FUNCTION) {
			zval tmp;
			ZVAL_FUNC(&tmp, func);
			zend_function_dtor(&tmp);
		}
		RETURN_FALSE;
	}
	// Fix issue caused when runkit_function_rename replaces a different, previously used function - https://github.com/runkit7/runkit7/issues/153
	php_runkit_fix_all_hardcoded_stack_sizes(dfunc_lower, func);

	php_runkit_add_to_misplaced_internal_functions(func, dfunc_lower);

	zend_string_release(dfunc_lower);

	php_runkit_clear_all_functions_runtime_cache();

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool runkit7_function_redefine(string funcname, string arglist, string code[, bool return_by_reference=false[, string doc_comment[, string return_type]]])
 *     proto bool runkit7_function_redefine(string funcname, closure code[, string doc_comment])
 */
PHP_FUNCTION(runkit7_function_redefine)
{
	php_runkit_function_add_or_update(INTERNAL_FUNCTION_PARAM_PASSTHRU, HASH_UPDATE);
}
/* }}} */

/* {{{ proto bool runkit7_function_copy(string funcname, string targetname)
 */
PHP_FUNCTION(runkit7_function_copy)
{
	zend_function *fe, *sfe;
	PHP_RUNKIT_FUNCTION_PARSE_RENAME_COPY_PARAMS;

	if ((sfe = php_runkit_fetch_function(sfunc, PHP_RUNKIT_FETCH_FUNCTION_INSPECT)) == NULL) {
		zend_string_release(dfunc_lower);
		RETURN_FALSE;
	}

	sfunc_lower = zend_string_tolower(sfunc);

	if (sfe->type == ZEND_USER_FUNCTION) {
		// Copy a user function to a user function
		fe = php_runkit_function_clone(sfe, dfunc, ZEND_USER_FUNCTION);
	} else {
		record_misplaced_internal_function(dfunc_lower);
		// FIXME copying an internal function to a user function.

		// TODO: Should I copy the new name for backtraces?
		fe = pemalloc(sizeof(zend_internal_function), 1);
		memcpy(fe, sfe, sizeof(zend_internal_function));
		// FIXME reference management of function names (need to decrement as well)
		// fe->common.function_name = dfunc;
		zend_string_addref(fe->common.function_name);
		zend_string_addref(fe->common.function_name);
	}
	php_runkit_add_to_misplaced_internal_functions(fe, dfunc_lower);

	// TODO: Does this even make sense to add?

	if (zend_hash_add_ptr(EG(function_table), dfunc_lower, fe) == NULL) {
		zend_string_release(dfunc_lower);
		zend_string_release(sfunc_lower);
		php_error_docref(NULL, E_WARNING, "Unable to add reference to new function name %s()", ZSTR_VAL(dfunc));
		// TODO: figure out if this is necessary
		PHP_RUNKIT_FREE_INTERNAL_FUNCTION_NAME(fe);
		php_runkit_function_dtor(fe);
		RETURN_FALSE;
	}

	zend_string_release(dfunc_lower);
	zend_string_release(sfunc_lower);

	RETURN_TRUE;
}
/* }}} */
#endif /* PHP_RUNKIT_MANIPULATION */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
