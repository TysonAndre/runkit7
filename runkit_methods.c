/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2006 The PHP Group, (c) 2008-2015 Dmitry Zenovich |
  | (c) 2015-2019 Tyson Andre                                            |
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
// For ZEND_ACC_ALLOW_STATIC
#include "Zend/zend_compile.h"

#ifdef PHP_RUNKIT_MANIPULATION

/* {{{ _php_runkit_get_method_prototype
	Locates the prototype method with name func_lower in the inheritance chain of ce. */
static inline zend_function *_php_runkit_get_method_prototype(zend_class_entry *ce, zend_string *func_lower)
{
	zend_class_entry *pce = ce;
	zend_function *proto = NULL;

	while (pce) {
		if ((proto = zend_hash_find_ptr(&pce->function_table, func_lower)) != NULL) {
			return proto;
		}
		pce = pce->parent;
	}
	return NULL;
}
/* }}} */

/* {{{ php_runkit_fetch_class_int
       Fetches the class entry **without** using the autoloader.
	   This could be any class entry type (interface, user-defined class, internal class, etc.) */
zend_class_entry *php_runkit_fetch_class_int(zend_string *classname)
{
#if PHP_VERSION_ID >= 70400
	return zend_lookup_class_ex(classname, (zend_string *)/* key = */ NULL, /* use_autoload = */ (int)0);
#else
	return zend_lookup_class_ex(classname, (zval *)/* key = */ NULL, /* use_autoload = */ (int)0);
#endif
}
/* }}} */

/* {{{ php_runkit_fetch_class
       Fetches a class entry which can be modified by runkit. Returns NULL if class entry is missing or it is an internal class */
zend_class_entry *php_runkit_fetch_class(zend_string *classname)
{
	zend_class_entry *ce;

	// FIXME why is this creating a string that later gets used in zend_std_cast_object_tostring
	if ((ce = php_runkit_fetch_class_int(classname)) == NULL) {
		return NULL;
	}

	if (ce->type != ZEND_USER_CLASS) {
		php_error_docref(NULL, E_WARNING, "class %s is not a user-defined class", ZSTR_VAL(classname));
		return NULL;
	}

	if (ce->ce_flags & ZEND_ACC_INTERFACE) {
		php_error_docref(NULL, E_WARNING, "class %s is an interface", ZSTR_VAL(classname));
		return NULL;
	}
	// TODO: What about traits? (ZEND_ACC_TRAIT)

	return ce;
}
/* }}} */

/* {{{ php_runkit_fetch_interface
 */
