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

#pragma once

#include <map>
#include <unordered_map>
#include <functional>
#include "cef-headers.hpp"

typedef std::function<void(CefRefPtr<CefBrowser>)> NotificationFunc;

#ifdef ENABLE_NOTIFICATION_QT_LOOP
#include <QObject>
#include <QTimer>
#include <mutex>
#include <deque>

typedef std::function<void()> MessageTask;

class MessageObject : public QObject {
	Q_OBJECT

	friend void QueueNotificationTask(CefRefPtr<CefBrowser> notification, NotificationFunc func);

	struct Task {
		CefRefPtr<CefBrowser> notification;
		NotificationFunc func;

		inline Task() {}
		inline Task(CefRefPtr<CefBrowser> notification_, NotificationFunc func_) : notification(notification_), func(func_) {}
	};

	std::mutex notificationTaskMutex;
	std::deque<Task> notificationTasks;

public slots:
	bool ExecuteNextNotificationTask();
	void ExecuteTask(MessageTask task);
	void DoCefMessageLoop(int ms);
	void Process();
};

extern void QueueNotificationTask(CefRefPtr<CefBrowser> notification, NotificationFunc func);
#endif

class NotificationApp : public CefApp, public CefRenderProcessHandler, public CefBrowserProcessHandler, public CefV8Handler {

	void ExecuteJSFunction(CefRefPtr<CefBrowser> notification, const char *functionName, CefV8ValueList arguments);

	typedef std::map<int, CefRefPtr<CefV8Value>> CallbackMap;

	bool shared_texture_available;
	CallbackMap callbackMap;
	int callbackId;
#if !defined(__APPLE__) && !defined(_WIN32)
	bool wayland;
#endif

public:
#if defined(__APPLE__) || defined(_WIN32)
	inline NotificationApp(bool shared_texture_available_ = false) : shared_texture_available(shared_texture_available_)
#else
	inline NotificationApp(bool shared_texture_available_ = false, bool wayland_ = false)
		: shared_texture_available(shared_texture_available_),
		  wayland(wayland_)
#endif
	{
	}

	virtual CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override;
	virtual CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override;
	virtual void OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command_line) override;
	virtual void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;
	virtual void OnBeforeCommandLineProcessing(const CefString &process_type,
						   CefRefPtr<CefCommandLine> command_line) override;
	virtual void OnContextCreated(CefRefPtr<CefBrowser> notification, CefRefPtr<CefFrame> frame,
				      CefRefPtr<CefV8Context> context) override;
	virtual bool OnProcessMessageReceived(CefRefPtr<CefBrowser> notification, CefRefPtr<CefFrame> frame,
					      CefProcessId source_process,
					      CefRefPtr<CefProcessMessage> message) override;
	virtual bool Execute(const CefString &name, CefRefPtr<CefV8Value> object, const CefV8ValueList &arguments,
			     CefRefPtr<CefV8Value> &retval, CefString &exception) override;

#ifdef ENABLE_NOTIFICATION_QT_LOOP
#if CHROME_VERSION_BUILD < 5938
	virtual void OnScheduleMessagePumpWork(int64 delay_ms) override;
#else
	virtual void OnScheduleMessagePumpWork(int64_t delay_ms) override;
#endif
	QTimer frameTimer;
#endif

#if !ENABLE_WASHIDDEN
	std::unordered_map<int, bool> notificationVis;

	void SetFrameDocumentVisibility(CefRefPtr<CefBrowser> notification, CefRefPtr<CefFrame> frame, bool isVisible);
	void SetDocumentVisibility(CefRefPtr<CefBrowser> notification, bool isVisible);
#endif

	IMPLEMENT_REFCOUNTING(NotificationApp);
};
