/******************************************************************************
 Copyright (C) 2014 by John R. Bradley <jrb@turrettech.com>
 Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#include <obs-frontend-api.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/util.hpp>
#include <util/dstr.hpp>
#include <obs-module.h>
#include <obs.hpp>
#include <functional>
#include <sstream>
#include <thread>
#include <mutex>
#include <nlohmann/json.hpp>
#include <obs-websocket-api.h>

#include "spt-notification-source.hpp"
#include "notification-scheme.hpp"
#include "notification-app.hpp"
#include "notification-version.h"

#include "cef-headers.hpp"

#ifdef _WIN32
#include <util/windows/ComPtr.hpp>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#else
#include "signal-restore.hpp"
#endif

#ifdef ENABLE_WAYLAND
#include <obs-nix-platform.h>
#endif

#ifdef ENABLE_NOTIFICATION_QT_LOOP
#include <QApplication>
#include <QThread>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("spt-notification", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "CEF-based web notification source & panels";
}

using namespace std;

static thread manager_thread;
static bool manager_initialized = false;
os_event_t *cef_started_event = nullptr;

#if defined(_WIN32)
static int adapterCount = 0;
#endif
static std::wstring deviceId;

bool hwaccel = false;

/* ========================================================================= */

#ifdef ENABLE_NOTIFICATION_QT_LOOP
extern MessageObject messageObject;
#endif

class NotificationTask : public CefTask {
public:
	std::function<void()> task;

	inline NotificationTask(std::function<void()> task_) : task(task_) {}
	virtual void Execute() override
	{
#ifdef ENABLE_NOTIFICATION_QT_LOOP
		/* you have to put the tasks on the Qt event queue after this
		 * call otherwise the CEF message pump may stop functioning
		 * correctly, it's only supposed to take 10ms max */
		QMetaObject::invokeMethod(&messageObject, "ExecuteTask", Qt::QueuedConnection,
					  Q_ARG(MessageTask, task));
#else
		task();
#endif
	}

	IMPLEMENT_REFCOUNTING(NotificationTask);
};

bool QueueCEFTask(std::function<void()> task)
{
	return CefPostTask(TID_UI, CefRefPtr<NotificationTask>(new NotificationTask(task)));
}

/* ========================================================================= */

static const char *default_css = "\
body { \
background-color: rgba(0, 0, 0, 0); \
margin: 0px auto; \
overflow: hidden; \
}";

static void notification_source_get_defaults(obs_data_t *settings)
{
   struct dstr url_with_slug;
   dstr_init_copy(&url_with_slug, "https://beta.spectrumlive.xyz/live/");
   dstr_cat(&url_with_slug, obs_data_get_string(settings, "live_slug"));
   obs_data_set_default_string(settings, "url", url_with_slug.array);
	obs_data_set_default_int(settings, "width", 800);
	obs_data_set_default_int(settings, "height", 600);
	obs_data_set_default_int(settings, "fps", 30);
#ifdef ENABLE_BROWSER_SHARED_TEXTURE
	obs_data_set_default_bool(settings, "fps_custom", false);
#else
	obs_data_set_default_bool(settings, "fps_custom", true);
#endif
	obs_data_set_default_bool(settings, "shutdown", false);
	obs_data_set_default_bool(settings, "restart_when_active", false);
	obs_data_set_default_int(settings, "webpage_control_level", (int)DEFAULT_CONTROL_LEVEL);
	obs_data_set_default_string(settings, "css", default_css);
	obs_data_set_default_bool(settings, "reroute_audio", false);
}

static bool is_local_file_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool enabled = obs_data_get_bool(settings, "is_local_file");
	obs_property_t *url = obs_properties_get(props, "url");
	obs_property_t *local_file = obs_properties_get(props, "local_file");
	obs_property_set_visible(url, !enabled);
	obs_property_set_visible(local_file, enabled);

	return true;
}