int php_runkit_fetch_interface(zend_string *classname, zend_class_entry **pce)
{
	if ((*pce = php_runkit_fetch_class_int(classname)) == NULL) {
		return FAILURE;
	}

	if (!((*pce)->ce_flags & ZEND_ACC_INTERFACE)) {
		php_error_docref(NULL, E_WARNING, "class %s is not an interface", ZSTR_VAL(classname));
		return FAILURE;
	}

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_fetch_class_method
 */
static int php_runkit_fetch_class_method(zend_string *classname, zend_string *fname, zend_class_entry **pce, zend_function **pfe)
{
	zend_class_entry *ce;
	zend_function *fe;
	zend_string *fname_lower;

	if ((ce = php_runkit_fetch_class_int(classname)) == NULL) {
		return FAILURE;
	}

	if (ce->type != ZEND_USER_CLASS) {
		php_error_docref(NULL, E_WARNING, "class %s is not a user-defined class", ZSTR_VAL(classname));
		return FAILURE;
	}

	if (pce) {
		*pce = ce;
	}

	fname_lower = zend_string_tolower(fname);

	if ((fe = zend_hash_find_ptr(&ce->function_table, fname_lower)) == NULL) {
		php_error_docref(NULL, E_WARNING, "%s::%s() not found", ZSTR_VAL(classname), ZSTR_VAL(fname));
		zend_string_release(fname_lower);
		return FAILURE;
	}

	zend_string_release(fname_lower);
	if (fe->type != ZEND_USER_FUNCTION) {
		php_error_docref(NULL, E_WARNING, "%s::%s() is not a user function", ZSTR_VAL(classname), ZSTR_VAL(fname));
		return FAILURE;
	}

	if (pfe) {
		*pfe = fe;
	}

	return SUCCESS;
}
/* }}} */

/* {{{ php_runkit_update_children_methods_foreach */
void php_runkit_update_children_methods_foreach(HashTable *ht, zend_class_entry *ancestor_class, zend_class_entry *parent_class, zend_function *fe, zend_string *fname_lower, zend_function *orig_fe)
{
	zend_class_entry *ce;
	ZEND_HASH_FOREACH_PTR(ht, ce) {
		php_runkit_update_children_methods(ce, ancestor_class, parent_class, fe, fname_lower, orig_fe);
	} ZEND_HASH_FOREACH_END();
}
/* }}} */


/* {{{ php_runkit_inherit_magic */
inline static void php_runkit_inherit_magic(zend_class_entry *ce, const zend_function *fe, const zend_function *orig_fe)
{
	if ((ce)->__get == (orig_fe) && (ce)->parent->__get == (fe)) {
		(ce)->__get        = (ce)->parent->__get;
		ensure_all_objects_of_class_have_magic_methods(ce);
	} else if ((ce)->__set        == (orig_fe) && (ce)->parent->__set == (fe)) {
		(ce)->__set        = (ce)->parent->__set;
		ensure_all_objects_of_class_have_magic_methods(ce);
	} else if ((ce)->__unset      == (orig_fe) && (ce)->parent->__unset == (fe)) {
		(ce)->__unset      = (ce)->parent->__unset;
		ensure_all_objects_of_class_have_magic_methods(ce);
	} else if ((ce)->__isset      == (orig_fe) && (ce)->parent->__isset == (fe)) {
		(ce)->__isset      = (ce)->parent->__isset;
		ensure_all_objects_of_class_have_magic_methods(ce);
	} else if ((ce)->__call       == (orig_fe) && (ce)->parent->__call == (fe)) {
		(ce)->__call       = (ce)->parent->__call;
	} else if ((ce)->__callstatic == (orig_fe) && (ce)->parent->__callstatic == (fe)) {
		(ce)->__callstatic = (ce)->parent->__callstatic;
	} else if ((ce)->__tostring == (orig_fe) && (ce)->parent->__tostring == (fe)) {
		(ce)->__tostring   = (ce)->parent->__tostring;
	} else if ((ce)->clone        == (orig_fe) && (ce)->parent->clone == (fe)) {
		(ce)->clone        = (ce)->parent->clone;
	} else if ((ce)->destructor   == (orig_fe) && (ce)->parent->destructor == (fe)) {
		(ce)->destructor   = (ce)->parent->destructor;
	} else if ((ce)->constructor  == (orig_fe) && (ce)->parent->constructor == (fe)) {
		(ce)->constructor  = (ce)->parent->constructor;
	} else if ((ce)->__debugInfo  == (orig_fe) && (ce)->parent->__debugInfo == (fe)) {
		(ce)->__debugInfo  = (ce)->parent->__debugInfo;
	} else if (instanceof_function_ex(ce, zend_ce_serializable, 1) &&
		   (ce)->serialize_func == (orig_fe) && (ce)->parent->serialize_func == (fe)) {
		(ce)->serialize_func = (ce)->parent->serialize_func;
	} else if (instanceof_function_ex(ce, zend_ce_serializable, 1) &&
		   (ce)->unserialize_func == (orig_fe) && (ce)->parent->unserialize_func == (fe)) {
		(ce)->unserialize_func = (ce)->parent->unserialize_func;
	}
}
/* }}} */

/* {{{ php_runkit_update_children_methods
	Scan the class_table for children of the class just updated */
void php_runkit_update_children_methods(zend_class_entry *ce, zend_class_entry *ancestor_class, zend_class_entry *parent_class, zend_function *fe, zend_string *fname_lower, zend_function *orig_fe)
{

	// TODO: This will probably segfault. Is there a type safe version of this?
	zend_class_entry *scope;
	zend_function *cfe = NULL;

	if (ce->parent != parent_class) {
		/* Not a child, ignore */
		return;
	}

	if ((cfe = zend_hash_find_ptr(&ce->function_table, fname_lower)) != NULL) {
		// TODO: Abstract this into a helper method.
		scope = php_runkit_locate_scope(ce, cfe, fname_lower);
		if (scope != ancestor_class) {
			/* This method was defined below our current level, leave it be */
			cfe->common.prototype = _php_runkit_get_method_prototype(scope->parent, fname_lower);
			php_runkit_update_children_methods_foreach(EG(class_table),
						ancestor_class, ce, fe, fname_lower, orig_fe);
			return;
		}
	}

	if (cfe) {
		php_runkit_remove_function_from_reflection_objects(cfe);
		if (zend_hash_del(&ce->function_table, fname_lower) == FAILURE) {
			php_error_docref(NULL, E_WARNING, "Error updating child class");
			return;
		}
	}

	// TODO: memcpy?
	if (zend_hash_add_ptr(&ce->function_table, fname_lower, fe) == NULL) {
		php_error_docref(NULL, E_WARNING, "Error updating child class");
		return;
	}
	PHP_RUNKIT_FUNCTION_ADD_REF(fe);
	php_runkit_inherit_magic(ce, fe, orig_fe);

	/* Process children of this child */
	php_runkit_update_children_methods_foreach(EG(class_table),
				       ancestor_class, ce, fe, fname_lower, orig_fe);

	return;
}
/* }}} */

/* {{{ php_runkit_clean_children_methods_foreach */
void php_runkit_clean_children_methods_foreach(HashTable *ht, zend_class_entry *ancestor_class, zend_class_entry *parent_class, zend_string *fname_lower, zend_function *orig_cfe)
{
	zend_class_entry *ce;
	ZEND_HASH_FOREACH_PTR(ht, ce) {
		php_runkit_clean_children_methods(ce, ancestor_class, parent_class, fname_lower, orig_cfe);
	} ZEND_HASH_FOREACH_END();
}
/* }}} */

/* {{{ php_runkit_clean_children
	Scan the class_table for children of the class just updated */
void php_runkit_clean_children_methods(zend_class_entry *ce, zend_class_entry *ancestor_class, zend_class_entry *parent_class, zend_string *fname_lower, zend_function *orig_cfe)
{
	zend_class_entry *scope;
	zend_function *cfe = NULL;

	if (ce->parent != parent_class) {
		/* Not a child, ignore */
		return;
	}

	if ((cfe = zend_hash_find_ptr(&ce->function_table, fname_lower)) != NULL) {
		scope = php_runkit_locate_scope(ce, cfe, fname_lower);
		if (scope != ancestor_class) {
			/* This method was defined below our current level, leave it be */
			return;
		}
	}

	if (!cfe) {
		/* Odd.... nothing to destroy.... */
		return;
	}

	/* Process children of this child */
	php_runkit_clean_children_methods_foreach(EG(class_table), ancestor_class, ce, fname_lower, orig_cfe);

	php_runkit_remove_function_from_reflection_objects(cfe);

	zend_hash_del(&ce->function_table, fname_lower);

	PHP_RUNKIT_DEL_MAGIC_METHOD(ce, orig_cfe);
}
/* }}} */

/* {{{ php_runkit_method_add_or_update
 */
static void php_runkit_method_add_or_update(INTERNAL_FUNCTION_PARAMETERS, int add_or_update)
{
	zend_string *classname = NULL, *methodname = NULL, *arguments = NULL, *phpcode = NULL, *doc_comment = NULL;
	parsed_return_type return_type;
	parsed_is_strict is_strict;
	zend_class_entry *ce, *ancestor_class = NULL;
	zend_function *func, *fe, *source_fe = NULL, *orig_fe = NULL;
	zend_string *methodname_lower;
	long argc = ZEND_NUM_ARGS();
	long flags = ZEND_ACC_PUBLIC;
	zval *args;
	long opt_arg_pos = 3;
	zend_bool remove_temp = 0;
	debug_printf("php_runkit_method_add_or_update\n");

	if (argc < 2 || zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, 2, "SS", &classname, &methodname) == FAILURE ||
			!ZSTR_LEN(classname) || !ZSTR_LEN(methodname)) {
		php_error_docref(NULL, E_ERROR, "Class name and method name should not be empty");
		RETURN_FALSE;
	}

	if (argc < 3) {
		php_error_docref(NULL, E_ERROR, "Method body should be provided");
		RETURN_FALSE;
	}

	if (!php_runkit_parse_args_to_zvals(argc, &args)) {
		RETURN_FALSE;
	}

	if (!php_runkit_parse_function_arg(argc, args, 2, &source_fe, &arguments, &phpcode, &opt_arg_pos, "Method")) {
		efree(args);
		RETURN_FALSE;
	}

	if (argc > opt_arg_pos) {
		if (Z_TYPE(args[opt_arg_pos]) == IS_NULL || Z_TYPE(args[opt_arg_pos]) == IS_LONG) {
			convert_to_long_ex(&(args[opt_arg_pos]));
			flags = Z_LVAL(args[opt_arg_pos]);
			if (flags & PHP_RUNKIT_ACC_RETURN_REFERENCE && source_fe) {
				php_error_docref(NULL, E_WARNING, "RUNKIT_ACC_RETURN_REFERENCE flag is not applicable for closures, use & in closure definition instead");
			}
		} else {
			php_error_docref(NULL, E_WARNING, "Flags should be a long integer or NULL");
		}
	}

	doc_comment = php_runkit_parse_doc_comment_arg(argc, args, opt_arg_pos + 1);

	return_type = php_runkit_parse_return_type_arg(argc, args, opt_arg_pos + 2);

	is_strict = php_runkit_parse_is_strict_arg(argc, args, opt_arg_pos + 3);

	efree(args);

	if (!return_type.valid) {
		RETURN_FALSE;
	}

	if (source_fe && return_type.return_type) {
		// TODO: Check what needs to be needs to be changed in opcode array if return_type is changed
		php_error_docref(NULL, E_WARNING, "Overriding return_type is not currently supported for closures, use return type in closure definition instead (or pass in code as string)");
		RETURN_FALSE;
	}

	methodname_lower = zend_string_tolower(methodname);

	if (add_or_update == HASH_UPDATE) {
		if (php_runkit_fetch_class_method(classname, methodname_lower, &ce, &fe) == FAILURE) {
			zend_string_release(methodname_lower);
			RETURN_FALSE;
		}
		ancestor_class = php_runkit_locate_scope(ce, fe, methodname_lower);
		orig_fe = fe;

		if (php_runkit_check_call_stack(&fe->op_array) == FAILURE) {
			php_error_docref(NULL, E_WARNING, "Cannot redefine a method while that method is active.");
			zend_string_release(methodname_lower);
			RETURN_FALSE;
		}
	} else {
		if ((ce = php_runkit_fetch_class(classname)) == NULL) {
			zend_string_release(methodname_lower);
			RETURN_FALSE;
		}
		ancestor_class = ce;
		if ((fe = zend_hash_find_ptr(&ce->function_table, methodname_lower)) != NULL) {
			if (php_runkit_locate_scope(ce, fe, methodname_lower) == ce) {
				php_error_docref(NULL, E_WARNING, "%s::%s() already exists", ZSTR_VAL(classname), ZSTR_VAL(methodname));
				zend_string_release(methodname_lower);
				RETURN_FALSE;
			} else {
				php_runkit_remove_function_from_reflection_objects(fe);
				zend_hash_del(&ce->function_table, methodname_lower);
			}
		}
	}

	if (!source_fe) {
		if (php_runkit_generate_lambda_method(arguments, return_type.return_type, is_strict.is_strict, phpcode, &source_fe,
						     (flags & PHP_RUNKIT_ACC_RETURN_REFERENCE) == PHP_RUNKIT_ACC_RETURN_REFERENCE,
							 ((flags & ZEND_ACC_STATIC) != 0)) == FAILURE) {
			zend_string_release(methodname_lower);
			RETURN_FALSE;
		}
		remove_temp = 1;
	}

	func = php_runkit_function_clone(source_fe, methodname, (orig_fe ? orig_fe->type : ZEND_USER_FUNCTION));

	if (flags & ZEND_ACC_PRIVATE) {
		func->common.fn_flags &= ~ZEND_ACC_PPP_MASK;
		func->common.fn_flags |= ZEND_ACC_PRIVATE;
	} else if (flags & ZEND_ACC_PROTECTED) {
		func->common.fn_flags &= ~ZEND_ACC_PPP_MASK;
		func->common.fn_flags |= ZEND_ACC_PROTECTED;
	} else {
		func->common.fn_flags &= ~ZEND_ACC_PPP_MASK;
		func->common.fn_flags |= ZEND_ACC_PUBLIC;
	}
	// Note: If this user-defined function was from a closure,
	// we mark it as not being a closure.
	// This is done to avoid trying to manipulate the zend_function like a closure,
	// e.g. by increasing its reference count (closures do, functions/methods don't).
	func->common.fn_flags &= ~ZEND_ACC_CLOSURE;

	if (flags & ZEND_ACC_STATIC) {
		func->common.fn_flags |= ZEND_ACC_STATIC;
	} else {
#ifdef ZEND_ACC_ALLOW_STATIC
		// ZEND_ACC_ALLOW_STATIC was removed in php 8.0-dev
		func->common.fn_flags |= ZEND_ACC_ALLOW_STATIC;
#endif ZEND_ACC_ALLOW_STATIC
	}

	if (doc_comment == NULL && source_fe->op_array.doc_comment == NULL &&
	   orig_fe && orig_fe->type == ZEND_USER_FUNCTION && orig_fe->op_array.doc_comment) {
		doc_comment = orig_fe->op_array.doc_comment;
	}
	php_runkit_modify_function_doc_comment(func, doc_comment);

	// TODO: Seeing if this broke it.
	php_runkit_clear_all_functions_runtime_cache();

	if (orig_fe) {
		php_runkit_remove_function_from_reflection_objects(orig_fe);
	}

	if (runkit_zend_hash_add_or_update_ptr(&ce->function_table, methodname_lower, func, add_or_update) == NULL) {
		php_error_docref(NULL, E_WARNING, "Unable to add method to class");
		php_runkit_function_dtor(func);
		zend_string_release(methodname_lower);
		if (remove_temp) {
			php_runkit_cleanup_lambda_method();
		}
		RETURN_FALSE;
	}

	if (remove_temp && php_runkit_cleanup_lambda_method() == FAILURE) {
		zend_string_release(methodname_lower);
		RETURN_FALSE;
	}

	if ((fe = zend_hash_find_ptr(&ce->function_table, methodname_lower)) == NULL) {
		php_error_docref(NULL, E_WARNING, "Unable to locate newly added method");
		zend_string_release(methodname_lower);
		RETURN_FALSE;
	}

	fe->common.scope = ce;
	fe->common.prototype = _php_runkit_get_method_prototype(ce->parent, methodname_lower);

	PHP_RUNKIT_ADD_MAGIC_METHOD(ce, methodname_lower, fe, orig_fe);
	php_runkit_update_children_methods_foreach(EG(class_table), ancestor_class, ce, fe, methodname_lower, orig_fe);

	zend_string_release(methodname_lower);

	RETURN_TRUE;
}
/* }}} */

