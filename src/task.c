/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2018 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Martin Schröder <m.schroeder2007@gmail.com>                 |
  +----------------------------------------------------------------------+
*/


#include "php.h"
#include "zend.h"
#include "zend_API.h"
#include "zend_interfaces.h"
#include "zend_exceptions.h"

#include "php_task.h"

ZEND_DECLARE_MODULE_GLOBALS(task)

zend_class_entry *concurrent_task_ce;

const zend_uchar CONCURRENT_FIBER_TYPE_TASK = 1;

const zend_uchar CONCURRENT_TASK_OPERATION_NONE = 0;
const zend_uchar CONCURRENT_TASK_OPERATION_START = 1;
const zend_uchar CONCURRENT_TASK_OPERATION_RESUME = 2;

static zend_object_handlers concurrent_task_handlers;


void concurrent_task_start(concurrent_task *task)
{
	concurrent_context *context;

	task->operation = CONCURRENT_TASK_OPERATION_NONE;
	task->fiber.context = concurrent_fiber_create_context();

	if (task->fiber.context == NULL) {
		zend_throw_error(NULL, "Failed to create native fiber context");
		return;
	}

	if (!concurrent_fiber_create(task->fiber.context, concurrent_fiber_run, task->fiber.stack_size)) {
		zend_throw_error(NULL, "Failed to create native fiber");
		return;
	}

	task->fiber.stack = (zend_vm_stack) emalloc(CONCURRENT_FIBER_VM_STACK_SIZE);
	task->fiber.stack->top = ZEND_VM_STACK_ELEMENTS(task->fiber.stack) + 1;
	task->fiber.stack->end = (zval *) ((char *) task->fiber.stack + CONCURRENT_FIBER_VM_STACK_SIZE);
	task->fiber.stack->prev = NULL;

	task->fiber.status = CONCURRENT_FIBER_STATUS_RUNNING;

	context = TASK_G(current_context);
	TASK_G(current_context) = task->context;

	if (!concurrent_fiber_switch_to(&task->fiber)) {
		zend_throw_error(NULL, "Failed switching to fiber");
	}

	TASK_G(current_context) = context;

	zend_fcall_info_args_clear(&task->fiber.fci, 1);
}

void concurrent_task_continue(concurrent_task *task)
{
	task->operation = CONCURRENT_TASK_OPERATION_NONE;
	task->fiber.status = CONCURRENT_FIBER_STATUS_RUNNING;

	if (!concurrent_fiber_switch_to(&task->fiber)) {
		zend_throw_error(NULL, "Failed switching to fiber");
	}
}

static void concurrent_task_continuation(void *obj, zval *result, zend_bool success)
{
	concurrent_task *task;

	task = (concurrent_task *) obj;

	ZEND_ASSERT(task->fiber.status == CONCURRENT_FIBER_STATUS_SUSPENDED);

	if (success) {
		if (task->fiber.value != NULL) {
			ZVAL_COPY(task->fiber.value, result);
		}
	} else {
		ZVAL_COPY(&task->error, result);
	}

	task->fiber.value = NULL;

	concurrent_task_scheduler_enqueue(task);

	OBJ_RELEASE(&task->fiber.std);
}

static void concurrent_task_execute_inline(concurrent_task *task, concurrent_task *inner)
{
	concurrent_context *context;
	zend_bool success;

	inner->operation = CONCURRENT_TASK_OPERATION_NONE;

	context = TASK_G(current_context);
	TASK_G(current_context) = inner->context;

	inner->fiber.fci.retval = &inner->result;

	zend_call_function(&inner->fiber.fci, &inner->fiber.fcc);

	zval_ptr_dtor(&inner->fiber.fci.function_name);
	zend_fcall_info_args_clear(&inner->fiber.fci, 1);

	TASK_G(current_context) = context;

	if (UNEXPECTED(EG(exception))) {
		inner->fiber.status = CONCURRENT_FIBER_STATUS_DEAD;

		ZVAL_OBJ(&inner->result, EG(exception));
		EG(exception) = NULL;
	} else {
		inner->fiber.status = CONCURRENT_FIBER_STATUS_FINISHED;

		success = 1;
	}

	concurrent_awaitable_trigger_continuation(&inner->continuation, &inner->result, success);
}