static bool is_fps_custom(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	bool enabled = obs_data_get_bool(settings, "fps_custom");
	obs_property_t *fps = obs_properties_get(props, "fps");
	obs_property_set_visible(fps, enabled);

	return true;
}

static obs_properties_t *notification_source_get_properties(void *data)
{
   UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "width", obs_module_text("Width"), 1, 8192, 1);
	obs_properties_add_int(props, "height", obs_module_text("Height"), 1, 8192, 1);

	obs_properties_add_bool(props, "reroute_audio", obs_module_text("RerouteAudio"));

	obs_property_t *fps_set = obs_properties_add_bool(props, "fps_custom", obs_module_text("CustomFrameRate"));
	obs_property_set_modified_callback(fps_set, is_fps_custom);

#ifndef ENABLE_BROWSER_SHARED_TEXTURE
	obs_property_set_enabled(fps_set, false);
#endif

	obs_properties_add_button(props, "refreshnocache", obs_module_text("RefreshNoCache"),
				  [](obs_properties_t *, obs_property_t *, void *data) {
					  static_cast<NotificationSource *>(data)->Refresh();
					  return false;
				  });
	return props;
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	NotificationSource *bs = static_cast<NotificationSource *>(src);

	if (bs) {
		obs_source_t *source = bs->source;
		OBSDataAutoRelease settings = obs_source_get_settings(source);
		obs_data_set_string(settings, "local_file", new_path);
		obs_source_update(source, settings);
	}

	UNUSED_PARAMETER(data);
}

static obs_missing_files_t *notification_source_missingfiles(void *data)
{
	NotificationSource *bs = static_cast<NotificationSource *>(data);
	obs_missing_files_t *files = obs_missing_files_create();

	if (bs) {
		obs_source_t *source = bs->source;
      OBSDataAutoRelease settings = obs_source_get_settings(source);

		bool enabled = obs_data_get_bool(settings, "is_local_file");
		const char *path = obs_data_get_string(settings, "local_file");

		if (enabled && strcmp(path, "") != 0) {
			if (!os_file_exists(path)) {
				obs_missing_file_t *file = obs_missing_file_create(
					path, missing_file_callback, OBS_MISSING_FILE_SOURCE, bs->source, NULL);

				obs_missing_files_add_file(files, file);
			}
		}
	}

	return files;
}

static CefRefPtr<NotificationApp> app;