/* {{{ php_runkit_method_copy
 */
static int php_runkit_method_copy(zend_string *dclass, zend_string *dfunc, zend_string *sclass, zend_string *sfunc)
{
	zend_class_entry *dce, *sce;
	zend_function *sfe, *dfe;
	zend_string *dfunc_lower;
	zend_function *fe;

	if (php_runkit_fetch_class_method(sclass, sfunc, &sce, &sfe) == FAILURE) {
		return FAILURE;
	}

	if ((dce = php_runkit_fetch_class(dclass)) == NULL) {
		return FAILURE;
	}

	dfunc_lower = zend_string_tolower(dfunc);

	if ((fe = zend_hash_find_ptr(&dce->function_table, dfunc_lower)) != NULL) {
		if (php_runkit_locate_scope(dce, fe, dfunc_lower) == dce) {
			php_error_docref(NULL, E_WARNING, "Destination method %s::%s() already exists", ZSTR_VAL(dclass), ZSTR_VAL(dfunc));
			zend_string_release(dfunc_lower);
			return FAILURE;
		} else {
			php_runkit_remove_function_from_reflection_objects(fe);
			zend_hash_del(&dce->function_table, dfunc_lower);
			php_runkit_clear_all_functions_runtime_cache();
		}
	}
	/* Postcondition: Method does not already exist in dce's function table (Would return FAILURE if it did) */

	dfe = php_runkit_function_clone(sfe, dfunc, ZEND_USER_FUNCTION);

	// TODO: Check if this is a memory leak.
	if (zend_hash_add_ptr(&dce->function_table, dfunc_lower, dfe) == NULL) {
		php_error_docref(NULL, E_WARNING, "Error adding method to class %s::%s()", ZSTR_VAL(dclass), ZSTR_VAL(dfunc));
		zend_string_release(dfunc_lower);
		efree(dfe);
		return FAILURE;
	}

	dfe->common.scope = dce;
	dfe->common.prototype = _php_runkit_get_method_prototype(dce->parent, dfunc_lower);

	PHP_RUNKIT_ADD_MAGIC_METHOD(dce, dfunc_lower, dfe, NULL);

	php_runkit_update_children_methods_foreach(EG(class_table), dce, dce, dfe, dfunc_lower, NULL);

	zend_string_release(dfunc_lower);
	return SUCCESS;
}
/* }}} */