concurrent_task *concurrent_task_object_create()
{
	concurrent_task *task;
	zend_long stack_size;

	task = emalloc(sizeof(concurrent_task));
	ZEND_SECURE_ZERO(task, sizeof(concurrent_task));

	task->fiber.type = CONCURRENT_FIBER_TYPE_TASK;
	task->fiber.status = CONCURRENT_FIBER_STATUS_INIT;

	task->id = TASK_G(counter) + 1;
	TASK_G(counter) = task->id;

	stack_size = TASK_G(stack_size);

	if (stack_size == 0) {
		stack_size = 4096 * (((sizeof(void *)) < 8) ? 16 : 128);
	}

	task->fiber.stack_size = stack_size;

	ZVAL_NULL(&task->result);
	ZVAL_UNDEF(&task->error);

	// The final send value is the task result, pointer will be overwritten and restored during await.
	task->fiber.value = &task->result;

	zend_object_std_init(&task->fiber.std, concurrent_task_ce);
	task->fiber.std.handlers = &concurrent_task_handlers;

	return task;
}

static void concurrent_task_object_destroy(zend_object *object)
{
	concurrent_task *task;

	task = (concurrent_task *) object;

	if (task->fiber.status == CONCURRENT_FIBER_STATUS_SUSPENDED) {
		task->fiber.status = CONCURRENT_FIBER_STATUS_DEAD;

		concurrent_fiber_switch_to(&task->fiber);
	}

	if (task->fiber.status == CONCURRENT_FIBER_STATUS_INIT) {
		zend_fcall_info_args_clear(&task->fiber.fci, 1);

		zval_ptr_dtor(&task->fiber.fci.function_name);
	}

	if (task->continuation != NULL) {
		concurrent_awaitable_dispose_continuation(&task->continuation);
	}

	zval_ptr_dtor(&task->result);
	zval_ptr_dtor(&task->error);

	OBJ_RELEASE(&task->context->std);

	concurrent_fiber_destroy(task->fiber.context);

	zend_object_std_dtor(&task->fiber.std);
}

ZEND_METHOD(Task, __construct)
{
	ZEND_PARSE_PARAMETERS_NONE();

	zend_throw_error(NULL, "Tasks must not be constructed by userland code");
}

ZEND_METHOD(Task, isRunning)
{
	concurrent_fiber *fiber;

	ZEND_PARSE_PARAMETERS_NONE();

	fiber = TASK_G(current_fiber);

	RETURN_BOOL(fiber != NULL && fiber->type == CONCURRENT_FIBER_TYPE_TASK);
}

ZEND_METHOD(Task, async)
{
	concurrent_task * task;

	zval *params;
	zval obj;

	task = concurrent_task_object_create();
	task->scheduler = concurrent_task_scheduler_get();
	task->context = concurrent_context_get();

	ZEND_ASSERT(task->scheduler != NULL);

	params = NULL;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 2)
		Z_PARAM_FUNC_EX(task->fiber.fci, task->fiber.fcc, 1, 0)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY(params)
	ZEND_PARSE_PARAMETERS_END();

	task->fiber.fci.no_separation = 1;

	if (params == NULL) {
		task->fiber.fci.param_count = 0;
	} else {
		zend_fcall_info_args(&task->fiber.fci, params);
	}

	Z_TRY_ADDREF_P(&task->fiber.fci.function_name);

	GC_ADDREF(&task->context->std);

	concurrent_task_scheduler_enqueue(task);

	ZVAL_OBJ(&obj, &task->fiber.std);

	RETURN_ZVAL(&obj, 1, 1);
}

ZEND_METHOD(Task, asyncWithContext)
{
	concurrent_task * task;

	zval *ctx;
	zval *params;
	zval obj;

	task = concurrent_task_object_create();

	params = NULL;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 2, 3)
		Z_PARAM_ZVAL(ctx)
		Z_PARAM_FUNC_EX(task->fiber.fci, task->fiber.fcc, 1, 0)
		Z_PARAM_OPTIONAL
		Z_PARAM_ARRAY(params)
	ZEND_PARSE_PARAMETERS_END();

	task->fiber.fci.no_separation = 1;

	if (params == NULL) {
		task->fiber.fci.param_count = 0;
	} else {
		zend_fcall_info_args(&task->fiber.fci, params);
	}

	Z_TRY_ADDREF_P(&task->fiber.fci.function_name);

	task->scheduler = concurrent_task_scheduler_get();
	task->context = (concurrent_context *) Z_OBJ_P(ctx);

	ZEND_ASSERT(task->scheduler != NULL);

	GC_ADDREF(&task->context->std);

	concurrent_task_scheduler_enqueue(task);

	ZVAL_OBJ(&obj, &task->fiber.std);

	RETURN_ZVAL(&obj, 1, 1);
}