static void NotificationInit(void)
{
	string path = obs_get_module_binary_path(obs_current_module());
	path = path.substr(0, path.find_last_of('/') + 1);
	path += "//spt-notification-page";
#ifdef _WIN32
	path += ".exe";
	CefMainArgs args;
#else
	/* On non-windows platforms, ie macOS, we'll want to pass thru flags to
	 * CEF */
	struct obs_cmdline_args cmdline_args = obs_get_cmdline_args();
	CefMainArgs args(cmdline_args.argc, cmdline_args.argv);
#endif

	BPtr<char> conf_path = obs_module_config_path("");
	os_mkdir(conf_path);

	CefSettings settings;
	settings.log_severity = LOGSEVERITY_DISABLE;
	BPtr<char> log_path = obs_module_config_path("debug.log");
	BPtr<char> log_path_abs = os_get_abs_path_ptr(log_path);
	CefString(&settings.log_file) = log_path_abs;
	settings.windowless_rendering_enabled = true;
	settings.no_sandbox = true;

	uint32_t obs_ver = obs_get_version();
	uint32_t obs_maj = obs_ver >> 24;
	uint32_t obs_min = (obs_ver >> 16) & 0xFF;
	uint32_t obs_pat = obs_ver & 0xFFFF;

	/* This allows servers the ability to determine that notification panels and
	 * notification sources are coming from SPT. */
	std::stringstream prod_ver;
	prod_ver << "Chrome/";
	prod_ver << std::to_string(cef_version_info(4)) << "." << std::to_string(cef_version_info(5)) << "."
		 << std::to_string(cef_version_info(6)) << "." << std::to_string(cef_version_info(7));
	prod_ver << " SPT/";
	prod_ver << std::to_string(obs_maj) << "." << std::to_string(obs_min) << "." << std::to_string(obs_pat);

#if CHROME_VERSION_BUILD >= 4472
	CefString(&settings.user_agent_product) = prod_ver.str();
#else
	CefString(&settings.product_version) = prod_ver.str();
#endif

#ifdef ENABLE_NOTIFICATION_QT_LOOP
	settings.external_message_pump = true;
	settings.multi_threaded_message_loop = false;
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
	// Override locale path from SPT binary path to plugin binary path
	string locales = obs_get_module_binary_path(obs_current_module());
	locales = locales.substr(0, locales.find_last_of('/') + 1);
	locales += "locales";
	BPtr<char> abs_locales = os_get_abs_path_ptr(locales.c_str());
	CefString(&settings.locales_dir_path) = abs_locales;
#endif

	std::string obs_locale = obs_get_locale();
	std::string accepted_languages;
	if (obs_locale != "en-US") {
		accepted_languages = obs_locale;
		accepted_languages += ",";
		accepted_languages += "en-US,en";
	} else {
		accepted_languages = "en-US,en";
	}

	BPtr<char> conf_path_abs = os_get_abs_path_ptr(conf_path);
	CefString(&settings.locale) = obs_get_locale();
	CefString(&settings.accept_language_list) = accepted_languages;
#if CHROME_VERSION_BUILD <= 6533
	settings.persist_user_preferences = 1;
#endif
	CefString(&settings.cache_path) = conf_path_abs;
#if !defined(__APPLE__) || defined(ENABLE_NOTIFICATION_LEGACY)
	char *abs_path = os_get_abs_path_ptr(path.c_str());
	CefString(&settings.notification_subprocess_path) = abs_path;
	bfree(abs_path);
#endif

	bool tex_sharing_avail = false;

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
	if (hwaccel) {
		obs_enter_graphics();
		hwaccel = tex_sharing_avail = gs_shared_texture_available();
		obs_leave_graphics();
	}
#endif

#if defined(__APPLE__) || defined(_WIN32) || !defined(ENABLE_WAYLAND)
	app = new NotificationApp(tex_sharing_avail);
#else
	app = new NotificationApp(tex_sharing_avail, obs_get_nix_platform() == OBS_NIX_PLATFORM_WAYLAND);
#endif

#ifdef _WIN32
	CefExecuteProcess(args, app, nullptr);
#endif

#if !defined(_WIN32)
	BackupSignalHandlers();
	CefInitialize(args, settings, app, nullptr);
	RestoreSignalHandlers();
#elif (CHROME_VERSION_BUILD > 3770)
	CefInitialize(args, settings, app, nullptr);
#else
	/* Massive (but amazing) hack to prevent chromium from modifying our
	 * process tokens and permissions, which caused us problems with winrt,
	 * used with window capture.  Note, the structure internally is just
	 * two pointers normally.  If it causes problems with future versions
	 * we'll just switch back to the static library but I doubt we'll need
	 * to. */
	uintptr_t zeroed_memory_lol[32] = {};
	CefInitialize(args, settings, app, zeroed_memory_lol);
#endif

#if !ENABLE_LOCAL_FILE_URL_SCHEME
	/* Register http://absolute/ scheme handler for older
	 * CEF builds which do not support file:// URLs */
	CefRegisterSchemeHandlerFactory("http", "absolute", new NotificationSchemeHandlerFactory());
#endif
	os_event_signal(cef_started_event);
}

static void NotificationShutdown(void)
{
#if !ENABLE_LOCAL_FILE_URL_SCHEME
	CefClearSchemeHandlerFactories();
#endif
#ifdef ENABLE_NOTIFICATION_QT_LOOP
	while (messageObject.ExecuteNextNotificationTask())
		;
	CefDoMessageLoopWork();
#endif
	CefShutdown();
	app = nullptr;
}