/* **************
   * Method API *
   ************** */

/* {{{ proto bool runkit7_method_add(string classname, string methodname, string args, string code[, long flags[, string doc_comment[, string return_type]])
       proto bool runkit7_method_add(string classname, string methodname, closure code[, long flags[, string doc_comment]])
       Add a method to an already defined class */
PHP_FUNCTION(runkit7_method_add)
{
	php_runkit_method_add_or_update(INTERNAL_FUNCTION_PARAM_PASSTHRU, HASH_ADD);
}
/* }}} */

/* {{{ proto bool runkit7_method_redefine(string classname, string methodname, string args, string code[, long flags[, string doc_comment, string return_type]]])
       proto bool runkit7_method_redefine(string classname, string methodname, closure code[, long flags[, string doc_comment]])
       Redefine an already defined class method */
PHP_FUNCTION(runkit7_method_redefine)
{
	php_runkit_method_add_or_update(INTERNAL_FUNCTION_PARAM_PASSTHRU, HASH_UPDATE);
}
/* }}} */

/* {{{ proto bool runkit7_method_remove(string classname, string methodname)
	Remove a method from a class definition */
PHP_FUNCTION(runkit7_method_remove)
{
	zend_class_entry *ce, *ancestor_class = NULL;
	zend_function *fe;
	zend_string *methodname;
	zend_string *classname;
	zend_string *methodname_lower;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "SS", &classname, &methodname) == FAILURE) {
		php_error_docref(NULL, E_WARNING, "Can't parse parameters");
		RETURN_FALSE;
	}

	if (!ZSTR_LEN(classname) || !ZSTR_LEN(methodname)) {
		php_error_docref(NULL, E_WARNING, "Empty parameter given");
		RETURN_FALSE;
	}

	if (php_runkit_fetch_class_method(classname, methodname, &ce, &fe) == FAILURE) {
		php_error_docref(NULL, E_WARNING, "Unknown method %s::%s()", ZSTR_VAL(classname), ZSTR_VAL(methodname));
		RETURN_FALSE;
	}

	methodname_lower = zend_string_tolower(methodname);

	ancestor_class = php_runkit_locate_scope(ce, fe, methodname_lower);

	php_runkit_clean_children_methods_foreach(EG(class_table), ancestor_class, ce, methodname_lower, fe);

	php_runkit_clear_all_functions_runtime_cache();

	php_runkit_remove_function_from_reflection_objects(fe);

	if (zend_hash_del(&ce->function_table, methodname_lower) == FAILURE) {
		php_error_docref(NULL, E_WARNING, "Unable to remove method from class");
		zend_string_release(methodname_lower);
		RETURN_FALSE;
	}

	zend_string_release(methodname_lower);
	PHP_RUNKIT_DEL_MAGIC_METHOD(ce, fe);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool runkit7_method_rename(string classname, string methodname, string newname)
	Rename a method within a class */
