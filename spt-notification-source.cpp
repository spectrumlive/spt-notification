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

#include "spt-notification-source.hpp"
#include "notification-client.hpp"
#include "notification-scheme.hpp"
#include "wide-string.hpp"
#include <nlohmann/json.hpp>
#include <util/threading.h>
#include <QApplication>
#include <util/dstr.h>
#include <functional>
#include <thread>
#include <mutex>

#ifdef __linux__
#include "linux-keyboard-helpers.hpp"
#endif

#ifdef ENABLE_NOTIFICATION_QT_LOOP
#include <QEventLoop>
#include <QThread>
#endif

using namespace std;

extern bool QueueCEFTask(std::function<void()> task);

static mutex notification_list_mutex;
static NotificationSource *first_notification = nullptr;

static void SendNotificationVisibility(CefRefPtr<CefBrowser> notification, bool isVisible)
{
	if (!notification)
		return;

#if ENABLE_WASHIDDEN
	if (isVisible) {
		notification->GetHost()->WasResized();
		notification->GetHost()->WasHidden(false);
		notification->GetHost()->Invalidate(PET_VIEW);
	} else {
		notification->GetHost()->WasHidden(true);
	}
#endif

	CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("Visibility");
	CefRefPtr<CefListValue> args = msg->GetArgumentList();
	args->SetBool(0, isVisible);
	SendNotificationProcessMessage(notification, PID_RENDERER, msg);
}

void DispatchJSEvent(std::string eventName, std::string jsonString, NotificationSource *notification = nullptr);

NotificationSource::NotificationSource(obs_data_t *, obs_source_t *source_) : source(source_)
{

	/* Register Refresh hotkey */
	auto refreshFunction = [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
		if (pressed) {
			NotificationSource *bs = (NotificationSource *)data;
			bs->Refresh();
		}
	};

	obs_hotkey_register_source(source, "ObsNotification.Refresh", obs_module_text("RefreshNoCache"), refreshFunction,
				   (void *)this);

	auto jsEventFunction = [](void *p, calldata_t *calldata) {
		const auto eventName = calldata_string(calldata, "eventName");
		if (!eventName)
			return;
		auto jsonString = calldata_string(calldata, "jsonString");
		if (!jsonString)
			jsonString = "null";
		DispatchJSEvent(eventName, jsonString, (NotificationSource *)p);
	};

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void javascript_event(string eventName, string jsonString)", jsEventFunction,
			 (void *)this);

	/* defer update */
	obs_source_update(source, nullptr);

	lock_guard<mutex> lock(notification_list_mutex);
	p_prev_next = &first_notification;
	next = first_notification;
	if (first_notification)
		first_notification->p_prev_next = &next;
	first_notification = this;
}

static void ActuallyCloseNotification(CefRefPtr<CefBrowser> cefNotification)
{
	CefRefPtr<CefClient> client = cefNotification->GetHost()->GetClient();
	NotificationClient *bc = reinterpret_cast<NotificationClient *>(client.get());
	if (bc) {
		bc->bs = nullptr;
	}

	/*
         * This stops rendering
         * http://magpcss.org/ceforum/viewtopic.php?f=6&t=12079
         * https://bitbucket.org/chromiumembedded/cef/issues/1363/washidden-api-got-broken-on-branch-2062)
         */
	cefNotification->GetHost()->WasHidden(true);
	cefNotification->GetHost()->CloseBrowser(true);
}

NotificationSource::~NotificationSource()
{
	if (cefNotification)
		ActuallyCloseNotification(cefNotification);
}

void NotificationSource::Destroy()
{
	destroying = true;
	DestroyTextures();

	lock_guard<mutex> lock(notification_list_mutex);
	if (next)
		next->p_prev_next = p_prev_next;
	*p_prev_next = next;

	QueueCEFTask([this]() { delete this; });
}

void NotificationSource::ExecuteOnNotification(NotificationFunc func, bool async)
{
	if (!async) {
#ifdef ENABLE_NOTIFICATION_QT_LOOP
		if (QThread::currentThread() == qApp->thread()) {
			if (!!cefNotification)
				func(cefNotification);
			return;
		}
#endif
		os_event_t *finishedEvent;
		os_event_init(&finishedEvent, OS_EVENT_TYPE_AUTO);
		bool success = QueueCEFTask([&]() {
			if (!!cefNotification)
				func(cefNotification);
			os_event_signal(finishedEvent);
		});
		if (success) {
			os_event_wait(finishedEvent);
		}
		os_event_destroy(finishedEvent);
	} else {
		CefRefPtr<CefBrowser> notification = GetNotification();
		if (!!notification) {
#ifdef ENABLE_NOTIFICATION_QT_LOOP
			QueueNotificationTask(cefNotification, func);
#else
			QueueCEFTask([=]() { func(notification); });
#endif
		}
	}
}