#ifndef ENABLE_NOTIFICATION_QT_LOOP
static void NotificationManagerThread(void)
{
	NotificationInit();
	CefRunMessageLoop();
	NotificationShutdown();
}
#endif

extern "C" EXPORT void spt_notification_initialize(void)
{
	if (!os_atomic_set_bool(&manager_initialized, true)) {
#ifdef ENABLE_NOTIFICATION_QT_LOOP
		NotificationInit();
#else
		manager_thread = thread(NotificationManagerThread);
#endif
	}
}

void RegisterNotificationSource()
{
	struct obs_source_info info = {};
	info.id = "notification_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_INTERACTION |
			    OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_SRGB;
	info.get_properties = notification_source_get_properties;
	info.get_defaults = notification_source_get_defaults;
	info.icon_type = SPT_ICON_TYPE_NOTIFICATION;

	info.get_name = [](void *) {
		return obs_module_text("NotificationSource");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		spt_notification_initialize();
		return new NotificationSource(settings, source);
	};
	info.destroy = [](void *data) {
		static_cast<NotificationSource *>(data)->Destroy();
	};
	info.missing_files = notification_source_missingfiles;
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<NotificationSource *>(data)->Update(settings);
	};
	info.get_width = [](void *data) {
		return (uint32_t) static_cast<NotificationSource *>(data)->width;
	};
	info.get_height = [](void *data) {
		return (uint32_t) static_cast<NotificationSource *>(data)->height;
	};
	info.video_tick = [](void *data, float) {
		static_cast<NotificationSource *>(data)->Tick();
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<NotificationSource *>(data)->Render();
	};
#if CHROME_VERSION_BUILD < 4103
	info.audio_mix = [](void *data, uint64_t *ts_out, struct audio_output_data *audio_output, size_t channels,
			    size_t sample_rate) {
		return static_cast<NotificationSource *>(data)->AudioMix(ts_out, audio_output, channels, sample_rate);
	};
	info.enum_active_sources = [](void *data, obs_source_enum_proc_t cb, void *param) {
		static_cast<NotificationSource *>(data)->EnumAudioStreams(cb, param);
	};
#endif
	info.mouse_click = [](void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up,
			      uint32_t click_count) {
		static_cast<NotificationSource *>(data)->SendMouseClick(event, type, mouse_up, click_count);
	};
	info.mouse_move = [](void *data, const struct obs_mouse_event *event, bool mouse_leave) {
		static_cast<NotificationSource *>(data)->SendMouseMove(event, mouse_leave);
	};
	info.mouse_wheel = [](void *data, const struct obs_mouse_event *event, int x_delta, int y_delta) {
		static_cast<NotificationSource *>(data)->SendMouseWheel(event, x_delta, y_delta);
	};
	info.focus = [](void *data, bool focus) {
		static_cast<NotificationSource *>(data)->SendFocus(focus);
	};
	info.key_click = [](void *data, const struct obs_key_event *event, bool key_up) {
		static_cast<NotificationSource *>(data)->SendKeyClick(event, key_up);
	};
	info.show = [](void *data) {
		static_cast<NotificationSource *>(data)->SetShowing(true);
	};
	info.hide = [](void *data) {
		static_cast<NotificationSource *>(data)->SetShowing(false);
	};
	info.activate = [](void *data) {
		NotificationSource *bs = static_cast<NotificationSource *>(data);
		if (bs->restart)
			bs->Refresh();
		bs->SetActive(true);
	};
	info.deactivate = [](void *data) {
		static_cast<NotificationSource *>(data)->SetActive(false);
	};

	obs_register_source(&info);
}

/* ========================================================================= */

extern void DispatchJSEvent(std::string eventName, std::string jsonString, NotificationSource *notification = nullptr);