PHP_FUNCTION(runkit7_method_rename)
{
	zend_class_entry *ce, *ancestor_class = NULL;
	zend_function *fe, *func, *old_fe;
	zend_string *classname;
	zend_string *methodname;
	zend_string *newname;
	zend_string *newname_lower;
	zend_string *methodname_lower;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "SSS",	&classname, &methodname, &newname) == FAILURE) {
		RETURN_FALSE;
	}

	if (!ZSTR_LEN(classname) || !ZSTR_LEN(methodname) || !ZSTR_LEN(newname)) {
		php_error_docref(NULL, E_WARNING, "Empty parameter given");
		RETURN_FALSE;
	}

	if (php_runkit_fetch_class_method(classname, methodname, &ce, &fe) == FAILURE) {
		php_error_docref(NULL, E_WARNING, "Unknown method %s::%s()", ZSTR_VAL(classname), ZSTR_VAL(methodname));
		RETURN_FALSE;
	}

	newname_lower = zend_string_tolower(newname);
	methodname_lower = zend_string_tolower(methodname);

	if ((old_fe = zend_hash_find_ptr(&ce->function_table, newname_lower)) != NULL) {
		if (php_runkit_locate_scope(ce, old_fe, newname_lower) == ce) {
			php_error_docref(NULL, E_WARNING, "%s::%s() already exists", ZSTR_VAL(classname), ZSTR_VAL(methodname));
			zend_string_release(methodname_lower);
			zend_string_release(newname_lower);
			RETURN_FALSE;
		} else {
			php_runkit_remove_function_from_reflection_objects(old_fe);
			zend_hash_del(&ce->function_table, newname_lower);
		}
	}

	ancestor_class = php_runkit_locate_scope(ce, fe, methodname_lower);
	php_runkit_clean_children_methods_foreach(EG(class_table), ancestor_class, ce, methodname_lower, fe);

	php_runkit_clear_all_functions_runtime_cache();

	/* TODO: Figure out how to find the original function type. */
	func = php_runkit_function_clone(fe, newname, fe->type);

	if (zend_hash_add_ptr(&ce->function_table, newname_lower, func) == NULL) {
		zend_string_release(newname_lower);
		zend_string_release(methodname_lower);
		php_error_docref(NULL, E_WARNING, "Unable to add new reference to class method");
		php_runkit_function_dtor(func);
		RETURN_FALSE;
	}

	php_runkit_remove_function_from_reflection_objects(fe);

	if (zend_hash_del(&ce->function_table, methodname_lower) == FAILURE) {
		zend_string_release(newname_lower);
		zend_string_release(methodname_lower);
		php_error_docref(NULL, E_WARNING, "Unable to remove old method reference from class");
		RETURN_FALSE;
	}

	PHP_RUNKIT_DEL_MAGIC_METHOD(ce, fe);

	if (php_runkit_fetch_class_method(classname, newname, &ce, &fe) == FAILURE) {
		zend_string_release(newname_lower);
		zend_string_release(methodname_lower);
		php_error_docref(NULL, E_WARNING, "Unable to locate newly renamed method");
		RETURN_FALSE;
	}

	fe->common.scope = ce;
	fe->common.prototype = _php_runkit_get_method_prototype(ce->parent, newname_lower);

	PHP_RUNKIT_ADD_MAGIC_METHOD(ce, newname_lower, fe, NULL);
	php_runkit_update_children_methods_foreach(EG(class_table), ce, ce, fe, newname_lower, NULL);

	zend_string_release(newname_lower);
	zend_string_release(methodname_lower);

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool runkit7_method_copy(string destclass, string destmethod, string srcclass[, string srcmethod])
	Copy a method from one name to another or from one class to another */
PHP_FUNCTION(runkit7_method_copy)
{
	zend_string *dclass, *dfunc, *sclass, *sfunc = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "SSS|S", &dclass, &dfunc, &sclass, &sfunc) == FAILURE) {
		RETURN_FALSE;
	}

	if (!sfunc) {
		sfunc = dfunc;
	}

	RETURN_BOOL(php_runkit_method_copy(dclass, dfunc, sclass, sfunc) == SUCCESS ? 1 : 0);
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
