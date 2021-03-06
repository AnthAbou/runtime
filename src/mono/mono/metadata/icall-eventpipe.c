#include <config.h>
#include <glib.h>
#include <mono/utils/mono-compiler.h>

#ifdef ENABLE_NETCORE
#include <mono/metadata/icall-decl.h>

#ifdef ENABLE_PERFTRACING
#include <mono/eventpipe/ep-rt-config.h>
#include <mono/eventpipe/ep.h>

#include <mono/utils/mono-time.h>
#include <mono/utils/mono-proclib.h>
#include <mono/utils/mono-threads.h>
#include <mono/utils/mono-rand.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/profiler.h>

typedef enum _EventPipeActivityControlCode {
	EP_ACTIVITY_CONTROL_GET_ID = 1,
	EP_ACTIVITY_CONTROL_SET_ID = 2,
	EP_ACTIVITY_CONTROL_CREATE_ID = 3,
	EP_ACTIVITY_CONTROL_GET_SET_ID = 4,
	EP_ACTIVITY_CONTROL_CREATE_SET_ID = 5
} EventPipeActivityControlCode;

typedef struct _EventPipeProviderConfigurationNative {
	gunichar2 *provider_name;
	uint64_t keywords;
	uint32_t logging_level;
	gunichar2 *filter_data;
} EventPipeProviderConfigurationNative;

typedef struct _EventProviderEventData {
	uint64_t ptr;
	uint32_t size;
	uint32_t reserved;
} EventProviderEventData;

typedef struct _EventPipeSessionInfo {
	int64_t starttime_as_utc_filetime;
	int64_t start_timestamp;
	int64_t timestamp_frequency;
} EventPipeSessionInfo;

typedef struct _EventPipeEventInstanceData {
	intptr_t provider_id;
	uint32_t event_id;
	uint32_t thread_id;
	int64_t timestamp;
	uint8_t activity_id [EP_ACTIVITY_ID_SIZE];
	uint8_t related_activity_id [EP_ACTIVITY_ID_SIZE];
	const uint8_t *payload;
	uint32_t payload_len;
} EventPipeEventInstanceData;

gboolean ep_rt_mono_initialized;
MonoNativeTlsKey ep_rt_mono_thread_holder_tls_id;
gpointer ep_rt_mono_rand_provider;

static ep_rt_thread_holder_alloc_func thread_holder_alloc_callback_func;
static ep_rt_thread_holder_free_func thread_holder_free_callback_func;

void
mono_eventpipe_raise_thread_exited (uint64_t);

static
gboolean
rand_try_get_bytes_func (guchar *buffer, gssize buffer_size, MonoError *error)
{
	g_assert (ep_rt_mono_rand_provider != NULL);
	return mono_rand_try_get_bytes (&ep_rt_mono_rand_provider, buffer, buffer_size, error);
}

static
EventPipeThread *
eventpipe_thread_get (void)
{
	EventPipeThreadHolder *thread_holder = mono_native_tls_get_value (ep_rt_mono_thread_holder_tls_id);
	return thread_holder ? ep_thread_holder_get_thread (thread_holder) : NULL;
}

static
EventPipeThread *
eventpipe_thread_get_or_create (void)
{
	EventPipeThreadHolder *thread_holder = (EventPipeThreadHolder *)mono_native_tls_get_value (ep_rt_mono_thread_holder_tls_id);
	if (!thread_holder && thread_holder_alloc_callback_func) {
		thread_holder = thread_holder_alloc_callback_func ();
		mono_native_tls_set_value (ep_rt_mono_thread_holder_tls_id, thread_holder);
	}
	return ep_thread_holder_get_thread (thread_holder);
}

static
void
eventpipe_thread_exited (void)
{
	if (ep_rt_mono_initialized) {
		EventPipeThreadHolder *thread_holder = (EventPipeThreadHolder *)mono_native_tls_get_value (ep_rt_mono_thread_holder_tls_id);
		if (thread_holder && thread_holder_free_callback_func)
			thread_holder_free_callback_func (thread_holder);
		mono_native_tls_set_value (ep_rt_mono_thread_holder_tls_id, NULL);
	}
}

static
void
profiler_eventpipe_thread_exited (MonoProfiler *prof, uintptr_t tid)
{
	eventpipe_thread_exited ();
}