static void handle_obs_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
		DispatchJSEvent("obsStreamingStarting", "null");
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		DispatchJSEvent("obsStreamingStarted", "null");
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		DispatchJSEvent("obsStreamingStopping", "null");
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		DispatchJSEvent("obsStreamingStopped", "null");
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTING:
		DispatchJSEvent("obsRecordingStarting", "null");
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		DispatchJSEvent("obsRecordingStarted", "null");
		break;
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
		DispatchJSEvent("obsRecordingPaused", "null");
		break;
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
		DispatchJSEvent("obsRecordingUnpaused", "null");
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
		DispatchJSEvent("obsRecordingStopping", "null");
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		DispatchJSEvent("obsRecordingStopped", "null");
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTING:
		DispatchJSEvent("obsReplaybufferStarting", "null");
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
		DispatchJSEvent("obsReplaybufferStarted", "null");
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
		DispatchJSEvent("obsReplaybufferSaved", "null");
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPING:
		DispatchJSEvent("obsReplaybufferStopping", "null");
		break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
		DispatchJSEvent("obsReplaybufferStopped", "null");
		break;
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
		DispatchJSEvent("obsVirtualcamStarted", "null");
		break;
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
		DispatchJSEvent("obsVirtualcamStopped", "null");
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED: {
		OBSSourceAutoRelease source = obs_frontend_get_current_scene();

		if (!source)
			break;

		const char *name = obs_source_get_name(source);
		if (!name)
			break;

		nlohmann::json json = {{"name", name},
				       {"width", obs_source_get_width(source)},
				       {"height", obs_source_get_height(source)}};

		DispatchJSEvent("obsSceneChanged", json.dump());
		break;
	}
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED: {
		struct obs_frontend_source_list list = {};
		obs_frontend_get_scenes(&list);
		std::vector<const char *> scenes_vector;
		for (size_t i = 0; i < list.sources.num; i++) {
			obs_source_t *source = list.sources.array[i];
			scenes_vector.push_back(obs_source_get_name(source));
		}
		nlohmann::json json = scenes_vector;
		obs_frontend_source_list_free(&list);

		DispatchJSEvent("obsSceneListChanged", json.dump());
		break;
	}
	case OBS_FRONTEND_EVENT_TRANSITION_CHANGED: {
      OBSSourceAutoRelease source = obs_frontend_get_current_transition();

		if (!source)
			break;

		const char *name = obs_source_get_name(source);
		if (!name)
			break;

		nlohmann::json json = {{"name", name}};

		DispatchJSEvent("obsTransitionChanged", json.dump());
		break;
	}
	case OBS_FRONTEND_EVENT_TRANSITION_LIST_CHANGED: {
		struct obs_frontend_source_list list = {};
		obs_frontend_get_transitions(&list);
		std::vector<const char *> transitions_vector;
		for (size_t i = 0; i < list.sources.num; i++) {
			obs_source_t *source = list.sources.array[i];
			transitions_vector.push_back(obs_source_get_name(source));
		}
		nlohmann::json json = transitions_vector;
		obs_frontend_source_list_free(&list);

		DispatchJSEvent("obsTransitionListChanged", json.dump());
		break;
	}
	case OBS_FRONTEND_EVENT_EXIT:
		DispatchJSEvent("obsExit", "null");
		break;
	default:;
	}
}

#ifdef _WIN32
static inline void EnumAdapterCount()
{
	ComPtr<IDXGIFactory1> factory;
	ComPtr<IDXGIAdapter1> adapter;
	HRESULT hr;
	UINT i = 0;

	hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
	if (FAILED(hr))
		return;

	while (factory->EnumAdapters1(i++, &adapter) == S_OK) {
		DXGI_ADAPTER_DESC desc;

		hr = adapter->GetDesc(&desc);
		if (FAILED(hr))
			continue;

		if (i == 1)
			deviceId = desc.Description;

		/* ignore Microsoft's 'basic' renderer' */
		if (desc.VendorId == 0x1414 && desc.DeviceId == 0x8c)
			continue;

		adapterCount++;
	}
}
#endif

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
#ifdef _WIN32
static const wchar_t *blacklisted_devices[] = {L"Intel", L"Microsoft", L"Radeon HD 8850M", L"Radeon HD 7660", nullptr};