bool NotificationSource::CreateNotification()
{
	return QueueCEFTask([this]() {
#ifdef ENABLE_BROWSER_SHARED_TEXTURE
		if (hwaccel) {
			obs_enter_graphics();
			tex_sharing_avail = gs_shared_texture_available();
			obs_leave_graphics();
		}
#else
		bool hwaccel = false;
#endif

		CefRefPtr<NotificationClient> notificationClient =
			new NotificationClient(this, hwaccel && tex_sharing_avail, reroute_audio, webpage_control_level);

		CefWindowInfo windowInfo;
#if CHROME_VERSION_BUILD < 4430
		windowInfo.width = width;
		windowInfo.height = height;
#else
		windowInfo.bounds.width = width;
		windowInfo.bounds.height = height;
#endif
		windowInfo.windowless_rendering_enabled = true;

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
		windowInfo.shared_texture_enabled = hwaccel;
#endif

		CefBrowserSettings cefNotificationSettings;

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
#ifdef NOTIFICATION_EXTERNAL_BEGIN_FRAME_ENABLED
		if (!fps_custom) {
			windowInfo.external_begin_frame_enabled = true;
			cefNotificationSettings.windowless_frame_rate = 0;
		} else {
			cefNotificationSettings.windowless_frame_rate = fps;
		}
#else
		struct obs_video_info ovi;
		obs_get_video_info(&ovi);
		canvas_fps = (double)ovi.fps_num / (double)ovi.fps_den;
		cefNotificationSettings.windowless_frame_rate = (fps_custom) ? fps : canvas_fps;
#endif
#else
		cefNotificationSettings.windowless_frame_rate = fps;
#endif

		cefNotificationSettings.default_font_size = 16;
		cefNotificationSettings.default_fixed_font_size = 16;

#if ENABLE_LOCAL_FILE_URL_SCHEME && CHROME_VERSION_BUILD < 4430
		if (is_local) {
			/* Disable web security for file:// URLs to allow
			 * local content access to remote APIs */
			cefNotificationSettings.web_security = STATE_DISABLED;
		}
#endif
		auto notification = CefBrowserHost::CreateBrowserSync(windowInfo, notificationClient, url, cefNotificationSettings,
								 CefRefPtr<CefDictionaryValue>(), nullptr);

		SetNotification(notification);

		if (reroute_audio)
			cefNotification->GetHost()->SetAudioMuted(true);
		if (obs_source_showing(source))
			is_showing = true;

		SendNotificationVisibility(cefNotification, is_showing);
	});
}

void NotificationSource::DestroyNotification()
{
	ExecuteOnNotification(ActuallyCloseNotification, true);
	SetNotification(nullptr);
}
#if CHROME_VERSION_BUILD < 4103
void NotificationSource::ClearAudioStreams()
{
	QueueCEFTask([this]() {
		audio_streams.clear();
		std::lock_guard<std::mutex> lock(audio_sources_mutex);
		audio_sources.clear();
	});
}
#endif
void NotificationSource::SendMouseClick(const struct obs_mouse_event *event, int32_t type, bool mouse_up,
				   uint32_t click_count)
{
	uint32_t modifiers = event->modifiers;
	int32_t x = event->x;
	int32_t y = event->y;

	ExecuteOnNotification(
		[=](CefRefPtr<CefBrowser> cefNotification) {
			CefMouseEvent e;
			e.modifiers = modifiers;
			e.x = x;
			e.y = y;
			CefBrowserHost::MouseButtonType buttonType = (CefBrowserHost::MouseButtonType)type;
			cefNotification->GetHost()->SendMouseClickEvent(e, buttonType, mouse_up, click_count);
		},
		true);
}

void NotificationSource::SendMouseMove(const struct obs_mouse_event *event, bool mouse_leave)
{
	uint32_t modifiers = event->modifiers;
	int32_t x = event->x;
	int32_t y = event->y;

	ExecuteOnNotification(
		[=](CefRefPtr<CefBrowser> cefNotification) {
			CefMouseEvent e;
			e.modifiers = modifiers;
			e.x = x;
			e.y = y;
			cefNotification->GetHost()->SendMouseMoveEvent(e, mouse_leave);
		},
		true);
}