ZEND_METHOD(Task, await)
{
	zend_class_entry *ce;
	concurrent_fiber *fiber;
	concurrent_task *task;
	concurrent_task *inner;
	concurrent_deferred *defer;
	concurrent_context *context;
	size_t stack_page_size;

	zval *val;
	zval *value;
	zval error;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 1)
		Z_PARAM_ZVAL(val)
	ZEND_PARSE_PARAMETERS_END();

	fiber = TASK_G(current_fiber);

	if (fiber == NULL) {
		ce = Z_OBJCE_P(val);

		if (Z_TYPE_P(val) != IS_OBJECT || ce != concurrent_task_ce) {
			zend_throw_error(NULL, "Only tasks can be waited when no scheduler is running");
			return;
		}

		inner = (concurrent_task *) Z_OBJ_P(val);

		ZEND_ASSERT(inner->scheduler != NULL);

		if (inner->scheduler->running) {
			zend_throw_error(NULL, "Cannot dispatch tasks because the dispatcher is already running");
			return;
		}

		concurrent_task_scheduler_run_loop(inner->scheduler);

		if (inner->fiber.status == CONCURRENT_FIBER_STATUS_FINISHED) {
			RETURN_ZVAL(&inner->result, 1, 0);
		}

		if (inner->fiber.status == CONCURRENT_FIBER_STATUS_DEAD) {
			Z_ADDREF_P(&inner->result);

			execute_data->opline--;
			zend_throw_exception_internal(&inner->result);
			execute_data->opline++;

			return;
		}

		zend_throw_error(NULL, "Awaited task did not run to completion");
		return;
	}

	if (fiber->type != CONCURRENT_FIBER_TYPE_TASK) {
		zend_throw_error(NULL, "Await must be called from within a running task");
		return;
	}

	if (UNEXPECTED(fiber->status != CONCURRENT_FIBER_STATUS_RUNNING)) {
		zend_throw_error(NULL, "Cannot await in a task that is not running");
		return;
	}

	task = (concurrent_task *) fiber;

	ZEND_ASSERT(task->scheduler != NULL);

	if (Z_TYPE_P(val) != IS_OBJECT) {
		RETURN_ZVAL(val, 1, 0);
	}

	ce = Z_OBJCE_P(val);

	// Immediately return non-awaitable objects.
	if (instanceof_function_ex(ce, concurrent_awaitable_ce, 1) != 1) {
		RETURN_ZVAL(val, 1, 0);
	}

	if (ce == concurrent_task_ce) {
		inner = (concurrent_task *) Z_OBJ_P(val);

		if (inner->scheduler != task->scheduler) {
			zend_throw_error(NULL, "Cannot await a task that runs on a different task scheduler");
			return;
		}

		if (inner->fiber.status == CONCURRENT_FIBER_STATUS_INIT) {
			if (inner->fiber.stack_size <= task->fiber.stack_size) {
				concurrent_task_execute_inline(task, inner);
			}
		}

		if (inner->fiber.status == CONCURRENT_FIBER_STATUS_FINISHED) {
			RETURN_ZVAL(&inner->result, 1, 0);
		}

		if (inner->fiber.status == CONCURRENT_FIBER_STATUS_DEAD) {
			Z_ADDREF_P(&inner->result);

			execute_data->opline--;
			zend_throw_exception_internal(&inner->result);
			execute_data->opline++;

			return;
		}

		if (inner->continuation == NULL) {
			inner->continuation = concurrent_awaitable_create_continuation(task, concurrent_task_continuation);
		} else {
			concurrent_awaitable_append_continuation(inner->continuation, task, concurrent_task_continuation);
		}
	} else if (ce == concurrent_deferred_awaitable_ce) {
		defer = ((concurrent_deferred_awaitable *) Z_OBJ_P(val))->defer;

		if (defer->status == CONCURRENT_DEFERRED_STATUS_RESOLVED) {
			RETURN_ZVAL(&defer->result, 1, 0);
		}

		if (defer->status == CONCURRENT_DEFERRED_STATUS_FAILED) {
			Z_ADDREF_P(&defer->result);

			execute_data->opline--;
			zend_throw_exception_internal(&defer->result);
			execute_data->opline++;

			return;
		}

		if (defer->continuation == NULL) {
			defer->continuation = concurrent_awaitable_create_continuation(task, concurrent_task_continuation);
		} else {
			concurrent_awaitable_append_continuation(defer->continuation, task, concurrent_task_continuation);
		}
	} else {
		RETURN_ZVAL(val, 1, 0);
	}

	GC_ADDREF(&task->fiber.std);

	// Switch the value pointer to the return value of await() until the task is continued.
	value = task->fiber.value;
	task->fiber.value = USED_RET() ? return_value : NULL;

	task->fiber.status = CONCURRENT_FIBER_STATUS_SUSPENDED;

	context = TASK_G(current_context);

	CONCURRENT_FIBER_BACKUP_EG(task->fiber.stack, stack_page_size, task->fiber.exec);
	concurrent_fiber_yield(task->fiber.context);
	CONCURRENT_FIBER_RESTORE_EG(task->fiber.stack, stack_page_size, task->fiber.exec);

	TASK_G(current_context) = context;

	task->fiber.value = value;

	if (task->fiber.status == CONCURRENT_FIBER_STATUS_DEAD) {
		zend_throw_error(NULL, "Task has been destroyed");
		return;
	}

	if (Z_TYPE_P(&task->error) != IS_UNDEF) {
		error = task->error;
		ZVAL_UNDEF(&task->error);

		execute_data->opline--;
		zend_throw_exception_internal(&error);
		execute_data->opline++;
	}
}