void
mono_eventpipe_init (
	EventPipeMonoFuncTable *table,
	ep_rt_thread_holder_alloc_func thread_holder_alloc_func,
	ep_rt_thread_holder_free_func thread_holder_free_func)
{
	g_assert (table != NULL);
	table->ep_rt_mono_100ns_datetime = mono_100ns_datetime;
	table->ep_rt_mono_100ns_ticks = mono_100ns_ticks;
	table->ep_rt_mono_cpu_count = mono_cpu_count;
	table->ep_rt_mono_process_current_pid = mono_process_current_pid;
	table->ep_rt_mono_native_thread_id_get = mono_native_thread_id_get;
	table->ep_rt_mono_native_thread_id_equals = mono_native_thread_id_equals;
	table->ep_rt_mono_runtime_is_shutting_down = mono_runtime_is_shutting_down;
	table->ep_rt_mono_rand_try_get_bytes = rand_try_get_bytes_func;
	table->ep_rt_mono_thread_get = eventpipe_thread_get;
	table->ep_rt_mono_thread_get_or_create = eventpipe_thread_get_or_create;
	table->ep_rt_mono_thread_exited = eventpipe_thread_exited;
	table->ep_rt_mono_w32file_close = mono_w32file_close;
	table->ep_rt_mono_w32file_create = mono_w32file_create;
	table->ep_rt_mono_w32file_write = mono_w32file_write;

	thread_holder_alloc_callback_func = thread_holder_alloc_func;
	thread_holder_free_callback_func = thread_holder_free_func;
	mono_native_tls_alloc (&ep_rt_mono_thread_holder_tls_id, NULL);

	mono_100ns_ticks ();
	mono_rand_open ();
	ep_rt_mono_rand_provider = mono_rand_init (NULL, 0);

	ep_rt_mono_initialized = TRUE;

	MonoProfilerHandle profiler = mono_profiler_create (NULL);
	mono_profiler_set_thread_stopped_callback (profiler, profiler_eventpipe_thread_exited);
}

void
mono_eventpipe_fini (void)
{
	if (ep_rt_mono_initialized)
		mono_rand_close (ep_rt_mono_rand_provider);

	ep_rt_mono_rand_provider = NULL;
	thread_holder_alloc_callback_func = NULL;
	thread_holder_free_callback_func = NULL;
	ep_rt_mono_initialized = FALSE;
}

gconstpointer
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_CreateProvider (
	MonoStringHandle provider_name,
	MonoDelegateHandle callback_func,
	MonoError *error)
{
	EventPipeProvider *provider = NULL;

	if (MONO_HANDLE_IS_NULL (provider_name)) {
		mono_error_set_argument_null (error, "providerName", "");
		return NULL;
	}

	char *provider_name_utf8 = mono_string_handle_to_utf8 (provider_name, error);

	//TODO: Need to pin delegate if we switch to safe mode or maybe we should get funcptr in icall?
	provider = ep_create_provider (provider_name_utf8, MONO_HANDLE_GETVAL (callback_func, delegate_trampoline), NULL);

	g_free (provider_name_utf8);
	return provider;
}

intptr_t
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_DefineEvent (
	intptr_t provider_handle,
	uint32_t event_id,
	int64_t keywords,
	uint32_t event_version,
	uint32_t level,
	const uint8_t *metadata,
	uint32_t metadata_len)
{
	g_assert (provider_handle != 0);

	EventPipeProvider *provider = (EventPipeProvider *)provider_handle;
	EventPipeEvent *ep_event = ep_provider_add_event (provider, event_id, (uint64_t)keywords, event_version, (EventPipeEventLevel)level, /* needStack = */ true, metadata, metadata_len);

	g_assert (ep_event != NULL);
	return (intptr_t)ep_event;
}

void
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_DeleteProvider (intptr_t provider_handle)
{
	if (provider_handle) {
		ep_delete_provider ((EventPipeProvider *)provider_handle);
	}
}

void
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_Disable (uint64_t session_id)
{
	ep_disable (session_id);
}

uint64_t
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_Enable (
	const gunichar2 *output_file,
	/* EventPipeSerializationFormat */int32_t format,
	uint32_t circular_buffer_size_mb,
	/* EventPipeProviderConfigurationNative[] */const void *providers,
	uint32_t providers_len)
{
	ERROR_DECL (error);
	EventPipeSessionID session_id = 0;

	if (circular_buffer_size_mb == 0 || format > EP_SERIALIZATION_FORMAT_COUNT || providers_len == 0 || providers == NULL)
		return 0;

	char *output_file_utf8 = mono_utf16_to_utf8 (output_file, g_utf16_len (output_file), error);

	session_id = ep_enable (
		output_file_utf8,
		circular_buffer_size_mb,
		providers,
		providers_len,
		output_file != NULL ? EP_SESSION_TYPE_FILE : EP_SESSION_TYPE_LISTENER,
		(EventPipeSerializationFormat)format,
		true,
		NULL,
		true);
	ep_start_streaming (session_id);

	g_free (output_file_utf8);
	return session_id;
}