void NotificationSource::SendMouseWheel(const struct obs_mouse_event *event, int x_delta, int y_delta)
{
	uint32_t modifiers = event->modifiers;
	int32_t x = event->x;
	int32_t y = event->y;

	ExecuteOnNotification(
		[=](CefRefPtr<CefBrowser> cefNotification) {
			CefMouseEvent e;
			e.modifiers = modifiers;
			e.x = x;
			e.y = y;
			cefNotification->GetHost()->SendMouseWheelEvent(e, x_delta, y_delta);
		},
		true);
}

void NotificationSource::SendFocus(bool focus)
{
	ExecuteOnNotification(
		[=](CefRefPtr<CefBrowser> cefNotification) {
#if CHROME_VERSION_BUILD < 4430
			cefNotification->GetHost()->SendFocusEvent(focus);
#else
			cefNotification->GetHost()->SetFocus(focus);
#endif
		},
		true);
}

void NotificationSource::SendKeyClick(const struct obs_key_event *event, bool key_up)
{
	if (destroying)
		return;

	std::string text = event->text;
#ifdef __linux__
	uint32_t native_vkey = KeyboardCodeFromXKeysym(event->native_vkey);
	uint32_t modifiers = event->native_modifiers;
#elif defined(_WIN32) || defined(__APPLE__)
	uint32_t native_vkey = event->native_vkey;
	uint32_t modifiers = event->modifiers;
#else
	uint32_t native_vkey = event->native_vkey;
	uint32_t native_scancode = event->native_scancode;
	uint32_t modifiers = event->native_modifiers;
#endif

	ExecuteOnNotification(
		[=](CefRefPtr<CefBrowser> cefNotification) {
			CefKeyEvent e;
			e.windows_key_code = native_vkey;
#ifdef __APPLE__
			e.native_key_code = native_vkey;
#endif

			e.type = key_up ? KEYEVENT_KEYUP : KEYEVENT_RAWKEYDOWN;

			if (!text.empty()) {
				wstring wide = to_wide(text);
				if (wide.size())
					e.character = wide[0];
			}

			//e.native_key_code = native_vkey;
			e.modifiers = modifiers;

			cefNotification->GetHost()->SendKeyEvent(e);
			if (!text.empty() && !key_up) {
				e.type = KEYEVENT_CHAR;
#ifdef __linux__
				e.windows_key_code = KeyboardCodeFromXKeysym(e.character);
#elif defined(_WIN32)
				e.windows_key_code = e.character;
#elif !defined(__APPLE__)
				e.native_key_code = native_scancode;
#endif
				cefNotification->GetHost()->SendKeyEvent(e);
			}
		},
		true);
}

void NotificationSource::SetShowing(bool showing)
{
	if (destroying)
		return;

	is_showing = showing;

	if (shutdown_on_invisible) {
		if (showing) {
			Update();
		} else {
			DestroyNotification();
		}
	} else {
		ExecuteOnNotification(
			[=](CefRefPtr<CefBrowser> cefNotification) {
				CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("Visibility");
				CefRefPtr<CefListValue> args = msg->GetArgumentList();
				args->SetBool(0, showing);
				SendNotificationProcessMessage(cefNotification, PID_RENDERER, msg);
			},
			true);
		nlohmann::json json;
		json["visible"] = showing;
		DispatchJSEvent("obsSourceVisibleChanged", json.dump(), this);
#if defined(NOTIFICATION_EXTERNAL_BEGIN_FRAME_ENABLED) && defined(ENABLE_BROWSER_SHARED_TEXTURE)
		if (showing && !fps_custom) {
			reset_frame = false;
		}
#endif

		SendNotificationVisibility(cefNotification, showing);

		if (showing)
			return;

		obs_enter_graphics();

		if (!hwaccel && texture) {
			DestroyTextures();
		}

		obs_leave_graphics();
	}
}

void NotificationSource::SetActive(bool active)
{
	ExecuteOnNotification(
		[=](CefRefPtr<CefBrowser> cefNotification) {
			CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("Active");
			CefRefPtr<CefListValue> args = msg->GetArgumentList();
			args->SetBool(0, active);
			SendNotificationProcessMessage(cefNotification, PID_RENDERER, msg);
		},
		true);
	nlohmann::json json;
	json["active"] = active;
	DispatchJSEvent("obsSourceActiveChanged", json.dump(), this);
}

