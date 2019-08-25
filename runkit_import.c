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

/* TODO specify and fix the way this deals with private, protected, and public consts. */

#include "runkit.h"
#include "php_runkit_zval.h"

#ifdef PHP_RUNKIT_MANIPULATION
/* {{{ php_runkit_import_functions
 */
static int php_runkit_import_functions(HashTable *function_table,
		                               long flags,
									   zend_bool *clear_cache)
{
	zend_function *fe;
	zend_ulong idx;
	zend_string *key;

	ZEND_HASH_FOREACH_KEY_PTR(function_table, idx, key, fe) {
		zend_function *orig_fe;
		zend_string *new_key;
		zend_bool add_function = 1;
		zend_bool exists = 0;
		zend_bool create_as_internal = 0;

		new_key = fe->common.function_name;

		if (!fe || fe->type != ZEND_USER_FUNCTION) {
			continue;
		}

		if (key != NULL) {
			new_key = key;
			exists = ((orig_fe = zend_hash_find_ptr(EG(function_table), new_key)) != NULL);
		} else {
			exists = ((orig_fe = zend_hash_index_find_ptr(EG(function_table), idx)) != NULL);
		}

		if (exists) {
			create_as_internal = orig_fe->type == ZEND_INTERNAL_FUNCTION;
			if (create_as_internal && !RUNKIT_G(internal_override)) {
				php_error_docref(NULL, E_WARNING, "%s() is an internal function and runkit.internal_override is disabled", ZSTR_VAL(new_key));
				continue;
			}
			php_runkit_remove_function_from_reflection_objects(orig_fe);
			if (flags & PHP_RUNKIT_IMPORT_OVERRIDE) {
				if (key != NULL) {
					if (zend_hash_del(EG(function_table), new_key) == FAILURE) {
						php_error_docref(NULL, E_WARNING, "Inconsistency cleaning up import environment");
						return FAILURE;
					}
				} else {
					if (zend_hash_index_del(EG(function_table), idx) == FAILURE) {
						php_error_docref(NULL, E_WARNING, "Inconsistency cleaning up import environment");
						return FAILURE;
					}
				}
				*clear_cache = 1;
			} else {
				add_function = 0;
			}
		}

		if (add_function) {
			zend_function *add_fe = fe;
			if (create_as_internal) {
				add_fe = php_runkit_function_clone(fe, new_key, ZEND_INTERNAL_FUNCTION);
			}
			if (zend_hash_add_ptr(EG(function_table), new_key, add_fe) == NULL) {
				php_error_docref(NULL, E_WARNING, "Failure importing %s()", ZSTR_VAL(add_fe->common.function_name));
				if (add_fe != fe) {
					PHP_RUNKIT_DESTROY_FUNCTION(add_fe);
				}
				PHP_RUNKIT_DESTROY_FUNCTION(fe);
				return FAILURE;
			} else {
				PHP_RUNKIT_FUNCTION_ADD_REF(fe);
				if (add_fe != fe) {
					PHP_RUNKIT_FUNCTION_ADD_REF(add_fe);
				}
			}
		}
	} ZEND_HASH_FOREACH_END();

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_import_class_methods
 */
static int php_runkit_import_class_methods(zend_class_entry *dce, zend_class_entry *ce, int override
                                           , zend_bool *clear_cache)
{
	zend_function *fe;

	ZEND_HASH_FOREACH_PTR(&ce->function_table, fe) {
		zend_function *dfe = NULL;
		zend_class_entry *fe_scope;
		/* TODO: check for memory leaks */
		zend_string * const fn = zend_string_tolower(fe->common.function_name);  // TODO: copy?

		fe_scope = php_runkit_locate_scope(ce, fe, fn);

		if (fe_scope != ce) {
			zend_string_release(fn);
			/* This is an inherited function, let's skip it */
			continue;
		}

		dfe = zend_hash_find_ptr(&dce->function_table, fn);
		if (dfe != NULL) {
			zend_class_entry *scope;
			if (!override) {
				php_error_docref(NULL, E_NOTICE, "%s::%s() already exists, not importing", ZSTR_VAL(dce->name), ZSTR_VAL(fe->common.function_name));
				zend_string_release(fn);
				continue;
			}

			scope = php_runkit_locate_scope(dce, dfe, fn);

			if (php_runkit_check_call_stack(&dfe->op_array) == FAILURE) {
				php_error_docref(NULL, E_WARNING, "Cannot override active method %s::%s(). Skipping.", ZSTR_VAL(dce->name), ZSTR_VAL(fe->common.function_name));
				zend_string_release(fn);
				continue;
			}

			*clear_cache = 1;

			php_runkit_clean_children_methods_foreach(EG(class_table), scope, dce, fn, dfe);
			php_runkit_remove_function_from_reflection_objects(dfe);
			if (zend_hash_del(&dce->function_table, fn) == FAILURE) {
				php_error_docref(NULL, E_WARNING, "Error removing old method in destination class %s::%s", ZSTR_VAL(dce->name), ZSTR_VAL(fe->common.function_name));
				zend_string_release(fn);
				continue;
			}
		}

		fe->common.scope = dce;
		if (zend_hash_add_ptr(&dce->function_table, fn, fe) == NULL) {
			php_error_docref(NULL, E_WARNING, "Failure importing %s::%s()", ZSTR_VAL(ce->name), ZSTR_VAL(fe->common.function_name));
			zend_string_release(fn);
			continue;
		} else {
			PHP_RUNKIT_FUNCTION_ADD_REF(fe);
		}

		if ((fe = zend_hash_find_ptr(&dce->function_table, fn)) == NULL) {
			php_error_docref(NULL, E_WARNING, "Cannot get newly created method %s::%s()", ZSTR_VAL(ce->name), ZSTR_VAL(fn));
			zend_string_release(fn);
			continue;
		}
		PHP_RUNKIT_ADD_MAGIC_METHOD(dce, fn, fe, dfe);
		php_runkit_update_children_methods_foreach(EG(class_table),
		                               dce, dce, fe, fn, dfe);

		zend_string_release(fn);
	} ZEND_HASH_FOREACH_END();

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_import_class_consts
 */
static int php_runkit_import_class_consts(zend_class_entry *dce, zend_class_entry *ce, int override)
{
	zend_string *key;
	zend_class_constant *c;
	zval *c_zval;

	ZEND_HASH_FOREACH_STR_KEY_PTR(&ce->constants_table, key, c) { /* } */
		long action = HASH_ADD;
		zend_long access_type;
		c_zval = &c->value;

		if (key == NULL) {
			php_error_docref(NULL, E_WARNING, "Constant has invalid key name");
			continue;
		}
		if (zend_hash_exists(&dce->constants_table, key)) {
			if (override) {
				action = HASH_UPDATE;
			} else {
				php_error_docref(NULL, E_NOTICE, "%s::%s already exists, not importing", ZSTR_VAL(dce->name), ZSTR_VAL(key));
				continue;
			}
		}
		access_type = Z_ACCESS_FLAGS(*c_zval);

		php_runkit_zval_resolve_class_constant(c_zval, dce);
		Z_TRY_ADDREF_P(c_zval);

		// TODO: Does this need to create copies of the zend_class_constant?
		if (runkit_zend_hash_add_or_update_ptr(&dce->constants_table, key, c, action) == NULL) { /* } */
			Z_TRY_DELREF_P(c_zval);
			php_error_docref(NULL, E_WARNING, "Unable to import %s::%s", ZSTR_VAL(dce->name), ZSTR_VAL(key));
		}

		php_runkit_update_children_consts_foreach(EG(class_table), dce, c_zval, key, access_type);
	} ZEND_HASH_FOREACH_END();
	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_import_class_static_props
 */
static int php_runkit_import_class_static_props(zend_class_entry *dce, zend_class_entry *ce, const int override, int remove_from_objects)
{
	zend_string *key;
	zend_property_info *property_info_ptr;


	ZEND_HASH_FOREACH_STR_KEY_PTR(&ce->properties_info, key, property_info_ptr) {
		zval *p;
		zend_property_info *ex_property_info_ptr;
		if (key == NULL) {
			php_error_docref(NULL, E_WARNING, "Property has invalid key name");
			continue;
		}
		if (!(property_info_ptr->flags & ZEND_ACC_STATIC)) {
			continue;
		}
		p = &CE_STATIC_MEMBERS(ce)[property_info_ptr->offset];
		if ((ex_property_info_ptr = zend_hash_find_ptr(&dce->properties_info, key)) != NULL) {
			if (override) {
				if (php_runkit_def_prop_remove_int(dce, key, NULL, 0, 0, NULL) != SUCCESS) {
					php_error_docref(NULL, E_WARNING, "Unable to import %s::$%s (cannot remove old member)", ZSTR_VAL(dce->name), ZSTR_VAL(key));
					continue;
				}
				php_runkit_zval_resolve_class_constant(p, dce);
				if (php_runkit_def_prop_add_int(dce, key, p, property_info_ptr->flags,
												property_info_ptr->doc_comment, dce,
												override, remove_from_objects) != SUCCESS) {
					php_error_docref(NULL, E_WARNING, "Unable to import %s::$%s (cannot add new member)", ZSTR_VAL(dce->name), ZSTR_VAL(key));
					continue;
				}
				php_error_docref(NULL, E_WARNING, "Attempting to override definition of %s::$%s , runkit doesn't support that.", ZSTR_VAL(dce->name), ZSTR_VAL(key));
				continue;
			} else {
				php_error_docref(NULL, E_NOTICE, "%s::$%s already exists, not importing", ZSTR_VAL(dce->name), ZSTR_VAL(key));
				continue;
			}
		} else {
			if (php_runkit_def_prop_add_int(dce, key, p, property_info_ptr->flags,
											property_info_ptr->doc_comment,
											dce,
											override, remove_from_objects) != SUCCESS) {
				php_error_docref(NULL, E_WARNING, "Unable to import %s::$%s (cannot add new member)", ZSTR_VAL(dce->name), ZSTR_VAL(key));
				continue;
			}
		}
	} ZEND_HASH_FOREACH_END();
	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_import_class_props
 */
static int php_runkit_import_class_props(zend_class_entry *dce, zend_class_entry *ce, int override, int remove_from_objects)
{
	zend_string *key;

	zend_property_info *property_info_ptr;

	ZEND_HASH_FOREACH_STR_KEY_PTR(&ce->properties_info, key, property_info_ptr) {
		zval *p;
		// TODO: Check if this was a property_info_ptr

		// Ignore non-string keys
		if (key == NULL) {
			continue;
		}

		if (property_info_ptr->flags & ZEND_ACC_STATIC) {
			continue;
		}
		if (zend_hash_exists(&dce->properties_info, key)) {
			if (!override) {
				php_error_docref(NULL, E_NOTICE, "%s%s%s already exists, not importing",
								 ZSTR_VAL(dce->name), (property_info_ptr->flags & ZEND_ACC_STATIC) ? "::$" : "->", ZSTR_VAL(key));
				continue;
			}
		}
		p = &ce->default_properties_table[property_info_ptr->offset];
		// TODO: what does this do?
		// TODO: Figure out if this is the correct replacement - copied from add_class_vars
		// if ((p_src = zend_hash_find(&ce->default_properties, property_info_ptr->name)) == NULL)
		if (Z_TYPE_P(p) == IS_UNDEF) {
			php_error_docref(NULL, E_WARNING,
							 "Cannot import broken default property %s->%s", ZSTR_VAL(ce->name), ZSTR_VAL(key));
			continue;
		}

		// TODO: Fix constant check
		php_runkit_zval_resolve_class_constant(p, dce);
		php_runkit_def_prop_add_int(dce, key, p,
									property_info_ptr->flags, property_info_ptr->doc_comment,
									dce, override, remove_from_objects);
	} ZEND_HASH_FOREACH_END();

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_import_classes
 */
static int php_runkit_import_classes(HashTable *class_table, const long flags
                                     , zend_bool *clear_cache)
{
	zval *ce_zv;
	zend_string *key;
	HashTable processed_class_names;

	zend_hash_init(&processed_class_names, zend_hash_num_elements(class_table), NULL, NULL, 0);
	ZEND_HASH_FOREACH_STR_KEY_VAL(class_table, key, ce_zv) {
		zend_class_entry *ce = NULL;
		zend_class_entry *dce;

		if (Z_TYPE_P(ce_zv) != IS_PTR) {
			php_error_docref(NULL, E_ERROR, "Non-class in class table!");
			return FAILURE;
		}
		ce = Z_PTR_P(ce_zv);
		if (ce == NULL) {
			php_error_docref(NULL, E_ERROR, "Null class in class table!");
		}

		if (ce->type != ZEND_USER_CLASS) {
			if (ce->type == ZEND_INTERNAL_CLASS) {
				php_error_docref(NULL, E_ERROR, "Cannot import internal class!");
			} else {
				php_error_docref(NULL, E_ERROR, "Corrupt class table?");
			}
			return FAILURE;
		}

		if (key != NULL) {
			zend_string *classname = ce->name;
			zend_string *classname_lower;
			if ((zend_hash_find_ptr(&processed_class_names, classname)) != NULL) {
				// Only process a given classname once
				// (There are multiple redundant entries. One value of `key` is "\0filename\0...function", another is the plain classname)
				// Could also use `ce` as the unique key.
				continue;
			}
			zend_hash_add_ptr(&processed_class_names, classname, ce);
			classname_lower = zend_string_tolower(classname);

			if (!zend_hash_exists(EG(class_table), classname_lower)) {
				php_runkit_class_copy(ce, ce->name);
			}
			zend_string_release(classname_lower);

			if ((dce = php_runkit_fetch_class(ce->name)) == NULL) {
				/* Oddly non-existent target class or error retrieving it... Or it's an internal class... */
				php_error_docref(NULL, E_WARNING, "Cannot redeclare class %s", ZSTR_VAL(ce->name));
				continue;
			}

			if (flags & PHP_RUNKIT_IMPORT_CLASS_CONSTS) {
				php_runkit_import_class_consts(dce, ce, (flags & PHP_RUNKIT_IMPORT_OVERRIDE));
			}
			if (flags & PHP_RUNKIT_IMPORT_CLASS_STATIC_PROPS) {
				php_runkit_import_class_static_props(dce, ce, (flags & PHP_RUNKIT_IMPORT_OVERRIDE) != 0,
				                                     (flags & PHP_RUNKIT_OVERRIDE_OBJECTS) != 0);
			}

			if (flags & PHP_RUNKIT_IMPORT_CLASS_PROPS) {
				php_runkit_import_class_props(dce, ce, (flags & PHP_RUNKIT_IMPORT_OVERRIDE) != 0,
				                              (flags & PHP_RUNKIT_OVERRIDE_OBJECTS) != 0);
			}

			if (flags & PHP_RUNKIT_IMPORT_CLASS_METHODS) {
				php_runkit_import_class_methods(dce, ce, flags & (PHP_RUNKIT_IMPORT_OVERRIDE)
				                                , clear_cache);
			}
		}
	} ZEND_HASH_FOREACH_END();
	zend_hash_destroy(&processed_class_names);

	// TODO: blindly assuming that temporary classes are cleaned up properly in zend_hash_destroy.
	// See if this is wrong.

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_compile_filename
 * Duplicate of Zend's compile_filename which explicitly calls the internal compile_file() implementation.
 *
 * This is only used when an accelerator has replaced zend_compile_file() with an alternate method
 * which has been known to cause issues with overly-optimistic early binding.
 *
 * It would be cleaner to temporarily set zend_compile_file back to compile_file, but that wouldn't be
 * particularly thread-safe so.... */
static zend_op_array *php_runkit_compile_filename(int type, zval *filename)
{
	zend_file_handle file_handle;
	zval tmp;
	zend_op_array *retval;
	zend_string *opened_path = NULL;

	if (Z_TYPE_P(filename) != IS_STRING) {
		tmp = *filename;
		zval_copy_ctor(&tmp);
		convert_to_string(&tmp);
		filename = &tmp;
	}
	file_handle.filename = filename->value.str->val;
	file_handle.free_filename = 0;
	file_handle.type = ZEND_HANDLE_FILENAME;
	file_handle.opened_path = NULL;
	file_handle.handle.fp = NULL;

	/* Use builtin compiler only -- bypass accelerators and whatnot */
	retval = compile_file(&file_handle, type);
	if (retval && file_handle.handle.stream.handle) {
		// TODO: Duplicate instead?
		if (!file_handle.opened_path) {
			file_handle.opened_path = opened_path = zend_string_copy(Z_STR_P(filename));
		}

		zend_hash_add_empty_element(&EG(included_files), file_handle.opened_path);

		if (opened_path) {
			zend_string_release(opened_path);
		}
	}
	zend_destroy_file_handle(&file_handle);

	if (filename == &tmp) {
		zval_dtor(&tmp);
	}
	return retval;
}
/* }}} */

static HashTable *current_class_table, *tmp_class_table, *tmp_eg_class_table, *current_eg_class_table, *current_function_table, *tmp_function_table;
static uint32_t php_runkit_old_compiler_options;
/* void (*php_runkit_old_error_cb)(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args); */

#if PHP_VERSION_ID >= 70300
#define HAS_EARLY_BINDING(op_array) ((op_array)->fn_flags & ZEND_ACC_EARLY_BINDING)
#else
#define HAS_EARLY_BINDING(op_array) ((op_array)->early_binding != (uint32_t)-1)
#endif

/* {{{ php_runkit_error_cb */
/* Disabled because php deprecation notices were causing EG(class_table) to be restored too early */
/*
void php_runkit_error_cb(int type, const char *error_filename, const uint error_lineno, const char *format, va_list args) {
	zend_error_cb = php_runkit_old_error_cb;
	CG(class_table) = current_class_table;
	EG(class_table) = current_eg_class_table;
	CG(function_table) = current_function_table;
	CG(compiler_options) = php_runkit_old_compiler_options;

	php_runkit_old_error_cb(type, error_filename, error_lineno, format, args);
}
*/
/* }}} */

// Handle change to zend_build_delayed_early_binding_list in php 7.4
#if PHP_VERSION_ID >= 70400
#ifndef ZEND_DECLARE_INHERITED_CLASS_DELAYED
#define ZEND_DECLARE_INHERITED_CLASS_DELAYED ZEND_DECLARE_CLASS_DELAYED
#endif
#endif

uint32_t compute_early_binding_opline_num(const zend_op_array *op_array) /* {{{ */
{
#if PHP_VERSION_ID < 70300
	return op_array->early_binding;
#else
	// Copied from zend_build_delayed_early_binding_list in Zend/zend_compile.c
	// TODO: Locate documentation of change and copy that
	uint32_t  first_early_binding_opline = (uint32_t)-1;
	uint32_t *prev_opline_num = &first_early_binding_opline;
	zend_op  *opline = op_array->opcodes;
	zend_op  *end = opline + op_array->last;

	while (opline < end) {
		if (opline->opcode == ZEND_DECLARE_INHERITED_CLASS_DELAYED) {
			*prev_opline_num = opline - op_array->opcodes;
			prev_opline_num = &opline->result.opline_num;
		}
		++opline;
	}
	*prev_opline_num = -1;
	return first_early_binding_opline;
#endif
}
/* }}} */

/* {{{ array runkit7_import(string filename[, long flags])
	Import functions and class definitions from a file
	Similar to include(), but doesn't execute root op_array, and allows pre-existing functions/methods to be overridden */
PHP_FUNCTION(runkit7_import)
{
	zend_op_array *new_op_array;
	zval *filename;
	zend_long flags = PHP_RUNKIT_IMPORT_CLASS_METHODS;
	zend_bool clear_cache = 0;

	zend_op_array *(*local_compile_filename)(int type, zval *filename) = compile_filename;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "z|l", &filename, &flags) == FAILURE) {
		RETURN_FALSE;
	}
	if (flags & PHP_RUNKIT_IMPORT_OVERRIDE) {
		if (flags & (PHP_RUNKIT_IMPORT_CLASS_PROPS | PHP_RUNKIT_IMPORT_CLASS_STATIC_PROPS)) {
			/* TODO: Investigate if static props are possible. https://github.com/runkit7/runkit7/issues/73 */
			php_error_docref(NULL, E_WARNING, "Using PHP_RUNKIT_IMPORT_OVERRIDE in combination with PHP_RUNKIT_IMPORT_CLASS_PROPS/PHP_RUNKIT_IMPORT_CLASS_STATIC_PROPS is not supported.");
			RETURN_FALSE;
		}
	}
	if (flags & (PHP_RUNKIT_IMPORT_CLASS_PROPS | PHP_RUNKIT_IMPORT_CLASS_STATIC_PROPS)) {
		/* TODO: Investigate if static props are possible. https://github.com/runkit7/runkit7/issues/73 */
		php_error_docref(NULL, E_WARNING, "PHP_RUNKIT_IMPORT_CLASS_PROPS/PHP_RUNKIT_IMPORT_CLASS_STATIC_PROPS are not supported yet, runkit_props.c has not been fully ported.");
		RETURN_FALSE;
	}
	convert_to_string(filename);

	if (compile_file != zend_compile_file) {
		/* An accelerator or other dark force is at work
		 * Use the wrapper method to force the builtin compiler
		 * to be used */
		local_compile_filename = php_runkit_compile_filename;
	}

	tmp_class_table = (HashTable *)emalloc(sizeof(HashTable));
	zend_hash_init_ex(tmp_class_table, 64, NULL, ZEND_CLASS_DTOR, 0, 0);
	tmp_eg_class_table = (HashTable *)emalloc(sizeof(HashTable));
	zend_hash_init_ex(tmp_eg_class_table, 10, NULL, ZEND_CLASS_DTOR, 0, 0);
	tmp_function_table = (HashTable *)emalloc(sizeof(HashTable));
	zend_hash_init_ex(tmp_function_table, 100, NULL, ZEND_FUNCTION_DTOR, 0, 0);

	current_class_table = CG(class_table);
	CG(class_table) = tmp_class_table;
	current_eg_class_table = EG(class_table);
	EG(class_table) = tmp_eg_class_table;
	current_function_table = CG(function_table);
	CG(function_table) = tmp_function_table;
	/* php_runkit_old_error_cb = zend_error_cb; */
	/* zend_error_cb = php_runkit_error_cb; */
	php_runkit_old_compiler_options = CG(compiler_options);
	CG(compiler_options) |= ZEND_COMPILE_DELAYED_BINDING;

	new_op_array = local_compile_filename(ZEND_INCLUDE, filename);

	/* zend_error_cb = php_runkit_old_error_cb; */
	CG(class_table) = current_class_table;
	EG(class_table) = current_eg_class_table;
	CG(function_table) = current_function_table;
	CG(compiler_options) = php_runkit_old_compiler_options;

	if (!new_op_array) {
		zend_hash_destroy(tmp_class_table);
		efree(tmp_class_table);
		zend_hash_destroy(tmp_eg_class_table);
		efree(tmp_eg_class_table);
		zend_hash_destroy(tmp_function_table);
		efree(tmp_function_table);
		if (!EG(exception)) {
			php_error_docref(NULL, E_WARNING, "Import Failure");
		}
		RETURN_FALSE;
	}

	// This is similar to, but not identical to the function zend_do_delayed_early_binding.
	// However, the superclasses will be found in the original class table,
	// but the new classes are found in a separate, temporary class table.
	if (HAS_EARLY_BINDING(new_op_array)) {
		// For emitting errors about inheritance, PHP expects the compiled filename to be set.
		// Allows emitting reasonable error messages for "Cannot make static method X::foo() non static"
		zend_string *const original_filename = zend_get_compiled_filename();

		zend_bool orig_in_compilation = CG(in_compilation);
		uint32_t opline_num = compute_early_binding_opline_num(new_op_array);
		zend_set_compiled_filename(Z_STR_P(filename));

		CG(in_compilation) = 1;
		while (opline_num != (uint32_t)-1) {
			// TODO: Check if it's still op2, figure out the set of expected opcodes
			const zend_op *const parent_name_opcode = &(new_op_array->opcodes[opline_num - 1]);
			zval *parent_name = RUNKIT_RT_CONSTANT(new_op_array, parent_name_opcode, parent_name_opcode->op2);
			zval *key = parent_name + 1;
			zend_class_entry *pce;
			ZEND_ASSERT(Z_TYPE_P(parent_name) == IS_STRING);
#if PHP_VERSION_ID >= 70400
			pce = zend_lookup_class_ex(Z_STR_P(parent_name), Z_STR_P(key), 0);
#else
			pce = zend_lookup_class_ex(Z_STR_P(parent_name), key, 0);
#endif

			// TODO: Check if this is the same in php 7.0
            if (pce != NULL) {
#if PHP_VERSION_ID >= 70400
                do_bind_class(key, Z_STR_P(parent_name));
#else
                do_bind_inherited_class(new_op_array, &new_op_array->opcodes[opline_num], tmp_class_table, pce, 0);
#endif
            }
			opline_num = new_op_array->opcodes[opline_num].result.opline_num;
		}
		CG(in_compilation) = orig_in_compilation;
		zend_restore_compiled_filename(original_filename);
	}

	/* We never really needed the main loop opcodes to begin with */
	destroy_op_array(new_op_array);
	efree(new_op_array);

	if (flags & PHP_RUNKIT_IMPORT_FUNCTIONS) {
		php_runkit_import_functions(tmp_function_table, flags, &clear_cache);
	}

	if (flags & PHP_RUNKIT_IMPORT_CLASSES) {
		php_runkit_import_classes(tmp_class_table, flags, &clear_cache);
	}

	zend_hash_destroy(tmp_class_table);
	efree(tmp_class_table);
	zend_hash_destroy(tmp_eg_class_table);
	efree(tmp_eg_class_table);
	zend_hash_destroy(tmp_function_table);
	efree(tmp_function_table);

	if (clear_cache) {
		php_runkit_clear_all_functions_runtime_cache();
	}

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