int32_t
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_EventActivityIdControl (
	uint32_t control_code,
	/* GUID * */uint8_t *activity_id)
{
	int32_t result = 0;
	EventPipeThread *thread = ep_thread_get ();

	if (thread == NULL)
		return 1;

	uint8_t current_activity_id [EP_ACTIVITY_ID_SIZE];
	EventPipeActivityControlCode activity_control_code = (EventPipeActivityControlCode)control_code;
	switch (activity_control_code) {
	case EP_ACTIVITY_CONTROL_GET_ID:
		ep_thread_get_activity_id (thread, activity_id, EP_ACTIVITY_ID_SIZE);
		break;
	case EP_ACTIVITY_CONTROL_SET_ID:
		ep_thread_set_activity_id (thread, activity_id, EP_ACTIVITY_ID_SIZE);
		break;
	case EP_ACTIVITY_CONTROL_CREATE_ID:
		ep_thread_create_activity_id (activity_id, EP_ACTIVITY_ID_SIZE);
		break;
	case EP_ACTIVITY_CONTROL_GET_SET_ID:
		ep_thread_get_activity_id (thread, activity_id, EP_ACTIVITY_ID_SIZE);
		ep_thread_create_activity_id (current_activity_id, G_N_ELEMENTS (current_activity_id));
		ep_thread_set_activity_id (thread, current_activity_id, G_N_ELEMENTS (current_activity_id));
		break;
	default:
		result = 1;
		break;
	}

	return result;
}

MonoBoolean
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_GetNextEvent (
	uint64_t session_id,
	/* EventPipeEventInstanceData * */void *instance)
{
	g_assert (instance != NULL);

	EventPipeEventInstance *const next_instance = ep_get_next_event (session_id);
	EventPipeEventInstanceData *const data = (EventPipeEventInstanceData *)instance;
	if (next_instance && data) {
		const EventPipeEvent *const ep_event = ep_event_instance_get_ep_event (next_instance);
		if (ep_event) {
			data->provider_id = (intptr_t)ep_event_get_provider (ep_event);
			data->event_id = ep_event_get_event_id (ep_event);
		}
		data->thread_id = ep_event_instance_get_thread_id (next_instance);
		data->timestamp = ep_event_instance_get_timestamp (next_instance);
		memcpy (&data->activity_id, ep_event_instance_get_activity_id_cref (next_instance), EP_ACTIVITY_ID_SIZE);
		memcpy (&data->related_activity_id, ep_event_instance_get_related_activity_id_cref (next_instance), EP_ACTIVITY_ID_SIZE);
		data->payload = ep_event_instance_get_data (next_instance);
		data->payload_len = ep_event_instance_get_data_len (next_instance);
	}

	return next_instance != NULL;
}

intptr_t
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_GetProvider (const gunichar2 *provider_name)
{
	ERROR_DECL (error);
	char * provider_name_utf8 = NULL;
	EventPipeProvider *provider = NULL;

	if (provider_name) {
		provider_name_utf8 = mono_utf16_to_utf8 (provider_name, g_utf16_len (provider_name), error);
		provider = ep_get_provider (provider_name_utf8);
	}

	g_free (provider_name_utf8);
	return (intptr_t)provider;
}

MonoBoolean
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_GetSessionInfo (
	uint64_t session_id,
	/* EventPipeSessionInfo * */void *session_info)
{
	bool result = false;
	if (session_info) {
		EventPipeSession *session = ep_get_session ((EventPipeSessionID)session_id);
		if (session) {
			EventPipeSessionInfo *instance = (EventPipeSessionInfo *)session_info;
			instance->starttime_as_utc_filetime = ep_session_get_session_start_time (session);
			instance->start_timestamp = ep_session_get_session_start_timestamp (session);
			instance->timestamp_frequency = ep_perf_frequency_query ();
			result = true;
		}
	}

	return result;
}

intptr_t
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_GetWaitHandle (uint64_t session_id)
{
	return (intptr_t)ep_get_wait_handle (session_id);
}