void NotificationSource::Refresh()
{
	ExecuteOnNotification([](CefRefPtr<CefBrowser> cefNotification) { cefNotification->ReloadIgnoreCache(); }, true);
}

void NotificationSource::SetNotification(CefRefPtr<CefBrowser> b)
{
	std::lock_guard<std::recursive_mutex> auto_lock(lockNotification);
	cefNotification = b;
}

CefRefPtr<CefBrowser> NotificationSource::GetNotification()
{
	std::lock_guard<std::recursive_mutex> auto_lock(lockNotification);
	return cefNotification;
}

#ifdef ENABLE_BROWSER_SHARED_TEXTURE
#ifdef NOTIFICATION_EXTERNAL_BEGIN_FRAME_ENABLED
inline void NotificationSource::SignalBeginFrame()
{
	if (reset_frame) {
		ExecuteOnNotification(
			[](CefRefPtr<CefBrowser> cefNotification) { cefNotification->GetHost()->SendExternalBeginFrame(); },
			true);

		reset_frame = false;
	}
}
#endif
#endif

void NotificationSource::Update(obs_data_t *settings)
{
	if (settings) {
		bool n_is_local;
		int n_width;
		int n_height;
		bool n_fps_custom;
		int n_fps;
		bool n_shutdown;
		bool n_restart;
		bool n_reroute;
		ControlLevel n_webpage_control_level;
		std::string n_url;
		std::string n_css;

		n_is_local = obs_data_get_bool(settings, "is_local_file");
		n_width = (int)obs_data_get_int(settings, "width");
		n_height = (int)obs_data_get_int(settings, "height");
		n_fps_custom = obs_data_get_bool(settings, "fps_custom");
		n_fps = (int)obs_data_get_int(settings, "fps");
		n_shutdown = obs_data_get_bool(settings, "shutdown");
		n_restart = obs_data_get_bool(settings, "restart_when_active");
		n_css = obs_data_get_string(settings, "css");
		n_url = obs_data_get_string(settings, n_is_local ? "local_file" : "url");
		n_reroute = obs_data_get_bool(settings, "reroute_audio");
		n_webpage_control_level =
			static_cast<ControlLevel>(obs_data_get_int(settings, "webpage_control_level"));

		if (n_is_local && !n_url.empty()) {
			n_url = CefURIEncode(n_url, false);

#ifdef _WIN32
			size_t slash = n_url.find("%2F");
			size_t colon = n_url.find("%3A");

			if (slash != std::string::npos && colon != std::string::npos && colon < slash)
				n_url.replace(colon, 3, ":");
#endif

			while (n_url.find("%5C") != std::string::npos)
				n_url.replace(n_url.find("%5C"), 3, "/");

			while (n_url.find("%2F") != std::string::npos)
				n_url.replace(n_url.find("%2F"), 3, "/");

#if !ENABLE_LOCAL_FILE_URL_SCHEME
			/* http://absolute/ based mapping for older CEF */
			n_url = "http://absolute/" + n_url;
#elif defined(_WIN32)
			/* Widows-style local file URL:
			 * file:///C:/file/path.webm */
			n_url = "file:///" + n_url;
#else
			/* UNIX-style local file URL:
			 * file:///home/user/file.webm */
			n_url = "file://" + n_url;
#endif
		}

#if ENABLE_LOCAL_FILE_URL_SCHEME
		if (astrcmpi_n(n_url.c_str(), "http://absolute/", 16) == 0) {
			/* Replace http://absolute/ URLs with file://
			 * URLs if file:// URLs are enabled */
			n_url = "file:///" + n_url.substr(16);
			n_is_local = true;
		}
#endif

		if (n_is_local == is_local && n_fps_custom == fps_custom && n_fps == fps &&
		    n_shutdown == shutdown_on_invisible && n_restart == restart && n_css == css && n_url == url &&
		    n_reroute == reroute_audio && n_webpage_control_level == webpage_control_level) {

			if (n_width == width && n_height == height)
				return;

			width = n_width;
			height = n_height;
			ExecuteOnNotification(
				[=](CefRefPtr<CefBrowser> cefNotification) {
					const CefSize cefSize(width, height);
					cefNotification->GetHost()->GetClient()->GetDisplayHandler()->OnAutoResize(
						cefNotification, cefSize);
					cefNotification->GetHost()->WasResized();
					cefNotification->GetHost()->Invalidate(PET_VIEW);
				},
				true);
			return;
		}

		is_local = n_is_local;
		width = n_width;
		height = n_height;
		fps = n_fps;
		fps_custom = n_fps_custom;
		shutdown_on_invisible = n_shutdown;
		reroute_audio = n_reroute;
		webpage_control_level = n_webpage_control_level;
		restart = n_restart;
		css = n_css;
		url = n_url;

		obs_source_set_audio_active(source, reroute_audio);
	}

	DestroyNotification();
	DestroyTextures();
#if CHROME_VERSION_BUILD < 4103
	ClearAudioStreams();
#endif
	if (!shutdown_on_invisible || obs_source_showing(source))
		create_notification = true;

	first_update = false;
}