static inline bool is_intel(const std::wstring &str)
{
	return wstrstri(str.c_str(), L"Intel") != 0;
}

static void check_hwaccel_support(void)
{
	/* do not use hardware acceleration if a blacklisted device is the
	 * default and on 2 or more adapters */
	const wchar_t **device = blacklisted_devices;

	if (adapterCount >= 2 || !is_intel(deviceId)) {
		while (*device) {
			if (!!wstrstri(deviceId.c_str(), *device)) {
				hwaccel = false;
				blog(LOG_INFO, "[spt-notification]: "
					       "Blacklisted device "
					       "detected, "
					       "disabling notification "
					       "source hardware "
					       "acceleration.");
				break;
			}
			device++;
		}
	}
}
#else
static void check_hwaccel_support(void)
{
	return;
}
#endif
#endif

bool obs_module_load(void)
{
#ifdef ENABLE_NOTIFICATION_QT_LOOP
	qRegisterMetaType<MessageTask>("MessageTask");
#endif

	os_event_init(&cef_started_event, OS_EVENT_TYPE_MANUAL);

#if defined(_WIN32) && CHROME_VERSION_BUILD < 5615
	/* CefEnableHighDPISupport doesn't do anything on OS other than Windows. Would also crash macOS at this point as CEF is not directly linked */
	CefEnableHighDPISupport();
#endif

#ifdef _WIN32
	EnumAdapterCount();
#else
#if defined(__APPLE__) && !defined(ENABLE_NOTIFICATION_LEGACY)
	/* Load CEF at runtime as required on macOS */
	CefScopedLibraryLoader library_loader;
	if (!library_loader.LoadInMain())
		return false;
#endif
#endif
	blog(LOG_INFO, "[spt-notification]: Version %s", OBS_NOTIFICATION_VERSION_STRING);
	blog(LOG_INFO, "[spt-notification]: CEF Version %i.%i.%i.%i (runtime), %s (compiled)", cef_version_info(4),
	     cef_version_info(5), cef_version_info(6), cef_version_info(7), CEF_VERSION);

	RegisterNotificationSource();
	obs_frontend_add_event_callback(handle_obs_frontend_event, nullptr);

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
   OBSDataAutoRelease private_data = obs_get_private_data();
   hwaccel = obs_data_get_bool(private_data, "BrowserHWAccel");

   if (hwaccel) {
      check_hwaccel_support();
   }
#endif

#if defined(__APPLE__) && CHROME_VERSION_BUILD < 4183
	// Make sure CEF malloc hijacking happens early in the process
	spt_notification_initialize();
#endif

	return true;
}

void obs_module_post_load(void)
{
	auto vendor = obs_websocket_register_vendor("spt-notification");
	if (!vendor)
		return;

	auto emit_event_request_cb = [](obs_data_t *request_data, obs_data_t *, void *) {
		const char *event_name = obs_data_get_string(request_data, "event_name");
		if (!event_name)
			return;

      OBSDataAutoRelease event_data = obs_data_get_obj(request_data, "event_data");
		const char *event_data_string = event_data ? obs_data_get_json(event_data) : "{}";

		DispatchJSEvent(event_name, event_data_string, nullptr);
	};

	if (!obs_websocket_vendor_register_request(vendor, "emit_event", emit_event_request_cb, nullptr))
		blog(LOG_WARNING, "[spt-notification]: Failed to register obs-websocket request emit_event");
}

void obs_module_unload(void)
{
#ifdef ENABLE_NOTIFICATION_QT_LOOP
	NotificationShutdown();
#else
	if (manager_thread.joinable()) {
		while (!QueueCEFTask([]() { CefQuitMessageLoop(); }))
			os_sleep_ms(5);

		manager_thread.join();
	}
#endif

	os_event_destroy(cef_started_event);
}