void
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_WriteEventData (
	intptr_t event_handle,
	/* EventProviderEventData[] */const void *event_data,
	uint32_t data_len,
	/* GUID * */const uint8_t *activity_id,
	/* GUID * */const uint8_t *related_activity_id)
{
	;
}

#else /* ENABLE_PERFTRACING */

gconstpointer
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_CreateProvider (
	MonoStringHandle provider_name,
	MonoDelegateHandle callback_func,
	MonoError *error)
{
	mono_error_set_not_implemented (error, "System.Diagnostics.Tracing.EventPipeInternal.CreateProvider");
	return NULL;
}

intptr_t
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_DefineEvent (
	intptr_t provider_handle,
	uint32_t event_id,
	int64_t keywords,
	uint32_t event_version,
	uint32_t level,
	const uint8_t *metadata,
	uint32_t metadata_len)
{
	ERROR_DECL (error);
	mono_error_set_not_implemented (error, "System.Diagnostics.Tracing.EventPipeInternal.DefineEvent");
	mono_error_set_pending_exception (error);
	return 0;
}

void
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_DeleteProvider (intptr_t provider_handle)
{
	ERROR_DECL (error);
	mono_error_set_not_implemented (error, "System.Diagnostics.Tracing.EventPipeInternal.DeleteProvider");
	mono_error_set_pending_exception (error);
}

void
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_Disable (uint64_t session_id)
{
	ERROR_DECL (error);
	mono_error_set_not_implemented (error, "System.Diagnostics.Tracing.EventPipeInternal.Disable");
	mono_error_set_pending_exception (error);
}

uint64_t
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_Enable (
	const gunichar2 *output_file,
	/* EventPipeSerializationFormat */int32_t format,
	uint32_t circular_buffer_size_mb,
	/* EventPipeProviderConfigurationNative[] */const void *providers,
	uint32_t providers_len)
{
	ERROR_DECL (error);
	mono_error_set_not_implemented (error, "System.Diagnostics.Tracing.EventPipeInternal.Enable");
	mono_error_set_pending_exception (error);
	return 0;
}

int32_t
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_EventActivityIdControl (
	uint32_t control_code,
	/* GUID * */uint8_t *activity_id)
{
	ERROR_DECL (error);
	mono_error_set_not_implemented (error, "System.Diagnostics.Tracing.EventPipeInternal.EventActivityIdControl");
	mono_error_set_pending_exception (error);
	return 0;
}

MonoBoolean
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_GetNextEvent (
	uint64_t session_id,
	/* EventPipeEventInstanceData * */void *instance)
{
	ERROR_DECL (error);
	mono_error_set_not_implemented (error, "System.Diagnostics.Tracing.EventPipeInternal.GetNextEvent");
	mono_error_set_pending_exception (error);
	return FALSE;
}

intptr_t
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_GetProvider (const gunichar2 *provider_name)
{
	ERROR_DECL (error);
	mono_error_set_not_implemented (error, "System.Diagnostics.Tracing.EventPipeInternal.GetProvider");
	mono_error_set_pending_exception (error);
	return 0;
}

MonoBoolean
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_GetSessionInfo (
	uint64_t session_id,
	/* EventPipeSessionInfo * */void *session_info)
{
	ERROR_DECL (error);
	mono_error_set_not_implemented (error, "System.Diagnostics.Tracing.EventPipeInternal.GetSessionInfo");
	mono_error_set_pending_exception (error);
	return FALSE;
}

intptr_t
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_GetWaitHandle (uint64_t session_id)
{
	ERROR_DECL (error);
	mono_error_set_not_implemented (error, "System.Diagnostics.Tracing.EventPipeInternal.GetWaitHandle");
	mono_error_set_pending_exception (error);
	return 0;
}

void
ves_icall_System_Diagnostics_Tracing_EventPipeInternal_WriteEventData (
	intptr_t event_handle,
	/* EventProviderEventData[] */const void *event_data,
	uint32_t data_len,
	/* GUID * */const uint8_t *activity_id,
	/* GUID * */const uint8_t *related_activity_id)
{
	ERROR_DECL (error);
	mono_error_set_not_implemented (error, "System.Diagnostics.Tracing.EventPipeInternal.WriteEventData");
	mono_error_set_pending_exception (error);
}

#endif /* ENABLE_PERFTRACING */
#endif /* ENABLE_NETCORE */

MONO_EMPTY_SOURCE_FILE (eventpipe_rt_mono);