void NotificationSource::Tick()
{
	if (create_notification && CreateNotification())
		create_notification = false;
#if defined(ENABLE_BROWSER_SHARED_TEXTURE)
#if defined(NOTIFICATION_EXTERNAL_BEGIN_FRAME_ENABLED)
	if (!fps_custom)
		reset_frame = true;
#else
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	double video_fps = (double)ovi.fps_num / (double)ovi.fps_den;

	if (!fps_custom) {
		if (!!cefNotification && canvas_fps != video_fps) {
			cefNotification->GetHost()->SetWindowlessFrameRate(video_fps);
			canvas_fps = video_fps;
		}
	}
#endif
#endif
}

extern void ProcessCef();

void NotificationSource::Render()
{
	bool flip = false;
#if defined(ENABLE_BROWSER_SHARED_TEXTURE) && CHROME_VERSION_BUILD < 6367
	flip = hwaccel;
#endif

	if (texture) {
#ifdef __APPLE__
		gs_effect_t *effect = obs_get_base_effect((hwaccel) ? OBS_EFFECT_DEFAULT_RECT : OBS_EFFECT_DEFAULT);
#else
		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
#endif

		bool linear_sample = extra_texture == NULL;
		gs_texture_t *draw_texture = texture;
		if (!linear_sample && !obs_source_get_texcoords_centered(source)) {
			gs_copy_texture(extra_texture, texture);
			draw_texture = extra_texture;

			linear_sample = true;
		}

		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

		gs_eparam_t *const image = gs_effect_get_param_by_name(effect, "image");

		const char *tech;
		if (linear_sample) {
			gs_effect_set_texture_srgb(image, draw_texture);
			tech = "Draw";
		} else {
			gs_effect_set_texture(image, draw_texture);
			tech = "DrawSrgbDecompress";
		}

		const uint32_t flip_flag = flip ? GS_FLIP_V : 0;
		while (gs_effect_loop(effect, tech))
			gs_draw_sprite(draw_texture, flip_flag, 0, 0);

		gs_blend_state_pop();

		gs_enable_framebuffer_srgb(previous);
	}

#if defined(NOTIFICATION_EXTERNAL_BEGIN_FRAME_ENABLED) && defined(ENABLE_BROWSER_SHARED_TEXTURE)
	SignalBeginFrame();
#elif defined(ENABLE_NOTIFICATION_QT_LOOP)
	ProcessCef();
#endif
}

static void ExecuteOnNotification(NotificationFunc func, NotificationSource *bs)
{
	lock_guard<mutex> lock(notification_list_mutex);

	if (bs) {
		NotificationSource *bsw = reinterpret_cast<NotificationSource *>(bs);
		bsw->ExecuteOnNotification(func, true);
	}
}

static void ExecuteOnAllNotifications(NotificationFunc func)
{
	lock_guard<mutex> lock(notification_list_mutex);

	NotificationSource *bs = first_notification;
	while (bs) {
		NotificationSource *bsw = reinterpret_cast<NotificationSource *>(bs);
		bsw->ExecuteOnNotification(func, true);
		bs = bs->next;
	}
}

void DispatchJSEvent(std::string eventName, std::string jsonString, NotificationSource *notification)
{
	const auto jsEvent = [=](CefRefPtr<CefBrowser> cefNotification) {
		CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("DispatchJSEvent");
		CefRefPtr<CefListValue> args = msg->GetArgumentList();

		args->SetString(0, eventName);
		args->SetString(1, jsonString);
		SendNotificationProcessMessage(cefNotification, PID_RENDERER, msg);
	};

	if (!notification)
		ExecuteOnAllNotifications(jsEvent);
	else
		ExecuteOnNotification(jsEvent, notification);
}
