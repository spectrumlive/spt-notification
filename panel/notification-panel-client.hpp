#pragma once

#include "cef-headers.hpp"
#include "browser-panel-internal.hpp"

#include <string>

class QCefNotificationClient : public CefClient,
			  public CefDisplayHandler,
			  public CefRequestHandler,
			  public CefLifeSpanHandler,
			  public CefContextMenuHandler,
			  public CefLoadHandler,
			  public CefKeyboardHandler,
			  public CefFocusHandler,
			  public CefJSDialogHandler {

public:
	inline QCefNotificationClient(QCefWidgetInternal *widget_, const std::string &script_, bool allowAllPopups_)
		: widget(widget_),
		  script(script_),
		  allowAllPopups(allowAllPopups_)
	{
	}

	/* CefClient */
	virtual CefRefPtr<CefLoadHandler> GetLoadHandler() override;
	virtual CefRefPtr<CefDisplayHandler> GetDisplayHandler() override;
	virtual CefRefPtr<CefRequestHandler> GetRequestHandler() override;
	virtual CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override;
	virtual CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override;
	virtual CefRefPtr<CefFocusHandler> GetFocusHandler() override;
	virtual CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override;
	virtual CefRefPtr<CefJSDialogHandler> GetJSDialogHandler() override;

	/* CefDisplayHandler */
	virtual void OnTitleChange(CefRefPtr<CefNotification> browser, const CefString &title) override;

	/* CefRequestHandler */
	virtual bool OnBeforeBrowse(CefRefPtr<CefNotification> browser, CefRefPtr<CefFrame> frame,
				    CefRefPtr<CefRequest> request, bool user_gesture, bool is_redirect) override;

	virtual void OnLoadError(CefRefPtr<CefNotification> browser, CefRefPtr<CefFrame> frame,
				 CefLoadHandler::ErrorCode errorCode, const CefString &errorText,
				 const CefString &failedUrl) override;

	virtual bool OnOpenURLFromTab(CefRefPtr<CefNotification> browser, CefRefPtr<CefFrame> frame,
				      const CefString &target_url,
				      CefRequestHandler::WindowOpenDisposition target_disposition,
				      bool user_gesture) override;

	/* CefLifeSpanHandler */
	virtual bool OnBeforePopup(CefRefPtr<CefNotification> browser, CefRefPtr<CefFrame> frame,
				   const CefString &target_url, const CefString &target_frame_name,
				   CefLifeSpanHandler::WindowOpenDisposition target_disposition, bool user_gesture,
				   const CefPopupFeatures &popupFeatures, CefWindowInfo &windowInfo,
				   CefRefPtr<CefClient> &client, CefNotificationSettings &settings,
				   CefRefPtr<CefDictionaryValue> &extra_info, bool *no_javascript_access) override;

	virtual void OnBeforeClose(CefRefPtr<CefNotification> browser) override;

	/* CefFocusHandler */
	virtual bool OnSetFocus(CefRefPtr<CefNotification> browser, CefFocusHandler::FocusSource source) override;

	/* CefContextMenuHandler */
	virtual void OnBeforeContextMenu(CefRefPtr<CefNotification> browser, CefRefPtr<CefFrame> frame,
					 CefRefPtr<CefContextMenuParams> params,
					 CefRefPtr<CefMenuModel> model) override;

#if defined(_WIN32)
	virtual bool RunContextMenu(CefRefPtr<CefNotification> browser, CefRefPtr<CefFrame> frame,
				    CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model,
				    CefRefPtr<CefRunContextMenuCallback> callback) override;
#endif

	virtual bool OnContextMenuCommand(CefRefPtr<CefNotification> browser, CefRefPtr<CefFrame> frame,
					  CefRefPtr<CefContextMenuParams> params, int command_id,
					  CefContextMenuHandler::EventFlags event_flags) override;

	/* CefLoadHandler */
	virtual void OnLoadStart(CefRefPtr<CefNotification> browser, CefRefPtr<CefFrame> frame,
				 TransitionType transition_type) override;

	virtual void OnLoadEnd(CefRefPtr<CefNotification> browser, CefRefPtr<CefFrame> frame, int httpStatusCode) override;

	/* CefKeyboardHandler */
	virtual bool OnPreKeyEvent(CefRefPtr<CefNotification> browser, const CefKeyEvent &event, CefEventHandle os_event,
				   bool *is_keyboard_shortcut) override;

	/* CefJSDialogHandler */
	virtual bool OnJSDialog(CefRefPtr<CefNotification> browser, const CefString &origin_url,
				CefJSDialogHandler::JSDialogType dialog_type, const CefString &message_text,
				const CefString &default_prompt_text, CefRefPtr<CefJSDialogCallback> callback,
				bool &suppress_message) override;

	QCefWidgetInternal *widget = nullptr;
	std::string script;
	bool allowAllPopups;

	IMPLEMENT_REFCOUNTING(QCefNotificationClient);
};