ZEND_METHOD(Task, __wakeup)
{
	ZEND_PARSE_PARAMETERS_NONE();

	zend_throw_error(NULL, "Unserialization of a task is not allowed");
}

ZEND_BEGIN_ARG_INFO(arginfo_task_ctor, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_task_is_running, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_task_async, 0, 1, Concurrent\\Task, 0)
	ZEND_ARG_CALLABLE_INFO(0, callback, 0)
	ZEND_ARG_ARRAY_INFO(0, arguments, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_task_async_with_context, 0, 2, Concurrent\\Task, 0)
	ZEND_ARG_OBJ_INFO(0, context, Concurrent\\Context, 0)
	ZEND_ARG_CALLABLE_INFO(0, callback, 0)
	ZEND_ARG_ARRAY_INFO(0, arguments, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_task_await, 0, 0, 1)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO(arginfo_task_wakeup, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry task_functions[] = {
	ZEND_ME(Task, __construct, arginfo_task_ctor, ZEND_ACC_PRIVATE | ZEND_ACC_CTOR)
	ZEND_ME(Task, isRunning, arginfo_task_is_running, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Task, async, arginfo_task_async, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Task, asyncWithContext, arginfo_task_async_with_context, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Task, await, arginfo_task_await, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Task, __wakeup, arginfo_task_wakeup, ZEND_ACC_PUBLIC)
	ZEND_FE_END
};


void concurrent_task_ce_register()
{
	zend_class_entry ce;

	INIT_CLASS_ENTRY(ce, "Concurrent\\Task", task_functions);
	concurrent_task_ce = zend_register_internal_class(&ce);
	concurrent_task_ce->ce_flags |= ZEND_ACC_FINAL;
	concurrent_task_ce->serialize = zend_class_serialize_deny;
	concurrent_task_ce->unserialize = zend_class_unserialize_deny;

	memcpy(&concurrent_task_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	concurrent_task_handlers.free_obj = concurrent_task_object_destroy;
	concurrent_task_handlers.clone_obj = NULL;

	zend_class_implements(concurrent_task_ce, 1, concurrent_awaitable_ce);
}


/*
 * vim: sw=4 ts=4
 * vim600: fdm=marker
 */
